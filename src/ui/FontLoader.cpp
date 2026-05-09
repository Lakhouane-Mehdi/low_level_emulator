#include "FontLoader.hpp"

#include <filesystem>
#include <vector>

std::optional<std::string> loadMonoFont(sf::Font& font) {
    namespace fs = std::filesystem;
    const std::vector<std::string> candidates = {
        // Bundled (placed next to the binary by CMake).
        "assets/fonts/mono.ttf",
        "../assets/fonts/mono.ttf",
        "../../assets/fonts/mono.ttf",
        // Windows
        "C:/Windows/Fonts/consola.ttf",
        "C:/Windows/Fonts/CascadiaMono.ttf",
        "C:/Windows/Fonts/cour.ttf",
        // macOS
        "/System/Library/Fonts/Menlo.ttc",
        "/Library/Fonts/Courier New.ttf",
        // Linux (DejaVu / Liberation are installed almost everywhere)
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
    };
    for (const auto& path : candidates) {
        std::error_code ec;
        if (fs::exists(path, ec) && font.openFromFile(path)) {
            return path;
        }
    }
    return std::nullopt;
}
