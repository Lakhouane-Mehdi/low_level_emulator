#include "Chip8.hpp"
#include "isa/IInstructionSet.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>

namespace {

constexpr uint8_t FONTSET[80] = {
    0xF0, 0x90, 0x90, 0x90, 0xF0, // 0
    0x20, 0x60, 0x20, 0x20, 0x70, // 1
    0xF0, 0x10, 0xF0, 0x80, 0xF0, // 2
    0xF0, 0x10, 0xF0, 0x10, 0xF0, // 3
    0x90, 0x90, 0xF0, 0x10, 0x10, // 4
    0xF0, 0x80, 0xF0, 0x10, 0xF0, // 5
    0xF0, 0x80, 0xF0, 0x90, 0xF0, // 6
    0xF0, 0x10, 0x20, 0x40, 0x40, // 7
    0xF0, 0x90, 0xF0, 0x90, 0xF0, // 8
    0xF0, 0x90, 0xF0, 0x10, 0xF0, // 9
    0xF0, 0x90, 0xF0, 0x90, 0x90, // A
    0xE0, 0x90, 0xE0, 0x90, 0xE0, // B
    0xF0, 0x80, 0x80, 0x80, 0xF0, // C
    0xE0, 0x90, 0x90, 0x90, 0xE0, // D
    0xF0, 0x80, 0xF0, 0x80, 0xF0, // E
    0xF0, 0x80, 0xF0, 0x80, 0x80, // F
};

// SUPER-CHIP big font: 10 bytes per digit, 8x10 pixels, digits 0-9.
constexpr uint8_t BIG_FONTSET[100] = {
    0x3C,0x7E,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0x7E,0x3C, // 0
    0x18,0x38,0x58,0x18,0x18,0x18,0x18,0x18,0x18,0x3C, // 1
    0x3E,0x7F,0xC3,0x06,0x0C,0x18,0x30,0x60,0xFF,0xFF, // 2
    0x3C,0x7E,0xC3,0x03,0x0E,0x0E,0x03,0xC3,0x7E,0x3C, // 3
    0x06,0x0E,0x1E,0x36,0x66,0xC6,0xFF,0xFF,0x06,0x06, // 4
    0xFF,0xFF,0xC0,0xC0,0xFC,0xFE,0x03,0xC3,0x7E,0x3C, // 5
    0x3E,0x7C,0xC0,0xC0,0xFC,0xFE,0xC3,0xC3,0x7E,0x3C, // 6
    0xFF,0xFF,0x03,0x06,0x0C,0x18,0x30,0x60,0x60,0x60, // 7
    0x3C,0x7E,0xC3,0xC3,0x7E,0x7E,0xC3,0xC3,0x7E,0x3C, // 8
    0x3C,0x7E,0xC3,0xC3,0x7F,0x3F,0x03,0x03,0x3E,0x7C, // 9
};

constexpr uint32_t PIXEL_ON  = 0xFFFFFFFF;
constexpr uint32_t PIXEL_OFF = 0x00000000;

// Boot-time entropy for the CPU's PRNG. Mixes std::random_device with the
// monotonic clock — random_device alone can be a constant on some MinGW
// builds, and the clock alone is too easy to collide on warm reboots.
uint64_t bootEntropy() {
    std::random_device rd;
    uint64_t a = (uint64_t(rd()) << 32) | rd();
    uint64_t b = static_cast<uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    return a ^ (b * 0x9E3779B97F4A7C15ULL);
}

} // namespace

Chip8::Quirks Chip8::modernQuirks() {
    Quirks q;
    q.shift_in_place     = true;
    q.load_store_no_inc  = true;
    q.jump_with_offset_x = true;
    q.vf_reset           = false;
    q.display_wait       = false;
    q.clip_sprites       = true;
    return q;
}

Chip8::Quirks Chip8::legacyQuirks() {
    Quirks q;
    q.shift_in_place     = false;
    q.load_store_no_inc  = false;
    q.jump_with_offset_x = false;
    q.vf_reset           = true;
    q.display_wait       = true;
    q.clip_sprites       = true;
    return q;
}

