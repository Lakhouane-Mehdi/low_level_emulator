#include "PauseMenu.hpp"

#include <algorithm>
#include <cstdio>

const std::vector<std::string> PauseMenu::kPaletteNames = {
    "mono", "amber", "green", "gameboy", "c64", "ice", "hotdog"
};
const std::vector<std::string> PauseMenu::kQuirksPresetNames = {
    "Modern (SCHIP)", "Legacy (VIP)"
};

PauseMenu::PauseMenu(const sf::Font& font, Config& cfg, Chip8::Quirks& quirks)
    : font_(font), cfg_(cfg), quirks_(quirks) {
    // Seed palette_idx_ from the Config's current colors. If it's not a
    // built-in palette, fall back to "mono" so the cycle works.
    for (size_t i = 0; i < kPaletteNames.size(); ++i) {
        uint32_t on = 0, off = 0;
        Config::applyPalette(kPaletteNames[i], on, off);
        if (on == cfg_.color_on && off == cfg_.color_off) {
            palette_idx_ = static_cast<int>(i);
            break;
        }
    }
    quirks_preset_ = cfg_.legacy_quirks ? 1 : 0;
    buildItems();
}

void PauseMenu::buildItems() {
    items_.clear();
    items_.push_back({"Resume",            "Esc",   ItemAction{Result::Resume}});
    items_.push_back({"Step instruction",  "N",     ItemAction{Result::Step}});
    items_.push_back({"Save state",        "F5",    ItemAction{Result::SaveState}});
    items_.push_back({"Load state",        "F9",    ItemAction{Result::LoadState}});

    items_.push_back({"Palette", "F10", ItemCycle{kPaletteNames, &palette_idx_}});
    items_.push_back({"Speed",   "-/+", ItemRange{&cfg_.cycles_per_frame,
                                                  cfg_.cycles_min,
                                                  cfg_.cycles_max, 1}});
    items_.push_back({"Quirks",  "F8/F12", ItemCycle{kQuirksPresetNames, &quirks_preset_}});
    items_.push_back({"MX-8 extensions", "F11", ItemToggle{&quirks_.mx8_extensions}});

    items_.push_back({"Reset CPU",   "P",     ItemAction{Result::ResetCPU}});
    items_.push_back({"Change ROM...", "",    ItemAction{Result::ChangeROM}});
    items_.push_back({"Quit",        "Alt+F4",ItemAction{Result::Quit}});

    selected_ = firstSelectable();
}

void PauseMenu::setGameArea(float x, float y, float w, float h) {
    area_x_ = x; area_y_ = y; area_w_ = w; area_h_ = h;
}

void PauseMenu::open() {
    selected_ = firstSelectable();
}

int PauseMenu::firstSelectable() const {
    return items_.empty() ? -1 : 0;
}

int PauseMenu::nextSelectable(int from, int delta) const {
    if (items_.empty()) return -1;
    int n = static_cast<int>(items_.size());
    int i = ((from + delta) % n + n) % n;
    return i;
}

std::string PauseMenu::valueText(const Item& it) const {
    return std::visit([](auto&& k) -> std::string {
        using T = std::decay_t<decltype(k)>;
        if constexpr (std::is_same_v<T, ItemAction>) {
            return "";
        } else if constexpr (std::is_same_v<T, ItemCycle>) {
            int i = std::clamp(*k.index_ptr, 0, (int)k.values.size() - 1);
            return "< " + k.values[i] + " >";
        } else if constexpr (std::is_same_v<T, ItemRange>) {
            char buf[24];
            std::snprintf(buf, sizeof(buf), "< %d >", *k.value_ptr);
            return buf;
        } else if constexpr (std::is_same_v<T, ItemToggle>) {
            return *k.value_ptr ? "[ on ]" : "[ off ]";
        }
        return "";
    }, it.kind);
}

