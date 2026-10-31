// White-box property check of the actual game adapter: compare every output X
// with its retained scalar oracle, including every published discontinuity.
// No ROM, guest execution, window, audio device or synthetic success flags.
#include "../src/mmz_extended_view.cpp"

namespace {
int unknown_delegate(int,int,int,int* x) { *x=17;return 1; }
}
int main() {
    auto bus=std::make_unique<gba::GbaBus>();
    auto ppu=std::make_unique<gba::GbaPpu>();
    gbarecomp::set_active_bus(bus.get());gbarecomp::set_active_ppu(ppu.get());
    auto* io=const_cast<uint8_t*>(bus->io().raw());
    auto write16=[&](unsigned address,unsigned value) {io[address]=value;io[address+1]=value>>8;};
    constexpr unsigned widths[]{240,242,256,284,288,382,510,640};
    constexpr int rows[]{-6,0,7,15,31,63,64,95,120,143,159,165};
    uint64_t mappings=0,pixels=0;
    gba::g_ws_bg_x_provider=mmz::anchored_hud_bg_x;
    mmz::g_previous_bg_x_provider=nullptr;
    for (unsigned width:widths) for (unsigned split=0;split<3;++split) {
        const unsigned gap=width-240;
        mmz::g_extra_left=split==0 ? 0 : split==1 ? gap/2:gap;
        mmz::g_extra_right=gap-mmz::g_extra_left;
        for (unsigned flags=0;flags<32;++flags) for (unsigned scene=0;scene<6;++scene) {
            mmz::g_player_hud_active=flags&1;mmz::g_boss_hud_active=flags&2;
            gba::g_ws_pillarbox=bool(flags&4);gba::g_ws_pillarbox_left=bool(flags&8);
            gba::g_ws_pillarbox_right=bool(flags&16);
            write16(0,scene==1 ? 0x1300:scene>=2 ? 0x3300:0x1f00);
            write16(0x0a,scene==1 ? 0x0284:0);
            write16(0x48,scene==5 ? 0x3f:0x3e);
            write16(0x44,scene==2 ? 0x0040:scene==3 ? 0x7850:scene==4 ? 0x14c8:0x00a0);
            for (int y:rows) for (int bg=0;bg<4;++bg) {
                gba::WsBgXSpan spans[8];
                const int count=mmz::anchored_hud_bg_x_row(bg,y,int(width),int(gap/3),spans,8);
                if (count<1 || count>8) return 1;
                unsigned selected=0;
                for (unsigned x=0;x<width;++x) {
                    while (selected<unsigned(count) && int(x)>=spans[selected].end) ++selected;
                    if (selected>=unsigned(count) || int(x)<spans[selected].begin) return 2;
                    int expected=0;
                    const int action=mmz::anchored_hud_bg_x(bg,int(x),y,&expected);
                    const auto& span=spans[selected];
                    const int actual=span.source_x+span.step*(int(x)-span.begin);
                    if (action!=span.action || (action==1 && actual!=expected)) {
                        std::fprintf(stderr,"HUD mapping differs width=%u split=%u flags=%u scene=%u y=%d bg=%d x=%u\n",width,split,flags,scene,y,bg,x);
                        return 3;
                    }
                    ++pixels;
                }
                ++mappings;
            }
        }
    }
    gba::WsBgXSpan spans[8];mmz::g_previous_bg_x_provider=unknown_delegate;
    if (mmz::anchored_hud_bg_x_row(0,0,288,24,spans,8)!=-1) return 4;
    mmz::g_previous_bg_x_provider=nullptr;
    gba::g_ws_bg_x_provider=unknown_delegate;
    if (mmz::anchored_hud_bg_x_row(0,0,288,24,spans,8)!=-1) return 5;
    gbarecomp::set_active_ppu(nullptr);gbarecomp::set_active_bus(nullptr);
    std::printf("MMZ published HUD rows: %llu mappings/%llu exact scalar pixels; unknown/chained binding declines\n",
        (unsigned long long)mappings,(unsigned long long)pixels);
}
