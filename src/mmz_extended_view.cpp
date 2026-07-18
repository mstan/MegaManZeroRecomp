#include "mmz_extended_view.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <vector>

#include "gba_bus.h"
#include "gba_ppu.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"

extern "C" unsigned g_ws_extra_left;
extern "C" unsigned g_ws_extra_right;

namespace mmz {
namespace {

constexpr std::uint32_t kScrollPublish = 0x0800527Cu;
constexpr std::uint32_t kPpuPublish = 0x08001B68u;
constexpr std::uint32_t kTextBgStreamer = 0x08003560u;
constexpr std::uint32_t kTextBgFullReload = 0x08003F1Cu;
constexpr std::uint32_t kBg1FullReload = 0x08004ED8u;
constexpr std::uint32_t kBg1Streamer = 0x0800507Cu;
constexpr std::uint32_t kBg1State = 0x02005004u;
constexpr std::uint32_t kBg2State = 0x02005074u;
constexpr std::uint32_t kBg3State = 0x020050E4u;
constexpr std::uint32_t kBg2Vram = 0x06002000u;
constexpr std::uint32_t kBg3Vram = 0x06003000u;
constexpr std::uint32_t kBg1Map = 0x0202165Cu;
constexpr std::uint32_t kBgcntShadow = 0x02004F36u;
constexpr std::uint32_t kCamera = 0x02022C44u;
constexpr std::uint32_t kScreenY = 0x08290DECu;
constexpr std::uint32_t kScreenX = 0x082915ECu;
constexpr std::uint32_t kRoomGrid = 0x082921ECu;
constexpr std::uint32_t kDescriptorFamilies = 0x08293BECu;
constexpr std::uint32_t kVariantSelectors = 0x02005208u;
constexpr std::uint32_t kHudState = 0x02023640u;
constexpr std::uint32_t kPlayerHudDraw = 0x080BC48Cu;
constexpr std::uint32_t kBossHudDraw = 0x080BC680u;
// UpdateSpawnManager's ordered spawn-point scans use strict camera-X +/- 12
// metatile bounds. These are the three decoded immediate sites, independently
// identified in MMZ1 and corroborated by RMZ3's UpdateSpawnManager decomp.
constexpr std::uint32_t kSpawnLeftBoundInitial = 0x0800E2D2u;
constexpr std::uint32_t kSpawnLeftBoundForward = 0x0800E2FEu;
constexpr std::uint32_t kSpawnRightBound = 0x0800E31Cu;
constexpr std::size_t kMapBytes = 0x1000u;
constexpr std::uint32_t kSyntheticReturn = 0xF0000001u;

struct StreamCall {
    bool valid = false;
    std::uint32_t state = 0;
    std::uint32_t target_pc = 0;
    std::uint32_t vram_base = 0;
    std::uint32_t r2 = 0;
    std::uint32_t r3 = 0;
    // 0x08003560 reads the persistent layer state through +0x14. The transient
    // current-position argument is only the leading X/Y pair.
    std::array<std::uint32_t, 6> previous{};
    std::array<std::uint32_t, 2> current{};
};

std::array<StreamCall, 3> g_observed{};
void (*g_previous_hook)(std::uint32_t) = nullptr;
bool g_in_synthetic_stream = false;
unsigned g_extra_left = 0;
unsigned g_extra_right = 0;
unsigned g_seen_layers = 0;
unsigned g_layers_updated_since_publish = 0;
std::array<std::array<std::array<std::uint8_t, kMapBytes>, 2>, 3>
    g_margin_maps{};
bool g_margin_valid[3][2]{};
std::array<std::array<std::uint32_t, 3>, 3> g_layout_signatures{};
std::array<bool, 3> g_layout_signature_valid{};
std::uint32_t g_bg1_data = 0;
std::int32_t g_bg1_camera_x = 0;
int (*g_previous_rom_override)(std::uint32_t, std::uint32_t,
                               std::uint32_t*) = nullptr;
RuntimeThumbAluImmediateOverride g_previous_thumb_alu_imm_override = nullptr;
int (*g_previous_obj_x_provider)(int, int*) = nullptr;
int (*g_previous_bg_x_provider)(int, int, int, int*) = nullptr;
std::array<bool, 12> g_obj_clip_literal_seen{};
std::array<bool, 3> g_spawn_bound_seen{};
unsigned long long g_seen_state_epoch = 0;
std::array<std::array<std::int32_t, 2>, 3> g_last_layer_position{};
std::array<bool, 3> g_last_layer_position_valid{};
int g_last_pillarbox = -1;
int g_last_pillarbox_left = -1;
int g_last_pillarbox_right = -1;
bool g_player_hud_pending = false;
bool g_boss_hud_pending = false;
bool g_player_hud_active = false;
bool g_boss_hud_active = false;

bool trace_enabled();

int thumb_alu_immediate_override(std::uint32_t pc, std::uint32_t original,
                                 std::uint32_t* out_value) {
    if (out_value && original == 12u) {
        unsigned extra_cells = 0;
        std::size_t trace_index = 0;
        if (pc == kSpawnLeftBoundInitial) {
            extra_cells = (g_extra_left + 15u) / 16u;
            trace_index = 0;
        } else if (pc == kSpawnLeftBoundForward) {
            extra_cells = (g_extra_left + 15u) / 16u;
            trace_index = 1;
        } else if (pc == kSpawnRightBound) {
            extra_cells = (g_extra_right + 15u) / 16u;
            trace_index = 2;
        } else {
            return g_previous_thumb_alu_imm_override
                ? g_previous_thumb_alu_imm_override(pc, original, out_value)
                : 0;
        }
        // Return zero for the native-width case so the generated instruction
        // consumes its compile-time operand exactly as before.
        if (extra_cells != 0u) {
            *out_value = 12u + extra_cells;
            if (trace_enabled() && !g_spawn_bound_seen[trace_index]) {
                g_spawn_bound_seen[trace_index] = true;
                std::fprintf(stderr,
                    "[mmz:extended-view] widened spawn immediate "
                    "%08X: %u -> %u\n", pc, original, *out_value);
            }
            return 1;
        }
    }
    return g_previous_thumb_alu_imm_override
        ? g_previous_thumb_alu_imm_override(pc, original, out_value) : 0;
}

constexpr std::array<std::uint32_t, 12> kObjClipLiterals = {
    0x08002264u, 0x08002360u, 0x08002460u, 0x0800257Cu,
    0x080026FCu, 0x08002844u, 0x08002968u, 0x08002AB4u,
    0x08002BE0u, 0x08002E28u, 0x080030F4u, 0x08003350u,
};

int rom_read32_override(std::uint32_t address, std::uint32_t original,
                        std::uint32_t* out_value) {
    if (out_value && original == 0x0000016Fu) {
        for (std::size_t i = 0; i < kObjClipLiterals.size(); ++i) {
            if (address == kObjClipLiterals[i]) {
                // Guest compares (screen_x + 128) <= threshold.
                *out_value = 0x0000016Fu + g_extra_right;
                if (trace_enabled() && !g_obj_clip_literal_seen[i]) {
                    g_obj_clip_literal_seen[i] = true;
                    std::fprintf(stderr,
                        "[mmz:extended-view] widened OBJ clip literal "
                        "%08X: %u -> %u\n", address, original, *out_value);
                }
                return 1;
            }
        }
    }
    return g_previous_rom_override
        ? g_previous_rom_override(address, original, out_value) : 0;
}

int extended_obj_x(int raw_x, int* out_x) {
    // MMZ1's widened guest writers accept x in [240, view_width), while all
    // twelve paths reject x < -128. In that otherwise-unused raw interval the
    // 9-bit value unambiguously represents an extended positive coordinate.
    if (out_x && raw_x >= 0x100 &&
        raw_x < static_cast<int>(240u + g_extra_right)) {
        *out_x = raw_x;
        return 1;
    }
    return g_previous_obj_x_provider
        ? g_previous_obj_x_provider(raw_x, out_x) : 0;
}

int anchored_hud_bg_x(int bg, int output_x, int screen_y, int* out_hw_x) {
    auto fallback = [&]() {
        return g_previous_bg_x_provider
            ? g_previous_bg_x_provider(bg, output_x, screen_y, out_hw_x) : 0;
    };
    if (bg != 0 || !out_hw_x || gba::g_ws_pillarbox ||
        (g_extra_left == 0 && g_extra_right == 0)) {
        return fallback();
    }

    const int left = static_cast<int>(g_extra_left);
    const int view_width = static_cast<int>(240u + g_extra_left + g_extra_right);

    // 0x080BC48C authors Zero's complete BG0 cluster in columns 0..2: HP,
    // emblem, weapon icons, and rank. Present those authentic samples at the
    // extended content edge and suppress the old centered copy. If that side
    // is intentionally pillarboxed, retain the native position so the PPU's
    // final black margin cannot cover the HUD.
    if (g_player_hud_active && g_extra_left != 0 &&
        !gba::g_ws_pillarbox_left) {
        constexpr int kPlayerWidth = 24;
        if (output_x >= 0 && output_x < kPlayerWidth) {
            *out_hw_x = output_x;
            return 1;
        }
        if (output_x >= left && output_x < left + kPlayerWidth) return -1;
    }

    // 0x080BC680 authors the boss gauge in columns 28..29 (X=224..239).
    if (g_boss_hud_active && g_extra_right != 0 &&
        !gba::g_ws_pillarbox_right) {
        constexpr int kBossSourceX = 224;
        constexpr int kBossWidth = 16;
        const int destination = view_width - kBossWidth;
        if (output_x >= destination && output_x < view_width) {
            *out_hw_x = kBossSourceX + output_x - destination;
            return 1;
        }
        const int original = left + kBossSourceX;
        if (output_x >= original && output_x < original + kBossWidth)
            return -1;
    }
    return fallback();
}

bool trace_enabled() {
    static const bool enabled = std::getenv("GBARECOMP_MMZ_WS_TRACE") != nullptr;
    return enabled;
}

std::uint8_t* map_ptr(gba::GbaBus& bus, int layer_index) {
    if (layer_index == 0) {
        return bus.ewram_ptr() + (kBg1Map - 0x02000000u);
    }
    const std::uint32_t base = layer_index == 1 ? kBg2Vram : kBg3Vram;
    return bus.vram_ptr() + (base - 0x06000000u);
}

std::uint32_t read32(const gba::GbaBus& bus, std::uint32_t addr) {
    const std::uint8_t* p = nullptr;
    if (addr >= 0x02000000u && addr + 3u < 0x02040000u) {
        p = bus.ewram_ptr() + (addr - 0x02000000u);
    } else if (addr >= 0x03000000u && addr + 3u < 0x03008000u) {
        p = bus.iwram_ptr() + (addr - 0x03000000u);
    }
    if (!p) return 0;
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint16_t read16(const gba::GbaBus& bus, std::uint32_t addr) {
    const std::uint8_t* p = nullptr;
    if (addr >= 0x02000000u && addr + 1u < 0x02040000u) {
        p = bus.ewram_ptr() + (addr - 0x02000000u);
    } else if (addr >= 0x03000000u && addr + 1u < 0x03008000u) {
        p = bus.iwram_ptr() + (addr - 0x03000000u);
    }
    if (!p) return 0;
    return static_cast<std::uint16_t>(p[0] |
        (static_cast<std::uint16_t>(p[1]) << 8));
}

bool peek8(const gba::GbaBus& bus, std::uint32_t addr, std::uint8_t& value) {
    if (addr >= 0x02000000u && addr < 0x02040000u) {
        value = bus.ewram_ptr()[addr - 0x02000000u];
        return true;
    }
    if (addr >= 0x08000000u) {
        const std::uint32_t off = (addr - 0x08000000u) & 0x01FFFFFFu;
        if (off < bus.rom_size()) {
            value = bus.rom_ptr()[off];
            return true;
        }
    }
    return false;
}

bool peek16(const gba::GbaBus& bus, std::uint32_t addr,
            std::uint16_t& value) {
    std::uint8_t lo = 0;
    std::uint8_t hi = 0;
    if (!peek8(bus, addr, lo) || !peek8(bus, addr + 1u, hi)) return false;
    value = static_cast<std::uint16_t>(lo |
        (static_cast<std::uint16_t>(hi) << 8));
    return true;
}

bool peek32(const gba::GbaBus& bus, std::uint32_t addr,
            std::uint32_t& value) {
    std::uint8_t bytes[4]{};
    for (unsigned i = 0; i < 4; ++i) {
        if (!peek8(bus, addr + i, bytes[i])) return false;
    }
    value = static_cast<std::uint32_t>(bytes[0]) |
        (static_cast<std::uint32_t>(bytes[1]) << 8) |
        (static_cast<std::uint32_t>(bytes[2]) << 16) |
        (static_cast<std::uint32_t>(bytes[3]) << 24);
    return true;
}

struct AuthoredCell {
    std::uint32_t descriptor = 0;
};

bool resolve_authored_cell(const gba::GbaBus& bus, std::uint32_t fixed_x,
                           std::uint32_t fixed_y, AuthoredCell& out) {
    // Side-effect-free mirror of the descriptor-selection prefix at
    // 0x08006D48..0x08006D7C and its authored-grid indexing. The guest's full
    // resolver continues into tileset/palette side effects, so presentation
    // safety checks must not dispatch it speculatively.
    if (fixed_x > 0x00770FFFu || fixed_y > 0x004F5FFFu) return false;
    std::uint8_t world_cell_x = 0;
    std::uint8_t world_cell_y = 0;
    if (!peek8(bus, kScreenX + (fixed_x >> 12), world_cell_x) ||
        !peek8(bus, kScreenY + (fixed_y >> 12), world_cell_y)) return false;
    std::uint8_t room = 0;
    if (!peek8(bus, kRoomGrid +
        (static_cast<std::uint32_t>(world_cell_y) << 7) + world_cell_x,
        room)) return false;
    const std::uint32_t family = room & 0x0Fu;
    std::uint32_t family_table = 0;
    std::uint32_t selectors = 0;
    if (!peek32(bus, kDescriptorFamilies + family * 4u, family_table) ||
        !peek32(bus, kVariantSelectors, selectors)) return false;
    const std::uint32_t selector = (selectors >> (family * 2u)) & 3u;
    std::uint32_t descriptor = 0;
    if (!peek32(bus, family_table + selector * 4u, descriptor)) return false;

    std::uint32_t origin_x = 0;
    std::uint32_t origin_y = 0;
    std::uint32_t header = 0;
    if (!peek32(bus, descriptor + 0x6Cu, origin_x) ||
        !peek32(bus, descriptor + 0x70u, origin_y) ||
        !peek32(bus, descriptor + 0x74u, header)) return false;
    const std::int64_t local_x = static_cast<std::int64_t>(fixed_x >> 8) -
        static_cast<std::int64_t>(origin_x) * 240;
    const std::int64_t local_y = static_cast<std::int64_t>(fixed_y >> 8) -
        static_cast<std::int64_t>(origin_y) * 160;
    if (local_x < 0 || local_y < 0) return false;
    std::uint8_t cell_x = 0;
    std::uint8_t cell_y = 0;
    if (!peek8(bus, kScreenX + static_cast<std::uint32_t>(local_x >> 4),
               cell_x) ||
        !peek8(bus, kScreenY + static_cast<std::uint32_t>(local_y >> 4),
               cell_y)) return false;
    std::uint8_t cells_w = 0;
    std::uint8_t cells_h = 0;
    if (!peek8(bus, header + 2u, cells_w) ||
        !peek8(bus, header + 3u, cells_h) ||
        cell_x >= cells_w || cell_y >= cells_h) return false;
    out = {descriptor};
    return true;
}

constexpr std::uint32_t kAuthoredCellFixed = 16u << 8;

constexpr std::uint32_t authored_cells_crossed(std::uint32_t first,
                                               std::uint32_t last) {
    return first <= last
        ? last / kAuthoredCellFixed - first / kAuthoredCellFixed + 1u
        : 0u;
}

static_assert(authored_cells_crossed(0u, kAuthoredCellFixed - 1u) == 1u);
static_assert(authored_cells_crossed(kAuthoredCellFixed - 1u,
                                     kAuthoredCellFixed) == 2u);
static_assert(authored_cells_crossed(3u * kAuthoredCellFixed + 7u,
                                     5u * kAuthoredCellFixed + 9u) == 3u);

// Prove that every authored cell intersected by the widened strip belongs to
// the same room descriptor as the authentic edge at the same Y.
// Sampling the first covered coordinate in each globally aligned 16-pixel
// cell is sufficient because descriptor selection and authored-grid bounds
// are constant within that cell. Collision/behavior words deliberately are
// not compared: ramps, hazards, and platforms legitimately change those
// words inside one room, and using them as a presentation boundary made a
// whole side flicker to black when the viewport crossed a 16-pixel row.
// Walking the full X/Y grid still catches internal A -> B -> A descriptor
// transitions that an endpoint or top/middle/bottom test would miss.
bool authored_strip_matches_edge(const gba::GbaBus& bus,
                                 std::uint32_t authentic_x,
                                 std::uint32_t outer_x,
                                 std::uint32_t top_y,
                                 std::uint32_t bottom_y) {
    if (top_y > bottom_y) return false;
    const std::uint32_t first_x =
        authentic_x < outer_x ? authentic_x : outer_x;
    const std::uint32_t last_x =
        authentic_x < outer_x ? outer_x : authentic_x;
    const std::uint32_t first_x_cell = first_x / kAuthoredCellFixed;
    const std::uint32_t last_x_cell = last_x / kAuthoredCellFixed;
    const std::uint32_t first_y_cell = top_y / kAuthoredCellFixed;
    const std::uint32_t last_y_cell = bottom_y / kAuthoredCellFixed;

    for (std::uint32_t y_cell = first_y_cell;; ++y_cell) {
        const std::uint32_t y = y_cell == first_y_cell
            ? top_y : y_cell * kAuthoredCellFixed;
        AuthoredCell authentic{};
        if (!resolve_authored_cell(bus, authentic_x, y, authentic))
            return false;

        for (std::uint32_t x_cell = first_x_cell;; ++x_cell) {
            const std::uint32_t x = x_cell == first_x_cell
                ? first_x : x_cell * kAuthoredCellFixed;
            AuthoredCell candidate{};
            if (!resolve_authored_cell(bus, x, y, candidate) ||
                authentic.descriptor != candidate.descriptor) {
                return false;
            }
            if (x_cell == last_x_cell) break;
        }
        if (y_cell == last_y_cell) break;
    }
    return true;
}

void invalidate_layer(int layer_index) {
    g_margin_valid[layer_index][0] = false;
    g_margin_valid[layer_index][1] = false;
    g_seen_layers &= ~(1u << static_cast<unsigned>(layer_index));
}

void invalidate_all_layers() {
    for (int layer = 0; layer < 3; ++layer) invalidate_layer(layer);
}

void reset_presentation_caches() {
    invalidate_all_layers();
    g_seen_layers = 0;
    g_layers_updated_since_publish = 0;
    for (auto& call : g_observed) call.valid = false;
    g_layout_signature_valid.fill(false);
    g_last_layer_position_valid.fill(false);
    g_player_hud_pending = false;
    g_boss_hud_pending = false;
    g_player_hud_active = false;
    g_boss_hud_active = false;
}

void write32_iwram(gba::GbaBus& bus, std::uint32_t addr, std::uint32_t value) {
    if (addr < 0x03000000u || addr + 3u >= 0x03008000u) return;
    std::uint8_t* p = bus.iwram_ptr() + (addr - 0x03000000u);
    p[0] = static_cast<std::uint8_t>(value);
    p[1] = static_cast<std::uint8_t>(value >> 8);
    p[2] = static_cast<std::uint8_t>(value >> 16);
    p[3] = static_cast<std::uint8_t>(value >> 24);
}

class GuestWriteTransaction final : public gba::BusWriteObserver {
public:
    explicit GuestWriteTransaction(gba::GbaBus& bus) : bus_(bus) {}

    bool begin() {
        previous_ = bus_.exchange_write_observer(this);
        if (previous_) {
            bus_.set_write_observer(previous_);
            previous_ = nullptr;
            return false;
        }
        active_ = true;
        return true;
    }

    bool on_bus_write(gba::BusWriteRegion region, std::uint32_t off,
                      std::uint32_t, std::uint8_t width,
                      std::uint32_t old_value, std::uint32_t) override {
        if (region == gba::BusWriteRegion::Device) {
            device_accessed_ = true;
            return true;
        }
        writes_.push_back({region, off, width, old_value});
        return false;
    }

    void on_bus_read(std::uint32_t) override { device_accessed_ = true; }

    bool device_accessed() const { return device_accessed_; }

    void rollback_and_end() {
        if (!active_) return;
        bus_.set_write_observer(nullptr);
        for (auto it = writes_.rbegin(); it != writes_.rend(); ++it) {
            std::uint8_t* base = nullptr;
            std::size_t size = 0;
            switch (it->region) {
                case gba::BusWriteRegion::Ewram:
                    base = bus_.ewram_ptr(); size = 0x40000u; break;
                case gba::BusWriteRegion::Iwram:
                    base = bus_.iwram_ptr(); size = 0x8000u; break;
                case gba::BusWriteRegion::Pal:
                    base = bus_.pal_ptr(); size = 0x400u; break;
                case gba::BusWriteRegion::Vram:
                    base = bus_.vram_ptr(); size = 0x18000u; break;
                case gba::BusWriteRegion::Oam:
                    base = bus_.oam_ptr(); size = 0x400u; break;
                case gba::BusWriteRegion::Device:
                    break;
            }
            if (!base || it->off + it->width > size) continue;
            for (std::uint8_t byte = 0; byte < it->width; ++byte) {
                base[it->off + byte] = static_cast<std::uint8_t>(
                    it->old_value >> (byte * 8u));
            }
        }
        bus_.set_write_observer(previous_);
        active_ = false;
    }

    ~GuestWriteTransaction() override { rollback_and_end(); }

private:
    struct Write {
        gba::BusWriteRegion region;
        std::uint32_t off;
        std::uint8_t width;
        std::uint32_t old_value;
    };
    gba::GbaBus& bus_;
    gba::BusWriteObserver* previous_ = nullptr;
    std::vector<Write> writes_;
    bool active_ = false;
    bool device_accessed_ = false;
};

bool run_guest_streamer(gba::GbaBus& bus,
                        const StreamCall& call,
                        int horizontal_offset,
                        int layer_index,
                        int side_index) {
    const std::uint32_t original_sp = g_cpu.R[13];
    if (original_sp < 0x03000200u || original_sp > 0x03008000u) return false;

    constexpr std::uint32_t kReserve = 0x200u;
    const std::uint32_t scratch = original_sp - kReserve;
    if (scratch < 0x03000000u) return false;
    std::uint8_t* scratch_ptr = bus.iwram_ptr() + (scratch - 0x03000000u);
    std::array<std::uint8_t, kReserve> saved_stack{};
    std::memcpy(saved_stack.data(), scratch_ptr, saved_stack.size());
    constexpr std::array<std::uint8_t, 16> kStackCanary = {
        0x4Du, 0x4Du, 0x5Au, 0x2Du, 0x57u, 0x53u, 0x2Du, 0x43u,
        0x41u, 0x4Eu, 0x41u, 0x52u, 0x59u, 0x21u, 0xA5u, 0x5Au};
    std::memcpy(scratch_ptr, kStackCanary.data(), kStackCanary.size());
    std::uint8_t* live_map = map_ptr(bus, layer_index);
    std::array<std::uint8_t, kMapBytes> saved_map{};
    std::memcpy(saved_map.data(), live_map, saved_map.size());
    const bool cache_was_valid = g_margin_valid[layer_index][side_index];
    if (cache_was_valid) {
        std::memcpy(live_map, g_margin_maps[layer_index][side_index].data(),
                    kMapBytes);
    }

    const ArmCpuState saved_cpu = g_cpu;
    const std::uint32_t saved_call_depth = runtime_call_stack_depth();
    if (saved_call_depth > 1024u) {
        std::memcpy(live_map, saved_map.data(), saved_map.size());
        std::memcpy(scratch_ptr, saved_stack.data(), saved_stack.size());
        return false;
    }
    std::array<std::uint32_t, 1024> saved_call_stack{};
    if (saved_call_depth != 0) {
        std::memcpy(saved_call_stack.data(), runtime_call_stack_data(),
                    saved_call_depth * sizeof(std::uint32_t));
    }
    const unsigned saved_shadow = g_runtime_shadow_tick;
    const unsigned long long saved_shadow_cycles = g_runtime_shadow_cycles;
    const unsigned long long saved_idle_epoch = g_idle_disturb_epoch;
    const unsigned saved_insn_trace = g_runtime_insn_trace;
    const std::uint32_t previous_ptr = scratch + 0x180u;
    const std::uint32_t current_ptr = scratch + 0x1C0u;
    auto previous = call.previous;
    auto current = call.current;
    previous[0] += static_cast<std::uint32_t>(horizontal_offset);
    current[0] += static_cast<std::uint32_t>(horizontal_offset);
    for (std::size_t i = 0; i < previous.size(); ++i) {
        write32_iwram(bus, previous_ptr + static_cast<std::uint32_t>(i * 4u),
                      previous[i]);
    }
    for (std::size_t i = 0; i < current.size(); ++i) {
        write32_iwram(bus, current_ptr + static_cast<std::uint32_t>(i * 4u),
                      current[i]);
    }

    GuestWriteTransaction transaction(bus);
    if (!transaction.begin()) {
        std::memcpy(live_map, saved_map.data(), saved_map.size());
        std::memcpy(scratch_ptr, saved_stack.data(), saved_stack.size());
        return false;
    }

    g_runtime_shadow_tick = 1u;
    g_runtime_insn_trace = 0u;
    const bool full_seed = !cache_was_valid;
    const std::uint32_t target_pc = full_seed
        ? (layer_index == 0 ? kBg1FullReload : kTextBgFullReload)
        : call.target_pc;
    g_cpu.R[0] = previous_ptr;
    g_cpu.R[1] = current_ptr;
    g_cpu.R[2] = call.r2;
    g_cpu.R[3] = call.r3;
    g_cpu.R[13] = scratch + 0x140u;
    g_cpu.R[14] = kSyntheticReturn;
    g_cpu.cpsr |= CPSR_T_BIT;
    runtime_call_push_return(kSyntheticReturn);
    runtime_dispatch(target_pc | 1u);
    runtime_call_cancel_return(kSyntheticReturn);
    runtime_call_stack_restore(saved_call_stack.data(), saved_call_depth);
    const bool safe = !transaction.device_accessed() &&
        std::memcmp(scratch_ptr, kStackCanary.data(), kStackCanary.size()) == 0;
    if (!safe && trace_enabled()) {
        std::fprintf(stderr,
            "[mmz:extended-view] rejected synthetic layer=%d side=%d "
            "device=%d stack_canary=%d target=%08X\n",
            layer_index + 1, side_index, transaction.device_accessed(),
            std::memcmp(scratch_ptr, kStackCanary.data(),
                        kStackCanary.size()) == 0,
            target_pc);
    }
    if (safe) {
        std::memcpy(g_margin_maps[static_cast<std::size_t>(layer_index)]
                                 [static_cast<std::size_t>(side_index)].data(),
                    live_map, kMapBytes);
        g_margin_valid[layer_index][side_index] = true;
    }
    transaction.rollback_and_end();
    std::memcpy(live_map, saved_map.data(), saved_map.size());

    g_runtime_shadow_tick = saved_shadow;
    g_runtime_shadow_cycles = saved_shadow_cycles;
    g_idle_disturb_epoch = saved_idle_epoch;
    g_runtime_insn_trace = saved_insn_trace;
    g_cpu = saved_cpu;
    std::memcpy(scratch_ptr, saved_stack.data(), saved_stack.size());
    return safe;
}

void extended_view_hook(std::uint32_t pc) {
    if (g_in_synthetic_stream) {
        return;
    }
    if (g_previous_hook) g_previous_hook(pc);

    // Fixed-width sessions never change these values after installation.
    // Resize-driven sessions update the runtime globals whenever the drawable
    // aspect changes. Adopt that geometry before running any more guest-side
    // presentation work, and discard maps synthesized for the prior offsets.
    if (g_extra_left != g_ws_extra_left ||
        g_extra_right != g_ws_extra_right) {
        g_extra_left = g_ws_extra_left;
        g_extra_right = g_ws_extra_right;
        reset_presentation_caches();
        g_last_pillarbox = g_last_pillarbox_left =
            g_last_pillarbox_right = -1;
        if (trace_enabled()) {
            std::fprintf(stderr,
                "[mmz:extended-view] live geometry changed: margins=%u/%u "
                "logical=%ux160\n",
                g_extra_left, g_extra_right,
                240u + g_extra_left + g_extra_right);
        }
    }

    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus) return;
    if (g_seen_state_epoch != g_runtime_state_epoch) {
        g_seen_state_epoch = g_runtime_state_epoch;
        reset_presentation_caches();
    }
    if (pc == kPlayerHudDraw && g_cpu.R[0] == kHudState) {
        g_player_hud_pending = read32(*bus, kHudState + 4u) != 0;
    } else if (pc == kBossHudDraw && g_cpu.R[0] == kHudState) {
        g_boss_hud_pending = read32(*bus, kHudState + 8u) != 0;
    }
    if (pc == kPpuPublish) {
        // These exact guest routines are reached only when DrawStatus authors
        // the corresponding BG0 tiles. Latch that LLE fact for the published
        // frame; stale menu/text maps never opt into HUD anchoring.
        g_player_hud_active = g_player_hud_pending;
        g_boss_hud_active = g_boss_hud_pending;
        g_player_hud_pending = false;
        g_boss_hud_pending = false;
        const std::uint8_t* io = bus->io().raw();
        auto io16 = [io](std::uint32_t off) {
            return static_cast<std::uint16_t>(
                io[off] | (static_cast<std::uint16_t>(io[off + 1]) << 8));
        };
        const std::uint16_t dispcnt = io16(0x00u);
        const bool mode0 = (dispcnt & 7u) == 0;
        bool layout_supported = mode0;
        constexpr std::uint32_t expected_bases[3] = {2u, 4u, 6u};
        for (int layer = 0; layer < 3; ++layer) {
            const std::uint16_t bgcnt = read16(*bus, kBgcntShadow + layer * 2u);
            layout_supported = layout_supported &&
                ((bgcnt >> 14) & 3u) == 1u &&
                ((bgcnt >> 8) & 31u) == expected_bases[layer];
        }
        // The guest only streams a strip when a layer moves. Captured margin
        // maps remain valid until the existing transition/layout/teleport
        // invalidation paths clear them, so a still camera must stay wide.
        const bool layout_ok = layout_supported && g_seen_layers == 0x7u;
        gba::g_ws_pillarbox = layout_ok ? 0 : 1;
        gba::g_ws_pillarbox_left = 0;
        gba::g_ws_pillarbox_right = 0;
        if (layout_ok) {
            // MMZ's ChunkMap header is stride, default chunk, authored width,
            // authored height. The streamer uses stride for row addressing;
            // only authored width (+2) is a presentation boundary.
            std::uint8_t screens_w = 0;
            if (!peek8(*bus, g_bg1_data + 2u, screens_w) || screens_w == 0) {
                gba::g_ws_pillarbox = 1;
            } else {
                const std::int64_t max_x =
                    static_cast<std::int64_t>(screens_w) * 240;
                const std::int64_t camera_x = g_bg1_camera_x;
                gba::g_ws_pillarbox_left =
                    camera_x - static_cast<std::int64_t>(g_extra_left) < 0;
                gba::g_ws_pillarbox_right =
                    camera_x + 239 + static_cast<std::int64_t>(g_extra_right) >=
                    max_x;

                std::uint32_t viewport_x = 0;
                std::uint32_t viewport_y = 0;
                if (!peek32(*bus, kCamera + 0x38u, viewport_x) ||
                    !peek32(*bus, kCamera + 0x3Cu, viewport_y) ||
                    viewport_x < (120u << 8) ||
                    viewport_y < (80u << 8)) {
                    gba::g_ws_pillarbox = 1;
                } else {
                    // Camera viewport coordinates are world 24.8 fixed point;
                    // layer streamer coordinates above are descriptor-local.
                    const std::uint32_t world_left = viewport_x - (120u << 8);
                    const std::uint32_t world_top =
                        viewport_y - (80u << 8);
                    const std::uint32_t world_bottom =
                        viewport_y + (79u << 8);
                    if (g_extra_left != 0) {
                        const std::uint32_t delta = g_extra_left << 8;
                        const bool safe_left = world_left >= delta &&
                            authored_strip_matches_edge(
                                *bus, world_left, world_left - delta,
                                world_top, world_bottom);
                        gba::g_ws_pillarbox_left |= !safe_left;
                    }
                    if (g_extra_right != 0) {
                        const std::uint32_t authentic_right =
                            world_left + (239u << 8);
                        const bool safe_right = authored_strip_matches_edge(
                            *bus, authentic_right,
                            authentic_right + (g_extra_right << 8),
                            world_top, world_bottom);
                        gba::g_ws_pillarbox_right |= !safe_right;
                    }

                    const std::uint8_t camera_mode =
                        bus->ewram_ptr()[(kCamera - 0x02000000u) + 0x18u];
                    if (camera_mode == 4u || camera_mode == 5u) {
                        std::uint32_t left_bound = 0;
                        std::uint32_t right_bound = 0;
                        if (!peek32(*bus, kCamera + 0x5Cu, left_bound) ||
                            !peek32(*bus, kCamera + 0x60u, right_bound)) {
                            gba::g_ws_pillarbox = 1;
                        } else {
                            const std::int64_t outer_left =
                                static_cast<std::int64_t>(viewport_x) -
                                (static_cast<std::int64_t>(120u + g_extra_left)
                                 << 8);
                            const std::int64_t outer_right =
                                static_cast<std::int64_t>(viewport_x) +
                                (static_cast<std::int64_t>(119u + g_extra_right)
                                 << 8);
                            gba::g_ws_pillarbox_left |=
                                outer_left < static_cast<std::int64_t>(left_bound);
                            gba::g_ws_pillarbox_right |=
                                outer_right >= static_cast<std::int64_t>(right_bound);
                        }
                    }
                }

            }
        } else if (!layout_supported) {
            invalidate_all_layers();
        }
        if (trace_enabled() &&
            (g_last_pillarbox != gba::g_ws_pillarbox ||
             g_last_pillarbox_left != gba::g_ws_pillarbox_left ||
             g_last_pillarbox_right != gba::g_ws_pillarbox_right)) {
            std::fprintf(stderr,
                "[mmz:extended-view] margins global/left/right=%d/%d/%d "
                "layer_x=%d map=%08X viewport=%08X,%08X "
                "dispcnt=%04X win0=%04X/%04X winin/out=%04X/%04X\n",
                gba::g_ws_pillarbox, gba::g_ws_pillarbox_left,
                gba::g_ws_pillarbox_right, g_bg1_camera_x, g_bg1_data,
                read32(*bus, kCamera + 0x38u),
                read32(*bus, kCamera + 0x3Cu), dispcnt,
                io16(0x40u), io16(0x44u), io16(0x48u), io16(0x4Au));
            g_last_pillarbox = gba::g_ws_pillarbox;
            g_last_pillarbox_left = gba::g_ws_pillarbox_left;
            g_last_pillarbox_right = gba::g_ws_pillarbox_right;
        }
        g_layers_updated_since_publish = 0;
        return;
    }
    if (pc == kBg1Streamer || pc == kBg1FullReload ||
        pc == kTextBgStreamer || pc == kTextBgFullReload) {
        const std::uint32_t vram_base = g_cpu.R[2];
        int layer_index = -1;
        if ((pc == kBg1Streamer || pc == kBg1FullReload) &&
            g_cpu.R[0] == kBg1State &&
            vram_base == kBg1Map) {
            layer_index = 0;
        } else if (pc == kTextBgStreamer || pc == kTextBgFullReload) {
            layer_index = vram_base == kBg2Vram ? 1 :
                          vram_base == kBg3Vram ? 2 : -1;
        }
        if (layer_index > 0 &&
            g_cpu.R[0] != (layer_index == 1 ? kBg2State : kBg3State)) {
            return;
        }
        if (layer_index < 0) return;
        if (pc == kBg1FullReload || pc == kTextBgFullReload)
            invalidate_layer(layer_index);
        StreamCall& call = g_observed[static_cast<std::size_t>(layer_index)];
        call.valid = true;
        call.state = g_cpu.R[0];
        call.target_pc = pc;
        call.vram_base = vram_base;
        call.r2 = g_cpu.R[2];
        call.r3 = g_cpu.R[3];
        for (std::size_t i = 0; i < call.previous.size(); ++i) {
            call.previous[i] = read32(*bus, g_cpu.R[0] +
                static_cast<std::uint32_t>(i * 4u));
        }
        for (std::size_t i = 0; i < call.current.size(); ++i) {
            call.current[i] = read32(*bus, g_cpu.R[1] +
                static_cast<std::uint32_t>(i * 4u));
        }
        return;
    }
    if (pc != kScrollPublish) return;

    const std::uint32_t state = g_cpu.R[0];
    int layer_index = -1;
    std::uint32_t vram_base = 0;
    if (state == kBg1State) {
        layer_index = 0;
    } else if (state == kBg2State) {
        layer_index = 1;
        vram_base = kBg2Vram;
    } else if (state == kBg3State) {
        layer_index = 2;
        vram_base = kBg3Vram;
    } else return;

    StreamCall& call = g_observed[static_cast<std::size_t>(layer_index)];
    if (!call.valid || call.state != state ||
        (layer_index != 0 && call.vram_base != vram_base)) {
        return;
    }

    const std::array<std::uint32_t, 3> signature = {
        call.previous[3], call.previous[4], call.previous[5]};
    if (!g_layout_signature_valid[layer_index] ||
        g_layout_signatures[layer_index] != signature) {
        invalidate_layer(layer_index);
        g_layout_signatures[layer_index] = signature;
        g_layout_signature_valid[layer_index] = true;
        if (trace_enabled()) {
            std::fprintf(stderr,
                "[mmz:extended-view] layer=%d tiles=%08X chunks=%08X "
                "map=%08X camera=%d,%d\n",
                layer_index + 1, signature[0], signature[1], signature[2],
                static_cast<std::int32_t>(call.current[0]),
                static_cast<std::int32_t>(call.current[1]));
        }
    }

    const std::array<std::int32_t, 2> position = {
        static_cast<std::int32_t>(call.current[0]),
        static_cast<std::int32_t>(call.current[1])};
    if (g_last_layer_position_valid[layer_index]) {
        const auto& last = g_last_layer_position[layer_index];
        const std::int64_t dx = static_cast<std::int64_t>(position[0]) - last[0];
        const std::int64_t dy = static_cast<std::int64_t>(position[1]) - last[1];
        if (dx < -30 || dx > 30 || dy < -30 || dy > 30)
            invalidate_layer(layer_index);
    }
    g_last_layer_position[layer_index] = position;
    g_last_layer_position_valid[layer_index] = true;

    g_in_synthetic_stream = true;
    const bool left_ok = run_guest_streamer(*bus, call,
        -static_cast<int>(g_extra_left), layer_index, 0);
    const bool right_ok = run_guest_streamer(*bus, call,
        static_cast<int>(g_extra_right), layer_index, 1);
    g_in_synthetic_stream = false;
    if (left_ok && right_ok) {
        g_seen_layers |= 1u << static_cast<unsigned>(layer_index);
        g_layers_updated_since_publish |=
            1u << static_cast<unsigned>(layer_index);
        if (layer_index == 0) {
            g_bg1_data = call.previous[5];
            g_bg1_camera_x = static_cast<std::int32_t>(call.current[0]);
        }
    } else {
        invalidate_layer(layer_index);
    }
    call.valid = false;
}

int margin_tilemap_provider(int bg, int hw_x, int screen_y,
                            std::uint16_t* out_entry) {
    if (!out_entry || bg < 1 || bg > 3) return 0;
    const int layer_index = bg - 1;
    const int side_index = hw_x < 0 ? 0 : 1;
    if (!g_margin_valid[layer_index][side_index]) return 0;
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus) return 0;
    const std::uint8_t* io = bus->io().raw();
    auto io16 = [io](std::uint32_t off) {
        return static_cast<std::uint16_t>(
            io[off] | (static_cast<std::uint16_t>(io[off + 1]) << 8));
    };
    const std::uint16_t bgcnt = io16(0x08u + static_cast<std::uint32_t>(bg * 2));
    const std::uint32_t size_code = (bgcnt >> 14) & 3u;
    // The captured MMZ maps at the validated gameplay checkpoint are the
    // horizontal 512x256 shape. Pillarbox rather than guess at other layouts.
    const std::uint32_t expected_base = static_cast<std::uint32_t>(bg * 2);
    if (size_code != 1u || ((bgcnt >> 8) & 31u) != expected_base) return 0;
    const std::uint32_t scroll_off = 0x10u + static_cast<std::uint32_t>(bg * 4);
    const std::uint32_t hofs = io16(scroll_off) & 0x01FFu;
    const std::uint32_t vofs = io16(scroll_off + 2u) & 0x01FFu;
    const std::uint32_t tex_x =
        static_cast<std::uint32_t>(hw_x + static_cast<int>(hofs)) & 511u;
    const std::uint32_t tex_y =
        (static_cast<std::uint32_t>(screen_y) + vofs) & 255u;
    const std::uint32_t tile_x = tex_x >> 3;
    const std::uint32_t tile_y = tex_y >> 3;
    const std::uint32_t block = tile_x >> 5;
    const std::uint32_t map_off = block * 0x800u +
        ((tile_y & 31u) * 32u + (tile_x & 31u)) * 2u;
    const auto& map = g_margin_maps[static_cast<std::size_t>(layer_index)]
                                   [static_cast<std::size_t>(side_index)];
    *out_entry = static_cast<std::uint16_t>(
        map[map_off] | (static_cast<std::uint16_t>(map[map_off + 1]) << 8));
    return 1;
}

}  // namespace

