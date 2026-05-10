#include "App.hpp"
#include "../core/CoreEvents.hpp"
#include "../core/isa/IInstructionSet.hpp"
#include "../ui/FontLoader.hpp"
#include "../ui/RomBrowser.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <optional>

namespace fs = std::filesystem;

App::App(Config cfg)
    : cfg_(std::move(cfg)),
      window_() {
    cpu_.quirks = cfg_.legacy_quirks ? Chip8::legacyQuirks() : Chip8::modernQuirks();
    cpu_.quirks.mx8_extensions = cfg_.mx8_extensions;
    cpu_.installISA(&ISA::by_name(cfg_.isa_name));
    paused_ = cfg_.start_paused;
}

bool App::initWindow() {
    window_.create(
        sf::VideoMode({static_cast<unsigned>(cfg_.windowWidth()),
                       static_cast<unsigned>(cfg_.windowHeight())}),
        "CHIP-8 Emulator + Debugger");
    window_.setFramerateLimit(60);

    auto loaded = loadMonoFont(font_);
    if (!loaded) {
        std::fprintf(stderr,
            "FontLoader: no monospace font found.\n"
            "Place a .ttf at assets/fonts/mono.ttf or install a system mono font.\n");
        return false;
    }
    return true;
}

bool App::selectROM() {
    fs::path romPath;
    if (!cfg_.rom_path.empty()) {
        romPath = cfg_.rom_path;
    } else {
        auto roms = RomBrowser::listROMs(cfg_.roms_dir);
        if (roms.empty()) {
            std::fprintf(stderr,
                "No ROMs found in '%s/' and no path given.\n", cfg_.roms_dir.c_str());
            return false;
        }
        romPath = RomBrowser::pick(window_, font_, roms);
        if (romPath.empty()) return false;
    }
    if (!cpu_.loadROM(romPath.string())) return false;
    cpu_.quirks = cfg_.legacy_quirks ? Chip8::legacyQuirks() : Chip8::modernQuirks();
    cpu_.quirks.mx8_extensions = cfg_.mx8_extensions;
    // Apply deterministic seed AFTER load so reset() inside loadROM doesn't
    // matter (reset() preserves rng anyway, but explicit > implicit).
    if (cfg_.rng_seed >= 0) cpu_.setSeed(static_cast<uint64_t>(cfg_.rng_seed));
    window_.setTitle("CHIP-8: " + romPath.filename().string());
    return true;
}

int App::run() {
    if (!initWindow()) return 1;
    if (!selectROM())  return 0;
    mainLoop();
    return 0;
}

void App::pushRewindFrame() {
    rewind_buf_.push_back(cpu_.snapshot());
    const size_t cap = static_cast<size_t>(cfg_.rewind_seconds * 60);
    if (rewind_buf_.size() > cap) rewind_buf_.pop_front();
}

void App::onSaveState() { saved_state_ = cpu_.snapshot(); }
void App::onLoadState() { if (saved_state_) cpu_.restore(*saved_state_); }

void App::onAdjustSpeed(int delta) {
    cfg_.cycles_per_frame = std::max(cfg_.cycles_min,
                                     std::min(cfg_.cycles_max, cfg_.cycles_per_frame + delta));
}

bool App::reachedStepStop() {
    if (step_over_target_ && cpu_.pc == *step_over_target_) {
        step_over_target_.reset();
        return true;
    }
    if (step_out_baseline_sp_ && cpu_.sp < *step_out_baseline_sp_) {
        step_out_baseline_sp_.reset();
        return true;
    }
    if (cpu_.hit_breakpoint) return true;
    return false;
}

bool App::drainWatchpoint(DebugView& dbg) {
    if (!cpu_.mem.watch_triggered()) return false;
    auto ev = cpu_.mem.consume_watch_event();
    using WK = Memory::WatchKind;
    const char* kind =
        (ev.kind == WK::Read)  ? "READ" :
        (ev.kind == WK::Write) ? "WRITE" : "RW";
    char buf[80];
    std::snprintf(buf, sizeof(buf), "WP %s @ %03X = %02X", kind, ev.addr, ev.value);
    dbg.setStatusMessage(buf, 240);
    return true;
}

