// chip8_run — headless CHIP-8 runner for CI, fuzzing, regression suites.
//
// Loads a ROM, runs for N frames at a configurable cycles-per-frame, and
// either prints the final framebuffer hash or asserts it matches a known
// value. No SFML, no UI, no audio. Exits:
//   0  success (or hash match if --expect-hash given)
//   1  hash mismatch / divergence
//   2  CPU halted before reaching the requested frame count
//   3  CLI / load error
//
// Modes:
//   ROM:           chip8_run rom.ch8 --frames N [...]
//   Replay:        chip8_run --replay file.json [--print-hash|--expect-hash ...]
//   Diff-replay:   chip8_run --diff-replay a.json b.json
//                  Binary-search the first frame at which two replays diverge
//                  in machine state. Reports per-component digest deltas at
//                  the divergence frame. Compares input event streams first;
//                  refuses to proceed if events differ (unless --force).
//   Self-check:    chip8_run --replay file.json --self-check
//                  Re-executes the replay against itself and verifies every
//                  embedded checkpoint hash matches.

#include "../src/core/Chip8.hpp"
#include "../src/core/CoreEvents.hpp"
#include "../src/core/Replay.hpp"
#include "../src/core/isa/IInstructionSet.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

struct Args {
    std::string rom_path;
    std::string replay_path;     // --replay file.json (mutually exclusive with rom_path)
    std::string diff_a;          // --diff-replay A B
    std::string diff_b;
    int         frames           = 60;
    int         cycles_per_frame = 12;
    int64_t     seed             = -1;        // -1 = boot entropy
    std::string isa_name         = "mx8";
    std::string quirks_preset    = "modern";  // modern | legacy
    bool        mx8_extensions   = false;
    bool        print_hash       = false;
    bool        print_state      = false;
    bool        have_expected    = false;
    uint64_t    expected_hash    = 0;
    bool        have_frames_arg  = false;     // distinguish "--frames N" from default
    bool        diff_force       = false;     // --force: diff even if inputs differ
    bool        self_check       = false;     // --self-check: replay vs itself
};

void usage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s rom.ch8 --frames N [options]\n"
        "       %s --replay file.json [--print-hash] [--expect-hash 0xVAL]\n"
        "       %s --replay file.json --self-check\n"
        "       %s --diff-replay A.json B.json [--force]\n"
        "Options:\n"
        "  --frames N             Frames to run (required for ROM mode; > 0).\n"
        "  --cycles-per-frame N   CPU cycles per emulator frame (default 12).\n"
        "  --seed N               Deterministic PRNG seed (int64; default: boot entropy).\n"
        "  --isa NAME             chip8 | schip | mx8 (default: mx8).\n"
        "  --quirks PRESET        modern | legacy (default: modern).\n"
        "  --mx8                  Enable MX-8 extensions (gates 5XY1.. / FX50..).\n"
        "  --replay file.json     Replay mode: load ROM/ISA/quirks/seed/events from file.\n"
        "                         Frames default to one past the last event/checkpoint.\n"
        "                         Verifies any embedded checkpoint hashes; exits 1 on drift.\n"
        "  --self-check           With --replay, re-run replay and verify every embedded\n"
        "                         checkpoint matches. Detects drift in the implementation.\n"
        "  --diff-replay A B      Binary-search first frame where two replays diverge.\n"
        "                         Compares input streams first; refuses if they differ\n"
        "                         (use --force to diff anyway). Reports per-component\n"
        "                         digest deltas at divergence.\n"
        "  --force                With --diff-replay: proceed even if input streams differ.\n"
        "  --print-hash           Print final framebuffer hash to stdout (hex).\n"
        "  --print-state          Print PC/I/SP/halt to stdout.\n"
        "  --expect-hash 0xVAL    Assert final hash matches; exit 1 on mismatch.\n"
        , prog, prog, prog, prog);
}

bool readFile(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    auto size = static_cast<long long>(f.tellg());
    if (size <= 0) return false;
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    f.read(reinterpret_cast<char*>(out.data()), size);
    return true;
}