PauseMenu::Result PauseMenu::activate(int idx) {
    if (idx < 0 || idx >= (int)items_.size()) return Result::Stay;
    Item& it = items_[idx];

    return std::visit([&](auto&& k) -> Result {
        using T = std::decay_t<decltype(k)>;
        if constexpr (std::is_same_v<T, ItemAction>) {
            return k.result;
        } else if constexpr (std::is_same_v<T, ItemCycle>) {
            // Enter cycles forward.
            int n = (int)k.values.size();
            *k.index_ptr = ((*k.index_ptr + 1) % n + n) % n;
            // Apply side effects for known cycle items.
            if (it.label == "Palette") {
                Config::applyPalette(kPaletteNames[*k.index_ptr],
                                     cfg_.color_on, cfg_.color_off);
            } else if (it.label == "Quirks") {
                bool legacy = (*k.index_ptr == 1);
                bool mx8_keep = quirks_.mx8_extensions;
                quirks_ = legacy ? Chip8::legacyQuirks() : Chip8::modernQuirks();
                quirks_.mx8_extensions = mx8_keep;
                cfg_.legacy_quirks = legacy;
            }
            return Result::Stay;
        } else if constexpr (std::is_same_v<T, ItemRange>) {
            // No-op on Enter for ranges; user uses Left/Right.
            return Result::Stay;
        } else if constexpr (std::is_same_v<T, ItemToggle>) {
            *k.value_ptr = !*k.value_ptr;
            return Result::Stay;
        }
        return Result::Stay;
    }, it.kind);
}

void PauseMenu::cycle(int idx, int delta) {
    if (idx < 0 || idx >= (int)items_.size()) return;
    Item& it = items_[idx];

    std::visit([&](auto&& k) {
        using T = std::decay_t<decltype(k)>;
        if constexpr (std::is_same_v<T, ItemCycle>) {
            int n = (int)k.values.size();
            *k.index_ptr = ((*k.index_ptr + delta) % n + n) % n;
            if (it.label == "Palette") {
                Config::applyPalette(kPaletteNames[*k.index_ptr],
                                     cfg_.color_on, cfg_.color_off);
            } else if (it.label == "Quirks") {
                bool legacy = (*k.index_ptr == 1);
                bool mx8_keep = quirks_.mx8_extensions;
                quirks_ = legacy ? Chip8::legacyQuirks() : Chip8::modernQuirks();
                quirks_.mx8_extensions = mx8_keep;
                cfg_.legacy_quirks = legacy;
            }
        } else if constexpr (std::is_same_v<T, ItemRange>) {
            int v = *k.value_ptr + delta * k.step;
            *k.value_ptr = std::clamp(v, k.min, k.max);
        } else if constexpr (std::is_same_v<T, ItemToggle>) {
            *k.value_ptr = !*k.value_ptr;
        }
        // Actions don't respond to Left/Right.
    }, it.kind);
}

PauseMenu::Result PauseMenu::handleEvent(const sf::Event& ev) {
    if (const auto* kp = ev.getIf<sf::Event::KeyPressed>()) {
        using K = sf::Keyboard::Key;
        switch (kp->code) {
        case K::Escape: return Result::Resume;
        case K::Up:     selected_ = nextSelectable(selected_, -1); return Result::Stay;
        case K::Down:   selected_ = nextSelectable(selected_, +1); return Result::Stay;
        case K::Left:   cycle(selected_, -1); return Result::Stay;
        case K::Right:  cycle(selected_, +1); return Result::Stay;
        case K::Enter:  return activate(selected_);
        case K::Home:   selected_ = firstSelectable(); return Result::Stay;
        case K::End:    selected_ = (int)items_.size() - 1; return Result::Stay;
        default: break;
        }
    } else if (const auto* mm = ev.getIf<sf::Event::MouseMoved>()) {
        // Hover-to-select. Match Y against item rows. Layout must mirror
        // draw()'s constants exactly.
        const float row_h  = 26.f;
        const float pad_y  = 80.f;
        const float menu_w = 460.f;
        const float menu_h = pad_y + row_h * items_.size() + 50.f;
        const float menu_x = area_x_ + (area_w_ - menu_w) * 0.5f;
        const float menu_y = area_y_ + (area_h_ - menu_h) * 0.5f;
        const float rows_y = menu_y + pad_y;
        float mx = static_cast<float>(mm->position.x);
        float my = static_cast<float>(mm->position.y);
        if (mx >= menu_x && mx < menu_x + menu_w) {
            int idx = static_cast<int>((my - rows_y) / row_h);
            if (idx >= 0 && idx < (int)items_.size()) selected_ = idx;
        }
    } else if (const auto* mp = ev.getIf<sf::Event::MouseButtonPressed>()) {
        if (mp->button == sf::Mouse::Button::Left) {
            return activate(selected_);
        } else if (mp->button == sf::Mouse::Button::Right) {
            cycle(selected_, +1);
        }
    } else if (const auto* sw = ev.getIf<sf::Event::MouseWheelScrolled>()) {
        if (sw->delta > 0) cycle(selected_, -1);
        else if (sw->delta < 0) cycle(selected_, +1);
    }
    return Result::Stay;
}

