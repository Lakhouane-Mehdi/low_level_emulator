#include "App.hpp"
#include "../core/CoreEvents.hpp"
#include "../core/isa/IInstructionSet.hpp"
#include "../ui/FontLoader.hpp"
#include "../ui/RomBrowser.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>

namespace fs = std::filesystem;

App::App(Config cfg)
    : cfg_(std::move(cfg)),
      window_(),
      // Anchor every 60 frames (1s at 60fps), keep the configured window.
      rewind_buf_(60, cfg_.rewind_seconds * 60) {
    cpu_.quirks = cfg_.legacy_quirks ? Chip8::legacyQuirks() : Chip8::modernQuirks();
    cpu_.quirks.mx8_extensions = cfg_.mx8_extensions;
    cpu_.installISA(&ISA::by_name(cfg_.isa_name));

    // The rewind buffer needs to see every CoreEvent for replay correctness.
    // It composes with the (separate) replay-recorder tap by chaining
    // through one App-side dispatcher, so both observers fire on each event.
    cpu_.setEventTap([this](const CoreEvent& ev) {
        rewind_buf_.noteEvent(frame_ordinal_, ev);
        recordEventIfActive(ev);   // no-op unless Shift+R recording is active
    });

    paused_ = cfg_.start_paused;
}

bool App::initWindow() {
    window_.create(
        sf::VideoMode({static_cast<unsigned>(cfg_.windowWidth()),
                       static_cast<unsigned>(cfg_.windowHeight())}),
        "CHIP-8 Emulator + Debugger   —   Made by Mehdi Lakhouane");
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

    // Cache the loaded bytes for replay recording — replays embed the
    // full ROM image so they're hermetic.
    {
        std::ifstream rf(romPath.string(), std::ios::binary | std::ios::ate);
        if (rf) {
            auto sz = rf.tellg();
            rf.seekg(0, std::ios::beg);
            recording_rom_bytes_.resize(static_cast<size_t>(sz));
            rf.read(reinterpret_cast<char*>(recording_rom_bytes_.data()), sz);
            recording_label_ = romPath.stem().string();
        }
    }

    cpu_.quirks = cfg_.legacy_quirks ? Chip8::legacyQuirks() : Chip8::modernQuirks();
    cpu_.quirks.mx8_extensions = cfg_.mx8_extensions;
    // Apply deterministic seed AFTER load so reset() inside loadROM doesn't
    // matter (reset() preserves rng anyway, but explicit > implicit).
    if (cfg_.rng_seed >= 0) cpu_.setSeed(static_cast<uint64_t>(cfg_.rng_seed));
    window_.setTitle("CHIP-8: " + romPath.filename().string() +
                     "   —   Made by Mehdi Lakhouane");
    return true;
}

int App::run() {
    if (!initWindow()) return 1;
    if (!selectROM())  return 0;
    mainLoop();
    return 0;
}

void App::pushRewindFrame() {
    // Hybrid model: RewindBuffer decides whether this frame becomes an
    // anchor (every N frames) or just trims the slabs.
    rewind_buf_.noteFrameBegin(frame_ordinal_, cpu_);
    rewind_buf_.trim(frame_ordinal_);
}

bool App::rewindToFrame(uint64_t target_frame) {
    auto plan = rewind_buf_.planRewindTo(target_frame);
    if (!plan) return false;

    cpu_.restore(plan->anchor);
    // Re-execute frames [anchor_frame, target_frame) deterministically.
    // For each frame, re-enqueue any events stamped on that frame, then
    // drain + cycle batch + tick — the same shape as the live frame loop.
    for (int f = 0; f < plan->frames_to_advance; ++f) {
        for (const auto& ev : plan->events_per_frame[f]) {
            cpu_.enqueue(ev);
        }
        cpu_.drainEvents();
        for (int c = 0; c < cfg_.cycles_per_frame; ++c) {
            cpu_.cycle();
            if (cpu_.halted()) break;
        }
        cpu_.tickTimers();
    }
    frame_ordinal_ = plan->target_frame;
    return true;
}

void App::onSaveState() { saved_state_ = cpu_.snapshot(); }
void App::onLoadState() { if (saved_state_) cpu_.restore(*saved_state_); }

void App::onAdjustSpeed(int delta) {
    cfg_.cycles_per_frame = std::max(cfg_.cycles_min,
                                     std::min(cfg_.cycles_max, cfg_.cycles_per_frame + delta));
}

