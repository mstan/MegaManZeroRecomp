#include <cstdio>
#include <cstring>

#include "runtime.h"

namespace {

void print_usage() {
    std::printf(
        "MegaManZeroRecomp [--bios <path>] [--rom <path>] [game.toml]\n"
        "The BIOS and ROM must match the SHA-1 identities in game.toml.\n");
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
    opts.builtin_game_name = "Mega Man Zero (USA)";
    opts.builtin_rom_sha1 = "193b14120119162518a73c70876f0b8bffdbd96e";
    return gbarecomp::run_game(argc, argv, opts);
}
