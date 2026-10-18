// mmz_adaptive_view_plugin.cpp — trusted activation plugin for Mega Man Zero's
// adaptive extended view.
//
// The extended-view implementation itself stays game-owned in
// mmz_extended_view.cpp. This file only publishes it to the mod catalog so the
// launcher's Mods page — rather than the generic Display settings — owns
// whether the enhancement is active. RunOptions::mod_owns_adaptive_view is the
// matching capability gate on the runtime side.

#include "mod_runtime.h"

namespace {

void reset_adaptive_view() {
    (void)gba_mod_set_adaptive_view_enabled(0);
}

void activate_adaptive_view() {
    (void)gba_mod_set_adaptive_view_enabled(1);
}

}  // namespace

GBA_MOD_CONSTRUCTOR(mmz_register_adaptive_view_plugin) {
    (void)gba_mod_register_reset_callback(reset_adaptive_view);
    (void)gba_mod_register_activation_plugin(
        "mega-man-zero.adaptive-view", activate_adaptive_view);
}
