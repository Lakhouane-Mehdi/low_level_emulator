#pragma once

#include <SFML/Graphics.hpp>
#include <filesystem>
#include <vector>

// Modal ROM picker. Lists every .ch8 file in `dir` recursively and lets the
// user pick one with arrow keys + Enter, or by clicking. Returns empty path
// if the user closes the window or hits Escape.
class RomBrowser {
public:
    static std::vector<std::filesystem::path> listROMs(const std::filesystem::path& dir);
    static std::filesystem::path pick(sf::RenderWindow& window, const sf::Font& font,
                                      const std::vector<std::filesystem::path>& roms);
};
