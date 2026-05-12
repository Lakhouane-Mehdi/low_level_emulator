#pragma once

#include "../app/Config.hpp"
#include "../core/Chip8.hpp"

#include <SFML/Graphics.hpp>
#include <SFML/Window/Event.hpp>
#include <functional>
#include <string>
#include <variant>
#include <vector>

// Modal in-game pause menu. Drawn over the game area on top of a dimming
// overlay. Pure UI: handles its own input, mutates a Config and a Chip8::
// Quirks held by reference. Returns a MenuResult each frame indicating
// whether the App should keep the menu open or transition to some action
// (resume play, change ROM, quit, etc.). All mutation paths that touch
// machine state still flow through the event queue from the App's
// dispatch side — the menu itself doesn't reach into the CPU directly.
class PauseMenu {
public:
    enum class Result {
        Stay,          // keep the menu open
        Resume,        // close menu, unpause
        Step,          // step one instruction then re-open menu
        SaveState,
        LoadState,
        ResetCPU,
        ChangeROM,     // close ROM, return to picker
        Quit
    };

    PauseMenu(const sf::Font& font, Config& cfg, Chip8::Quirks& quirks);

    // Reposition / resize on game-area changes.
    void setGameArea(float x, float y, float w, float h);

    // Event handling. Returns the Result for this frame.
    Result handleEvent(const sf::Event& ev);

    // Call once per frame even when no event happened (to keep the
    // "tick" state machine — currently a no-op but kept for future use).
    Result tick();

    void draw(sf::RenderTarget& target);

    // Reset the cursor to the top item when re-opening.
    void open();

private:
    // ---- model ----
    struct ItemAction  { Result result; };
    struct ItemCycle   { std::vector<std::string> values; int* index_ptr; };
    struct ItemRange   { int* value_ptr; int min, max, step; };
    struct ItemToggle  { bool* value_ptr; };
    using  ItemKind    = std::variant<ItemAction, ItemCycle, ItemRange, ItemToggle>;

    struct Item {
        std::string label;
        std::string hotkey_hint;
        ItemKind    kind;
    };

    void                buildItems();
    std::string         valueText(const Item& it) const;
    Result              activate(int idx);          // Enter on idx
    void                cycle(int idx, int delta);  // Left/Right on idx
    int                 firstSelectable() const;
    int                 nextSelectable(int from, int delta) const;

    const sf::Font&     font_;
    Config&             cfg_;
    Chip8::Quirks&      quirks_;

    std::vector<Item>   items_;
    int                 selected_ = 0;

    // Persistent indices for cycle items — must outlive items_ rebuilds
    // (which currently never happen, but keep the model stable).
    int                 palette_idx_     = 0;
    int                 quirks_preset_   = 0;     // 0 = modern, 1 = legacy

    // Layout (set from setGameArea).
    float               area_x_ = 0, area_y_ = 0, area_w_ = 0, area_h_ = 0;

    static const std::vector<std::string> kPaletteNames;
    static const std::vector<std::string> kQuirksPresetNames;
};
