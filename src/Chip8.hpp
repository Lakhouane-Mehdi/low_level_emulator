#pragma once

#include <cstdint>
#include <string>

class Chip8 {
public:
    static constexpr int DISPLAY_WIDTH  = 64;
    static constexpr int DISPLAY_HEIGHT = 32;
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

    Chip8();

    bool loadROM(const std::string& path);
    void cycle();
    void tickTimers();

private:
    void executeOpcode(uint16_t opcode);
};