void App::startRecording(const std::vector<uint8_t>& rom_bytes,
                         const std::string&          isa_name,
                         const std::string&          rom_label) {
    recording_replay_ = Replay{};
    recording_replay_.rom_bytes  = rom_bytes;
    recording_replay_.isa        = isa_name;
    recording_replay_.quirks     = cpu_.quirks;
    recording_replay_.have_seed  = (cfg_.rng_seed >= 0);
    recording_replay_.seed       = cfg_.rng_seed >= 0 ? cfg_.rng_seed : 0;
    recording_replay_.events.clear();
    recording_replay_.checkpoints.clear();
    recording_label_ = rom_label;
    frame_ordinal_   = 0;
    recording_       = true;
}

std::string App::stopRecordingAndSave() {
    if (!recording_) return "";
    // Drop a final checkpoint at the current frame for self-validation.
    recording_replay_.checkpoints.push_back(
        {frame_ordinal_, cpu_.framebufferHash()});

    fs::path dir = "replays";
    std::error_code ec;
    fs::create_directories(dir, ec);

    // Filename: <rom_label>-<unix_seconds>.json
    auto now = std::chrono::system_clock::now().time_since_epoch();
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(now).count();
    std::ostringstream name;
    name << (recording_label_.empty() ? "replay" : recording_label_)
         << "-" << secs << ".json";
    fs::path path = dir / name.str();

    bool ok = recording_replay_.save(path.string());
    recording_ = false;
    return ok ? path.string() : "";
}

void App::recordEventIfActive(const CoreEvent& ev) {
    if (!recording_) return;
    // Only record events that matter for replay (exclude debugger ones).
    bool keep = std::visit([](auto&& e) {
        using T = std::decay_t<decltype(e)>;
        return std::is_same_v<T, InjectKeyEvent>
            || std::is_same_v<T, WriteMemoryEvent>
            || std::is_same_v<T, WriteMemoryBlockEvent>
            || std::is_same_v<T, SetPCEvent>
            || std::is_same_v<T, ResetEvent>
            || std::is_same_v<T, SetSeedEvent>;
    }, ev);
    if (!keep) return;
    recording_replay_.events.push_back({frame_ordinal_, ev});
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
    // Frame ordinal advances after the cycle batch, so events submitted
    // during the next frame get the next ordinal — matches the replay
    // format's frame indexing semantics.
    ++frame_ordinal_;
}

