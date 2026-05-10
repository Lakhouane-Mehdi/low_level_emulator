#pragma once

#include "Chip8.hpp"
#include "CoreEvents.hpp"

#include <cstdint>
#include <string>
#include <vector>

// Replay file format. Hermetic: a replay file fully describes the
// initial machine state and the deterministic input stream needed to
// reproduce a run.
//
// Format = JSON v1. Schema:
//
//   {
//     "format":  "chip8-replay",
//     "version": 1,
//     "rom_bytes_hex": "00e012...",     // full ROM, no path dependency
//     "isa":     "mx8" | "schip" | "chip8",
//     "quirks":  { ... 7 booleans ... },
//     "seed":    <int64 | null>,        // null = boot entropy (non-deterministic)
//     "events": [
//       { "frame": <int>, "type": "InjectKey",        "key": <0..15>, "down": <bool> },
//       { "frame": <int>, "type": "WriteMemory",      "addr": <int>, "value": <int> },
//       { "frame": <int>, "type": "WriteMemoryBlock", "addr": <int>, "bytes_hex": "..." },
//       { "frame": <int>, "type": "SetPC",            "pc": <int> },
//       { "frame": <int>, "type": "Reset" },
//       { "frame": <int>, "type": "SetSeed",          "seed": <int64> }
//     ],
//     "checkpoints": [
//       { "frame": <int>, "hash": "0x...." }   // framebufferHash at end-of-frame
//     ]
//   }
//
// Events excluded by design: ToggleBreakpoint, SetWatchpoint,
// LoadROM (anchored separately). These are debugger / housekeeping
// concerns that don't affect deterministic replay.

struct ReplayEvent {
    uint64_t   frame = 0;     // wall-clock emulator frame ordinal (0-based)
    CoreEvent  event;
};

struct ReplayCheckpoint {
    uint64_t   frame = 0;
    uint64_t   hash  = 0;
};

struct Replay {
    static constexpr int VERSION = 1;

    int                              version = VERSION;
    std::vector<uint8_t>             rom_bytes;
    std::string                      isa = "mx8";
    Chip8::Quirks                    quirks{};
    bool                             have_seed = false;
    int64_t                          seed = 0;
    std::vector<ReplayEvent>         events;
    std::vector<ReplayCheckpoint>    checkpoints;

    // Save / load. Returns false on I/O or parse error and writes a
    // diagnostic to stderr.
    bool save(const std::string& path) const;
    bool load(const std::string& path);

    // String round-trip for tests.
    std::string toJson() const;
    bool fromJson(const std::string& json, std::string* err = nullptr);
};