Chip8::Chip8() {
    // Wire memory's bad-access channel into the CPU's halt path so an OOR
    // read/write turns into a clean BadMemoryAccess halt instead of a
    // silent corruption.
    mem.set_bad_access_callback([this](Memory::FaultKind, uint16_t) {
        halt(HaltReason::BadMemoryAccess);
    });
    // Seed PRNG once at construction. reset() does NOT reseed — that would
    // make every reload deterministic in a way users don't expect. To get
    // a deterministic stream call setSeed() (--seed CLI / FX70 / tests).
    setSeed(bootEntropy());
    // Default to MX-8 (the broadest ISA) so all opcodes decode in the
    // disassembler. Whether MX-8 OPCODES execute depends on
    // quirks.mx8_extensions, not the ISA — see Mx8ISA::execute.
    isa_ = &ISA::mx8();
    reset();
}

void Chip8::installISA(IInstructionSet* isa) {
    if (isa) isa_ = isa;
}

// ---- event queue ----------------------------------------------------
//
// Drain order is FIFO. Each event is applied to completion before the
// next is examined — applyEvent() can mutate memory (firing watchpoints),
// halt the CPU, or otherwise have side effects. Subsequent events still
// drain even if the CPU just halted; that matches user intent (you might
// enqueue [Reset, LoadROM] together to recover from a bad halt).

void Chip8::enqueue(CoreEvent ev) {
    events_.push_back(std::move(ev));
}

void Chip8::drainEvents() {
    while (!events_.empty()) {
        CoreEvent ev = std::move(events_.front());
        events_.pop_front();
        std::visit([this](auto&& e) { applyEvent(e); }, ev);
        ++total_events_applied_;
    }
}

void Chip8::applyEvent(const ResetEvent&) {
    reset();
}

void Chip8::applyEvent(const LoadROMEvent& ev) {
    (void)loadROMBytes(ev.bytes);
}

void Chip8::applyEvent(const WriteMemoryEvent& ev) {
    // Goes through the Memory API, so a watchpoint at this address fires
    // exactly as if the CPU had written here itself. Bounds-checked.
    mem.write(ev.addr, ev.value);
}

void Chip8::applyEvent(const WriteMemoryBlockEvent& ev) {
    if (ev.bytes.empty()) return;
    mem.write_block(ev.addr, ev.bytes.data(), ev.bytes.size());
}

void Chip8::applyEvent(const ToggleBreakpointEvent& ev) {
    auto it = breakpoints.find(ev.addr);
    if (it == breakpoints.end()) breakpoints.insert(ev.addr);
    else                         breakpoints.erase(it);
}

void Chip8::applyEvent(const ClearAllBreakpointsEvent&) {
    breakpoints.clear();
}

void Chip8::applyEvent(const SetWatchpointEvent& ev) {
    if (ev.erase) mem.remove_watchpoint(ev.addr);
    else          mem.add_watchpoint(ev.addr, ev.kind);
}

void Chip8::applyEvent(const ClearAllWatchpointsEvent&) {
    mem.clear_watchpoints();
}

void Chip8::applyEvent(const SetPCEvent& ev) {
    pc = ev.pc;
    hit_breakpoint = false;   // moving PC clears any pending breakpoint pause
}

void Chip8::applyEvent(const InjectKeyEvent& ev) {
    if (ev.key < KEY_COUNT) keys[ev.key] = ev.down ? 1 : 0;
}

void Chip8::applyEvent(const SetSeedEvent& ev) {
    setSeed(ev.seed);
}

void Chip8::pushStack(uint16_t addr) {
    if (sp >= STACK_SIZE) { halt(HaltReason::StackOverflow); return; }
    stack[sp++] = addr;
}

uint16_t Chip8::popStack() {
    if (sp == 0) { halt(HaltReason::StackUnderflow); return 0; }
    return stack[--sp];
}

