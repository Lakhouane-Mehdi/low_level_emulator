// Headless core tests: link only against src/core/, no SFML.
// Verifies determinism, snapshot round-trip, RNG, memory watchpoints,
// and a handful of opcode behaviors that are easy to get wrong.
//
// Pass: prints "ALL PASSED" and exits 0.
// Fail: prints the failing assertion + line and exits 1.

#include "../src/core/Chip8.hpp"
#include "../src/core/Memory.hpp"
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
    test_rnd_mask();

    if (g_failures > 0) {
        std::printf("\n%d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("\nALL PASSED\n");
    return 0;
}
