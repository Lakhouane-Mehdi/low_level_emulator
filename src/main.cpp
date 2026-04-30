#include "Chip8.hpp"
#include "Disassembler.hpp"

#include <SFML/Graphics.hpp>
#include <SFML/Audio.hpp>
#include <SFML/Window.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

// Game window is fixed at 768x384. In lo-res (64x32) each CHIP-8 pixel is 12x12;
// in hi-res (128x64) each pixel is 6x6. Sprite scale flips at runtime.
constexpr int LORES_SCALE = 12;
constexpr int HIRES_SCALE = 6;
constexpr int CYCLES_PER_FRAME = 10; // ~600Hz CPU at 60fps
constexpr int REWIND_FRAMES = 300;   // 5 seconds of history at 60fps

constexpr int GAME_W = Chip8::LORES_WIDTH * LORES_SCALE;   // 768
constexpr int GAME_H = Chip8::LORES_HEIGHT * LORES_SCALE;  // 384
constexpr int SIDEBAR_W = 480;
constexpr int WINDOW_W = GAME_W + SIDEBAR_W;               // 1248
constexpr int WINDOW_H = GAME_H;                            // 384

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

// Lists .ch8 files in a directory, sorted alphabetically.
std::vector<std::filesystem::path> listROMs(const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> roms;
    if (!std::filesystem::exists(dir)) return roms;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext == ".ch8") roms.push_back(entry.path());
    }
    std::sort(roms.begin(), roms.end());
    return roms;
}

// Modal ROM-picker. Returns the selected path, or empty if window closed.
std::filesystem::path pickROM(sf::RenderWindow& window, const sf::Font& font,
                              const std::vector<std::filesystem::path>& roms) {
    int selected = 0;
    sf::Text title(font, "Select a ROM (Up/Down + Enter, or click)", 18);
    title.setFillColor(sf::Color::White);
    title.setPosition({20.f, 16.f});

    while (window.isOpen()) {
        while (const std::optional event = window.pollEvent()) {
            if (event->is<sf::Event::Closed>()) {
                window.close();
                return {};
            } else if (const auto* kp = event->getIf<sf::Event::KeyPressed>()) {
                using K = sf::Keyboard::Key;
                if (kp->code == K::Escape) { window.close(); return {}; }
                if (kp->code == K::Up)   selected = (selected - 1 + (int)roms.size()) % (int)roms.size();
                if (kp->code == K::Down) selected = (selected + 1) % (int)roms.size();
                if (kp->code == K::Enter && !roms.empty()) return roms[selected];
            } else if (const auto* mp = event->getIf<sf::Event::MouseButtonPressed>()) {
                if (mp->button == sf::Mouse::Button::Left) {
                    int idx = (mp->position.y - 60) / 28;
                    if (idx >= 0 && idx < (int)roms.size()) return roms[idx];
                }
            }
        }

        window.clear(sf::Color(15, 15, 25));
        window.draw(title);
        for (size_t i = 0; i < roms.size(); ++i) {
            sf::Text item(font, roms[i].filename().string(), 18);
            item.setPosition({40.f, 60.f + (float)i * 28.f});
            item.setFillColor((int)i == selected ? sf::Color(255, 220, 80) : sf::Color(200, 200, 200));
            if ((int)i == selected) {
                sf::Text marker(font, ">", 18);
                marker.setFillColor(sf::Color(255, 220, 80));
                marker.setPosition({20.f, 60.f + (float)i * 28.f});
                window.draw(marker);
            }
            window.draw(item);
        }
        window.display();
    }
    return {};
}

// Builds the multi-line debugger string from CPU state.
std::string buildDebugText(const Chip8& cpu, bool paused) {
    std::ostringstream s;
    s << (paused ? "[PAUSED] Space=run  N=step" : "[RUNNING] Space=pause") << "\n";
    s << "Esc=quit  F1=quirks (" << (cpu.quirks.shift_in_place ? "MODERN" : "LEGACY") << ")  "
      << (cpu.hires ? "HIRES" : "LORES") << (cpu.halted ? " HALTED" : "") << "\n";
    s << "F5=save  F9=load  Backspace=rewind\n\n";

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
    for (int i = 0; i < 5; ++i) {
        uint16_t addr = cpu.pc + i * 2;
        if (addr + 1 >= Chip8::MEMORY_SIZE) break;
        uint16_t op = (cpu.memory[addr] << 8) | cpu.memory[addr + 1];
        std::snprintf(buf, sizeof(buf), "%s%03X: %04X  ", (i == 0 ? "> " : "  "), addr, op);
        s << buf << disassemble(op) << "\n";
    }

    s << "\nStack (SP=" << (int)cpu.sp << "):\n";
    for (int i = 0; i < Chip8::STACK_SIZE; ++i) {
        std::snprintf(buf, sizeof(buf), "%s[%X]=%03X  ",
                      (i == cpu.sp ? ">" : " "), i, cpu.stack[i]);
        s << buf;
        if (i % 4 == 3) s << "\n";
    }

    // Memory window: 32 bytes around I (the most interesting region during DXYN/BCD)
    s << "\nMemory @ I=" << std::uppercase;
    std::snprintf(buf, sizeof(buf), "%03X:\n", cpu.index); s << buf;
    int base = (cpu.index / 16) * 16;     // align to 16
    base = std::max(0, base - 16);         // show one row before I
    for (int row = 0; row < 2; ++row) {
        int addr = base + row * 16;
        if (addr >= Chip8::MEMORY_SIZE) break;
        std::snprintf(buf, sizeof(buf), "%03X: ", addr); s << buf;
        for (int col = 0; col < 16 && addr + col < Chip8::MEMORY_SIZE; ++col) {
            const char* mark = (addr + col == cpu.index) ? "[" : " ";
            std::snprintf(buf, sizeof(buf), "%s%02X", mark, cpu.memory[addr + col]);
            s << buf;
        }
        s << "\n";
    }
    return s.str();
}