void Chip8::setSeed(uint64_t seed) {
    rng.seed(seed);
}

void Chip8::reset() {
    mem.clear();
    v.fill(0);
    stack.fill(0);
    keys.fill(0);
    display.fill(PIXEL_OFF);
    rpl_flags.fill(0);
    pc = START_ADDRESS;
    index = 0;
    sp = 0;
    delay_timer = 0;
    sound_timer = 0;
    draw_flag = true;
    hires = false;
    waiting_vblank = false;
    halt_reason = HaltReason::Running;
    hit_breakpoint = false;
    total_cycles = 0;
    last_pc = 0;
    last_opcode = 0;
    trace.clear();
    mem.write_block(FONTSET_ADDRESS,  FONTSET,     sizeof(FONTSET));
    mem.write_block(BIG_FONT_ADDRESS, BIG_FONTSET, sizeof(BIG_FONTSET));
}

bool Chip8::loadROM(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        std::fprintf(stderr, "loadROM: cannot open '%s'\n", path.c_str());
        return false;
    }
    std::streamsize size = file.tellg();
    if (size <= 0 || size > (MEMORY_SIZE - START_ADDRESS)) {
        std::fprintf(stderr, "loadROM: invalid size %lld\n", (long long)size);
        return false;
    }
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    file.read(reinterpret_cast<char*>(bytes.data()), size);
    return loadROMBytes(bytes);
}

bool Chip8::loadROMBytes(const std::vector<uint8_t>& bytes) {
    if (bytes.empty() || bytes.size() > (MEMORY_SIZE - START_ADDRESS)) return false;
    reset();
    std::size_t wrote = mem.write_block(START_ADDRESS, bytes.data(), bytes.size());
    return wrote == bytes.size();
}

uint16_t Chip8::fetchOpcode(uint16_t addr) const {
    // Opcode fetch for the disassembler / debug view: const, non-watching.
    if (addr + 1 >= MEMORY_SIZE) return 0;
    return static_cast<uint16_t>((mem.peek(addr) << 8) | mem.peek(addr + 1));
}

uint64_t Chip8::framebufferHash() const {
    // FNV-1a 64. Reduce each pixel to one bit (on/off) before hashing so
    // the hash is renderer-palette-independent: changing color_on/off
    // doesn't change the hash, only changing pixel content does.
    constexpr uint64_t FNV_OFFSET = 0xCBF29CE484222325ULL;
    constexpr uint64_t FNV_PRIME  = 0x100000001B3ULL;
    uint64_t h = FNV_OFFSET;
    const int w = hires ? DISPLAY_WIDTH  : LORES_WIDTH;
    const int h_ = hires ? DISPLAY_HEIGHT : LORES_HEIGHT;
    for (int y = 0; y < h_; ++y) {
        // Pack 8 pixels per byte to keep the hash stable across pixel-format
        // changes and to make hex dumps of the hash material readable.
        for (int xb = 0; xb < w; xb += 8) {
            uint8_t bits = 0;
            for (int b = 0; b < 8 && xb + b < w; ++b) {
                if (display[y * DISPLAY_WIDTH + xb + b] != 0) {
                    bits |= static_cast<uint8_t>(1u << (7 - b));
                }
            }
            h ^= bits;
            h *= FNV_PRIME;
        }
    }
    // Mix in mode bit so a blank lo-res screen and blank hi-res screen
    // hash differently (they're semantically different states).
    h ^= hires ? 1ULL : 0ULL;
    h *= FNV_PRIME;
    return h;
}

void Chip8::halt(HaltReason r) {
    halt_reason = r;
}

const char* Chip8::haltReasonString() const {
    switch (halt_reason) {
    case HaltReason::Running:         return "running";
    case HaltReason::ExitOpcode:      return "EXIT (00FD)";
    case HaltReason::UnknownOpcode:   return "unknown opcode";
    case HaltReason::StackOverflow:   return "stack overflow";
    case HaltReason::StackUnderflow:  return "stack underflow";
    case HaltReason::BadMemoryAccess: return "bad memory access";
    }
    return "?";
}

