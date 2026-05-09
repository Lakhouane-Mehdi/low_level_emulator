#include "app/App.hpp"
#include "app/Config.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static void printUsage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s [options] [rom.ch8]\n"
        "Options:\n"
        "  --speed N        Set CPU cycles per frame (1..2000). Default 12 (~720Hz).\n"
        "  --legacy         Start with COSMAC VIP quirks (instead of modern SCHIP).\n"
        "  --mx8            Enable MX-8 custom opcodes (MUL/DIV/SWAP/MEMCPY/MEMSET/RNDSEED).\n"
        "  --seed N         Deterministic PRNG seed (any int64, hex with 0x prefix).\n"
        "  --isa NAME       chip8 | schip | mx8 (default: mx8 — superset, gates MX-8\n"
        "                   opcodes via the runtime quirk).\n"
        "  --paused         Start paused.\n"
        "  --roms-dir DIR   Directory to scan for the ROM picker (default: roms).\n"
        "  --rewind-sec N   Seconds of rewind history (default: 5).\n"
        "  --palette NAME   mono | amber | green | gameboy | c64 | ice | hotdog\n"
        "  -h, --help       This help.\n"
        "Hotkeys:\n"
        "  Space pause/run   N step   O step-over   Enter step-out\n"
        "  B set/clear breakpoint at PC (Shift+B clears all)\n"
        "  F1..F4,F6,F7 individual quirk toggles   F8 modern   F12 legacy\n"
        "  F5 save state   F9 load state   Backspace hold to rewind\n"
        "  - / +  decrease/increase CPU speed   Tab mute   P reset CPU\n"
        "  M cycle memory pane (I / PC / cursor)   Arrows move cursor\n"
        "  [/]  decrement/increment byte at cursor (paused mem editor)\n",
        prog);
}

int main(int argc, char* argv[]) {
    Config cfg;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto needArg = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "Missing value for %s\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "-h" || a == "--help") { printUsage(argv[0]); return 0; }
        else if (a == "--speed")        cfg.cycles_per_frame = std::atoi(needArg("--speed"));
        else if (a == "--legacy")       cfg.legacy_quirks = true;
        else if (a == "--mx8")          cfg.mx8_extensions = true;
        else if (a == "--seed") {
            cfg.rng_seed = std::strtoll(needArg("--seed"), nullptr, 0);
        }
        else if (a == "--isa") {
            cfg.isa_name = needArg("--isa");
        }
        else if (a == "--paused")       cfg.start_paused = true;
        else if (a == "--roms-dir")     cfg.roms_dir = needArg("--roms-dir");
        else if (a == "--rewind-sec")   cfg.rewind_seconds = std::atoi(needArg("--rewind-sec"));
        else if (a == "--palette") {
            std::string name = needArg("--palette");
            if (!Config::applyPalette(name, cfg.color_on, cfg.color_off)) {
                std::fprintf(stderr,
                    "Unknown palette '%s'. Try: mono, amber, green, gameboy, c64, ice, hotdog\n",
                    name.c_str());
                std::exit(2);
            }
        }
        else if (!a.empty() && a[0] != '-') cfg.rom_path = a;
        else {
            std::fprintf(stderr, "Unknown option: %s\n", a.c_str());
            printUsage(argv[0]);
            return 2;
        }
    }

    if (cfg.cycles_per_frame < cfg.cycles_min) cfg.cycles_per_frame = cfg.cycles_min;
    if (cfg.cycles_per_frame > cfg.cycles_max) cfg.cycles_per_frame = cfg.cycles_max;

    App app(std::move(cfg));
    return app.run();
}
