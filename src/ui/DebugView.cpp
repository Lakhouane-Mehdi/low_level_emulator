#include "DebugView.hpp"
#include "../core/CoreEvents.hpp"
#include "../core/Disassembler.hpp"

#include <algorithm>
#include <cstdio>
#include <sstream>

DebugView::DebugView(const sf::Font& font, float x, float y, float width, float height)
    : bg_({width, height}),
      text_(font, "", 14),
      left_(x) {
    bg_.setPosition({x, 0.f});
    bg_.setFillColor(sf::Color(20, 20, 30));
    text_.setFillColor(sf::Color(220, 220, 220));
    text_.setPosition({x + 12.f, y});
}

void DebugView::toggleBreakpointAtPC(Chip8& cpu) {
    toggleBreakpointAt(cpu, cpu.pc);
}

void DebugView::toggleBreakpointAt(Chip8& cpu, uint16_t addr) {
    // Status text reads the *current* breakpoint set (pre-toggle) since
    // the actual toggle applies on the next drainEvents() call.
    bool was_set = cpu.breakpoints.count(addr) > 0;
    cpu.enqueue(ToggleBreakpointEvent{addr});
    char buf[48];
    std::snprintf(buf, sizeof(buf),
                  was_set ? "Breakpoint cleared @ %03X" : "Breakpoint set @ %03X", addr);
    setStatusMessage(buf);
}

void DebugView::cycleMemPane() {
    pane_ = static_cast<MemPane>((static_cast<int>(pane_) + 1) % 3);
    const char* names[] = {"near I", "near PC", "cursor"};
    std::string s = "Memory pane: ";
    s += names[static_cast<int>(pane_)];
    setStatusMessage(s);
}

void DebugView::moveCursor(int delta) {
    int next = static_cast<int>(mem_cursor_) + delta;
    next = std::max(0, std::min(Chip8::MEMORY_SIZE - 1, next));
    mem_cursor_ = static_cast<uint16_t>(next);
}

void DebugView::cycleWatchpointAtCursor(Chip8& cpu) {
    // Cycle: none -> R -> W -> RW -> none. Decisions made against current
    // state; the actual mutation applies on next drainEvents().
    using WK = Memory::WatchKind;
    char buf[64];
    if (!cpu.mem.has_watchpoint(mem_cursor_)) {
        cpu.enqueue(SetWatchpointEvent{mem_cursor_, WK::Read, false});
        std::snprintf(buf, sizeof(buf), "Watch READ @ %03X", mem_cursor_);
    } else {
        WK cur = cpu.mem.watchpoints().at(mem_cursor_);
        if (cur == WK::Read) {
            cpu.enqueue(SetWatchpointEvent{mem_cursor_, WK::Write, false});
            std::snprintf(buf, sizeof(buf), "Watch WRITE @ %03X", mem_cursor_);
        } else if (cur == WK::Write) {
            cpu.enqueue(SetWatchpointEvent{mem_cursor_, WK::Both, false});
            std::snprintf(buf, sizeof(buf), "Watch RW @ %03X", mem_cursor_);
        } else {
            cpu.enqueue(SetWatchpointEvent{mem_cursor_, WK::Read, true});
            std::snprintf(buf, sizeof(buf), "Watch cleared @ %03X", mem_cursor_);
        }
    }
    setStatusMessage(buf);
}

void DebugView::clearAllWatchpoints(Chip8& cpu) {
    cpu.enqueue(ClearAllWatchpointsEvent{});
    setStatusMessage("All watchpoints cleared");
}

void DebugView::editByteAtCursor(Chip8& cpu, int delta) {
    // Mid-execution memory editor: enqueue a WriteMemory event. The next
    // drainEvents() call (typically next frame) applies the write through
    // the Memory API, firing any watchpoint at the cursor.
    uint8_t cur  = cpu.mem.peek(mem_cursor_);
    uint8_t next = static_cast<uint8_t>(cur + delta);
    cpu.enqueue(WriteMemoryEvent{mem_cursor_, next});
    char buf[48];
    std::snprintf(buf, sizeof(buf), "mem[%03X]: %02X -> %02X (queued)",
                  mem_cursor_, cur, next);
    setStatusMessage(buf);
}

