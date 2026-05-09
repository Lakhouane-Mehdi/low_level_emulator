#include "Memory.hpp"

#include <algorithm>
#include <cstring>

Memory::Memory() {
    bytes_.fill(0);
}

void Memory::clear() {
    bytes_.fill(0);
}

void Memory::note_watch(uint16_t addr, uint8_t value, WatchKind kind) {
    auto it = watchpoints_.find(addr);
    if (it == watchpoints_.end()) return;
    const uint8_t want = static_cast<uint8_t>(it->second);
    const uint8_t have = static_cast<uint8_t>(kind);
    if ((want & have) == 0) return;
    // Per-byte detection (Option A): every byte in a block is checked. But
    // we keep only the FIRST hit until the App consumes it — later hits in
    // the same opcode are dropped. Rationale: the App pauses on the first
    // event after the opcode completes (atomicity preserved), inspects, then
    // resumes; later hits in the same block would be redundant signals about
    // the same instruction the user is already paused on.
    if (!triggered_) {
        triggered_  = true;
        last_event_ = {addr, value, kind};
    }
}

void Memory::note_bad(FaultKind kind, uint16_t addr) {
    if (bad_cb_) bad_cb_(kind, addr);
}

uint8_t Memory::read(uint16_t addr) {
    if (addr >= SIZE) {
        note_bad(FaultKind::OutOfRangeRead, addr);
        return 0;
    }
    uint8_t v = bytes_[addr];
    note_watch(addr, v, WatchKind::Read);
    return v;
}

uint8_t Memory::peek(uint16_t addr) const {
    if (addr >= SIZE) return 0;
    return bytes_[addr];
}

void Memory::write(uint16_t addr, uint8_t value) {
    if (addr >= SIZE) {
        note_bad(FaultKind::OutOfRangeWrite, addr);
        return;
    }
    bytes_[addr] = value;
    note_watch(addr, value, WatchKind::Write);
}

std::size_t Memory::read_block(uint16_t addr, uint8_t* out, std::size_t n) {
    if (n == 0) return 0;
    if (addr >= SIZE) { note_bad(FaultKind::OutOfRangeRead, addr); return 0; }
    std::size_t avail = SIZE - addr;
    std::size_t copy  = std::min(n, avail);
    if (copy < n) note_bad(FaultKind::OutOfRangeRead, static_cast<uint16_t>(addr + copy));
    // Per-byte copy so watchpoints fire on every read in the block. That
    // matches user intent: a watch on 0x300 should trigger even if the read
    // is part of an 8-byte FX65.
    for (std::size_t i = 0; i < copy; ++i) {
        uint8_t v = bytes_[addr + i];
        out[i] = v;
        note_watch(static_cast<uint16_t>(addr + i), v, WatchKind::Read);
    }
    return copy;
}

std::size_t Memory::write_block(uint16_t addr, const uint8_t* in, std::size_t n) {
    if (n == 0) return 0;
    if (addr >= SIZE) { note_bad(FaultKind::OutOfRangeWrite, addr); return 0; }
    std::size_t avail = SIZE - addr;
    std::size_t copy  = std::min(n, avail);
    if (copy < n) note_bad(FaultKind::OutOfRangeWrite, static_cast<uint16_t>(addr + copy));
    for (std::size_t i = 0; i < copy; ++i) {
        bytes_[addr + i] = in[i];
        note_watch(static_cast<uint16_t>(addr + i), in[i], WatchKind::Write);
    }
    return copy;
}

std::vector<uint8_t> Memory::read_block(uint16_t addr, std::size_t n) {
    std::vector<uint8_t> v(n, 0);
    std::size_t got = read_block(addr, v.data(), n);
    v.resize(got);
    return v;
}

void Memory::add_watchpoint(uint16_t addr, WatchKind kind) {
    watchpoints_[addr] = kind;
}

void Memory::remove_watchpoint(uint16_t addr) {
    watchpoints_.erase(addr);
}

void Memory::clear_watchpoints() {
    watchpoints_.clear();
}

bool Memory::has_watchpoint(uint16_t addr) const {
    return watchpoints_.find(addr) != watchpoints_.end();
}

Memory::WatchEvent Memory::consume_watch_event() {
    triggered_ = false;
    return last_event_;
}

void Memory::snapshot_to(std::array<uint8_t, SIZE>& out) const {
    std::memcpy(out.data(), bytes_.data(), SIZE);
}

void Memory::restore_from(const std::array<uint8_t, SIZE>& in) {
    std::memcpy(bytes_.data(), in.data(), SIZE);
    // Restore intentionally skips watchpoint signaling — see header.
}