uint64_t parseU64(const char* s) {
    return std::strtoull(s, nullptr, 0);   // 0 = autodetect 0x / 0 / decimal
}

int64_t parseI64(const char* s) {
    return std::strtoll(s, nullptr, 0);
}

bool parseArgs(int argc, char** argv, Args& a) {
    if (argc < 2) return false;
    auto need = [&](int& i, const char* flag) -> const char* {
        if (i + 1 >= argc) {
            std::fprintf(stderr, "missing value for %s\n", flag);
            std::exit(3);
        }
        return argv[++i];
    };
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        if (s == "-h" || s == "--help") { usage(argv[0]); std::exit(0); }
        else if (s == "--frames")             { a.frames = std::atoi(need(i, "--frames")); a.have_frames_arg = true; }
        else if (s == "--cycles-per-frame")   a.cycles_per_frame = std::atoi(need(i, "--cycles-per-frame"));
        else if (s == "--seed")               a.seed             = parseI64(need(i, "--seed"));
        else if (s == "--isa")                a.isa_name         = need(i, "--isa");
        else if (s == "--quirks")             a.quirks_preset    = need(i, "--quirks");
        else if (s == "--mx8")                a.mx8_extensions   = true;
        else if (s == "--replay")             a.replay_path      = need(i, "--replay");
        else if (s == "--diff-replay") {
            a.diff_a = need(i, "--diff-replay");
            a.diff_b = need(i, "--diff-replay (second arg)");
        }
        else if (s == "--force")              a.diff_force       = true;
        else if (s == "--self-check")         a.self_check       = true;
        else if (s == "--print-hash")         a.print_hash       = true;
        else if (s == "--print-state")        a.print_state      = true;
        else if (s == "--expect-hash") {
            a.expected_hash = parseU64(need(i, "--expect-hash"));
            a.have_expected = true;
        }
        else if (!s.empty() && s[0] != '-' && a.rom_path.empty()) a.rom_path = s;
        else {
            std::fprintf(stderr, "unknown arg: %s\n", s.c_str());
            return false;
        }
    }

    // Mode validation: exactly one of {ROM, --replay, --diff-replay}.
    int mode_count = (!a.rom_path.empty()      ? 1 : 0)
                   + (!a.replay_path.empty()   ? 1 : 0)
                   + (!a.diff_a.empty()        ? 1 : 0);
    if (mode_count > 1) {
        std::fprintf(stderr,
            "modes are mutually exclusive: pick one of ROM / --replay / --diff-replay\n");
        return false;
    }
    if (mode_count == 0) return false;

    if (!a.rom_path.empty() && a.frames <= 0) return false;
    if (a.self_check && a.replay_path.empty()) {
        std::fprintf(stderr, "--self-check requires --replay\n");
        return false;
    }
    return true;
}

// --------- replay-execution helpers (shared by replay/diff/self-check) -----

// Set up a CPU according to a Replay's anchor configuration.
void setupCpuFromReplay(Chip8& cpu, const Replay& r) {
    cpu.installISA(&ISA::by_name(r.isa));
    cpu.quirks = r.quirks;
    cpu.loadROMBytes(r.rom_bytes);
    if (r.have_seed) cpu.setSeed(static_cast<uint64_t>(r.seed));
}

// Run a replay forward to `target_frame` (inclusive: returns state AFTER
// running frame target_frame's cycle batch). Cycles per frame fixed at 12
// to match the GUI App default — diff comparisons require both runs to
// use the same cadence anyway.
void runReplayTo(Chip8& cpu, const Replay& r, uint64_t target_frame, int cycles_per_frame) {
    std::size_t ev_idx = 0;
    for (uint64_t f = 0; f <= target_frame; ++f) {
        while (ev_idx < r.events.size() && r.events[ev_idx].frame == f) {
            cpu.enqueue(r.events[ev_idx].event);
            ++ev_idx;
        }
        cpu.drainEvents();
        for (int c = 0; c < cycles_per_frame; ++c) {
            cpu.cycle();
            if (cpu.halted()) return;
        }
        cpu.tickTimers();
    }
}

