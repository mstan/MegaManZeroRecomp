#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "runtime.h"
#include "mmz_extended_view.h"

#if defined(GBAGAME_RECOMP_UI)
#include "game_launcher_boot.h"
#endif

namespace {

void print_usage() {
    std::printf(
        "MegaManZeroRecomp [--bios <path>] [--rom <path>] "
        "[--view-width <240..480>] [game.toml]\n"
        "The BIOS and ROM must match the SHA-1 identities in game.toml.\n"
        "View width defaults to the faithful 240; 288 is the recommended "
        "experimental extended view. 384 is a progressive test width; "
        "480 is an exact-2x research mode.\n");
}

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0 ||
            std::strcmp(argv[i], "-h") == 0) {
            print_usage();
            return 0;
        }
    }

    gbarecomp::RunOptions opts;
    opts.builtin_game_name = "Mega Man Zero";
    opts.builtin_rom_sha1 = "193b14120119162518a73c70876f0b8bffdbd96e";
    // CRC32 of the pinned USA ROM (same dump the SHA-1 gates on); the
    // launcher's GAME card uses it for its "ROM verified" check.
    opts.builtin_rom_crc32 = 0x9707D2A1u;
    opts.max_view_width = 480;
    opts.extended_view_init = mmz::install_extended_view;
    opts.launcher_region = "USA";
    opts.launcher_save_path = "saves/megaman_zero_usa.sav";  // game.toml [save].path
    // Extended-view aspect cycle (EXPERIMENTAL-tagged in the launcher, the
    // snesrecomp/psxrecomp widescreen convention): every width the runtime's
    // extended view supports, native first. The committed index maps to
    // --view-width.
    static const char* const kAspectLabels[] = {
        "3:2 (Native)", "9:5 (288 px)", "12:5 (384 px)", "6:2 (480 px)"
    };
    static const std::uint16_t kAspectWidths[] = { 240, 288, 384, 480 };
    opts.launcher_aspect_labels      = kAspectLabels;
    opts.launcher_aspect_view_widths = kAspectWidths;
    opts.launcher_num_aspects        = 4;

#if defined(GBAGAME_RECOMP_UI)
    std::vector<std::string> args(argv, argv + argc);
    if (game_launcher_preboot(args, opts)) return 0;   // user quit the launcher
    std::vector<char*> av;
    av.reserve(args.size());
    for (auto& s : args) av.push_back(s.data());
    return gbarecomp::run_game(static_cast<int>(av.size()), av.data(), opts);
#else
    return gbarecomp::run_game(argc, argv, opts);
#endif
}
