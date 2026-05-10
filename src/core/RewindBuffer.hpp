#pragma once

#include "Chip8.hpp"
#include "CoreEvents.hpp"

#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

// Hybrid rewind: periodic full snapshots ("anchors") + event log between
// them. To rewind to frame N:
//   1. Find the latest anchor at frame K <= N.
//   2. Restore anchor.
//   3. Re-execute frames [K, N) deterministically, replaying any events
//      that were originally submitted on each of those frames.
//
// Why hybrid: storing one snapshot per frame at 60Hz costs ~360KB/s.
// One anchor per second + ~few-byte events scales 20-50x better and lets
// us offer minutes of rewind for the same memory budget.
//
// Correctness invariants this design relies on (already proven elsewhere):
//   - Snapshots capture RNG + keypad + memory + ISA-relevant state (v3).
//   - Live keypad input flows through InjectKeyEvent, not direct writes.
//   - Event drain is FIFO at a deterministic frame boundary.
class RewindBuffer {
public:
    // anchor_interval_frames: how often to snapshot (e.g. 60 = once per second).
    // max_window_frames: drop everything older than this many frames behind
    // the current frame on trim().
    RewindBuffer(int anchor_interval_frames, int max_window_frames);

    // ---- recording API (the App calls these) ----
    // Call exactly once per frame, BEFORE running the cycle batch.
    // The buffer decides whether to record an anchor.
    void noteFrameBegin(uint64_t frame, const Chip8& cpu);

    // Call for every event the CPU enqueues — driven by Chip8::setEventTap.
    void noteEvent(uint64_t frame, const CoreEvent& ev);

    // Call once per frame AFTER the cycle batch + tickTimers.
    void noteFrameEnd(uint64_t frame);

    // Drop anchors and events older than max_window_frames behind now_frame.
    void trim(uint64_t now_frame);

    // ---- rewind planning ----
    struct Plan {
        Chip8::Snapshot                                  anchor;
        uint64_t                                         anchor_frame = 0;
        uint64_t                                         target_frame = 0;
        // Events bucketed by frame ordinal. To replay, enqueue
        // events_per_frame[f] before drainEvents() on frame anchor_frame+f.
        std::vector<std::vector<CoreEvent>>              events_per_frame;
        int                                              frames_to_advance = 0;
    };
    // Returns nullopt if the buffer is empty or target precedes the
    // earliest anchor. The plan's target is clamped to the latest anchor
    // we have, so a target one frame ago and a target ten minutes ago
    // both resolve to "the closest valid frame".
    std::optional<Plan> planRewindTo(uint64_t target_frame) const;

    // Plan a rewind one frame back from now. Convenience for the
    // hold-Backspace UX.
    std::optional<Plan> planRewindOneFrame(uint64_t now_frame) const;

    // ---- introspection ----
    bool empty() const { return anchors_.empty(); }
    size_t anchorCount() const { return anchors_.size(); }
    size_t eventCount()  const { return events_.size(); }
    std::vector<uint64_t> anchorFrames() const;

    // Wipe everything (e.g. on ROM reload).
    void clear();

    // Tunables.
    int anchorIntervalFrames() const { return anchor_interval_; }

private:
    struct Anchor {
        uint64_t          frame;
        Chip8::Snapshot   snapshot;
    };
    struct StampedEvent {
        uint64_t   frame;
        CoreEvent  event;
    };

    int                       anchor_interval_;
    int                       max_window_;
    std::deque<Anchor>        anchors_;
    std::deque<StampedEvent>  events_;
};
