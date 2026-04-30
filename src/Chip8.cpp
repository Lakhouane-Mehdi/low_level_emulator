#include "Chip8.hpp"

#include <cstdio>
#include <cstdlib>
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

std::mt19937 rng{std::random_device{}()};
std::uniform_int_distribution<int> rand_byte(0, 255);
}

Chip8::Chip8() {
    pc = START_ADDRESS;
    for (int i = 0; i < 80; ++i) {
        memory[FONTSET_ADDRESS + i] = FONTSET[i];
    }
}

bool Chip8::loadROM(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        std::fprintf(stderr, "Failed to open ROM: %s\n", path.c_str());
        return false;
    }
    std::streamsize size = file.tellg();
    if (size <= 0 || size > (MEMORY_SIZE - START_ADDRESS)) {
        std::fprintf(stderr, "ROM size invalid: %lld bytes\n", (long long)size);
        return false;
    }
    file.seekg(0, std::ios::beg);
    file.read(reinterpret_cast<char*>(memory + START_ADDRESS), size);
    return true;
}

void Chip8::cycle() {
    uint16_t opcode = (memory[pc] << 8) | memory[pc + 1];
    pc += 2;
    executeOpcode(opcode);
}

void Chip8::tickTimers() {
    if (delay_timer > 0) --delay_timer;
    if (sound_timer > 0) --sound_timer;
}