void DebugView::setStatusMessage(const std::string& msg, int frames) {
    status_ = msg;
    status_frames_ = frames;
}

std::string DebugView::fmtQuirks(const Chip8::Quirks& q) {
    std::string s;
    s += q.shift_in_place     ? "SHFT-IN " : "shft-vy ";
    s += q.load_store_no_inc  ? "LDST-NOI " : "ldst-inc ";
    s += q.jump_with_offset_x ? "JMP-VX "  : "jmp-v0 ";
    s += q.vf_reset           ? "vfrst "   : "";
    s += q.display_wait       ? "vblank "  : "";
    s += q.clip_sprites       ? "clip"     : "wrap";
    if (q.mx8_extensions) s += " MX8";
    return s;
}

std::string DebugView::buildText(const Chip8& cpu, bool paused, bool rewinding,
                                 size_t rewindFrames, int cyclesPerFrame) {
    std::ostringstream s;
    char buf[64];

    s << (paused ? "[PAUSED]  " : "[RUN]     ");
    if (cpu.halted())  s << "HALTED: " << cpu.haltReasonString() << "  ";
    if (rewinding)     s << "[REWIND " << rewindFrames << "]  ";
    s << (cpu.hires ? "HIRES" : "LORES") << "\n";

    s << "Space=run/pause  N=step  O=step-over  Enter=step-out\n";
    s << "B=bp at PC  W=watch cursor  Shift+B/W=clear all  F1..F7 quirks  F8/F11/F12 sets\n";
    s << "F5=save  F9=load  Backspace=rewind  M=mempane  ";
    s << "Arrows=cursor  [/]=edit byte\n\n";

    s << "Speed: " << cyclesPerFrame << " cyc/frame  cycles=" << cpu.total_cycles << "\n";
    s << "Quirks: " << fmtQuirks(cpu.quirks) << "\n\n";

    std::snprintf(buf, sizeof(buf), "PC: %03X   I: %03X   SP: %02X\n", cpu.pc, cpu.index, cpu.sp);
    s << buf;
    std::snprintf(buf, sizeof(buf), "DT: %02X    ST: %02X\n\n", cpu.delay_timer, cpu.sound_timer);
    s << buf;

    s << "Registers:\n";
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            int r = row * 4 + col;
            std::snprintf(buf, sizeof(buf), "V%X=%02X  ", r, cpu.v[r]);
            s << buf;
        }
        s << "\n";
    }

    s << "\nNext instructions:\n";
    for (int i = 0; i < 6; ++i) {
        uint16_t addr = cpu.pc + i * 2;
        if (addr + 1 >= Chip8::MEMORY_SIZE) break;
        uint16_t op = cpu.fetchOpcode(addr);
        const char* mark = (i == 0) ? "> " : "  ";
        const char* bp   = cpu.breakpoints.count(addr) ? "*" : " ";
        std::snprintf(buf, sizeof(buf), "%s%s%03X: %04X  ", mark, bp, addr, op);
        s << buf << disassemble(op) << "\n";
    }

    s << "\nStack (SP=" << (int)cpu.sp << "):";
    if (cpu.sp == 0) s << " (empty)";
    s << "\n";
    for (int i = 0; i < Chip8::STACK_SIZE; ++i) {
        std::snprintf(buf, sizeof(buf), "%s[%X]=%03X  ",
                      (i == cpu.sp ? ">" : " "), i, cpu.stack[i]);
        s << buf;
        if (i % 4 == 3) s << "\n";
    }

    // Memory pane — three modes selected by the user.
    uint16_t pivot = cpu.index;
    const char* paneName = "I";
    if (pane_ == MemPane::NearPC)   { pivot = cpu.pc;   paneName = "PC"; }
    if (pane_ == MemPane::Cursor)   { pivot = mem_cursor_; paneName = "cur"; }
    std::snprintf(buf, sizeof(buf), "\nMemory @ %s=%03X:\n", paneName, pivot);
    s << buf;
    int base = std::max(0, (pivot / 16) * 16 - 16);
    for (int row = 0; row < 3; ++row) {
        int addr = base + row * 16;
        if (addr >= Chip8::MEMORY_SIZE) break;
        std::snprintf(buf, sizeof(buf), "%03X: ", addr); s << buf;
        for (int col = 0; col < 16 && addr + col < Chip8::MEMORY_SIZE; ++col) {
            const char* mark = " ";
            int a = addr + col;
            // Cursor / PC win — they're the user's active focus.
            if (pane_ == MemPane::Cursor && a == mem_cursor_) mark = "[";
            else if (pane_ == MemPane::NearPC && a == cpu.pc) mark = ">";
            // Watchpoint sigil takes the next priority slot, ahead of `{`.
            else if (cpu.mem.has_watchpoint(static_cast<uint16_t>(a))) {
                using WK = Memory::WatchKind;
                WK k = cpu.mem.watchpoints().at(static_cast<uint16_t>(a));
                mark = (k == WK::Read) ? "r" : (k == WK::Write) ? "w" : "*";
            }
            else if (a == cpu.index) mark = "{";
            // peek() — debugger inspection must not fire watchpoints.
            std::snprintf(buf, sizeof(buf), "%s%02X", mark, cpu.mem.peek(static_cast<uint16_t>(a)));
            s << buf;
        }
        s << "\n";
    }

    if (!cpu.breakpoints.empty()) {
        s << "\nBreakpoints (" << cpu.breakpoints.size() << "):";
        int n = 0;
        for (auto bp_addr : cpu.breakpoints) {
            std::snprintf(buf, sizeof(buf), " %03X", bp_addr);
            s << buf;
            if (++n >= 6) { s << " ..."; break; }
        }
        s << "\n";
    }

    if (!cpu.mem.watchpoints().empty()) {
        const auto& wps = cpu.mem.watchpoints();
        s << "\nWatchpoints (" << wps.size() << "):";
        int n = 0;
        for (const auto& [waddr, wkind] : wps) {
            using WK = Memory::WatchKind;
            const char* tag = (wkind == WK::Read) ? "r" : (wkind == WK::Write) ? "w" : "*";
            std::snprintf(buf, sizeof(buf), " %s%03X", tag, waddr);
            s << buf;
            if (++n >= 6) { s << " ..."; break; }
        }
        s << "\n";
    }

    s << "\nTrace (last " << cpu.trace.size() << "):\n";
    int show = std::min<int>(5, static_cast<int>(cpu.trace.size()));
    for (int i = static_cast<int>(cpu.trace.size()) - show;
         i < static_cast<int>(cpu.trace.size()); ++i) {
        const auto& e = cpu.trace[i];
        std::snprintf(buf, sizeof(buf), "  %03X: %04X  ", e.pc, e.opcode);
        s << buf << disassemble(e.opcode) << "\n";
    }

    if (status_frames_ > 0) {
        s << "\n>> " << status_;
    }
    s << "\n\n-- Made by Mehdi Lakhouane --";
    return s.str();
}

void DebugView::draw(sf::RenderTarget& target, const Chip8& cpu, bool paused, bool rewinding,
                     size_t rewindFrames, int cyclesPerFrame) {
    text_.setString(buildText(cpu, paused, rewinding, rewindFrames, cyclesPerFrame));
    target.draw(bg_);
    target.draw(text_);
    if (status_frames_ > 0) --status_frames_;
}
