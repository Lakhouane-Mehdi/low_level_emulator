#pragma once

#include "../core/Chip8.hpp"
#include "../app/Config.hpp"

#include <SFML/Graphics.hpp>

#include <array>

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

    // XO-CHIP four-color lookup, indexed by the per-pixel plane bitmask:
    //   0 = background (off), 1 = plane 0, 2 = plane 1, 3 = both planes.
    // For classic ROMs only indices 0 and 1 are ever produced, so the
    // display is identical to the two-color path. Rebuilt in setPalette.
    std::array<sf::Color, 4> plane_colors_{};
    void rebuildPlaneColors();

    static sf::Color toSfColor(uint32_t rgba);
};
