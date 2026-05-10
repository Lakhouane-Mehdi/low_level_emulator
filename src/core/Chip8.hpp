#pragma once

#include "CoreEvents.hpp"
#include "Memory.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

// Deterministic 256-bit-state PRNG used by the CPU. xoshiro256** is fast,
// snapshot-friendly (32 bytes), and more than enough for CHIP-8 RND.
// Source: https://prng.di.unimi.it/xoshiro256starstar.c (public domain).
struct Xoshiro256ss {
    uint64_t s[4];

    static constexpr uint64_t rotl(uint64_t x, int k) {
        return (x << k) | (x >> (64 - k));
    }

    uint64_t next() {
        const uint64_t result = rotl(s[1] * 5, 7) * 9;
        const uint64_t t = s[1] << 17;
        s[2] ^= s[0]; s[3] ^= s[1]; s[1] ^= s[2]; s[0] ^= s[3];
        s[2] ^= t;
        s[3] = rotl(s[3], 45);
        return result;
    }

    // SplitMix64 used to expand a single 64-bit seed into 4 state words —
    // recommended by the xoshiro authors so a tiny seed doesn't leave the
    // generator in a near-zero state.
    void seed(uint64_t x) {
        for (int i = 0; i < 4; ++i) {
            x += 0x9E3779B97F4A7C15ULL;
            uint64_t z = x;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
            s[i] = z ^ (z >> 31);
        }
    }
};

// CHIP-8 / SUPER-CHIP 1.1 core. Pure emulator: no SFML, no I/O beyond loadROM.
// All host-facing concerns (rendering, input, audio, UI) live in src/ui.
class Chip8 {
public:
    static constexpr int DISPLAY_WIDTH  = 128;
    static constexpr int DISPLAY_HEIGHT = 64;
    static constexpr int LORES_WIDTH    = 64;
    static constexpr int LORES_HEIGHT   = 32;
    static constexpr int MEMORY_SIZE    = 4096;
    static constexpr int REGISTER_COUNT = 16;
    static constexpr int STACK_SIZE     = 16;
    static constexpr int KEY_COUNT      = 16;
    static constexpr uint16_t START_ADDRESS    = 0x200;
    static constexpr uint16_t FONTSET_ADDRESS  = 0x50;
    static constexpr uint16_t BIG_FONT_ADDRESS = 0xA0;

    enum class HaltReason : uint8_t {
        Running,
        ExitOpcode,        // 00FD
        UnknownOpcode,
        StackOverflow,
        StackUnderflow,
        BadMemoryAccess,
    };

    // Independent quirks — flip each one separately. Defaults are SCHIP-modern.
    struct Quirks {
        bool shift_in_place     = true;   // 8XY6/8XYE: true=shift Vx, false=Vx=Vy>>... (VIP)
        bool load_store_no_inc  = true;   // FX55/FX65: true=I unchanged, false=I+=x+1 (VIP)
        bool jump_with_offset_x = false;  // BNNN: SCHIP uses VX where X = high nibble; VIP uses V0
        bool vf_reset           = false;  // VIP only: 8XY1/2/3 reset VF to 0
        bool display_wait       = false;  // VIP only: at most one DXYN per frame
        bool clip_sprites       = true;   // sprites clip at edges (modern); false = wrap (VIP)
        bool mx8_extensions     = false;  // enable MX-8 custom opcodes (5XY1..3, FN50/60/70)
    };

    static Quirks modernQuirks();   // SCHIP defaults
    static Quirks legacyQuirks();   // COSMAC VIP defaults

    // ---- public state ----
    // Memory is owned by the CPU but accessed only through its API.
    // No caller (debugger, UI, snapshot loader) reaches past `mem` to
    // touch raw bytes — that's the invariant the refactor enforces.
    Memory                                mem;
    std::array<uint8_t,  REGISTER_COUNT>  v{};
    std::array<uint16_t, STACK_SIZE>      stack{};
    std::array<uint8_t,  KEY_COUNT>       keys{};
    std::array<uint32_t, DISPLAY_WIDTH * DISPLAY_HEIGHT> display{};
    std::array<uint8_t,  8>               rpl_flags{};

    uint16_t   pc{};
    uint16_t   index{};
    uint8_t    sp{};
    uint8_t    delay_timer{};
    uint8_t    sound_timer{};

    bool       draw_flag{};
    bool       hires{};
    bool       waiting_vblank{};
    HaltReason halt_reason{HaltReason::Running};

    Quirks     quirks{};

    // ---- determinism: PRNG state ----
    // The xoshiro state is part of CPU state, captured in snapshots so that
    // rewind / save-state replays exactly. Seed nondeterministically at
    // construction; reseed via setSeed() (CLI flag, FX70, or tests).
    Xoshiro256ss rng;

    // Seed the PRNG. Idempotent — call any time. Used by Chip8() constructor,
    // FX70 (RNDSEED), the --seed CLI flag, and unit tests.
    void setSeed(uint64_t seed);

    // ---- debugger hooks (Phase 3) ----
    std::unordered_set<uint16_t> breakpoints;
    bool      hit_breakpoint{};       // set by cycle() when about to execute a breakpoint
    uint64_t  total_cycles{};
    uint16_t  last_pc{};              // pc of the most recently executed instruction
    uint16_t  last_opcode{};

    struct TraceEntry {
        uint16_t pc;
        uint16_t opcode;
    };
    static constexpr size_t TRACE_CAPACITY = 256;
    std::deque<TraceEntry> trace;     // ring buffer of last N executed instructions

    Chip8();