// Compute the digest of a replay at a specific frame.
Chip8::StateDigest digestAtFrame(const Replay& r, uint64_t target_frame, int cpf) {
    Chip8 cpu;
    setupCpuFromReplay(cpu, r);
    runReplayTo(cpu, r, target_frame, cpf);
    return cpu.stateDigest();
}

// Compare two CoreEvent variants for value equality. The events are
// pure data — no captures — so comparison is structural.
bool sameEvent(const CoreEvent& a, const CoreEvent& b) {
    if (a.index() != b.index()) return false;
    return std::visit([&](auto&& ea) -> bool {
        using T = std::decay_t<decltype(ea)>;
        const T& eb = std::get<T>(b);
        if constexpr (std::is_same_v<T, ResetEvent>)               return true;
        else if constexpr (std::is_same_v<T, ClearAllBreakpointsEvent>) return true;
        else if constexpr (std::is_same_v<T, ClearAllWatchpointsEvent>) return true;
        else if constexpr (std::is_same_v<T, InjectKeyEvent>)
            return ea.key == eb.key && ea.down == eb.down;
        else if constexpr (std::is_same_v<T, WriteMemoryEvent>)
            return ea.addr == eb.addr && ea.value == eb.value;
        else if constexpr (std::is_same_v<T, WriteMemoryBlockEvent>)
            return ea.addr == eb.addr && ea.bytes == eb.bytes;
        else if constexpr (std::is_same_v<T, ToggleBreakpointEvent>) return ea.addr == eb.addr;
        else if constexpr (std::is_same_v<T, SetWatchpointEvent>)
            return ea.addr == eb.addr && ea.kind == eb.kind && ea.erase == eb.erase;
        else if constexpr (std::is_same_v<T, SetPCEvent>)            return ea.pc == eb.pc;
        else if constexpr (std::is_same_v<T, SetSeedEvent>)          return ea.seed == eb.seed;
        else if constexpr (std::is_same_v<T, LoadROMEvent>)          return ea.bytes == eb.bytes;
        else                                                          return false;
    }, a);
}

// Find the first index where two replay-event vectors differ. Returns
// -1 if they're fully equal up to the shorter one and same length, or
// (size_t)min(len) if they're equal up to a length boundary but lengths
// differ.
struct InputDiff {
    bool        differ          = false;
    std::size_t first_diff_idx  = 0;
    bool        is_length_diff  = false;     // identical up to a point, then one is shorter
};
InputDiff diffInputs(const Replay& a, const Replay& b) {
    std::size_t n = std::min(a.events.size(), b.events.size());
    for (std::size_t i = 0; i < n; ++i) {
        if (a.events[i].frame != b.events[i].frame ||
            !sameEvent(a.events[i].event, b.events[i].event)) {
            return {true, i, false};
        }
    }
    if (a.events.size() != b.events.size()) {
        return {true, n, true};
    }
    return {false, 0, false};
}

void printEventBrief(const ReplayEvent& re) {
    std::fprintf(stderr, "    frame %llu: ", (unsigned long long)re.frame);
    std::visit([](auto&& e) {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, InjectKeyEvent>)
            std::fprintf(stderr, "InjectKey key=%X down=%d\n", e.key, (int)e.down);
        else if constexpr (std::is_same_v<T, WriteMemoryEvent>)
            std::fprintf(stderr, "WriteMemory addr=%03X value=%02X\n", e.addr, e.value);
        else if constexpr (std::is_same_v<T, WriteMemoryBlockEvent>)
            std::fprintf(stderr, "WriteMemoryBlock addr=%03X bytes=%zu\n", e.addr, e.bytes.size());
        else if constexpr (std::is_same_v<T, SetPCEvent>)
            std::fprintf(stderr, "SetPC pc=%03X\n", e.pc);
        else if constexpr (std::is_same_v<T, ResetEvent>)
            std::fprintf(stderr, "Reset\n");
        else if constexpr (std::is_same_v<T, SetSeedEvent>)
            std::fprintf(stderr, "SetSeed seed=%llu\n", (unsigned long long)e.seed);
        else
            std::fprintf(stderr, "<other>\n");
    }, re.event);
}