int main(int argc, char* argv[]) {
    sf::RenderWindow window(
        sf::VideoMode({static_cast<unsigned>(WINDOW_W), static_cast<unsigned>(WINDOW_H)}),
        "CHIP-8 Emulator + Debugger"
    );
    window.setFramerateLimit(60);

    sf::Font font;
    if (!font.openFromFile("C:/Windows/Fonts/consola.ttf")) {
        std::fprintf(stderr, "Failed to load font C:/Windows/Fonts/consola.ttf\n");
        return 1;
    }

    std::filesystem::path romPath;
    if (argc >= 2) {
        romPath = argv[1];
    } else {
        auto roms = listROMs("roms");
        if (roms.empty()) {
            std::fprintf(stderr, "No ROMs found in roms/ and no path given.\n");
            return 1;
        }
        romPath = pickROM(window, font, roms);
        if (romPath.empty()) return 0;
    }

    Chip8 cpu;
    if (!cpu.loadROM(romPath.string())) return 1;
    window.setTitle("CHIP-8: " + romPath.filename().string());

    sf::Image image({Chip8::DISPLAY_WIDTH, Chip8::DISPLAY_HEIGHT}, sf::Color::Black);
    sf::Texture texture(sf::Vector2u{Chip8::DISPLAY_WIDTH, Chip8::DISPLAY_HEIGHT});
    sf::Sprite sprite(texture);
    sprite.setScale({static_cast<float>(LORES_SCALE), static_cast<float>(LORES_SCALE)});
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
    std::optional<Chip8::Snapshot> savedState;
    std::deque<Chip8::Snapshot> rewindBuffer;
    bool rewinding = false;
    std::string statusMsg;
    int statusFrames = 0;

    while (window.isOpen()) {
        while (const std::optional event = window.pollEvent()) {
            if (event->is<sf::Event::Closed>()) {
                window.close();
            } else if (const auto* kp = event->getIf<sf::Event::KeyPressed>()) {
                using K = sf::Keyboard::Key;
                if (kp->code == K::Escape) window.close();
                else if (kp->code == K::Space) paused = !paused;
                else if (kp->code == K::N && paused) stepOnce = true;
                else if (kp->code == K::F1) {
                    cpu.quirks.shift_in_place = !cpu.quirks.shift_in_place;
                    cpu.quirks.load_store_no_inc = cpu.quirks.shift_in_place;
                }
                else if (kp->code == K::F5) {
                    savedState = cpu.snapshot();
                    statusMsg = "State saved";
                    statusFrames = 60;
                }
                else if (kp->code == K::F9) {
                    if (savedState) {
                        cpu.restore(*savedState);
                        statusMsg = "State loaded";
                    } else {
                        statusMsg = "No save state";
                    }
                    statusFrames = 60;
                }
                else {
                    int k = mapKey(kp->code);
                    if (k >= 0) cpu.keys[k] = 1;
                }
            } else if (const auto* kr = event->getIf<sf::Event::KeyReleased>()) {
                int k = mapKey(kr->code);
                if (k >= 0) cpu.keys[k] = 0;
            }
        }

        rewinding = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Backspace);

        if (rewinding && !rewindBuffer.empty()) {
            cpu.restore(rewindBuffer.back());
            rewindBuffer.pop_back();
        } else if (!paused) {
            // Snapshot before advancing so the buffer holds the *previous* state
            rewindBuffer.push_back(cpu.snapshot());
            if ((int)rewindBuffer.size() > REWIND_FRAMES) rewindBuffer.pop_front();
            for (int i = 0; i < CYCLES_PER_FRAME; ++i) cpu.cycle();
            cpu.tickTimers();
        } else if (stepOnce) {
            rewindBuffer.push_back(cpu.snapshot());
            if ((int)rewindBuffer.size() > REWIND_FRAMES) rewindBuffer.pop_front();
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
        // Hi-res uses the whole 128x64 texture at 6x scale; lo-res only uses
        // the top-left 64x32 region but we want it to fill the same 768x384
        // game area, so we scale that subregion by 12x via texture rect.
        if (cpu.hires) {
            sprite.setTextureRect(sf::IntRect({0, 0},
                {Chip8::DISPLAY_WIDTH, Chip8::DISPLAY_HEIGHT}));
            sprite.setScale({static_cast<float>(HIRES_SCALE), static_cast<float>(HIRES_SCALE)});
        } else {
            sprite.setTextureRect(sf::IntRect({0, 0},
                {Chip8::LORES_WIDTH, Chip8::LORES_HEIGHT}));
            sprite.setScale({static_cast<float>(LORES_SCALE), static_cast<float>(LORES_SCALE)});
        }

        if (cpu.sound_timer > 0 && !paused && !rewinding) {
            if (beep.getStatus() != sf::Sound::Status::Playing) beep.play();
        } else {
            if (beep.getStatus() == sf::Sound::Status::Playing) beep.stop();
        }

        std::string txt = buildDebugText(cpu, paused);
        if (rewinding) txt += "\n[REWIND] " + std::to_string(rewindBuffer.size()) + " frames left";
        if (statusFrames > 0) { txt += "\n" + statusMsg; --statusFrames; }
        debugText.setString(txt);

        window.clear(sf::Color::Black);
        window.draw(sprite);
        window.draw(sidebarBg);
        window.draw(debugText);
        window.display();
    }

    return 0;
}