    // Reset everything; reload font tables. Memory[0x200..] is left as the
    // currently-loaded ROM bytes — call loadROM again if you want to swap.
    void reset();

    // Load a CHIP-8 ROM at 0x200. Returns false on I/O error or oversize.
    bool loadROM(const std::string& path);
    bool loadROMBytes(const std::vector<uint8_t>& bytes);

    // Run one CPU instruction. No-op if halted, waiting on vblank, or at a
    // breakpoint (sets hit_breakpoint=true; clear it to step past).
    void cycle();

    // Decrement delay/sound timers and release the display_wait latch.
    // Call once per video frame (60Hz).
    void tickTimers();

    bool halted() const { return halt_reason != HaltReason::Running; }
    const char* haltReasonString() const;

    // ---- save state / rewind ----
    struct Snapshot {
        static constexpr uint32_t MAGIC = 0x43485038; // 'CHP8'
        // v2 = original
        // v3 = added xoshiro256** RNG state + keypad state for full
        //      deterministic replay (rewind through key-hold moments).
        static constexpr uint16_t VERSION = 3;

        uint32_t magic{MAGIC};
        uint16_t version{VERSION};
        std::array<uint8_t,  MEMORY_SIZE>     memory{};
        std::array<uint8_t,  REGISTER_COUNT>  v{};
        std::array<uint16_t, STACK_SIZE>      stack{};
        std::array<uint32_t, DISPLAY_WIDTH * DISPLAY_HEIGHT> display{};
        std::array<uint8_t,  8>               rpl_flags{};
        uint16_t pc{};
        uint16_t index{};
        uint8_t  sp{};
        uint8_t  delay_timer{};
        uint8_t  sound_timer{};
        bool     hires{};
        bool     waiting_vblank{};
        HaltReason halt_reason{HaltReason::Running};
        Quirks   quirks{};
        Xoshiro256ss rng{};                  // v3: PRNG state for replay determinism
        std::array<uint8_t, KEY_COUNT> keys{};  // v3: keypad latch for faithful rewind
    };
    Snapshot snapshot() const;
    void restore(const Snapshot& s);

    // ---- helpers exposed for the debugger ----
    uint16_t fetchOpcode(uint16_t addr) const;

    // Canonical FNV-1a hash of the framebuffer bytes — renderer-independent,
    // suitable for golden-image regression tests, replay self-check, CI
    // assertions. Only the active region is hashed (lo-res = top-left
    // 64x32, hi-res = full 128x64) so a hi-res ROM doesn't get a different
    // hash just because the unused bottom half changes.
    uint64_t framebufferHash() const;

    // ---- ISA management ----
    // CPU does not own the ISA pointer — singletons in IInstructionSet.hpp do.
    // installISA() is null-safe: passing nullptr keeps the previous ISA.
    void                installISA(class IInstructionSet* isa);
    class IInstructionSet* getISA() const { return isa_; }

    // ---- event queue (state mutation channel for UI/debugger/scripts) ----
    // enqueue() is non-blocking; events apply on the next drainEvents() call.
    // drainEvents() must be called at a deterministic sync point — the App
    // calls it once per frame, before the cycle batch. Never call mid-cycle.
    void   enqueue(CoreEvent ev);
    void   drainEvents();
    size_t pendingEvents() const { return events_.size(); }
    uint64_t totalEventsApplied() const { return total_events_applied_; }

    // Optional tap fired BEFORE each event lands on the queue. Used by the
    // App's replay recorder to capture events at the moment they're
    // submitted, so live keypad / debugger / scripted events are all
    // recorded uniformly. Pass nullptr to disable.
    using EventTap = std::function<void(const CoreEvent&)>;
    void   setEventTap(EventTap tap) { event_tap_ = std::move(tap); }

    // ---- machine API the ISA programs against ----
    // These are the only "non-trivial" mutators. Trivial ops (PC+=2, V[x]=nn,
    // index=nnn, etc.) the ISA does directly via the public state fields.
    void halt(HaltReason r);
    void pushStack(uint16_t addr);    // bounds-checked; halts on overflow
    uint16_t popStack();              // bounds-checked; halts on underflow

    // Display utilities — promoted to public so ISAs can call them. Each
    // sets draw_flag automatically.
    void clearScreen();
    void scrollDown(int lines);
    void scrollUp(int lines);     // SCHIP-1.1+ extension (00DN)
    void scrollLeft();
    void scrollRight();

    // The heart of DXYN. All sprite drawing — base 8xN, SCHIP 16x16,
    // clip-vs-wrap, VF collision flag, display_wait latch — lives here so
    // ISAs don't reimplement framebuffer manipulation.
    void drawSprite(uint8_t vx, uint8_t vy, uint8_t n);

private:
    void recordTrace(uint16_t pc, uint16_t op);

    // Per-event-type appliers. Hidden behind drainEvents()'s std::visit.
    void applyEvent(const ResetEvent&);
    void applyEvent(const LoadROMEvent&);
    void applyEvent(const WriteMemoryEvent&);
    void applyEvent(const WriteMemoryBlockEvent&);
    void applyEvent(const ToggleBreakpointEvent&);
    void applyEvent(const ClearAllBreakpointsEvent&);
    void applyEvent(const SetWatchpointEvent&);
    void applyEvent(const ClearAllWatchpointsEvent&);
    void applyEvent(const SetPCEvent&);
    void applyEvent(const InjectKeyEvent&);
    void applyEvent(const SetSeedEvent&);

    class IInstructionSet* isa_ = nullptr;   // not owned; see installISA

    std::deque<CoreEvent>  events_;
    uint64_t               total_events_applied_ = 0;   // diagnostic counter
    EventTap               event_tap_;
};
