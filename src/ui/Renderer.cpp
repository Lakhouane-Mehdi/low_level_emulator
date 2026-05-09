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
}

void Renderer::update(Chip8& cpu) {
    if (cpu.draw_flag) {
        const sf::Color on  = toSfColor(color_on_);
        const sf::Color off = toSfColor(color_off_);
        for (unsigned y = 0; y < Chip8::DISPLAY_HEIGHT; ++y) {
            for (unsigned x = 0; x < Chip8::DISPLAY_WIDTH; ++x) {
                uint32_t p = cpu.display[y * Chip8::DISPLAY_WIDTH + x];
                image_.setPixel({x, y}, p ? on : off);
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