// Per-component delta report between two digests. Prints to stderr.
void reportDigestDelta(const Chip8::StateDigest& a,
                       const Chip8::StateDigest& b,
                       uint64_t frame) {
    std::fprintf(stderr, "  divergence at frame %llu:\n", (unsigned long long)frame);
    auto cmpHash = [&](const char* label, uint64_t va, uint64_t vb) {
        if (va != vb) std::fprintf(stderr,
            "    %-12s differs: 0x%016llX  vs  0x%016llX\n",
            label, (unsigned long long)va, (unsigned long long)vb);
    };
    auto cmpInt = [&](const char* label, long long va, long long vb) {
        if (va != vb) std::fprintf(stderr,
            "    %-12s differs: %lld  vs  %lld\n", label, va, vb);
    };
    cmpHash("framebuffer", a.framebuffer_hash, b.framebuffer_hash);
    cmpHash("memory",      a.mem_hash,         b.mem_hash);
    cmpHash("registers",   a.regs_hash,        b.regs_hash);
    cmpHash("stack",       a.stack_hash,       b.stack_hash);
    cmpHash("rng",         a.rng_hash,         b.rng_hash);
    cmpInt ("pc",          a.pc,               b.pc);
    cmpInt ("index",       a.index,            b.index);
    cmpInt ("sp",          a.sp,               b.sp);
    cmpInt ("delay_timer", a.delay_timer,      b.delay_timer);
    cmpInt ("sound_timer", a.sound_timer,      b.sound_timer);
    cmpInt ("hires",       a.hires?1:0,        b.hires?1:0);
    cmpInt ("halt_reason", a.halt_reason,      b.halt_reason);
}

