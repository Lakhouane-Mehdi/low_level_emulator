#pragma once

#include "../core/Chip8.hpp"

#include <SFML/Graphics.hpp>
#include <string>

// Renders the right-hand sidebar: status, registers, disassembly with
// breakpoint markers, stack, memory window, and the instruction trace tail.
class DebugView {
public:
    enum class MemPane { NearI, NearPC, Cursor };

    DebugView(const sf::Font& font, float x, float y, float width, float height);

    // ---- input the App calls into ----
    void toggleBreakpointAtPC(Chip8& cpu);
    void toggleBreakpointAt(Chip8& cpu, uint16_t addr);
    void cycleMemPane();
    void moveCursor(int delta);
    void editByteAtCursor(Chip8& cpu, int delta);     // +/-1 in mem editor
    uint16_t cursor() const { return mem_cursor_; }

    // Cycle the watchpoint at the cursor through: none -> R -> W -> RW -> none.
    void cycleWatchpointAtCursor(Chip8& cpu);
    void clearAllWatchpoints(Chip8& cpu);

    void setStatusMessage(const std::string& msg, int frames = 90);

    // Build & draw.
    void draw(sf::RenderTarget& target, const Chip8& cpu, bool paused, bool rewinding,
              size_t rewindFrames, int cyclesPerFrame);

private:
    sf::RectangleShape bg_;
    sf::Text           text_;
    float              left_;

    MemPane            pane_ = MemPane::NearI;
    uint16_t           mem_cursor_ = 0x200;

    std::string        status_;
    int                status_frames_ = 0;

    std::string buildText(const Chip8& cpu, bool paused, bool rewinding,
                          size_t rewindFrames, int cyclesPerFrame);
    static std::string fmtQuirks(const Chip8::Quirks& q);
};
