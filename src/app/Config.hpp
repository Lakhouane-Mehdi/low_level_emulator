#pragma once

#include "../core/Chip8.hpp"

#include <cstdint>
#include <string>

// Tunables that the user might change at runtime or via CLI.
struct Config {
    int  cycles_per_frame = 12;     // ~720Hz at 60fps. Range [1..2000].
    int  cycles_min       = 1;
    int  cycles_max       = 2000;
    int  rewind_seconds   = 5;
    bool start_paused     = false;
    bool legacy_quirks    = false;  // start in COSMAC-VIP mode
    bool mx8_extensions   = false;  // start with MX-8 custom opcodes enabled

    // PRNG seed. -1 means "boot-entropy" (default). Any other value gives a
    // deterministic stream — useful for tests, replays, regression captures.
    int64_t rng_seed      = -1;

    // Which ISA to install on the CPU. Default is MX-8 because it's a
    // superset — the 6 custom opcodes are still gated by the runtime
    // mx8_extensions quirk, so plain CHIP-8 / SCHIP ROMs still work.
    std::string isa_name  = "mx8";

    // Window layout — derived from CHIP-8 dimensions but tunable so the
    // sidebar / scale stay in one place.
    int  lores_scale  = 12;
    int  hires_scale  = 6;
    int  sidebar_w    = 480;

    int gameWidth()  const { return Chip8::LORES_WIDTH  * lores_scale; }
    int gameHeight() const { return Chip8::LORES_HEIGHT * lores_scale; }
    int windowWidth()  const { return gameWidth() + sidebar_w; }
    int windowHeight() const { return gameHeight(); }

    // Visual palette (RGBA: 0xRRGGBBAA).
    uint32_t color_on  = 0xFFFFFFFFu;
    uint32_t color_off = 0x000000FFu;

    // Built-in palette presets, picked via --palette name.
    static bool applyPalette(const std::string& name, uint32_t& on, uint32_t& off) {
        struct P { const char* name; uint32_t on, off; };
        static const P presets[] = {
            {"mono",     0xFFFFFFFFu, 0x000000FFu},
            {"amber",    0xFFB000FFu, 0x1A0E00FFu},
            {"green",    0x33FF66FFu, 0x001A0CFFu},
            {"gameboy",  0x9BBC0FFFu, 0x0F380FFFu},
            {"c64",      0x6C5EB5FFu, 0x352879FFu},
            {"ice",      0x9FE9F2FFu, 0x10283CFFu},
            {"hotdog",   0xFFCC00FFu, 0x880000FFu},
        };
        for (const auto& p : presets) if (name == p.name) { on = p.on; off = p.off; return true; }
        return false;
    }

    std::string rom_path;          // optional CLI argument
    std::string roms_dir = "roms"; // browser default
};