void Chip8::executeOpcode(uint16_t opcode) {
    uint16_t nnn = opcode & 0x0FFF;
    uint8_t  nn  = opcode & 0x00FF;
    uint8_t  n   = opcode & 0x000F;
    uint8_t  x   = (opcode & 0x0F00) >> 8;
    uint8_t  y   = (opcode & 0x00F0) >> 4;

    switch (opcode & 0xF000) {
    case 0x0000:
        switch (nn) {
        case 0xE0: // CLS
            std::memset(display, 0, sizeof(display));
            draw_flag = true;
            break;
        case 0xEE: // RET
            --sp;
            pc = stack[sp];
            break;
        default:
            std::fprintf(stderr, "Unknown 0x0 opcode: 0x%04X\n", opcode);
            break;
        }
        break;

    case 0x1000: // JP addr
        pc = nnn;
        break;

    case 0x2000: // CALL addr
        stack[sp] = pc;
        ++sp;
        pc = nnn;
        break;

    case 0x3000: // SE Vx, byte
        if (v[x] == nn) pc += 2;
        break;

    case 0x4000: // SNE Vx, byte
        if (v[x] != nn) pc += 2;
        break;

    case 0x5000: // SE Vx, Vy
        if (v[x] == v[y]) pc += 2;
        break;

    case 0x6000: // LD Vx, byte
        v[x] = nn;
        break;

    case 0x7000: // ADD Vx, byte
        v[x] += nn;
        break;

    case 0x8000:
        switch (n) {
        case 0x0: v[x] = v[y]; break;
        case 0x1: v[x] |= v[y]; break;
        case 0x2: v[x] &= v[y]; break;
        case 0x3: v[x] ^= v[y]; break;
        case 0x4: {
            uint16_t sum = v[x] + v[y];
            v[0xF] = sum > 0xFF ? 1 : 0;
            v[x] = sum & 0xFF;
            break;
        }
        case 0x5: {
            uint8_t flag = v[x] >= v[y] ? 1 : 0;
            v[x] = v[x] - v[y];
            v[0xF] = flag;
            break;
        }
        case 0x6: {
            uint8_t flag = v[x] & 0x1;
            v[x] >>= 1;
            v[0xF] = flag;
            break;
        }
        case 0x7: {
            uint8_t flag = v[y] >= v[x] ? 1 : 0;
            v[x] = v[y] - v[x];
            v[0xF] = flag;
            break;
        }
        case 0xE: {
            uint8_t flag = (v[x] & 0x80) >> 7;
            v[x] <<= 1;
            v[0xF] = flag;
            break;
        }
        default:
            std::fprintf(stderr, "Unknown 0x8 opcode: 0x%04X\n", opcode);
            break;
        }
        break;

    case 0x9000: // SNE Vx, Vy
        if (v[x] != v[y]) pc += 2;
        break;

    case 0xA000: // LD I, addr
        index = nnn;
        break;

    case 0xB000: // JP V0, addr
        pc = nnn + v[0];
        break;

    case 0xC000: // RND Vx, byte
        v[x] = static_cast<uint8_t>(rand_byte(rng)) & nn;
        break;

    case 0xD000: {
        // DRW Vx, Vy, N - the heart of CHIP-8 graphics.
        //
        // Reads N bytes from memory starting at I. Each byte is one row of an
        // 8-pixel-wide sprite (MSB = leftmost pixel). The sprite is XORed onto
        // the screen at (Vx, Vy). If any lit pixel gets turned off by the XOR,
        // VF is set to 1 (collision); otherwise VF = 0.
        //
        // Why XOR? It makes "erase" free: drawing the same sprite twice at the
        // same spot wipes it. That's how every CHIP-8 game animates - draw
        // sprite at old pos to erase, then draw at new pos.
        //
        // Why a collision flag? Pong knows the ball hit the paddle because
        // drawing the ball-sprite over the paddle-sprite turns a pixel off,
        // which sets VF=1. The game polls VF and reverses the ball direction.
        //
        // Coordinates wrap MODULO the screen at the *start position* (% below)
        // but clip (don't wrap) for pixels that fall off the right/bottom edge
        // - that's the standard COSMAC VIP behavior most ROMs expect.
        uint8_t px = v[x] % DISPLAY_WIDTH;
        uint8_t py = v[y] % DISPLAY_HEIGHT;
        v[0xF] = 0;
        for (int row = 0; row < n; ++row) {
            if (py + row >= DISPLAY_HEIGHT) break;     // clip bottom
            uint8_t sprite = memory[index + row];      // one 8-pixel row
            for (int col = 0; col < 8; ++col) {
                if (px + col >= DISPLAY_WIDTH) break;  // clip right
                // Test bit (col) of sprite, MSB first: 0x80 >> col
                if ((sprite & (0x80 >> col)) != 0) {
                    uint32_t* pixel = &display[(py + row) * DISPLAY_WIDTH + (px + col)];
                    if (*pixel == 0xFFFFFFFF) v[0xF] = 1; // pixel was on -> collision
                    *pixel ^= 0xFFFFFFFF;                 // XOR toggle
                }
            }
        }
        draw_flag = true;
        break;
    }

    case 0xE000:
        switch (nn) {
        case 0x9E: // SKP Vx
            if (keys[v[x] & 0xF]) pc += 2;
            break;
        case 0xA1: // SKNP Vx
            if (!keys[v[x] & 0xF]) pc += 2;
            break;
        default:
            std::fprintf(stderr, "Unknown 0xE opcode: 0x%04X\n", opcode);
            break;
        }
        break;

    case 0xF000:
        switch (nn) {
        case 0x07: v[x] = delay_timer; break;
        case 0x0A: { // LD Vx, K - wait for key press
            bool pressed = false;
            for (uint8_t i = 0; i < KEY_COUNT; ++i) {
                if (keys[i]) {
                    v[x] = i;
                    pressed = true;
                    break;
                }
            }
            if (!pressed) pc -= 2; // re-execute next cycle
            break;
        }
        case 0x15: delay_timer = v[x]; break;
        case 0x18: sound_timer = v[x]; break;
        case 0x1E: index += v[x]; break;
        case 0x29: index = FONTSET_ADDRESS + (v[x] & 0xF) * 5; break;
        case 0x33: // BCD
            memory[index]     = v[x] / 100;
            memory[index + 1] = (v[x] / 10) % 10;
            memory[index + 2] = v[x] % 10;
            break;
        case 0x55: // LD [I], Vx
            for (int i = 0; i <= x; ++i) memory[index + i] = v[i];
            break;
        case 0x65: // LD Vx, [I]
            for (int i = 0; i <= x; ++i) v[i] = memory[index + i];
            break;
        default:
            std::fprintf(stderr, "Unknown 0xF opcode: 0x%04X\n", opcode);
            break;
        }
        break;

    default:
        std::fprintf(stderr, "Unknown opcode: 0x%04X\n", opcode);
        break;
    }
}
