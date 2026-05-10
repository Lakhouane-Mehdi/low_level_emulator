// Headless core tests: link only against src/core/, no SFML.
// Verifies determinism, snapshot round-trip, RNG, memory watchpoints,
// and a handful of opcode behaviors that are easy to get wrong.
//
// Pass: prints "ALL PASSED" and exits 0.
// Fail: prints the failing assertion + line and exits 1.

#include "../src/core/Chip8.hpp"
#include "../src/core/CoreEvents.hpp"
#include "../src/core/Memory.hpp"
#include "../src/core/Replay.hpp"
#include "../src/core/RewindBuffer.hpp"
#include "../src/core/isa/IInstructionSet.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

static int g_failures = 0;

#define CHECK(cond) do {                                              \
    if (!(cond)) {                                                    \
        std::printf("FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond);  \
        ++g_failures;                                                 \
    } else {                                                          \
        std::printf("ok    %s\n", #cond);                             \
    }                                                                 \
} while (0)

#define CHECK_EQ(a, b) do {                                            \
    auto __a = (a); auto __b = (b);                                    \
    if (!(__a == __b)) {                                               \
        std::printf("FAIL  %s:%d  %s == %s   got %lld vs %lld\n",      \
            __FILE__, __LINE__, #a, #b,                                \
            (long long)__a, (long long)__b);                           \
        ++g_failures;                                                  \
    } else {                                                           \
        std::printf("ok    %s == %s  (%lld)\n", #a, #b, (long long)__a); \
    }                                                                  \
} while (0)

// Helper: load a tiny program at 0x200.
static void load(Chip8& cpu, std::initializer_list<uint16_t> words) {
    std::vector<uint8_t> bytes;
    for (uint16_t w : words) {
        bytes.push_back(static_cast<uint8_t>(w >> 8));
        bytes.push_back(static_cast<uint8_t>(w & 0xFF));
    }
    bool ok = cpu.loadROMBytes(bytes);
    (void)ok;
}

// -------- 1. Determinism: same seed -> same RND stream --------
static void test_rng_determinism() {
    std::printf("\n[test_rng_determinism]\n");
    auto run = [](uint64_t seed) {
        Chip8 cpu;
        cpu.setSeed(seed);
        // Program: RND V0,FF; RND V1,FF; RND V2,FF; JP self
        load(cpu, {0xC0FF, 0xC1FF, 0xC2FF, 0x1206});
        for (int i = 0; i < 3; ++i) cpu.cycle();
        return std::array<uint8_t, 3>{cpu.v[0], cpu.v[1], cpu.v[2]};
    };
    auto a = run(0xCAFEBABE);
    auto b = run(0xCAFEBABE);
    auto c = run(0xDEADBEEF);
    CHECK(a == b);                  // same seed -> same bytes
    CHECK(a != c);                  // different seed -> different bytes
}

// -------- 2. Snapshot captures RNG state --------
static void test_snapshot_rng() {
    std::printf("\n[test_snapshot_rng]\n");
    Chip8 cpu;
    cpu.setSeed(42);
    load(cpu, {0xC0FF, 0xC1FF, 0xC2FF, 0xC3FF, 0xC4FF, 0x120A});
    cpu.cycle();    // V0
    auto snap = cpu.snapshot();
    cpu.cycle();    // V1
    cpu.cycle();    // V2
    uint8_t v1_first = cpu.v[1];
    uint8_t v2_first = cpu.v[2];
    cpu.restore(snap);
    cpu.cycle();
    cpu.cycle();
    CHECK_EQ(cpu.v[1], v1_first);
    CHECK_EQ(cpu.v[2], v2_first);
}

// -------- 3. Memory wrapper bounds --------
static void test_memory_bounds() {
    std::printf("\n[test_memory_bounds]\n");
    Memory m;
    m.write(0x100, 0xAB);
    CHECK_EQ((int)m.read(0x100), 0xAB);
    CHECK_EQ((int)m.read(0xFFFF), 0);   // OOR -> 0
    bool fault = false;
    m.set_bad_access_callback([&](Memory::FaultKind, uint16_t) { fault = true; });
    (void)m.read(0xFFFF);
    CHECK(fault);
}

// -------- 4. Watchpoint per-byte detection + first-hit semantics --------
static void test_memory_watchpoints() {
    std::printf("\n[test_memory_watchpoints]\n");
    Memory m;
    m.add_watchpoint(0x305, Memory::WatchKind::Write);
    m.add_watchpoint(0x308, Memory::WatchKind::Write);
    // Pre-set bytes to confirm reads don't trigger.
    uint8_t buf[8] = {1,2,3,4,5,6,7,8};
    CHECK(!m.watch_triggered());
    m.write_block(0x300, buf, 8);
    // Block crosses two watchpoints. Per spec we trigger first-hit only.
    CHECK(m.watch_triggered());
    auto ev = m.consume_watch_event();
    CHECK_EQ((int)ev.addr, 0x305);   // first-hit, not last
    CHECK_EQ((int)ev.value, 6);      // buf[5] == 6 written to 0x305
    CHECK(!m.watch_triggered());     // consumed
}

// -------- 5. Watchpoints survive restore; pending event dropped --------
static void test_watchpoints_across_restore() {
    std::printf("\n[test_watchpoints_across_restore]\n");
    Chip8 cpu;
    cpu.mem.add_watchpoint(0x300, Memory::WatchKind::Write);
    auto snap = cpu.snapshot();
    // Trigger a watch.
    cpu.mem.write(0x300, 0xAA);
    CHECK(cpu.mem.watch_triggered());
    cpu.restore(snap);
    CHECK(!cpu.mem.watch_triggered());           // pending event dropped
    CHECK(cpu.mem.has_watchpoint(0x300));        // watchpoint itself survived
}

// -------- 6. FX55 / FX65 round-trip via Memory API --------
static void test_load_store() {
    std::printf("\n[test_load_store]\n");
    Chip8 cpu;
    cpu.quirks = Chip8::modernQuirks();
    // Program:
    //   LD V0, 0x11; LD V1, 0x22; LD V2, 0x33; LD V3, 0x44
    //   LD I, 0x300
    //   LD [I], V3        ; stores V0..V3
    //   LD V0, 0; LD V1, 0; LD V2, 0; LD V3, 0   ; clear
    //   LD V3, [I]        ; loads back
    //   JP self
    load(cpu, {
        0x6011, 0x6122, 0x6233, 0x6344,
        0xA300, 0xF355,
        0x6000, 0x6100, 0x6200, 0x6300,
        0xF365,
        0x121A
    });
    for (int i = 0; i < 11; ++i) cpu.cycle();
    CHECK_EQ((int)cpu.v[0], 0x11);
    CHECK_EQ((int)cpu.v[1], 0x22);
    CHECK_EQ((int)cpu.v[2], 0x33);
    CHECK_EQ((int)cpu.v[3], 0x44);
}

// -------- 7. MX-8 MUL behavior + VF overflow --------
static void test_mx8_mul() {
    std::printf("\n[test_mx8_mul]\n");
    Chip8 cpu;
    cpu.quirks = Chip8::modernQuirks();
    cpu.quirks.mx8_extensions = true;
    // V1=20, V2=20, MUL V1,V2 -> V1=0x90, VF=0x01 (400 = 0x190)
    load(cpu, {0x6114, 0x6214, 0x5121, 0x1208});
    for (int i = 0; i < 3; ++i) cpu.cycle();
    CHECK_EQ((int)cpu.v[1],   0x90);
    CHECK_EQ((int)cpu.v[0xF], 0x01);
}

// -------- 8. MX-8 disabled -> UnknownOpcode halt --------
static void test_mx8_off_halts() {
    std::printf("\n[test_mx8_off_halts]\n");
    Chip8 cpu;
    cpu.quirks = Chip8::modernQuirks();
    cpu.quirks.mx8_extensions = false;
    // MUL V1, V2 (5121) — should halt with UnknownOpcode.
    load(cpu, {0x5121, 0x1202});
    cpu.cycle();
    CHECK(cpu.halted());
    CHECK_EQ((int)cpu.halt_reason, (int)Chip8::HaltReason::UnknownOpcode);
}

// -------- 9. Snapshot round-trip preserves opcode results --------
static void test_snapshot_full_state() {
    std::printf("\n[test_snapshot_full_state]\n");
    Chip8 cpu;
    cpu.setSeed(7);
    // Execute a few ops, snapshot, advance, restore, check identity.
    load(cpu, {0x6042, 0xA300, 0xC1FF, 0xC2FF, 0x1208});
    cpu.cycle();
    cpu.cycle();
    auto snap = cpu.snapshot();
    cpu.cycle();
    cpu.cycle();
    uint16_t pc_after = cpu.pc;
    uint8_t v1_after = cpu.v[1];
    uint8_t v2_after = cpu.v[2];

    cpu.restore(snap);
    CHECK_EQ((int)cpu.v[0], 0x42);
    CHECK_EQ((int)cpu.index, 0x300);
    cpu.cycle();
    cpu.cycle();
    CHECK_EQ((int)cpu.pc,    (int)pc_after);
    CHECK_EQ((int)cpu.v[1],  (int)v1_after);
    CHECK_EQ((int)cpu.v[2],  (int)v2_after);
}

// -------- 10. Snapshot captures keypad state --------
static void test_snapshot_keys() {
    std::printf("\n[test_snapshot_keys]\n");
    Chip8 cpu;
    cpu.keys[0x5] = 1;
    cpu.keys[0xA] = 1;
    auto snap = cpu.snapshot();
    cpu.keys[0x5] = 0;
    cpu.keys[0xA] = 0;
    cpu.keys[0x1] = 1;     // change keys after snapshot
    cpu.restore(snap);
    CHECK_EQ((int)cpu.keys[0x5], 1);     // restored
    CHECK_EQ((int)cpu.keys[0xA], 1);     // restored
    CHECK_EQ((int)cpu.keys[0x1], 0);     // post-snapshot change reverted
}

// -------- 11. EX9E skip honors restored key state --------
static void test_skip_op_after_restore() {
    std::printf("\n[test_skip_op_after_restore]\n");
    Chip8 cpu;
    // Program: LD V0,5 ; SKP V0 ; <skipped if K5 held> JP 0x208 ; <not skipped> JP 0x206
    // Layout: 200:6005  202:E09E  204:1208  206:1208  208:1208
    load(cpu, {0x6005, 0xE09E, 0x1208, 0x1208, 0x1208});
    cpu.cycle();                              // V0=5
    cpu.keys[5] = 1;                          // press
    auto held = cpu.snapshot();
    cpu.keys[5] = 0;                          // release
    cpu.cycle();                              // SKP — won't skip (key released)
    CHECK_EQ((int)cpu.pc, 0x204);
    cpu.restore(held);                        // rewind to held state
    CHECK_EQ((int)cpu.keys[5], 1);
    CHECK_EQ((int)cpu.pc, 0x202);             // back at SKP
    cpu.cycle();                              // SKP — should skip now
    CHECK_EQ((int)cpu.pc, 0x206);
}

// -------- ISA. Chip8ISA halts on SCHIP-only opcode (00FF HIGH) --------
static void test_chip8_isa_rejects_schip() {
    std::printf("\n[test_chip8_isa_rejects_schip]\n");
    Chip8 cpu;
    cpu.installISA(&ISA::chip8());
    load(cpu, {0x00FF, 0x1202});   // HIGH (SCHIP) — base CHIP-8 doesn't know it
    cpu.cycle();
    CHECK(cpu.halted());
    CHECK_EQ((int)cpu.halt_reason, (int)Chip8::HaltReason::UnknownOpcode);
}

// -------- ISA. SchipISA executes 00FF HIGH (sets hires) --------
static void test_schip_isa_runs_high() {
    std::printf("\n[test_schip_isa_runs_high]\n");
    Chip8 cpu;
    cpu.installISA(&ISA::schip());
    cpu.hires = false;
    load(cpu, {0x00FF, 0x1202});
    cpu.cycle();
    CHECK(!cpu.halted());
    CHECK(cpu.hires);
}

// -------- ISA. SchipISA halts on MX-8-only opcode (5121 MUL) --------
static void test_schip_isa_rejects_mx8() {
    std::printf("\n[test_schip_isa_rejects_mx8]\n");
    Chip8 cpu;
    cpu.installISA(&ISA::schip());
    cpu.quirks.mx8_extensions = true;   // even with the quirk, SchipISA doesn't decode it
    load(cpu, {0x5121, 0x1202});
    cpu.cycle();
    CHECK(cpu.halted());
}

// -------- ISA. Mx8ISA disassembles MUL even when execution would halt --------
static void test_disassembly_uses_widest_isa() {
    std::printf("\n[test_disassembly_uses_widest_isa]\n");
    // The free disassemble() function should always render MX-8 mnemonics.
    auto& isa = ISA::mx8();
    CHECK(isa.disassemble(0x5121) == "MUL V1, V2");
    CHECK(isa.disassemble(0x00FF) == "HIGH");
    CHECK(isa.disassemble(0x6A05) == "LD VA, 05");
}

// -------- Events. Memory writes are deferred until drainEvents --------
static void test_events_deferred() {
    std::printf("\n[test_events_deferred]\n");
    Chip8 cpu;
    cpu.enqueue(WriteMemoryEvent{0x300, 0xAB});
    // Pre-drain: byte unchanged.
    CHECK_EQ((int)cpu.mem.peek(0x300), 0x00);
    CHECK_EQ((int)cpu.pendingEvents(), 1);
    cpu.drainEvents();
    CHECK_EQ((int)cpu.mem.peek(0x300), 0xAB);
    CHECK_EQ((int)cpu.pendingEvents(), 0);
    CHECK_EQ((int)cpu.totalEventsApplied(), 1);
}

// -------- Events. FIFO ordering --------
static void test_events_fifo() {
    std::printf("\n[test_events_fifo]\n");
    Chip8 cpu;
    // Enqueue three writes to the same address; final value must be the last.
    cpu.enqueue(WriteMemoryEvent{0x400, 0x11});
    cpu.enqueue(WriteMemoryEvent{0x400, 0x22});
    cpu.enqueue(WriteMemoryEvent{0x400, 0x33});
    cpu.drainEvents();
    CHECK_EQ((int)cpu.mem.peek(0x400), 0x33);
    CHECK_EQ((int)cpu.totalEventsApplied(), 3);
}

// -------- Events. WriteMemory fires watchpoints same as CPU-driven writes --------
static void test_events_fire_watchpoints() {
    std::printf("\n[test_events_fire_watchpoints]\n");
    Chip8 cpu;
    cpu.mem.add_watchpoint(0x500, Memory::WatchKind::Write);
    cpu.enqueue(WriteMemoryEvent{0x500, 0x99});
    CHECK(!cpu.mem.watch_triggered());     // pre-drain: nothing happened yet
    cpu.drainEvents();
    CHECK(cpu.mem.watch_triggered());      // post-drain: watchpoint fired
    auto ev = cpu.mem.consume_watch_event();
    CHECK_EQ((int)ev.addr,  0x500);
    CHECK_EQ((int)ev.value, 0x99);
}

// -------- Events. ToggleBreakpoint + ClearAll --------
static void test_events_breakpoints() {
    std::printf("\n[test_events_breakpoints]\n");
    Chip8 cpu;
    cpu.enqueue(ToggleBreakpointEvent{0x250});
    cpu.enqueue(ToggleBreakpointEvent{0x260});
    cpu.drainEvents();
    CHECK_EQ((int)cpu.breakpoints.size(), 2);
    cpu.enqueue(ToggleBreakpointEvent{0x250});   // toggle off
    cpu.drainEvents();
    CHECK_EQ((int)cpu.breakpoints.size(), 1);
    CHECK(cpu.breakpoints.count(0x260) == 1);
    cpu.enqueue(ClearAllBreakpointsEvent{});
    cpu.drainEvents();
    CHECK_EQ((int)cpu.breakpoints.size(), 0);
}

// -------- Events. Watchpoint set/clear via events --------
static void test_events_watchpoints_via_events() {
    std::printf("\n[test_events_watchpoints_via_events]\n");
    Chip8 cpu;
    using WK = Memory::WatchKind;
    cpu.enqueue(SetWatchpointEvent{0x600, WK::Read,  false});
    cpu.enqueue(SetWatchpointEvent{0x601, WK::Write, false});
    cpu.drainEvents();
    CHECK(cpu.mem.has_watchpoint(0x600));
    CHECK(cpu.mem.has_watchpoint(0x601));
    cpu.enqueue(SetWatchpointEvent{0x600, WK::Read, true});  // erase
    cpu.drainEvents();
    CHECK(!cpu.mem.has_watchpoint(0x600));
    cpu.enqueue(ClearAllWatchpointsEvent{});
    cpu.drainEvents();
    CHECK(!cpu.mem.has_watchpoint(0x601));
}

// -------- Events. SetPC + InjectKey + Reset + SetSeed --------
static void test_events_misc() {
    std::printf("\n[test_events_misc]\n");
    Chip8 cpu;
    cpu.pc = 0x200;
    cpu.enqueue(SetPCEvent{0x300});
    cpu.enqueue(InjectKeyEvent{0xA, true});
    cpu.enqueue(SetSeedEvent{0xCAFEBABE});
    cpu.drainEvents();
    CHECK_EQ((int)cpu.pc, 0x300);
    CHECK_EQ((int)cpu.keys[0xA], 1);

    // Capture RNG output after seed-via-event; reseed direct, compare.
    Chip8 reference;
    reference.setSeed(0xCAFEBABE);
    CHECK_EQ((int)(cpu.rng.next() & 0xFF), (int)(reference.rng.next() & 0xFF));

    // Reset event puts PC back to 0x200 and clears keys.
    cpu.enqueue(ResetEvent{});
    cpu.drainEvents();
    CHECK_EQ((int)cpu.pc, 0x200);
    CHECK_EQ((int)cpu.keys[0xA], 0);
}

// -------- Events. WriteMemoryBlockEvent --------
static void test_events_write_block() {
    std::printf("\n[test_events_write_block]\n");
    Chip8 cpu;
    std::vector<uint8_t> patch = {0xDE, 0xAD, 0xBE, 0xEF};
    cpu.enqueue(WriteMemoryBlockEvent{0x700, patch});
    cpu.drainEvents();
    CHECK_EQ((int)cpu.mem.peek(0x700), 0xDE);
    CHECK_EQ((int)cpu.mem.peek(0x701), 0xAD);
    CHECK_EQ((int)cpu.mem.peek(0x702), 0xBE);
    CHECK_EQ((int)cpu.mem.peek(0x703), 0xEF);
}

// -------- Events. Snapshot does NOT carry queue (queue is host-side) --------
// The event queue is conceptually a UI-to-CPU mailbox. Snapshots capture
// machine state, not in-flight messages — otherwise rewinding through a
// pending action would re-replay it spuriously.
static void test_events_excluded_from_snapshot() {
    std::printf("\n[test_events_excluded_from_snapshot]\n");
    Chip8 cpu;
    auto pre = cpu.snapshot();
    cpu.enqueue(WriteMemoryEvent{0x100, 0xFF});
    CHECK_EQ((int)cpu.pendingEvents(), 1);
    cpu.restore(pre);                       // restore mid-queue
    // The pending event survives a restore — it's host-side state, not
    // captured in the snapshot. This is the right semantic: the user
    // pressed a key, the keypress shouldn't be cancelled by a rewind.
    CHECK_EQ((int)cpu.pendingEvents(), 1);
    cpu.drainEvents();
    CHECK_EQ((int)cpu.mem.peek(0x100), 0xFF);
}

// -------- Framebuffer hash. Stable, content-sensitive, mode-sensitive --------
static void test_framebuffer_hash() {
    std::printf("\n[test_framebuffer_hash]\n");
    Chip8 cpu;

    // Two fresh CPUs should hash to the same value (both blank lo-res).
    Chip8 other;
    CHECK_EQ((long long)cpu.framebufferHash(), (long long)other.framebufferHash());

    // Hash is stable: calling twice on same state gives same value.
    uint64_t h1 = cpu.framebufferHash();
    uint64_t h2 = cpu.framebufferHash();
    CHECK_EQ((long long)h1, (long long)h2);

    // Mode-sensitive: blank lo-res vs blank hi-res must differ.
    cpu.hires = true;
    CHECK(cpu.framebufferHash() != h1);
    cpu.hires = false;
    CHECK_EQ((long long)cpu.framebufferHash(), (long long)h1);

    // Content-sensitive: lighting one pixel changes the hash.
    cpu.display[0] = 0xFFFFFFFF;
    CHECK(cpu.framebufferHash() != h1);

    // Palette-independent: lighting one pixel with a different "on" value
    // (any nonzero) must hash the same (we only compare zero / non-zero).
    Chip8 a, b;
    a.display[5] = 0xFFFFFFFF;
    b.display[5] = 0x00000001;     // also "on", different shade
    CHECK_EQ((long long)a.framebufferHash(), (long long)b.framebufferHash());
}

// -------- Framebuffer hash. Determinism after seeded RND program --------
// This is the canonical regression-suite shape: rom + seed -> exact hash.
static void test_framebuffer_hash_seeded_program() {
    std::printf("\n[test_framebuffer_hash_seeded_program]\n");
    auto run = [](uint64_t seed) {
        Chip8 cpu;
        cpu.setSeed(seed);
        // Tiny program: load font glyph for digit 5, draw at (V0=0, V1=0),
        // then halt loop. Output is deterministic.
        load(cpu, {
            0x6000,         // LD V0, 0
            0x6100,         // LD V1, 0
            0x6205,         // LD V2, 5
            0xF229,         // LD F, V2  (I = font for 5)
            0xD015,         // DRW V0, V1, 5
            0x1209          // JP 0x209  (halt loop)
        });
        for (int i = 0; i < 6; ++i) cpu.cycle();
        return cpu.framebufferHash();
    };
    uint64_t a = run(1);
    uint64_t b = run(1);
    uint64_t c = run(999);
    CHECK_EQ((long long)a, (long long)b);   // same seed -> same hash
    // Same seed-AND-program means same output regardless of seed (no RND used).
    // So a==c too — verifies hash captures content not RNG state.
    CHECK_EQ((long long)a, (long long)c);

    // Sanity: known glyph should produce a nonzero pixel hash (not blank).
    Chip8 blank;
    CHECK(a != blank.framebufferHash());
}

// -------- Replay round-trip via JSON --------
static void test_replay_roundtrip() {
    std::printf("\n[test_replay_roundtrip]\n");
    Replay r;
    r.rom_bytes = {0x00, 0xE0, 0x12, 0x00};   // CLS; JP 200
    r.isa       = "schip";
    r.quirks    = Chip8::legacyQuirks();
    r.have_seed = true;
    r.seed      = 0xCAFE;
    r.events.push_back({10, InjectKeyEvent{0x5, true}});
    r.events.push_back({20, InjectKeyEvent{0x5, false}});
    r.events.push_back({25, WriteMemoryEvent{0x300, 0xAB}});
    r.events.push_back({30, WriteMemoryBlockEvent{0x400, {0xDE, 0xAD, 0xBE, 0xEF}}});
    r.events.push_back({40, SetPCEvent{0x250}});
    r.events.push_back({50, ResetEvent{}});
    r.events.push_back({60, SetSeedEvent{0xBEEF}});
    r.checkpoints.push_back({60,  0xDEADBEEF12345678ULL});
    r.checkpoints.push_back({120, 0x1122334455667788ULL});

    auto json = r.toJson();

    Replay r2;
    std::string err;
    bool ok = r2.fromJson(json, &err);
    if (!ok) std::printf("parse err: %s\n", err.c_str());
    CHECK(ok);

    CHECK_EQ((int)r2.version, Replay::VERSION);
    CHECK_EQ((int)r2.rom_bytes.size(), 4);
    CHECK_EQ((int)r2.rom_bytes[0], 0x00);
    CHECK_EQ((int)r2.rom_bytes[3], 0x00);
    CHECK(r2.isa == "schip");
    CHECK(r2.quirks.shift_in_place    == false);
    CHECK(r2.quirks.load_store_no_inc == false);
    CHECK(r2.quirks.display_wait      == true);
    CHECK(r2.have_seed);
    CHECK_EQ((int)r2.seed, 0xCAFE);
    CHECK_EQ((int)r2.events.size(), 7);
    CHECK_EQ((int)r2.checkpoints.size(), 2);
    CHECK_EQ((long long)r2.checkpoints[0].hash, (long long)0xDEADBEEF12345678ULL);

    // Pick out the first event and verify type + payload.
    const auto& first = r2.events[0];
    CHECK_EQ((int)first.frame, 10);
    CHECK(std::holds_alternative<InjectKeyEvent>(first.event));
    auto& ik = std::get<InjectKeyEvent>(first.event);
    CHECK_EQ((int)ik.key, 5);
    CHECK(ik.down);
}

// -------- Replay deterministic playback: events at frame N produce same hash --------
static void test_replay_deterministic_playback() {
    std::printf("\n[test_replay_deterministic_playback]\n");
    // Build a tiny program that just halts at PC 0x202 forever, but we'll
    // inject a key at frame 5 and write memory at frame 10 to verify the
    // replay applies events at the right frames.
    auto build_cpu = [](const Replay& r) {
        Chip8 cpu;
        cpu.installISA(&ISA::by_name(r.isa));
        cpu.quirks = r.quirks;
        cpu.loadROMBytes(r.rom_bytes);
        if (r.have_seed) cpu.setSeed(static_cast<uint64_t>(r.seed));
        return cpu;
    };
    auto run_replay = [&](const Replay& r, int total_frames) {
        Chip8 cpu = build_cpu(r);
        size_t ev_idx = 0;
        for (int f = 0; f < total_frames; ++f) {
            // Apply all events scheduled for this frame ordinal BEFORE
            // the cycle batch (matches GUI App drain semantics).
            while (ev_idx < r.events.size() && r.events[ev_idx].frame == (uint64_t)f) {
                cpu.enqueue(r.events[ev_idx].event);
                ++ev_idx;
            }
            cpu.drainEvents();
            for (int c = 0; c < 12; ++c) cpu.cycle();
            cpu.tickTimers();
        }
        return std::pair<uint64_t, uint8_t>{cpu.framebufferHash(), cpu.keys[5]};
    };

    Replay r;
    r.rom_bytes = {0x12, 0x00};            // JP 0x200 (infinite loop)
    r.isa       = "mx8";
    r.quirks    = Chip8::modernQuirks();
    r.have_seed = true;
    r.seed      = 1;
    r.events.push_back({5,  InjectKeyEvent{0x5, true}});
    r.events.push_back({10, WriteMemoryEvent{0x300, 0x42}});

    auto [h1, k1] = run_replay(r, 30);
    auto [h2, k2] = run_replay(r, 30);
    CHECK_EQ((long long)h1, (long long)h2);
    CHECK_EQ((int)k1, 1);
    CHECK_EQ((int)k2, 1);

    // Same replay run twice -> identical state (already proven above).
    // Now verify replay is sensitive to seed: a SetSeedEvent at frame 0
    // followed by RND at frame 1 produces different output for different
    // seeds.
    Replay r2 = r;
    // Replace ROM with: RND V0,FF; JP 0x202 (infinite loop, halts on seed)
    r2.rom_bytes = {0xC0, 0xFF, 0x12, 0x02};
    r2.have_seed = true;
    r2.seed = 1;
    auto [hA, kA] = run_replay(r2, 5);
    (void)kA;

    Replay r3 = r2;
    r3.seed = 999;
    auto [hC, kC] = run_replay(r3, 5);
    (void)kC;
    // Different seed => RND produces different bytes, so V0 differs.
    // Hash is a framebuffer hash though — V0 doesn't show up in pixels.
    // Skip this assertion: the replay IS deterministic regardless of seed
    // because the program doesn't draw anything. Instead verify the seed
    // actually got applied by checking a rendered run differs:
    Replay r4 = r2;
    // Program that draws a pixel at (RND, RND): different seeds produce
    // different positions, hence different framebuffer hashes.
    r4.rom_bytes = {
        0xC0, 0x3F,   // RND V0, 3F   (V0 in 0..63, lo-res X)
        0xC1, 0x1F,   // RND V1, 1F   (V1 in 0..31, lo-res Y)
        0xA2, 0x10,   // LD I, 0x210
        0xD0, 0x11,   // DRW V0, V1, 1
        0x12, 0x08,   // JP 0x208 (halt loop)
        // 0x20A..0x20F: padding
        0,0,0,0,0,0,
        // 0x210: 1-byte sprite (a single pixel pattern)
        0x80
    };
    r4.seed = 1;
    auto [h_seed1, _] = run_replay(r4, 5);
    Replay r5 = r4;
    r5.seed = 999;
    auto [h_seed999, __] = run_replay(r5, 5);
    (void)_; (void)__;
    // Different seeds -> different (x, y) -> different framebuffer hash.
    CHECK(h_seed1 != h_seed999);
}

// -------- RewindBuffer. Anchors take at interval boundaries --------
static void test_rewind_anchors_at_intervals() {
    std::printf("\n[test_rewind_anchors_at_intervals]\n");
    Chip8 cpu;
    RewindBuffer rb(/*anchor_interval=*/10, /*max_window=*/100);

    // Drive 25 frames. Should anchor at 0 (first frame always), 10, 20.
    for (uint64_t f = 0; f < 25; ++f) {
        rb.noteFrameBegin(f, cpu);
        rb.noteFrameEnd(f);
    }
    auto frames = rb.anchorFrames();
    CHECK_EQ((int)frames.size(), 3);
    CHECK_EQ((int)frames[0], 0);
    CHECK_EQ((int)frames[1], 10);
    CHECK_EQ((int)frames[2], 20);
}

// -------- RewindBuffer. trim() drops old anchors but keeps the latest --------
static void test_rewind_trim_keeps_latest() {
    std::printf("\n[test_rewind_trim_keeps_latest]\n");
    Chip8 cpu;
    RewindBuffer rb(/*anchor_interval=*/10, /*max_window=*/30);
    for (uint64_t f = 0; f < 100; ++f) {
        rb.noteFrameBegin(f, cpu);
        rb.noteFrameEnd(f);
        rb.trim(f);
    }
    // At frame 99 with max_window=30, anything before frame 69 should be dropped.
    auto frames = rb.anchorFrames();
    CHECK(frames.size() >= 1);
    CHECK(frames.front() >= 60);    // 60 or later — the floor of (99-30)/10
}

// -------- RewindBuffer. Reconstruction matches a fresh run, byte-for-byte --------
static void test_rewind_reconstruction_matches_fresh() {
    std::printf("\n[test_rewind_reconstruction_matches_fresh]\n");
    auto setup_cpu = []() {
        Chip8 c;
        c.installISA(&ISA::mx8());
        c.setSeed(0xCAFEBABE);
        // Tiny program: RND V0, FF; RND V1, FF; JP 0x200 (loop)
        std::vector<uint16_t> ops = {0xC0FF, 0xC1FF, 0x1200};
        std::vector<uint8_t> bytes;
        for (uint16_t w : ops) { bytes.push_back(w >> 8); bytes.push_back(w & 0xFF); }
        c.loadROMBytes(bytes);
        c.setSeed(0xCAFEBABE);   // re-seed after loadROM (loadROM calls reset)
        return c;
    };

    constexpr int CYCLES_PER_FRAME = 5;
    constexpr int ANCHOR_INTERVAL  = 7;       // intentionally non-multiple
    constexpr int TOTAL_FRAMES     = 50;
    constexpr uint64_t TARGET      = 23;

    // ---- Run A: fresh CPU, drive to frame TARGET, capture state hash ----
    Chip8 fresh = setup_cpu();
    uint64_t fresh_frame_ord = 0;
    for (; fresh_frame_ord < TARGET; ++fresh_frame_ord) {
        fresh.drainEvents();
        for (int c = 0; c < CYCLES_PER_FRAME; ++c) fresh.cycle();
        fresh.tickTimers();
    }
    uint64_t fresh_hash = fresh.framebufferHash();
    uint16_t fresh_pc   = fresh.pc;
    uint8_t  fresh_v0   = fresh.v[0];
    uint8_t  fresh_v1   = fresh.v[1];

    // ---- Run B: simulate App driving CPU + RewindBuffer; rewind to TARGET ----
    Chip8 b = setup_cpu();
    RewindBuffer rb(ANCHOR_INTERVAL, /*max_window=*/TOTAL_FRAMES + 10);
    b.setEventTap([&](const CoreEvent& ev){
        // Stamping: buffer captures the frame at submission time. Our test
        // emits no events, but wire it up for completeness.
        rb.noteEvent(/*frame=*/0, ev);
    });

    for (uint64_t f = 0; f < TOTAL_FRAMES; ++f) {
        rb.noteFrameBegin(f, b);
        b.drainEvents();
        for (int c = 0; c < CYCLES_PER_FRAME; ++c) b.cycle();
        b.tickTimers();
        rb.noteFrameEnd(f);
    }

    // Now rewind to TARGET.
    auto plan = rb.planRewindTo(TARGET);
    CHECK(plan.has_value());
    if (!plan) return;

    b.restore(plan->anchor);
    for (int f = 0; f < plan->frames_to_advance; ++f) {
        for (auto& ev : plan->events_per_frame[f]) b.enqueue(ev);
        b.drainEvents();
        for (int c = 0; c < CYCLES_PER_FRAME; ++c) b.cycle();
        b.tickTimers();
    }

    // State at frame TARGET should match the fresh run exactly.
    CHECK_EQ((long long)b.framebufferHash(), (long long)fresh_hash);
    CHECK_EQ((int)b.pc,    (int)fresh_pc);
    CHECK_EQ((int)b.v[0],  (int)fresh_v0);
    CHECK_EQ((int)b.v[1],  (int)fresh_v1);
}

// -------- RewindBuffer. Reconstruction with events: replay reproduces input timeline --------
static void test_rewind_with_events() {
    std::printf("\n[test_rewind_with_events]\n");
    auto setup_cpu = []() {
        Chip8 c;
        c.installISA(&ISA::mx8());
        // Program: SKP V0 ; JP 0x202 (no skip) ; LD V5, 0xAA ; JP self
        // Layout: 200:E09E  202:1202  204:65AA  206:1206
        std::vector<uint16_t> ops = {0xE09E, 0x1202, 0x65AA, 0x1206};
        std::vector<uint8_t> bytes;
        for (uint16_t w : ops) { bytes.push_back(w >> 8); bytes.push_back(w & 0xFF); }
        c.loadROMBytes(bytes);
        c.v[0] = 5;     // skip-if-key-5-pressed
        return c;
    };

    constexpr int CYCLES_PER_FRAME = 1;     // one op per frame for clarity
    constexpr int ANCHOR_INTERVAL  = 4;
    constexpr int TOTAL_FRAMES     = 12;

    // Reference run: press key 5 at frame 2.
    Chip8 ref = setup_cpu();
    uint8_t ref_v5_at_target = 0;
    for (uint64_t f = 0; f < TOTAL_FRAMES; ++f) {
        if (f == 2) ref.enqueue(InjectKeyEvent{5, true});
        ref.drainEvents();
        for (int c = 0; c < CYCLES_PER_FRAME; ++c) ref.cycle();
        ref.tickTimers();
        if (f == 6) ref_v5_at_target = ref.v[5];   // capture for comparison
    }
    // Expected: key 5 was pressed by frame 2; SKP V0 (V0=5) at frame 0
    // misses (key not yet pressed), JP back, then... the program loops at
    // 0x200/0x202 forever because it never advances. Let me just trust
    // the reference run here and assert reconstruction matches it.

    // Recording run: same input sequence + RewindBuffer.
    Chip8 b = setup_cpu();
    RewindBuffer rb(ANCHOR_INTERVAL, /*max_window=*/TOTAL_FRAMES + 10);
    uint64_t cur_frame = 0;
    b.setEventTap([&](const CoreEvent& ev){
        rb.noteEvent(cur_frame, ev);
    });

    for (cur_frame = 0; cur_frame < TOTAL_FRAMES; ++cur_frame) {
        rb.noteFrameBegin(cur_frame, b);
        if (cur_frame == 2) b.enqueue(InjectKeyEvent{5, true});
        b.drainEvents();
        for (int c = 0; c < CYCLES_PER_FRAME; ++c) b.cycle();
        b.tickTimers();
        rb.noteFrameEnd(cur_frame);
    }

    // Rewind to frame 7 — past the key press, anchor likely at frame 4.
    auto plan = rb.planRewindTo(7);
    CHECK(plan.has_value());
    if (!plan) return;

    b.restore(plan->anchor);
    for (int f = 0; f < plan->frames_to_advance; ++f) {
        for (auto& ev : plan->events_per_frame[f]) b.enqueue(ev);
        b.drainEvents();
        for (int c = 0; c < CYCLES_PER_FRAME; ++c) b.cycle();
        b.tickTimers();
    }

    // Re-run the reference fresh up to frame 7 too for direct comparison.
    Chip8 ref2 = setup_cpu();
    for (uint64_t f = 0; f <= 7; ++f) {
        if (f == 2) ref2.enqueue(InjectKeyEvent{5, true});
        ref2.drainEvents();
        for (int c = 0; c < CYCLES_PER_FRAME; ++c) ref2.cycle();
        ref2.tickTimers();
    }

    CHECK_EQ((int)b.pc,        (int)ref2.pc);
    CHECK_EQ((int)b.v[5],      (int)ref2.v[5]);
    CHECK_EQ((int)b.keys[5],   (int)ref2.keys[5]);
    (void)ref_v5_at_target;
}

// -------- RewindBuffer. Plan returns nullopt when empty --------
static void test_rewind_empty() {
    std::printf("\n[test_rewind_empty]\n");
    RewindBuffer rb(60, 1800);
    CHECK(!rb.planRewindTo(0).has_value());
    CHECK(!rb.planRewindOneFrame(100).has_value());
    CHECK_EQ((int)rb.anchorCount(), 0);
}

// -------- 12. CXNN with NN mask --------
static void test_rnd_mask() {
    std::printf("\n[test_rnd_mask]\n");
    Chip8 cpu;
    cpu.setSeed(1);
    // RND V0, 0x0F; loop 100 times, all results must have high nibble zero.
    load(cpu, {0xC00F, 0x1200});
    for (int i = 0; i < 100; ++i) {
        cpu.cycle();   // RND
        cpu.cycle();   // JP back
        if (cpu.v[0] & 0xF0) { CHECK(false); return; }
    }
    CHECK(true);
}

int main() {
    test_rng_determinism();
    test_snapshot_rng();
    test_memory_bounds();
    test_memory_watchpoints();
    test_watchpoints_across_restore();
    test_load_store();
    test_mx8_mul();
    test_mx8_off_halts();
    test_snapshot_full_state();
    test_snapshot_keys();
    test_skip_op_after_restore();
    test_chip8_isa_rejects_schip();
    test_schip_isa_runs_high();
    test_schip_isa_rejects_mx8();
    test_disassembly_uses_widest_isa();
    test_events_deferred();
    test_events_fifo();
    test_events_fire_watchpoints();
    test_events_breakpoints();
    test_events_watchpoints_via_events();
    test_events_misc();
    test_events_write_block();
    test_events_excluded_from_snapshot();
    test_framebuffer_hash();
    test_framebuffer_hash_seeded_program();
    test_replay_roundtrip();
    test_replay_deterministic_playback();
    test_rewind_anchors_at_intervals();
    test_rewind_trim_keeps_latest();
    test_rewind_reconstruction_matches_fresh();
    test_rewind_with_events();
    test_rewind_empty();
    test_rnd_mask();

    if (g_failures > 0) {
        std::printf("\n%d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("\nALL PASSED\n");
    return 0;
}
