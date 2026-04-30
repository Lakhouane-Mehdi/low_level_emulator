#include "Chip8.hpp"
#include "Disassembler.hpp"

#include <SFML/Graphics.hpp>
#include <SFML/Audio.hpp>
#include <SFML/Window.hpp>

#include <cmath>
#include <cstdio>
#include <optional>
#include <sstream>
#include <vector>

constexpr int SCALE = 12;
constexpr int CYCLES_PER_FRAME = 10; // ~600Hz CPU at 60fps

constexpr int GAME_W = Chip8::DISPLAY_WIDTH * SCALE;   // 768
constexpr int GAME_H = Chip8::DISPLAY_HEIGHT * SCALE;  // 384
constexpr int SIDEBAR_W = 352;
constexpr int WINDOW_W = GAME_W + SIDEBAR_W;           // 1120
constexpr int WINDOW_H = GAME_H;                        // 384

// Maps physical keys to CHIP-8 keypad:
//  1 2 3 C        1 2 3 4
//  4 5 6 D   -->  Q W E R
//  7 8 9 E        A S D F
//  A 0 B F        Z X C V
int mapKey(sf::Keyboard::Key k) {
    using K = sf::Keyboard::Key;
    switch (k) {
    case K::Num1: return 0x1;
    case K::Num2: return 0x2;
    case K::Num3: return 0x3;
    case K::Num4: return 0xC;
    case K::Q:    return 0x4;
    case K::W:    return 0x5;
    case K::E:    return 0x6;
    case K::R:    return 0xD;
    case K::A:    return 0x7;
    case K::S:    return 0x8;
    case K::D:    return 0x9;
    case K::F:    return 0xE;
    case K::Z:    return 0xA;
    case K::X:    return 0x0;
    case K::C:    return 0xB;
    case K::V:    return 0xF;
    default: return -1;
    }
}

sf::SoundBuffer makeBeepBuffer() {
    const unsigned sampleRate = 44100;
    const double freq = 440.0;
    const double duration = 0.05;
    std::vector<std::int16_t> samples(static_cast<size_t>(sampleRate * duration));
    for (size_t i = 0; i < samples.size(); ++i) {
        double t = static_cast<double>(i) / sampleRate;
        samples[i] = (std::sin(2 * 3.14159265 * freq * t) > 0 ? 6000 : -6000);
    }
    sf::SoundBuffer buf;
    if (!buf.loadFromSamples(samples.data(), samples.size(), 1, sampleRate,
                             {sf::SoundChannel::Mono})) {
        std::fprintf(stderr, "Failed to load beep samples\n");
    }
    return buf;
}

// Builds the multi-line debugger string from CPU state.
std::string buildDebugText(const Chip8& cpu, bool paused) {
    std::ostringstream s;
    s << (paused ? "[PAUSED] Space=run  N=step" : "[RUNNING] Space=pause") << "\n";
    s << "Esc=quit\n\n";

    s << "PC: " << std::uppercase;
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%03X", cpu.pc);   s << buf << "  ";
    std::snprintf(buf, sizeof(buf), "%03X", cpu.index); s << "I: " << buf << "\n";
    std::snprintf(buf, sizeof(buf), "%02X", cpu.sp);   s << "SP: " << buf << "  ";
    std::snprintf(buf, sizeof(buf), "%02X", cpu.delay_timer); s << "DT: " << buf << "  ";
    std::snprintf(buf, sizeof(buf), "%02X", cpu.sound_timer); s << "ST: " << buf << "\n\n";

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
        uint16_t op = (cpu.memory[addr] << 8) | cpu.memory[addr + 1];
        std::snprintf(buf, sizeof(buf), "%s%03X: %04X  ", (i == 0 ? "> " : "  "), addr, op);
        s << buf << disassemble(op) << "\n";
    }
    return s.str();
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::fprintf(stderr, "Usage: %s <rom.ch8>\n", argv[0]);
        return 1;
    }

    Chip8 cpu;
    if (!cpu.loadROM(argv[1])) return 1;

    sf::RenderWindow window(
        sf::VideoMode({static_cast<unsigned>(WINDOW_W), static_cast<unsigned>(WINDOW_H)}),
        "CHIP-8 Emulator + Debugger"
    );
    window.setFramerateLimit(60);

    sf::Image image({Chip8::DISPLAY_WIDTH, Chip8::DISPLAY_HEIGHT}, sf::Color::Black);
    sf::Texture texture(sf::Vector2u{Chip8::DISPLAY_WIDTH, Chip8::DISPLAY_HEIGHT});
    sf::Sprite sprite(texture);
    sprite.setScale({static_cast<float>(SCALE), static_cast<float>(SCALE)});

    // Load Consolas for the debugger sidebar
    sf::Font font;
    if (!font.openFromFile("C:/Windows/Fonts/consola.ttf")) {
        std::fprintf(stderr, "Failed to load font C:/Windows/Fonts/consola.ttf\n");
        return 1;
    }
    sf::Text debugText(font, "", 14);
    debugText.setFillColor(sf::Color(220, 220, 220));
    debugText.setPosition({static_cast<float>(GAME_W) + 12.f, 8.f});

    // Sidebar background panel
    sf::RectangleShape sidebarBg({static_cast<float>(SIDEBAR_W), static_cast<float>(WINDOW_H)});
    sidebarBg.setPosition({static_cast<float>(GAME_W), 0.f});
    sidebarBg.setFillColor(sf::Color(20, 20, 30));

    sf::SoundBuffer beepBuf = makeBeepBuffer();
    sf::Sound beep(beepBuf);
    beep.setLooping(true);

    bool paused = false;
    bool stepOnce = false;

    while (window.isOpen()) {
        while (const std::optional event = window.pollEvent()) {
            if (event->is<sf::Event::Closed>()) {
                window.close();
            } else if (const auto* kp = event->getIf<sf::Event::KeyPressed>()) {
                using K = sf::Keyboard::Key;
                if (kp->code == K::Escape) window.close();
                else if (kp->code == K::Space) paused = !paused;
                else if (kp->code == K::N && paused) stepOnce = true;
                else {
                    int k = mapKey(kp->code);
                    if (k >= 0) cpu.keys[k] = 1;
                }
            } else if (const auto* kr = event->getIf<sf::Event::KeyReleased>()) {
                int k = mapKey(kr->code);
                if (k >= 0) cpu.keys[k] = 0;
            }
        }

        if (!paused) {
            for (int i = 0; i < CYCLES_PER_FRAME; ++i) cpu.cycle();
            cpu.tickTimers();
        } else if (stepOnce) {
            cpu.cycle();
            stepOnce = false;
        }

        if (cpu.draw_flag) {
            for (unsigned y = 0; y < Chip8::DISPLAY_HEIGHT; ++y) {
                for (unsigned x = 0; x < Chip8::DISPLAY_WIDTH; ++x) {
                    uint32_t p = cpu.display[y * Chip8::DISPLAY_WIDTH + x];
                    image.setPixel({x, y}, p ? sf::Color::White : sf::Color::Black);
                }
            }
            (void)texture.loadFromImage(image);
            cpu.draw_flag = false;
        }

        if (cpu.sound_timer > 0 && !paused) {
            if (beep.getStatus() != sf::Sound::Status::Playing) beep.play();
        } else {
            if (beep.getStatus() == sf::Sound::Status::Playing) beep.stop();
        }

        debugText.setString(buildDebugText(cpu, paused));

        window.clear(sf::Color::Black);
        window.draw(sprite);
        window.draw(sidebarBg);
        window.draw(debugText);
        window.display();
    }

    return 0;
}