void App::runFrameOfCpu(DebugView& dbg) {
    // Deterministic event drain: applies any UI/debugger/script enqueued
    // mutations to CPU state BEFORE the cycle batch. This is the single
    // sync point where state changes from outside the CPU take effect.
    cpu_.drainEvents();

    if (paused_) return;
    pushRewindFrame();
    for (int i = 0; i < cfg_.cycles_per_frame; ++i) {
        cpu_.cycle();
        if (cpu_.halted()) break;
        if (cpu_.hit_breakpoint) {
            paused_ = true;
            cpu_.hit_breakpoint = false;
            break;
        }
        // Memory watchpoint check — same priority as a breakpoint, but the
        // pause happens AFTER the opcode finishes (atomicity preserved by
        // the per-byte first-hit semantics in Memory).
        if (drainWatchpoint(dbg)) {
            paused_ = true;
            break;
        }
        if (step_over_target_ || step_out_baseline_sp_) {
            if (reachedStepStop()) { paused_ = true; break; }
        }
    }
    cpu_.tickTimers();
}

void App::mainLoop() {
    Renderer renderer(cfg_);
    AudioBeep beep;
    DebugView dbg(font_,
                  static_cast<float>(cfg_.gameWidth()) + 12.f, 8.f,
                  static_cast<float>(cfg_.sidebar_w),
                  static_cast<float>(cfg_.windowHeight()));
    dbg.setStatusMessage("Ready", 60);

    while (window_.isOpen()) {
        // ---- events ----
        while (const std::optional event = window_.pollEvent()) {
            if (event->is<sf::Event::Closed>()) { window_.close(); break; }

            if (const auto* kp = event->getIf<sf::Event::KeyPressed>()) {
                using K = sf::Keyboard::Key;
                bool consumed = true;
                switch (kp->code) {
                case K::Escape: window_.close(); break;
                case K::Space:  paused_ = !paused_; break;
                case K::N:
                    if (paused_) step_once_ = true;
                    break;
                case K::O: {  // step over CALL — run until pc == addr after the CALL
                    if (paused_) {
                        uint16_t op = cpu_.fetchOpcode(cpu_.pc);
                        if ((op & 0xF000) == 0x2000) {
                            step_over_target_ = cpu_.pc + 2;
                            paused_ = false;
                            dbg.setStatusMessage("Stepping over CALL...");
                        } else {
                            step_once_ = true;  // not a CALL — fall back to step
                        }
                    }
                    break;
                }
                case K::Enter: {  // step out — run until SP < baseline
                    if (paused_) {
                        if (cpu_.sp == 0) {
                            dbg.setStatusMessage("Cannot step out: stack empty");
                        } else {
                            step_out_baseline_sp_ = cpu_.sp;
                            paused_ = false;
                            dbg.setStatusMessage("Stepping out...");
                        }
                    }
                    break;
                }
                case K::B:
                    if (kp->shift) {
                        cpu_.enqueue(ClearAllBreakpointsEvent{});
                        dbg.setStatusMessage("All breakpoints cleared");
                    } else {
                        dbg.toggleBreakpointAtPC(cpu_);
                    }
                    break;
                case K::W:
                    // Paused: cycle the watchpoint at the cursor.
                    // Unpaused: fall through to the CHIP-8 keypad (W = key 5).
                    if (paused_) {
                        if (kp->shift) dbg.clearAllWatchpoints(cpu_);
                        else           dbg.cycleWatchpointAtCursor(cpu_);
                    } else {
                        consumed = false;
                    }
                    break;

                // Per-quirk toggles
                case K::F1: cpu_.quirks.shift_in_place     = !cpu_.quirks.shift_in_place;     dbg.setStatusMessage("quirk: shift_in_place"); break;
                case K::F2: cpu_.quirks.load_store_no_inc  = !cpu_.quirks.load_store_no_inc;  dbg.setStatusMessage("quirk: load_store_no_inc"); break;
                case K::F3: cpu_.quirks.jump_with_offset_x = !cpu_.quirks.jump_with_offset_x; dbg.setStatusMessage("quirk: jump_with_offset_x"); break;
                case K::F4: cpu_.quirks.vf_reset           = !cpu_.quirks.vf_reset;           dbg.setStatusMessage("quirk: vf_reset"); break;
                case K::F6: cpu_.quirks.display_wait       = !cpu_.quirks.display_wait;       dbg.setStatusMessage("quirk: display_wait"); break;
                case K::F7: cpu_.quirks.clip_sprites       = !cpu_.quirks.clip_sprites;       dbg.setStatusMessage("quirk: clip_sprites"); break;
                case K::F8: cpu_.quirks = Chip8::modernQuirks(); dbg.setStatusMessage("quirks -> MODERN"); break;
                case K::F12: cpu_.quirks = Chip8::legacyQuirks(); dbg.setStatusMessage("quirks -> LEGACY (VIP)"); break;
                case K::F11:
                    cpu_.quirks.mx8_extensions = !cpu_.quirks.mx8_extensions;
                    dbg.setStatusMessage(cpu_.quirks.mx8_extensions
                        ? "MX-8 extensions: ON" : "MX-8 extensions: OFF");
                    break;
                case K::F10: {
                    // Cycle through built-in palettes.
                    static const char* names[] = {"mono","amber","green","gameboy","c64","ice","hotdog"};
                    static int idx = 0;
                    idx = (idx + 1) % (int)(sizeof(names)/sizeof(names[0]));
                    Config::applyPalette(names[idx], cfg_.color_on, cfg_.color_off);
                    renderer.setPalette(cfg_.color_on, cfg_.color_off);
                    cpu_.draw_flag = true;
                    dbg.setStatusMessage(std::string("Palette: ") + names[idx]);
                    break;
                }

                case K::F5: onSaveState(); dbg.setStatusMessage("State saved (F5)"); break;
                case K::F9:
                    if (saved_state_) { onLoadState(); dbg.setStatusMessage("State loaded (F9)"); }
                    else              { dbg.setStatusMessage("No save state"); }
                    break;
                case K::M: dbg.cycleMemPane(); break;
                case K::Up:    if (paused_) dbg.moveCursor(-16); else consumed = false; break;
                case K::Down:  if (paused_) dbg.moveCursor(+16); else consumed = false; break;
                case K::Left:  if (paused_) dbg.moveCursor(-1);  else consumed = false; break;
                case K::Right: if (paused_) dbg.moveCursor(+1);  else consumed = false; break;
                case K::LBracket: if (paused_) dbg.editByteAtCursor(cpu_, -1); break;
                case K::RBracket: if (paused_) dbg.editByteAtCursor(cpu_, +1); break;
                case K::Hyphen:    onAdjustSpeed(-1); break;
                case K::Equal:     onAdjustSpeed(+1); break;
                case K::P:  cpu_.enqueue(ResetEvent{}); dbg.setStatusMessage("CPU reset"); break;
                case K::Tab:  // mute toggle
                    muted_ = !muted_;
                    dbg.setStatusMessage(muted_ ? "Audio muted" : "Audio unmuted");
                    break;
                default:
                    consumed = false;
                    break;
                }
                if (!consumed) Input::handleKeyPressed(*kp, cpu_);
            }
            else if (const auto* kr = event->getIf<sf::Event::KeyReleased>()) {
                Input::handleKeyReleased(*kr, cpu_);
            }
            else if (const auto* mp = event->getIf<sf::Event::MouseButtonPressed>()) {
                // Left-click on disassembly area near top-right toggles a bp at clicked PC.
                // Lightweight heuristic: clicking the game area while paused doesn't matter.
                (void)mp;
            }
        }

        // ---- rewind / advance ----
        // Every branch must drain pending events at a known boundary
        // (typically before any cycle/restore) so debugger enqueues take
        // effect even when paused or stepping. Rewind drains BEFORE the
        // restore so any mid-rewind enqueue (rare) is honored against the
        // current state, not the restored one.
        rewinding_ = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Backspace);
        if (rewinding_ && !rewind_buf_.empty()) {
            cpu_.drainEvents();
            cpu_.restore(rewind_buf_.back());
            rewind_buf_.pop_back();
        } else if (step_once_) {
            cpu_.drainEvents();
            pushRewindFrame();
            cpu_.cycle();
            drainWatchpoint(dbg);   // surface trigger but stay paused
            step_once_ = false;
        } else {
            runFrameOfCpu(dbg);     // drains internally
        }

        // ---- output ----
        renderer.update(cpu_);
        beep.update(cpu_.sound_timer, paused_ || rewinding_ || muted_);

        window_.clear(sf::Color::Black);
        renderer.draw(window_);
        dbg.draw(window_, cpu_, paused_, rewinding_, rewind_buf_.size(), cfg_.cycles_per_frame);
        window_.display();
    }
}
