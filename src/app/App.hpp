#pragma once

#include "Config.hpp"
#include "../core/Chip8.hpp"
#include "../ui/Renderer.hpp"
#include "../ui/Input.hpp"
#include "../ui/AudioBeep.hpp"
#include "../ui/DebugView.hpp"

#include <SFML/Graphics.hpp>
#include <deque>
#include <optional>

class App {
public:
    explicit App(Config cfg);
    int run();   // returns process exit code

private:
    Config         cfg_;
    Chip8          cpu_;
    sf::Font       font_;
    sf::RenderWindow window_;

    bool                       paused_       = false;
    bool                       step_once_    = false;
    bool                       rewinding_    = false;
    bool                       muted_        = false;

    // Step-over: run until pc reaches `step_over_target_` (the instruction
    // immediately after a CALL). Step-out: run until SP drops below the
    // baseline captured when we asked to step out.
    std::optional<uint16_t>    step_over_target_;
    std::optional<int>         step_out_baseline_sp_;

    std::optional<Chip8::Snapshot> saved_state_;
    std::deque<Chip8::Snapshot>    rewind_buf_;

    // ---- helpers ----
    bool initWindow();
    bool selectROM();
    void mainLoop();
    void runFrameOfCpu(DebugView& dbg);
    bool drainWatchpoint(DebugView& dbg);   // returns true if a watch fired
    bool reachedStepStop();
    void pushRewindFrame();
    void onSaveState();
    void onLoadState();
    void onAdjustSpeed(int delta);
};
