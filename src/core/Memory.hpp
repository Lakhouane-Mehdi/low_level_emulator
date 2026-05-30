#pragma once

#include <array>
#include <cstdint>
#include <cstddef>
#include <functional>
#include <unordered_map>
#include <vector>

// Bounds-checked, watchable memory for the CHIP-8 / SUPER-CHIP / MX-8 VM.
//
// Every byte of state crosses this API — no caller may hold a raw pointer or
// index into the underlying array. That contract is what makes watchpoints,
// audit logging, and future protection bits possible.
//
// On out-of-range access:
//   - read(addr)  returns 0 and signals a bad-access event
//   - write(addr) silently drops the write and signals a bad-access event
// The signal goes to a callback the owner installs (CPU calls halt()).
// Throwing was rejected because reads happen in tight opcode dispatch.
class Memory {
public:
    // XO-CHIP extends the address space to 16 bits (64KB). Classic CHIP-8 and
    // SUPER-CHIP ROMs only ever touch the low 4KB, so they are unaffected:
    // the extra space is simply never addressed. Snapshots grow to match, but
    // the active framebuffer hash (the value golden tests pin) is unchanged
    // because it only covers the visible display region, never RAM.
    static constexpr std::size_t SIZE = 65536;

    enum class WatchKind : uint8_t { Read = 1, Write = 2, Both = 3 };

    enum class FaultKind : uint8_t { None, OutOfRangeRead, OutOfRangeWrite };

    struct WatchEvent {
        uint16_t  addr   = 0;
        uint8_t   value  = 0;     // value being written, or value just read
        WatchKind kind   = WatchKind::Read;
    };

    using BadAccessCallback = std::function<void(FaultKind, uint16_t addr)>;

    Memory();

    // ---- single-byte access (the hot path) ----
    // read() / write() fire watchpoints and bad-access callbacks.
    // peek() is the debugger / disassembler accessor: const, no side
    // effects, no watchpoint trigger, returns 0 on out-of-range.
    uint8_t read(uint16_t addr);
    void    write(uint16_t addr, uint8_t value);
    uint8_t peek(uint16_t addr) const;

    // ---- block access ----
    // read_block fills `out` with bytes [addr .. addr+n). On OOR it stops
    // at the boundary, signals once, and returns the count actually read.
    //
    // Watchpoint semantics for blocks: every byte in the block is *checked*
    // against the watchpoint table (Option A — per-byte). The block runs to
    // completion regardless, and the FIRST hit (if any) is recorded as the
    // event the App consumes. Later hits within the same block are
    // suppressed until consume_watch_event() runs. This preserves opcode
    // atomicity (an FX55 doesn't tear) while still giving the debugger a
    // deterministic, non-lossy first-hit signal.
    std::size_t read_block(uint16_t addr, uint8_t* out, std::size_t n);
    std::size_t write_block(uint16_t addr, const uint8_t* in, std::size_t n);

    // Convenience overloads.
    std::vector<uint8_t> read_block(uint16_t addr, std::size_t n);
    std::size_t write_block(uint16_t addr, const std::vector<uint8_t>& bytes) {
        return write_block(addr, bytes.data(), bytes.size());
    }

    // Wipe to zeros. Does NOT clear watchpoints (they're a debugger
    // concern that survives a CPU reset).
    void clear();

    // ---- watchpoints ----
    void add_watchpoint(uint16_t addr, WatchKind kind = WatchKind::Both);
    void remove_watchpoint(uint16_t addr);
    void clear_watchpoints();
    bool has_watchpoint(uint16_t addr) const;
    const std::unordered_map<uint16_t, WatchKind>& watchpoints() const { return watchpoints_; }

    // True if a watchpoint fired since the last call to consume_watch_event().
    bool watch_triggered() const { return triggered_; }
    WatchEvent consume_watch_event();

    // Owner installs this once; called whenever an OOR read or write happens.
    void set_bad_access_callback(BadAccessCallback cb) { bad_cb_ = std::move(cb); }

    // ---- snapshot helpers (round-trip without exposing raw storage) ----
    // Copies the full backing buffer for snapshot save.
    void snapshot_to(std::array<uint8_t, SIZE>& out) const;
    // Restores from snapshot. Skips watchpoints firing — restoration is not
    // a debugger-relevant write event.
    void restore_from(const std::array<uint8_t, SIZE>& in);

    static constexpr std::size_t size() { return SIZE; }

private:
    std::array<uint8_t, SIZE>                    bytes_{};
    std::unordered_map<uint16_t, WatchKind>      watchpoints_;
    bool                                         triggered_  = false;
    WatchEvent                                   last_event_{};
    BadAccessCallback                            bad_cb_;

    void note_watch(uint16_t addr, uint8_t value, WatchKind kind);
    void note_bad(FaultKind kind, uint16_t addr);
};
