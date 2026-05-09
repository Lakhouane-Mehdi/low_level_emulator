#include "RomBrowser.hpp"

#include <algorithm>
#include <cctype>
#include <optional>

namespace fs = std::filesystem;

std::vector<fs::path> RomBrowser::listROMs(const fs::path& dir) {
    std::vector<fs::path> roms;
    std::error_code ec;
    if (!fs::exists(dir, ec)) return roms;
    for (const auto& entry : fs::recursive_directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext == ".ch8") roms.push_back(entry.path());
    }
    std::sort(roms.begin(), roms.end());
    return roms;
}

fs::path RomBrowser::pick(sf::RenderWindow& window, const sf::Font& font,
                          const std::vector<fs::path>& roms) {
    if (roms.empty()) return {};

    int selected = 0;
    int scroll = 0;
    const int rowHeight = 22;
    const int topMargin = 60;

    sf::Text title(font, "Select a ROM (Up/Down + Enter, or click)  Esc to cancel", 18);
    title.setFillColor(sf::Color::White);
    title.setPosition({20.f, 16.f});

    while (window.isOpen()) {
        const int viewport = std::max(1, (int(window.getSize().y) - topMargin - 16) / rowHeight);

        while (const std::optional event = window.pollEvent()) {
            if (event->is<sf::Event::Closed>()) {
                window.close();
                return {};
            } else if (const auto* kp = event->getIf<sf::Event::KeyPressed>()) {
                using K = sf::Keyboard::Key;
                if (kp->code == K::Escape) return {};
                if (kp->code == K::Up)    selected = (selected - 1 + (int)roms.size()) % (int)roms.size();
                if (kp->code == K::Down)  selected = (selected + 1) % (int)roms.size();
                if (kp->code == K::PageUp)   selected = std::max(0, selected - viewport);
                if (kp->code == K::PageDown) selected = std::min((int)roms.size() - 1, selected + viewport);
                if (kp->code == K::Home)  selected = 0;
                if (kp->code == K::End)   selected = (int)roms.size() - 1;
                if (kp->code == K::Enter) return roms[selected];
            } else if (const auto* mp = event->getIf<sf::Event::MouseButtonPressed>()) {
                if (mp->button == sf::Mouse::Button::Left) {
                    int idx = scroll + (mp->position.y - topMargin) / rowHeight;
                    if (idx >= 0 && idx < (int)roms.size()) return roms[idx];
                }
            } else if (const auto* sw = event->getIf<sf::Event::MouseWheelScrolled>()) {
                scroll = std::max(0, std::min((int)roms.size() - viewport,
                                              scroll - static_cast<int>(sw->delta * 3)));
            }
        }

        // keep selected in viewport
        if (selected < scroll) scroll = selected;
        if (selected >= scroll + viewport) scroll = selected - viewport + 1;
        scroll = std::max(0, std::min(std::max(0, (int)roms.size() - viewport), scroll));

        window.clear(sf::Color(15, 15, 25));
        window.draw(title);
        const int end = std::min((int)roms.size(), scroll + viewport);
        for (int i = scroll; i < end; ++i) {
            sf::Text item(font, roms[i].filename().string(), 16);
            float yy = topMargin + static_cast<float>(i - scroll) * rowHeight;
            item.setPosition({40.f, yy});
            item.setFillColor(i == selected ? sf::Color(255, 220, 80) : sf::Color(200, 200, 200));
            if (i == selected) {
                sf::Text marker(font, ">", 16);
                marker.setFillColor(sf::Color(255, 220, 80));
                marker.setPosition({20.f, yy});
                window.draw(marker);
            }
            window.draw(item);
        }
        // scroll indicator
        if ((int)roms.size() > viewport) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%d / %d", selected + 1, (int)roms.size());
            sf::Text counter(font, buf, 14);
            counter.setFillColor(sf::Color(120, 120, 140));
            counter.setPosition({20.f, 36.f});
            window.draw(counter);
        }
        window.display();
    }
    return {};
}
