#pragma once

#include <SFML/Graphics/Font.hpp>
#include <optional>

// Loads a monospace font, trying a list of common system locations
// (Windows / Linux / macOS) plus the bundled assets/fonts/ folder.
// Returns the path that succeeded so the caller can log it.
std::optional<std::string> loadMonoFont(sf::Font& font);