void Chip8::recordTrace(uint16_t tpc, uint16_t op) {
    if (trace.size() >= TRACE_CAPACITY) trace.pop_front();
    trace.push_back({tpc, op});
}

void Chip8::cycle() {
    if (halted() || waiting_vblank) return;

    // Breakpoint check — fire BEFORE executing so the user sees PC at the bp.
    if (!hit_breakpoint && breakpoints.count(pc)) {
        hit_breakpoint = true;
        return;
    }
    hit_breakpoint = false;

    if (pc + 1 >= MEMORY_SIZE) { halt(HaltReason::BadMemoryAccess); return; }

    // Live execution fetch goes through Memory::read so an instruction-byte
    // watchpoint fires the moment that byte is dispatched. Disassembler
    // previews use Chip8::fetchOpcode (peek-based) so they don't trigger.
    uint16_t opcode = static_cast<uint16_t>((mem.read(pc) << 8) | mem.read(pc + 1));
    last_pc = pc;
    last_opcode = opcode;
    recordTrace(pc, opcode);
    pc += 2;
    ++total_cycles;

    // Dispatch via the active instruction set. ISA mutates state through
    // the public CPU API only — see installISA.
    isa_->execute(*this, opcode);
}

void Chip8::tickTimers() {
    if (delay_timer > 0) --delay_timer;
    if (sound_timer > 0) --sound_timer;
    waiting_vblank = false;
}

// ---------- DXYN: the heart of CHIP-8 graphics ----------
//
// Promoted to a public method so any ISA can render sprites without
// reimplementing framebuffer manipulation. Honors quirks.clip_sprites and
// quirks.display_wait, sets VF on collision, draws SCHIP 16x16 sprites
// when n==0 in hires mode.
void Chip8::drawSprite(uint8_t vx, uint8_t vy, uint8_t n) {
    const int w = hires ? DISPLAY_WIDTH  : LORES_WIDTH;
    const int h = hires ? DISPLAY_HEIGHT : LORES_HEIGHT;
    const uint8_t startX = v[vx] % w;
    const uint8_t startY = v[vy] % h;
    v[0xF] = 0;

    const bool wide = (hires && n == 0);   // SUPER-CHIP 16x16 sprite
    const int rows = wide ? 16 : n;
    const int cols = wide ? 16 : 8;

    for (int row = 0; row < rows; ++row) {
        int py = startY + row;
        if (quirks.clip_sprites) { if (py >= h) break; }
        else py %= h;

        uint16_t spriteRow;
        uint16_t addr0 = wide ? (index + row * 2) : (index + row);
        if (addr0 + (wide ? 1u : 0u) >= MEMORY_SIZE) {
            halt(HaltReason::BadMemoryAccess); return;
        }
        if (wide) {
            uint8_t hi = mem.read(addr0);
            uint8_t lo = mem.read(addr0 + 1);
            spriteRow = static_cast<uint16_t>((hi << 8) | lo);
        } else {
            spriteRow = mem.read(addr0);
        }

        const uint16_t mask = wide ? 0x8000 : 0x80;
        for (int col = 0; col < cols; ++col) {
            int px = startX + col;
            if (quirks.clip_sprites) { if (px >= w) break; }
            else px %= w;
            if ((spriteRow & (mask >> col)) == 0) continue;

            uint32_t* pixel = &display[py * DISPLAY_WIDTH + px];
            if (*pixel == PIXEL_ON) v[0xF] = 1;
            *pixel ^= PIXEL_ON;
        }
    }
    draw_flag = true;
    if (quirks.display_wait && !hires) waiting_vblank = true;
}

// ---------- display helpers ----------
void Chip8::clearScreen() {
    display.fill(PIXEL_OFF);
    draw_flag = true;
}

