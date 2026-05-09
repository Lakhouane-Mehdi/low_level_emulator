#pragma once

#include "../core/Chip8.hpp"
#include "../app/Config.hpp"

#include <SFML/Graphics.hpp>

// Maps the CHIP-8 framebuffer to an SFML sprite.
// Owns the texture/image; recreates them on demand when the palette changes.
class Renderer {
public:
    Renderer(const Config& cfg);

    // Pulls cpu.display into the texture (only when cpu.draw_flag is set).
    // Adjusts scale + texture rect for lo-res vs hi-res.
    void update(Chip8& cpu);

    // Draws the framebuffer into the game area of the window.
    void draw(sf::RenderTarget& target);

    void setPalette(uint32_t on, uint32_t off);

private:
    const Config& cfg_;
    sf::Image     image_;
    sf::Texture   texture_;
    sf::Sprite    sprite_;
    uint32_t      color_on_;
    uint32_t      color_off_;
    bool          last_hires_ = false;

    static sf::Color toSfColor(uint32_t rgba);
};