// Binary-search the smallest frame F where digest(A, F) != digest(B, F).
// Bounds: [0, max_frame). Returns max_frame if no divergence found in
// the range. Cost: O(log(max_frame)) digest computations on each side.
uint64_t findDivergenceFrame(const Replay& a, const Replay& b,
                             uint64_t max_frame, int cpf) {
    // Linear precheck: equal at frame 0 already? If they differ before
    // any cycle runs, the source is the static configuration (ROM/quirks/
    // seed/ISA), not a runtime divergence.
    if (digestAtFrame(a, 0, cpf).state_hash !=
        digestAtFrame(b, 0, cpf).state_hash) {
        return 0;
    }
    // Equal at the end? Then no divergence in range.
    if (max_frame == 0) return max_frame;
    if (digestAtFrame(a, max_frame - 1, cpf).state_hash ==
        digestAtFrame(b, max_frame - 1, cpf).state_hash) {
        return max_frame;
    }
    // Binary search: invariant - frames < lo are all equal, frames >= hi
    // are known to diverge. Find the first divergent frame.
    uint64_t lo = 0;
    uint64_t hi = max_frame - 1;
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2;
        if (digestAtFrame(a, mid, cpf).state_hash ==
            digestAtFrame(b, mid, cpf).state_hash) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

int runDiffReplay(const Args& a) {
    Replay ra, rb;
    if (!ra.load(a.diff_a)) return 3;
    if (!rb.load(a.diff_b)) return 3;

    // Phase 1: input streams must match (or --force).
    auto in_diff = diffInputs(ra, rb);
    if (in_diff.differ) {
        if (in_diff.is_length_diff) {
            std::fprintf(stderr,
                "input streams differ in length: A has %zu events, B has %zu (matched first %zu)\n",
                ra.events.size(), rb.events.size(), in_diff.first_diff_idx);
        } else {
            std::fprintf(stderr, "input streams differ at event index %zu:\n",
                         in_diff.first_diff_idx);
            std::fprintf(stderr, "  A:\n"); printEventBrief(ra.events[in_diff.first_diff_idx]);
            std::fprintf(stderr, "  B:\n"); printEventBrief(rb.events[in_diff.first_diff_idx]);
        }
        if (!a.diff_force) {
            std::fprintf(stderr,
                "(input divergence detected — same inputs are required for a meaningful "
                "execution diff. Pass --force to diff anyway.)\n");
            return 1;
        }
        std::fprintf(stderr, "(--force: continuing diff despite input divergence)\n");
    } else {
        std::fprintf(stderr, "input streams: %zu events, identical\n", ra.events.size());
    }

    // Decide a comparison range — one past the latest frame referenced
    // by either replay. Caps at 1e6 to keep search bounded.
    uint64_t max_frame = 0;
    for (auto& e : ra.events)      if (e.frame      > max_frame) max_frame = e.frame;
    for (auto& e : rb.events)      if (e.frame      > max_frame) max_frame = e.frame;
    for (auto& cp : ra.checkpoints) if (cp.frame    > max_frame) max_frame = cp.frame;
    for (auto& cp : rb.checkpoints) if (cp.frame    > max_frame) max_frame = cp.frame;
    if (max_frame == 0) max_frame = 60;
    max_frame += 1;
    if (max_frame > 1'000'000) max_frame = 1'000'000;

    int cpf = 12;
    std::fprintf(stderr, "searching divergence in frames [0, %llu) ...\n",
                 (unsigned long long)max_frame);
    uint64_t f = findDivergenceFrame(ra, rb, max_frame, cpf);
    if (f >= max_frame) {
        std::fprintf(stderr, "no execution divergence in range\n");
        return 0;
    }

    auto da = digestAtFrame(ra, f, cpf);
    auto db = digestAtFrame(rb, f, cpf);
    reportDigestDelta(da, db, f);
    return 1;
}

int runSelfCheck(const Args& a) {
    Replay r;
    if (!r.load(a.replay_path)) return 3;

    if (r.checkpoints.empty()) {
        std::fprintf(stderr, "self-check: replay has no checkpoints to verify\n");
        return 0;
    }
    int failures = 0;
    for (const auto& cp : r.checkpoints) {
        Chip8 cpu;
        setupCpuFromReplay(cpu, r);
        runReplayTo(cpu, r, cp.frame, /*cpf=*/12);
        uint64_t got = cpu.framebufferHash();
        bool ok = (got == cp.hash);
        std::fprintf(stderr, "%s frame %llu: got 0x%016llX, expected 0x%016llX\n",
                     ok ? "ok  " : "FAIL",
                     (unsigned long long)cp.frame,
                     (unsigned long long)got,
                     (unsigned long long)cp.hash);
        if (!ok) ++failures;
    }
    if (failures) {
        std::fprintf(stderr, "%d self-check failure(s)\n", failures);
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    Args a;
    if (!parseArgs(argc, argv, a)) {
        usage(argv[0]);
        return 3;
    }

    // Diff-replay and self-check are standalone subcommands — dispatch
    // before falling into the ROM/replay execution path.
    if (!a.diff_a.empty()) return runDiffReplay(a);
    if (a.self_check)      return runSelfCheck(a);

    Chip8 cpu;
    Replay replay;
    bool replay_mode = !a.replay_path.empty();

    if (replay_mode) {
        if (!replay.load(a.replay_path)) return 3;
        cpu.installISA(&ISA::by_name(replay.isa));
        cpu.quirks = replay.quirks;
        if (!cpu.loadROMBytes(replay.rom_bytes)) {
            std::fprintf(stderr, "replay loadROMBytes failed (size=%zu)\n", replay.rom_bytes.size());
            return 3;
        }
        if (replay.have_seed) cpu.setSeed(static_cast<uint64_t>(replay.seed));
        // Default frames = one past last scheduled event/checkpoint.
        if (!a.have_frames_arg) {
            uint64_t last = 0;
            for (auto& e  : replay.events)      if (e.frame  > last) last = e.frame;
            for (auto& cp : replay.checkpoints) if (cp.frame > last) last = cp.frame;
            a.frames = static_cast<int>(last) + 1;
            if (a.frames < 1) a.frames = 1;
        }
    } else {
        std::vector<uint8_t> rom_bytes;
        if (!readFile(a.rom_path, rom_bytes)) {
            std::fprintf(stderr, "cannot read ROM: %s\n", a.rom_path.c_str());
            return 3;
        }
        cpu.installISA(&ISA::by_name(a.isa_name));
        cpu.quirks = (a.quirks_preset == "legacy") ? Chip8::legacyQuirks()
                                                   : Chip8::modernQuirks();
        cpu.quirks.mx8_extensions = a.mx8_extensions;
        if (!cpu.loadROMBytes(rom_bytes)) {
            std::fprintf(stderr, "loadROMBytes failed (size=%zu)\n", rom_bytes.size());
            return 3;
        }
        if (a.seed >= 0) cpu.setSeed(static_cast<uint64_t>(a.seed));
    }

    // Frame loop. Drain events at the same boundary the GUI App uses, so
    // headless and GUI execution are byte-identical for the same input.
    // Replay mode injects events at scheduled frames before the drain.
    int frames_run = 0;
    bool halted_early = false;
    int  checkpoint_failures = 0;
    size_t ev_idx = 0, cp_idx = 0;

    for (int f = 0; f < a.frames; ++f) {
        if (replay_mode) {
            while (ev_idx < replay.events.size() &&
                   replay.events[ev_idx].frame == (uint64_t)f) {
                cpu.enqueue(replay.events[ev_idx].event);
                ++ev_idx;
            }
        }
        cpu.drainEvents();
        for (int c = 0; c < a.cycles_per_frame; ++c) {
            cpu.cycle();
            if (cpu.halted()) { halted_early = true; break; }
        }
        cpu.tickTimers();
        ++frames_run;

        // Verify any checkpoint scheduled for this frame.
        if (replay_mode) {
            while (cp_idx < replay.checkpoints.size() &&
                   replay.checkpoints[cp_idx].frame == (uint64_t)f) {
                uint64_t got = cpu.framebufferHash();
                uint64_t want = replay.checkpoints[cp_idx].hash;
                if (got != want) {
                    std::fprintf(stderr,
                        "checkpoint MISMATCH at frame %d: got 0x%016llX, expected 0x%016llX\n",
                        f, (unsigned long long)got, (unsigned long long)want);
                    ++checkpoint_failures;
                }
                ++cp_idx;
            }
        }

        if (halted_early) break;
    }

    uint64_t hash = cpu.framebufferHash();

    if (a.print_state) {
        std::printf("frames=%d  pc=%03X  I=%03X  sp=%02X  halted=%s (%s)\n",
                    frames_run, cpu.pc, cpu.index, cpu.sp,
                    cpu.halted() ? "yes" : "no",
                    cpu.haltReasonString());
    }
    if (a.print_hash) {
        std::printf("hash=0x%016llX\n", static_cast<unsigned long long>(hash));
    }

    if (a.have_expected) {
        if (hash != a.expected_hash) {
            std::fprintf(stderr,
                "hash mismatch: got 0x%016llX, expected 0x%016llX\n",
                static_cast<unsigned long long>(hash),
                static_cast<unsigned long long>(a.expected_hash));
            return 1;
        }
    }

    if (checkpoint_failures > 0) {
        std::fprintf(stderr, "%d checkpoint failure(s) during replay\n", checkpoint_failures);
        return 1;
    }

    if (halted_early && !a.have_expected) {
        // Halt-without-expectation is informational, not failure. But if
        // the user asked for a specific hash and the run halted early,
        // the hash check above already fired (or matched).
        return 2;
    }

    return 0;
}