void Chip8::scrollDown(int lines) {
    const int w = hires ? DISPLAY_WIDTH  : LORES_WIDTH;
    const int h = hires ? DISPLAY_HEIGHT : LORES_HEIGHT;
    for (int yy = h - 1; yy >= 0; --yy) {
        for (int xx = 0; xx < w; ++xx) {
            display[yy * DISPLAY_WIDTH + xx] =
                (yy - lines >= 0) ? display[(yy - lines) * DISPLAY_WIDTH + xx] : PIXEL_OFF;
        }
    }
    draw_flag = true;
}

void Chip8::scrollUp(int lines) {
    const int w = hires ? DISPLAY_WIDTH  : LORES_WIDTH;
    const int h = hires ? DISPLAY_HEIGHT : LORES_HEIGHT;
    for (int yy = 0; yy < h; ++yy) {
        for (int xx = 0; xx < w; ++xx) {
            display[yy * DISPLAY_WIDTH + xx] =
                (yy + lines < h) ? display[(yy + lines) * DISPLAY_WIDTH + xx] : PIXEL_OFF;
        }
    }
    draw_flag = true;
}

void Chip8::scrollRight() {
    const int w = hires ? DISPLAY_WIDTH  : LORES_WIDTH;
    const int h = hires ? DISPLAY_HEIGHT : LORES_HEIGHT;
    for (int yy = 0; yy < h; ++yy) {
        for (int xx = w - 1; xx >= 0; --xx) {
            display[yy * DISPLAY_WIDTH + xx] =
                (xx - 4 >= 0) ? display[yy * DISPLAY_WIDTH + (xx - 4)] : PIXEL_OFF;
        }
    }
    draw_flag = true;
}

void Chip8::scrollLeft() {
    const int w = hires ? DISPLAY_WIDTH  : LORES_WIDTH;
    const int h = hires ? DISPLAY_HEIGHT : LORES_HEIGHT;
    for (int yy = 0; yy < h; ++yy) {
        for (int xx = 0; xx < w; ++xx) {
            display[yy * DISPLAY_WIDTH + xx] =
                (xx + 4 < w) ? display[yy * DISPLAY_WIDTH + (xx + 4)] : PIXEL_OFF;
        }
    }
    draw_flag = true;
}

// ---------- snapshots ----------
Chip8::Snapshot Chip8::snapshot() const {
    Snapshot s;
    mem.snapshot_to(s.memory);   // bulk save without touching watchpoints
    s.v = v;
    s.stack = stack;
    s.display = display;
    s.rpl_flags = rpl_flags;
    s.pc = pc;
    s.index = index;
    s.sp = sp;
    s.delay_timer = delay_timer;
    s.sound_timer = sound_timer;
    s.hires = hires;
    s.waiting_vblank = waiting_vblank;
    s.halt_reason = halt_reason;
    s.quirks = quirks;
    s.rng = rng;          // v3: PRNG state
    s.keys = keys;        // v3: keypad latch — restore() in mid-keyhold rewind
    return s;
}

void Chip8::restore(const Snapshot& s) {
    if (s.magic != Snapshot::MAGIC) return;
    mem.restore_from(s.memory);   // bulk restore without firing watchpoints
    v = s.v;
    stack = s.stack;
    display = s.display;
    rpl_flags = s.rpl_flags;
    pc = s.pc;
    index = s.index;
    sp = s.sp;
    delay_timer = s.delay_timer;
    sound_timer = s.sound_timer;
    hires = s.hires;
    waiting_vblank = s.waiting_vblank;
    halt_reason = s.halt_reason;
    quirks = s.quirks;
    // v3 snapshots carry RNG + keypad. Older snapshots (v2) predate
    // deterministic replay; leave those fields at their current values.
    if (s.version >= 3) {
        rng  = s.rng;
        keys = s.keys;
    }
    draw_flag = true;
    hit_breakpoint = false;
    // Drop any pending watchpoint event — that signal belonged to the
    // pre-restore timeline. The restored memory may or may not retrigger
    // the watchpoint as execution continues; that's fine and intentional.
    if (mem.watch_triggered()) (void)mem.consume_watch_event();
}
