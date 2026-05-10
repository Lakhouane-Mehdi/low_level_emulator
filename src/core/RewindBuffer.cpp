#include "RewindBuffer.hpp"

#include <algorithm>

RewindBuffer::RewindBuffer(int anchor_interval_frames, int max_window_frames)
    : anchor_interval_(anchor_interval_frames < 1 ? 1 : anchor_interval_frames),
      max_window_(max_window_frames < anchor_interval_ ? anchor_interval_ : max_window_frames) {
}

void RewindBuffer::noteFrameBegin(uint64_t frame, const Chip8& cpu) {
    // Take an anchor when:
    //   - the buffer is empty (so we always have *some* rewind point), OR
    //   - frame falls on an interval boundary AND the last anchor is older
    //     than that boundary (avoids double-anchoring if noteFrameBegin is
    //     ever called twice for the same frame).
    bool take = anchors_.empty();
    if (!take && (frame % anchor_interval_) == 0 &&
        anchors_.back().frame < frame) {
        take = true;
    }
    if (take) {
        Anchor a;
        a.frame    = frame;
        a.snapshot = cpu.snapshot();
        anchors_.push_back(std::move(a));
    }
}

void RewindBuffer::noteEvent(uint64_t frame, const CoreEvent& ev) {
    events_.push_back({frame, ev});
}

void RewindBuffer::noteFrameEnd(uint64_t /*frame*/) {
    // Reserved hook — currently no end-of-frame work. Kept in the API
    // so the App's frame loop has a stable shape we can extend later
    // (e.g. periodic checkpoint hashes for self-validation).
}

void RewindBuffer::trim(uint64_t now_frame) {
    if (max_window_ <= 0) return;
    uint64_t cutoff = (now_frame > (uint64_t)max_window_)
        ? (now_frame - (uint64_t)max_window_)
        : 0;

    // Drop anchors strictly older than cutoff, but always keep at least
    // the most recent one (so rewind from "right now" still works).
    while (anchors_.size() > 1 && anchors_.front().frame < cutoff) {
        anchors_.pop_front();
    }
    // Drop events older than the oldest surviving anchor — nothing
    // earlier is reachable anymore.
    if (!anchors_.empty()) {
        const uint64_t oldest = anchors_.front().frame;
        while (!events_.empty() && events_.front().frame < oldest) {
            events_.pop_front();
        }
    }
}

void RewindBuffer::clear() {
    anchors_.clear();
    events_.clear();
}

std::vector<uint64_t> RewindBuffer::anchorFrames() const {
    std::vector<uint64_t> v;
    v.reserve(anchors_.size());
    for (const auto& a : anchors_) v.push_back(a.frame);
    return v;
}

std::optional<RewindBuffer::Plan>
RewindBuffer::planRewindTo(uint64_t target_frame) const {
    if (anchors_.empty()) return std::nullopt;

    // Find the latest anchor at or before target_frame. If target precedes
    // the earliest anchor, snap to the earliest available — better UX
    // than refusing the rewind.
    const Anchor* chosen = nullptr;
    for (const auto& a : anchors_) {
        if (a.frame <= target_frame) chosen = &a;
        else break;     // anchors are kept in ascending frame order
    }
    if (!chosen) chosen = &anchors_.front();

    // Clamp the target so we never replay forward past what was recorded.
    uint64_t reachable_target = target_frame;
    if (reachable_target < chosen->frame) reachable_target = chosen->frame;

    Plan plan;
    plan.anchor       = chosen->snapshot;
    plan.anchor_frame = chosen->frame;
    plan.target_frame = reachable_target;
    plan.frames_to_advance =
        static_cast<int>(reachable_target - chosen->frame);
    plan.events_per_frame.assign(
        static_cast<size_t>(plan.frames_to_advance), {});

    // Bucket events into per-frame lists.
    //
    // Anchor invariant: noteFrameBegin runs BEFORE the App's drainEvents
    // for frame K, so the anchor captures state where events stamped on
    // frame K are still PENDING (not yet applied). Snapshots also don't
    // capture the host-side event queue. So replay must re-enqueue every
    // event with frame in [anchor_frame, target_frame): the anchor frame
    // is INCLUSIVE on the lower bound, target frame is EXCLUSIVE on the
    // upper bound (we want to land at the start of target_frame, before
    // its events drain — same convention as the anchor itself).
    for (const auto& se : events_) {
        if (se.frame < chosen->frame) continue;
        if (se.frame >= reachable_target) break;
        size_t idx = static_cast<size_t>(se.frame - chosen->frame);
        if (idx < plan.events_per_frame.size()) {
            plan.events_per_frame[idx].push_back(se.event);
        }
    }
    return plan;
}

std::optional<RewindBuffer::Plan>
RewindBuffer::planRewindOneFrame(uint64_t now_frame) const {
    if (now_frame == 0) return std::nullopt;
    return planRewindTo(now_frame - 1);
}