PauseMenu::Result PauseMenu::tick() {
    return Result::Stay;
}

void PauseMenu::draw(sf::RenderTarget& target) {
    // Dim overlay over the game area only — the sidebar debugger stays
    // legible underneath.
    sf::RectangleShape overlay({area_w_, area_h_});
    overlay.setPosition({area_x_, area_y_});
    overlay.setFillColor(sf::Color(0, 0, 0, 170));
    target.draw(overlay);

    const float menu_w = 460.f;
    const float row_h  = 26.f;
    // pad_y = space reserved at the top for the title row + credit row.
    const float pad_y  = 80.f;
    const float menu_h = pad_y + row_h * items_.size() + 50.f;
    const float menu_x = area_x_ + (area_w_ - menu_w) * 0.5f;
    const float menu_y = area_y_ + (area_h_ - menu_h) * 0.5f;

    // Panel.
    sf::RectangleShape panel({menu_w, menu_h});
    panel.setPosition({menu_x, menu_y});
    panel.setFillColor(sf::Color(20, 24, 36, 240));
    panel.setOutlineColor(sf::Color(80, 100, 140));
    panel.setOutlineThickness(2.f);
    target.draw(panel);

    sf::Text title(font_, "PAUSED", 22);
    title.setFillColor(sf::Color(255, 220, 80));
    title.setPosition({menu_x + 16.f, menu_y + 14.f});
    target.draw(title);

    // Author credit — own row directly under the title, bright color so
    // it doesn't get lost against the panel background.
    sf::Text credit(font_, "Made by Mehdi Lakhouane", 14);
    credit.setFillColor(sf::Color(180, 220, 255));
    credit.setPosition({menu_x + 16.f, menu_y + 40.f});
    target.draw(credit);

    sf::Text hint(font_, "Up/Down move   Enter select   Left/Right adjust   Esc resume",
                  12);
    hint.setFillColor(sf::Color(140, 150, 170));
    hint.setPosition({menu_x + 16.f, menu_y + menu_h - 26.f});
    target.draw(hint);

    for (size_t i = 0; i < items_.size(); ++i) {
        const Item& it = items_[i];
        bool is_sel = ((int)i == selected_);

        float row_y = menu_y + pad_y + i * row_h;

        if (is_sel) {
            sf::RectangleShape hl({menu_w - 12.f, row_h - 4.f});
            hl.setPosition({menu_x + 6.f, row_y - 2.f});
            hl.setFillColor(sf::Color(80, 100, 160, 90));
            target.draw(hl);
        }

        std::string left = (is_sel ? "> " : "  ") + it.label;
        sf::Text label(font_, left, 16);
        label.setFillColor(is_sel ? sf::Color(255, 230, 120) : sf::Color(220, 220, 230));
        label.setPosition({menu_x + 16.f, row_y});
        target.draw(label);

        std::string vt = valueText(it);
        if (!vt.empty()) {
            sf::Text value(font_, vt, 16);
            value.setFillColor(is_sel ? sf::Color(255, 230, 120) : sf::Color(180, 200, 220));
            value.setPosition({menu_x + 240.f, row_y});
            target.draw(value);
        }

        if (!it.hotkey_hint.empty()) {
            sf::Text hk(font_, it.hotkey_hint, 12);
            hk.setFillColor(sf::Color(110, 130, 160));
            hk.setPosition({menu_x + menu_w - 70.f, row_y + 3.f});
            target.draw(hk);
        }
    }
}