void install_extended_view(unsigned extra_left, unsigned extra_right) {
    g_extra_left = extra_left;
    g_extra_right = extra_right;
    g_seen_state_epoch = g_runtime_state_epoch;
    reset_presentation_caches();
    g_last_pillarbox = g_last_pillarbox_left = g_last_pillarbox_right = -1;
    g_spawn_bound_seen.fill(false);
    gba::g_ws_pillarbox = 1;
    gba::g_ws_pillarbox_left = 0;
    gba::g_ws_pillarbox_right = 0;
    gba::g_ws_tilemap_provider = margin_tilemap_provider;
    if (gba::g_rom_read32_override != rom_read32_override) {
        g_previous_rom_override = gba::g_rom_read32_override;
        gba::g_rom_read32_override = rom_read32_override;
    }
    if (gba::g_ws_obj_x_provider != extended_obj_x) {
        g_previous_obj_x_provider = gba::g_ws_obj_x_provider;
        gba::g_ws_obj_x_provider = extended_obj_x;
    }
    if (gba::g_ws_bg_x_provider != anchored_hud_bg_x) {
        g_previous_bg_x_provider = gba::g_ws_bg_x_provider;
        gba::g_ws_bg_x_provider = anchored_hud_bg_x;
    }
    if ((extra_left != 0u || extra_right != 0u) &&
        g_runtime_thumb_alu_imm_override != thumb_alu_immediate_override) {
        g_previous_thumb_alu_imm_override =
            g_runtime_thumb_alu_imm_override;
        g_runtime_thumb_alu_imm_override = thumb_alu_immediate_override;
    }
    if (g_runtime_fn_entry_hook == extended_view_hook) return;
    g_previous_hook = g_runtime_fn_entry_hook;
    g_runtime_fn_entry_hook = extended_view_hook;
}

}  // namespace mmz
