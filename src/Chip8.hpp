#pragma once

#include <cstdint>
#include <string>

class Chip8 {
public:
    // CHIP-8 native: 64x32. SUPER-CHIP hi-res: 128x64. We always allocate
    // the larger buffer; in lo-res mode only the top-left 64x32 is used.
    static constexpr int DISPLAY_WIDTH  = 128;
    static constexpr int DISPLAY_HEIGHT = 64;
    static constexpr int LORES_WIDTH    = 64;
    static constexpr int LORES_HEIGHT   = 32;
    static constexpr int MEMORY_SIZE    = 4096;
    static constexpr int REGISTER_COUNT = 16;
    static constexpr int STACK_SIZE     = 16;
    static constexpr int KEY_COUNT      = 16;
    static constexpr uint16_t START_ADDRESS = 0x200;
    static constexpr uint16_t FONTSET_ADDRESS = 0x50;

    uint8_t  memory[MEMORY_SIZE]{};
    uint8_t  v[REGISTER_COUNT]{};
    uint16_t index{};
    uint16_t pc{};
    uint16_t stack[STACK_SIZE]{};
    uint8_t  sp{};
    uint8_t  delay_timer{};
    uint8_t  sound_timer{};
    uint8_t  keys[KEY_COUNT]{};
    uint32_t display[DISPLAY_WIDTH * DISPLAY_HEIGHT]{};
    bool     draw_flag{};
    bool     hires{};                   // SUPER-CHIP 128x64 mode
    bool     halted{};                  // 00FD exit
    uint8_t  rpl_flags[8]{};            // SUPER-CHIP user flags for FX75/FX85

    // Quirks: ambiguous opcodes behave differently across CHIP-8 implementations.
    // Modern defaults match SCHIP / most modern interpreters; legacy mode (false)
    // matches the original 1977 COSMAC VIP and is needed for some old ROMs.
    struct Quirks {
        bool shift_in_place = true;   // 8XY6/8XYE: true=shift Vx; false=Vx = Vy >> 1 (VIP)
        bool load_store_no_inc = true; // FX55/FX65: true=I unchanged; false=I += x+1 (VIP)
    } quirks;

    Chip8();

    bool loadROM(const std::string& path);
    void cycle();
    void tickTimers();

    // Snapshot: just the full mutable state. Memory is the largest piece
    // (4KB), so each snapshot is ~6KB - cheap enough for ring-buffer rewind.
    struct Snapshot {
        uint8_t  memory[MEMORY_SIZE];
        uint8_t  v[REGISTER_COUNT];
        uint16_t index;
        uint16_t pc;
        uint16_t stack[STACK_SIZE];
        uint8_t  sp;
        uint8_t  delay_timer;
        uint8_t  sound_timer;
        uint32_t display[DISPLAY_WIDTH * DISPLAY_HEIGHT];
        bool     hires;
    };
    Snapshot snapshot() const;
    void restore(const Snapshot& s);

private:
    void executeOpcode(uint16_t opcode);
};