void App::mainLoop() {
    Renderer renderer(cfg_);
    AudioBeep beep;
    DebugView dbg(font_,
                  static_cast<float>(cfg_.gameWidth()) + 12.f, 8.f,
                  static_cast<float>(cfg_.sidebar_w),
                  static_cast<float>(cfg_.windowHeight()));
    dbg.setStatusMessage("Ready", 60);

    PauseMenu menu(font_, cfg_, cpu_.quirks);
    menu.setGameArea(0.f, 0.f,
                     static_cast<float>(cfg_.gameWidth()),
                     static_cast<float>(cfg_.windowHeight()));

    while (window_.isOpen()) {
        // ---- events ----
        while (const std::optional event = window_.pollEvent()) {
            if (event->is<sf::Event::Closed>()) { window_.close(); break; }

            // Modal pause menu: when open, it consumes all input and decides
            // when to close, what action to take, etc. Game cycles also
            // stop while open (paused_ remains true).
            if (menu_open_) {
                auto r = menu.handleEvent(*event);
                if (r == PauseMenu::Result::Resume) {
                    menu_open_ = false;
                    paused_    = false;
                } else if (r == PauseMenu::Result::Step) {
                    step_once_ = true;
                    // Stay paused after the step — but close the menu so the
                    // step takes effect on the next frame.
                    menu_open_ = false;
                    paused_    = true;
                } else if (r == PauseMenu::Result::SaveState) {
                    onSaveState();
                    dbg.setStatusMessage("State saved");
                } else if (r == PauseMenu::Result::LoadState) {
                    if (saved_state_) { onLoadState(); dbg.setStatusMessage("State loaded"); }
                    else              { dbg.setStatusMessage("No save state"); }
                } else if (r == PauseMenu::Result::ResetCPU) {
                    cpu_.enqueue(ResetEvent{});
                    dbg.setStatusMessage("CPU reset queued");
                    menu_open_ = false;
                    paused_    = false;
                } else if (r == PauseMenu::Result::ChangeROM) {
                    want_change_rom_ = true;
                    menu_open_ = false;
                } else if (r == PauseMenu::Result::Quit) {
                    window_.close();
                }
                continue;       // do not run normal keyboard handling
            }

            if (const auto* kp = event->getIf<sf::Event::KeyPressed>()) {
                using K = sf::Keyboard::Key;
                bool consumed = true;
                switch (kp->code) {
                case K::Escape:
                    // Open the modal pause menu. (Quit lives on the menu's
                    // Quit item now; Alt+F4 still closes the window directly.)
                    menu_open_ = true;
                    paused_    = true;
                    menu.open();
                    break;
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
                case K::R:
                    // Shift+R toggles replay recording. Plain R is keypad
                    // key D so we only act on the shifted variant. The
                    // event tap is permanent (installed for rewind); the
                    // recorder just flips a flag — recordEventIfActive
                    // is a no-op until startRecording is called.
                    if (kp->shift) {
                        if (!recording_) {
                            startRecording(recording_rom_bytes_, cfg_.isa_name, recording_label_);
                            dbg.setStatusMessage("REC: started — Shift+R again to save");
                        } else {
                            std::string p = stopRecordingAndSave();
                            std::string msg = p.empty() ? "REC: save FAILED"
                                                        : ("REC: saved " + p);
                            dbg.setStatusMessage(msg, 240);
                        }
                    } else {
                        consumed = false;   // fall through to keypad
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
                    if (kp->shift) {
                        // Shift+F11: switch the installed ISA to XO-CHIP, or
                        // back to the configured ISA. MX-8 and XO-CHIP are
                        // mutually exclusive (they fight over 5XY2/5XY3), so
                        // this swaps the whole decoder rather than a quirk.
                        const char* cur = cpu_.getISA() ? cpu_.getISA()->name() : "";
                        if (std::string(cur) == "XO-CHIP") {
                            cpu_.installISA(&ISA::by_name(cfg_.isa_name));
                            dbg.setStatusMessage(std::string("ISA -> ") + cpu_.getISA()->name());
                        } else {
                            cpu_.installISA(&ISA::xochip());
                            dbg.setStatusMessage("ISA -> XO-CHIP");
                        }
                    } else {
                        cpu_.quirks.mx8_extensions = !cpu_.quirks.mx8_extensions;
                        dbg.setStatusMessage(cpu_.quirks.mx8_extensions
                            ? "MX-8 extensions: ON" : "MX-8 extensions: OFF");
                    }
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
        // Rewind: hybrid reconstruction via the latest anchor + replay of
        // recorded events. Each Backspace tick walks one frame back. The
        // RewindBuffer's anchor cadence (every 60 frames) means worst-case
        // reconstruction replays ~60 frames of cycles — well under 1ms.
        rewinding_ = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Backspace);
        if (rewinding_ && !rewind_buf_.empty()) {
            cpu_.drainEvents();   // honor any pending UI mutations first
            if (frame_ordinal_ > 0) rewindToFrame(frame_ordinal_ - 1);
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
        // Feed XO-CHIP audio state to the beep. For classic ROMs the pattern
        // stays all-zero at its reset default, so setXoPattern marks silence
        // and update() falls back to... nothing? No: classic ROMs never call
        // F002, so xo_active_ stays false and the 440Hz square wave is used.
        // We only push a pattern once a ROM has actually loaded one.
        {
            static const std::array<uint8_t, Chip8::AUDIO_PATTERN_BYTES> kZero{};
            if (cpu_.audio_pattern != kZero)
                beep.setXoPattern(cpu_.audio_pattern, cpu_.audio_pitch);
        }
        beep.update(cpu_.sound_timer, paused_ || rewinding_ || muted_);

        window_.clear(sf::Color::Black);
        renderer.draw(window_);
        // Frames-of-rewind-available: distance from now to the oldest anchor.
        size_t rewind_frames = 0;
        const auto anchors = rewind_buf_.anchorFrames();
        if (!anchors.empty() && frame_ordinal_ >= anchors.front()) {
            rewind_frames = static_cast<size_t>(frame_ordinal_ - anchors.front());
        }
        dbg.draw(window_, cpu_, paused_, rewinding_, rewind_frames, cfg_.cycles_per_frame);
        // Menu is drawn last so it overlays the game area. Sidebar stays
        // visible alongside the menu — useful for debugging while paused.
        if (menu_open_) menu.draw(window_);
        window_.display();

        // Change-ROM transition: leave the inner loop to re-pick a ROM,
        // then re-enter mainLoop() implicitly via the outer caller.
        if (want_change_rom_) {
            want_change_rom_ = false;
            cfg_.rom_path.clear();   // force the picker on selectROM
            if (!selectROM()) { window_.close(); return; }
            // Reset transient runtime state that's per-ROM.
            paused_ = cfg_.start_paused;
            rewind_buf_.clear();
            frame_ordinal_ = 0;
            saved_state_.reset();
            recording_ = false;
            dbg.setStatusMessage("ROM loaded", 90);
        }
    }
}
