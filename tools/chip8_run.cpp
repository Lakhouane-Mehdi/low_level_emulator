// chip8_run — headless CHIP-8 runner for CI, fuzzing, regression suites.
//
// Loads a ROM, runs for N frames at a configurable cycles-per-frame, and
// either prints the final framebuffer hash or asserts it matches a known
// value. No SFML, no UI, no audio. Exits:
//   0  success (or hash match if --expect-hash given)
//   1  hash mismatch
//   2  CPU halted before reaching the requested frame count
//   3  CLI / load error
//
// Usage:
//   chip8_run rom.ch8 --frames 600 --seed 42 --print-hash
//   chip8_run rom.ch8 --frames 600 --seed 42 --expect-hash 0x1234ABCD
//   chip8_run rom.ch8 --frames 60 --isa schip --quirks legacy

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
};

void usage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s rom.ch8 --frames N [options]\n"
        "       %s --replay file.json [--print-hash] [--expect-hash 0xVAL]\n"
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
        "  --print-hash           Print final framebuffer hash to stdout (hex).\n"
        "  --print-state          Print PC/I/SP/halt to stdout.\n"
        "  --expect-hash 0xVAL    Assert final hash matches; exit 1 on mismatch.\n"
        , prog, prog);
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

    // Replay mode XOR ROM mode.
    if (!a.replay_path.empty()) {
        if (!a.rom_path.empty()) {
            std::fprintf(stderr, "--replay is mutually exclusive with a positional ROM\n");
            return false;
        }
    } else if (a.rom_path.empty() || a.frames <= 0) {
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    Args a;
    if (!parseArgs(argc, argv, a)) {
        usage(argv[0]);
        return 3;
    }

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
