#pragma once

#include "Memory.hpp"

#include <cstdint>
#include <variant>
#include <vector>

// CoreEvent — typed, immutable-payload events that mutate CPU state.
//
// The contract:
//   1. Events are pure data. No lambdas, no std::function, no captured
//      closures. This is what makes replays / serialization / lockstep
//      networking possible later.
//   2. Events are drained at deterministic sync points (frame boundary,
//      before cycle batch). Never mid-cycle, never mid-opcode.
//   3. The CPU is the only entity that applies events. UI layers enqueue;
//      they do not mutate state directly.
//
// What's IN scope:
//   - Memory patches (debugger editor, scripted tests, replay)
//   - Debugger toggles (breakpoints, watchpoints)
//   - Control-flow forces (SetPC for "skip past stuck loop")
//   - Lifecycle (Reset, LoadROM, SetSeed)
//   - Programmatic input (InjectKey — for replay/scripting; live SFML
//     keypad input still goes direct, that's a different channel)
//
// What's OUT (for now, by design):
//   - Snapshot save/load — these already round-trip through Chip8::snapshot
//     and the App owns the rewind buffer. Routing them here would force a
//     return channel that complicates the unidirectional event flow.
//   - ISA install / quirk toggles — these are configuration, not state
//     mutation, and don't need replay-level reproducibility.
//   - Pause/Resume/Step — App-local UI mode, not machine state. Keeping
//     them direct preserves stepping responsiveness.

struct ResetEvent {};

struct LoadROMEvent {
    std::vector<uint8_t> bytes;
};

struct WriteMemoryEvent {
    uint16_t addr;
    uint8_t  value;
};

struct WriteMemoryBlockEvent {
    uint16_t             addr;
    std::vector<uint8_t> bytes;
};

struct ToggleBreakpointEvent {
    uint16_t addr;
};

struct ClearAllBreakpointsEvent {};

struct SetWatchpointEvent {
    uint16_t           addr;
    Memory::WatchKind  kind;        // pass any value; Read is the cycle anchor
    bool               erase = false; // true = remove, false = add/update
};

struct ClearAllWatchpointsEvent {};

struct SetPCEvent {
    uint16_t pc;
};

struct InjectKeyEvent {
    uint8_t key;     // 0..F
    bool    down;    // true = press, false = release
};

struct SetSeedEvent {
    uint64_t seed;
};

using CoreEvent = std::variant<
    ResetEvent,
    LoadROMEvent,
    WriteMemoryEvent,
    WriteMemoryBlockEvent,
    ToggleBreakpointEvent,
    ClearAllBreakpointsEvent,
    SetWatchpointEvent,
    ClearAllWatchpointsEvent,
    SetPCEvent,
    InjectKeyEvent,
    SetSeedEvent
>;
