#include "Renderer.hpp"

Renderer::Renderer(const Config& cfg)
    : cfg_(cfg),
      image_({Chip8::DISPLAY_WIDTH, Chip8::DISPLAY_HEIGHT}, sf::Color::Black),
      texture_(sf::Vector2u{Chip8::DISPLAY_WIDTH, Chip8::DISPLAY_HEIGHT}),
      sprite_(texture_),
      color_on_(cfg.color_on),
      color_off_(cfg.color_off) {
    sprite_.setScale({static_cast<float>(cfg.lores_scale), static_cast<float>(cfg.lores_scale)});
    sprite_.setTextureRect(sf::IntRect({0, 0}, {Chip8::LORES_WIDTH, Chip8::LORES_HEIGHT}));
    rebuildPlaneColors();
}

sf::Color Renderer::toSfColor(uint32_t rgba) {
    return sf::Color(
        static_cast<uint8_t>((rgba >> 24) & 0xFF),
        static_cast<uint8_t>((rgba >> 16) & 0xFF),
        static_cast<uint8_t>((rgba >>  8) & 0xFF),
        static_cast<uint8_t>( rgba        & 0xFF));
}

void Renderer::setPalette(uint32_t on, uint32_t off) {
    color_on_  = on;
    color_off_ = off;
    rebuildPlaneColors();
}

void Renderer::rebuildPlaneColors() {
    const sf::Color off = toSfColor(color_off_);
    const sf::Color on  = toSfColor(color_on_);
    // Index = plane bitmask. Plane 1's color and the "both planes" color are
    // derived from the active palette so every palette gets a coherent
    // four-color ramp without a separate config surface:
    //   1 (plane 0)  -> foreground (on)
    //   2 (plane 1)  -> a 60% blend of on toward off (a dimmer foreground)
    //   3 (both)     -> a tint of on toward white (the brightest cell)
    auto blend = [](sf::Color a, sf::Color b, float t) {
        auto mix = [t](uint8_t x, uint8_t y) {
            return static_cast<uint8_t>(x + (y - x) * t);
        };
        return sf::Color(mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b), 255);
    };
    plane_colors_[0] = off;
    plane_colors_[1] = on;
    plane_colors_[2] = blend(on, off, 0.40f);
    plane_colors_[3] = blend(on, sf::Color::White, 0.50f);
}

void Renderer::update(Chip8& cpu) {
    if (cpu.draw_flag) {
        for (unsigned y = 0; y < Chip8::DISPLAY_HEIGHT; ++y) {
            for (unsigned x = 0; x < Chip8::DISPLAY_WIDTH; ++x) {
                // display[] holds a 2-bit plane bitmask (0..3). Classic ROMs
                // only ever produce 0 or 1, so this matches the old on/off
                // rendering exactly while giving XO-CHIP its four colors.
                uint32_t mask = cpu.display[y * Chip8::DISPLAY_WIDTH + x] & 0x3;
                image_.setPixel({x, y}, plane_colors_[mask]);
            }
        }
        (void)texture_.loadFromImage(image_);
        cpu.draw_flag = false;
    }

    if (cpu.hires != last_hires_) {
        last_hires_ = cpu.hires;
        if (cpu.hires) {
            sprite_.setTextureRect(sf::IntRect({0, 0}, {Chip8::DISPLAY_WIDTH, Chip8::DISPLAY_HEIGHT}));
            sprite_.setScale({static_cast<float>(cfg_.hires_scale), static_cast<float>(cfg_.hires_scale)});
        } else {
            sprite_.setTextureRect(sf::IntRect({0, 0}, {Chip8::LORES_WIDTH, Chip8::LORES_HEIGHT}));
            sprite_.setScale({static_cast<float>(cfg_.lores_scale), static_cast<float>(cfg_.lores_scale)});
        }
    }
}

void Renderer::draw(sf::RenderTarget& target) {
    target.draw(sprite_);
}
