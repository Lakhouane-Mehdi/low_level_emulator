#include "Replay.hpp"

#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <variant>

// ---- minimal JSON helpers ----------------------------------------------
//
// The replay schema is closed (no nested arrays beyond events/checkpoints,
// no floats, no escaping beyond \\ and \"), so a tiny hand-rolled parser
// keeps us dep-free. If the schema ever grows beyond this we'd swap in
// nlohmann/json or similar.

namespace {

std::string hexBytes(const std::vector<uint8_t>& bytes) {
    static const char* H = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (uint8_t b : bytes) {
        out += H[(b >> 4) & 0xF];
        out += H[b & 0xF];
    }
    return out;
}

bool parseHex(const std::string& s, std::vector<uint8_t>& out) {
    if (s.size() % 2 != 0) return false;
    out.clear();
    out.reserve(s.size() / 2);
    auto nyb = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < s.size(); i += 2) {
        int hi = nyb(s[i]), lo = nyb(s[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return true;
}

// JSON encoder. We only need numbers, bools, strings, arrays, objects.
struct W {
    std::string out;
    int indent = 0;

    void pad() { for (int i = 0; i < indent; ++i) out += "  "; }
    void str(const std::string& s) {
        out += '"';
        for (char c : s) {
            if (c == '"' || c == '\\') { out += '\\'; out += c; }
            else if (c == '\n') out += "\\n";
            else                out += c;
        }
        out += '"';
    }
    void num(long long n) {
        char buf[32]; std::snprintf(buf, sizeof(buf), "%lld", n);
        out += buf;
    }
    void unum(unsigned long long n) {
        char buf[32]; std::snprintf(buf, sizeof(buf), "%llu", n);
        out += buf;
    }
    void hex(unsigned long long n) {
        char buf[32]; std::snprintf(buf, sizeof(buf), "0x%016llX", n);
        out += '"'; out += buf; out += '"';
    }
};

// JSON parser. Single-pass over an input string. Tracks position with
// `p`. Reports the offending offset on failure.
struct P {
    const std::string& s;
    size_t             p = 0;
    std::string        err;

    explicit P(const std::string& src) : s(src) {}

    void skipWs() {
        while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r')) ++p;
    }
    bool eof() const { return p >= s.size(); }
    char peek() { skipWs(); return eof() ? '\0' : s[p]; }
    bool eat(char c) { skipWs(); if (eof() || s[p] != c) return false; ++p; return true; }

    void fail(const std::string& msg) {
        if (!err.empty()) return;
        char buf[64]; std::snprintf(buf, sizeof(buf), " at offset %zu", p);
        err = msg + buf;
    }

    bool readString(std::string& out) {
        skipWs();
        if (!eat('"')) { fail("expected string"); return false; }
        out.clear();
        while (!eof() && s[p] != '"') {
            if (s[p] == '\\' && p + 1 < s.size()) {
                char e = s[p + 1];
                if (e == 'n') out += '\n';
                else if (e == 't') out += '\t';
                else              out += e;
                p += 2;
            } else {
                out += s[p++];
            }
        }
        if (!eat('"')) { fail("unterminated string"); return false; }
        return true;
    }

    bool readNumber(long long& n) {
        skipWs();
        size_t start = p;
        if (!eof() && (s[p] == '-' || s[p] == '+')) ++p;
        while (!eof() && std::isdigit(static_cast<unsigned char>(s[p]))) ++p;
        if (p == start) { fail("expected number"); return false; }
        n = std::strtoll(s.substr(start, p - start).c_str(), nullptr, 10);
        return true;
    }

    bool readBool(bool& b) {
        skipWs();
        if (s.compare(p, 4, "true")  == 0) { p += 4; b = true;  return true; }
        if (s.compare(p, 5, "false") == 0) { p += 5; b = false; return true; }
        fail("expected bool");
        return false;
    }

    bool readNull() {
        skipWs();
        if (s.compare(p, 4, "null") == 0) { p += 4; return true; }
        return false;
    }

    // Parse an object as key/value callbacks. Caller's onKey(name) returns
    // true if it consumed the value, false to skip. On skip we discard.
    template <class F>
    bool readObject(F&& onKey) {
        if (!eat('{')) { fail("expected '{'"); return false; }
        if (eat('}')) return true;
        while (true) {
            std::string name;
            if (!readString(name)) return false;
            if (!eat(':')) { fail("expected ':'"); return false; }
            if (!onKey(name)) {
                if (!skipValue()) return false;
            }
            if (eat(',')) continue;
            if (eat('}')) return true;
            fail("expected ',' or '}'");
            return false;
        }
    }

    template <class F>
    bool readArray(F&& onItem) {
        if (!eat('[')) { fail("expected '['"); return false; }
        if (eat(']')) return true;
        while (true) {
            if (!onItem()) return false;
            if (eat(',')) continue;
            if (eat(']')) return true;
            fail("expected ',' or ']'");
            return false;
        }
    }

    bool skipValue() {
        skipWs();
        if (eof()) { fail("unexpected EOF"); return false; }
        char c = s[p];
        if (c == '"') { std::string s2; return readString(s2); }
        if (c == '{') return readObject([this](const std::string&){ return false; });
        if (c == '[') return readArray([this](){ return skipValue(); });
        if (c == 't' || c == 'f') { bool b; return readBool(b); }
        if (c == 'n') { return readNull(); }
        long long n; return readNumber(n);
    }
};

void writeQuirks(W& w, const Chip8::Quirks& q) {
    w.out += "{\n"; w.indent++;
    auto kv = [&](const char* k, bool v, bool last) {
        w.pad(); w.str(k); w.out += ": "; w.out += v ? "true" : "false";
        w.out += last ? "\n" : ",\n";
    };
    kv("shift_in_place",     q.shift_in_place,     false);
    kv("load_store_no_inc",  q.load_store_no_inc,  false);
    kv("jump_with_offset_x", q.jump_with_offset_x, false);
    kv("vf_reset",           q.vf_reset,           false);
    kv("display_wait",       q.display_wait,       false);
    kv("clip_sprites",       q.clip_sprites,       false);
    kv("mx8_extensions",     q.mx8_extensions,     true);
    w.indent--; w.pad(); w.out += "}";
}

void writeEvent(W& w, const ReplayEvent& re) {
    w.out += "{ ";
    w.str("frame"); w.out += ": "; w.unum(re.frame); w.out += ", ";
    w.str("type");  w.out += ": ";

    std::visit([&](auto&& e) {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, InjectKeyEvent>) {
            w.str("InjectKey");
            w.out += ", "; w.str("key");  w.out += ": "; w.num(e.key);
            w.out += ", "; w.str("down"); w.out += ": "; w.out += e.down ? "true" : "false";
        }
        else if constexpr (std::is_same_v<T, WriteMemoryEvent>) {
            w.str("WriteMemory");
            w.out += ", "; w.str("addr");  w.out += ": "; w.num(e.addr);
            w.out += ", "; w.str("value"); w.out += ": "; w.num(e.value);
        }
        else if constexpr (std::is_same_v<T, WriteMemoryBlockEvent>) {
            w.str("WriteMemoryBlock");
            w.out += ", "; w.str("addr");      w.out += ": "; w.num(e.addr);
            w.out += ", "; w.str("bytes_hex"); w.out += ": "; w.str(hexBytes(e.bytes));
        }
        else if constexpr (std::is_same_v<T, SetPCEvent>) {
            w.str("SetPC");
            w.out += ", "; w.str("pc"); w.out += ": "; w.num(e.pc);
        }
        else if constexpr (std::is_same_v<T, ResetEvent>) {
            w.str("Reset");
        }
        else if constexpr (std::is_same_v<T, SetSeedEvent>) {
            w.str("SetSeed");
            w.out += ", "; w.str("seed"); w.out += ": "; w.unum(e.seed);
        }
        else {
            // Other events (breakpoints, watchpoints, LoadROM) are excluded
            // from replays by design — emit a recognizable but harmless
            // marker rather than crashing.
            w.str("Unsupported");
        }
    }, re.event);

    w.out += " }";
}

bool readEventsArray(P& p, std::vector<ReplayEvent>& out) {
    return p.readArray([&]() {
        ReplayEvent re;
        std::string type;
        // Slot for fields we discover before knowing the type.
        long long      addr = 0, value = 0, key = 0, pc = 0;
        long long      seed = 0;
        bool           down = false;
        std::string    bytes_hex;
        bool           saw_addr=false, saw_value=false, saw_key=false, saw_down=false;
        bool           saw_pc=false, saw_seed=false, saw_bytes=false;

        if (!p.readObject([&](const std::string& k) {
            if (k == "frame") { long long n; if (!p.readNumber(n)) return true; re.frame = (uint64_t)n; return true; }
            if (k == "type")  { return p.readString(type); }
            if (k == "key")   { long long n; if (!p.readNumber(n)) return true; key = n; saw_key = true; return true; }
            if (k == "down")  { bool b; if (!p.readBool(b)) return true; down = b; saw_down = true; return true; }
            if (k == "addr")  { long long n; if (!p.readNumber(n)) return true; addr = n; saw_addr = true; return true; }
            if (k == "value") { long long n; if (!p.readNumber(n)) return true; value = n; saw_value = true; return true; }
            if (k == "pc")    { long long n; if (!p.readNumber(n)) return true; pc = n; saw_pc = true; return true; }
            if (k == "seed")  { long long n; if (!p.readNumber(n)) return true; seed = n; saw_seed = true; return true; }
            if (k == "bytes_hex") { return p.readString(bytes_hex) && (saw_bytes = true); }
            return false;   // unknown key -> skip
        })) return false;

        if (type == "InjectKey" && saw_key && saw_down) {
            re.event = InjectKeyEvent{static_cast<uint8_t>(key), down};
        } else if (type == "WriteMemory" && saw_addr && saw_value) {
            re.event = WriteMemoryEvent{static_cast<uint16_t>(addr), static_cast<uint8_t>(value)};
        } else if (type == "WriteMemoryBlock" && saw_addr && saw_bytes) {
            std::vector<uint8_t> bytes;
            if (!parseHex(bytes_hex, bytes)) { p.fail("bad bytes_hex"); return false; }
            re.event = WriteMemoryBlockEvent{static_cast<uint16_t>(addr), std::move(bytes)};
        } else if (type == "SetPC" && saw_pc) {
            re.event = SetPCEvent{static_cast<uint16_t>(pc)};
        } else if (type == "Reset") {
            re.event = ResetEvent{};
        } else if (type == "SetSeed" && saw_seed) {
            re.event = SetSeedEvent{static_cast<uint64_t>(seed)};
        } else {
            // Unsupported / malformed — skip silently. Forward-compat:
            // a v2 replay may add event types this binary doesn't know.
            return true;
        }
        out.push_back(std::move(re));
        return true;
    });
}

bool readQuirksObject(P& p, Chip8::Quirks& q) {
    return p.readObject([&](const std::string& k) {
        bool b;
        if (!p.readBool(b)) return true;
        if      (k == "shift_in_place")     q.shift_in_place     = b;
        else if (k == "load_store_no_inc")  q.load_store_no_inc  = b;
        else if (k == "jump_with_offset_x") q.jump_with_offset_x = b;
        else if (k == "vf_reset")           q.vf_reset           = b;
        else if (k == "display_wait")       q.display_wait       = b;
        else if (k == "clip_sprites")       q.clip_sprites       = b;
        else if (k == "mx8_extensions")     q.mx8_extensions     = b;
        return true;
    });
}

} // namespace

// ---- Replay public API -------------------------------------------------

std::string Replay::toJson() const {
    W w;
    w.out += "{\n"; w.indent++;
    w.pad(); w.str("format");  w.out += ": ";  w.str("chip8-replay"); w.out += ",\n";
    w.pad(); w.str("version"); w.out += ": ";  w.num(version); w.out += ",\n";
    w.pad(); w.str("rom_bytes_hex"); w.out += ": "; w.str(hexBytes(rom_bytes)); w.out += ",\n";
    w.pad(); w.str("isa"); w.out += ": "; w.str(isa); w.out += ",\n";
    w.pad(); w.str("quirks"); w.out += ": "; writeQuirks(w, quirks); w.out += ",\n";
    w.pad(); w.str("seed"); w.out += ": ";
    if (have_seed) w.num(seed);
    else           w.out += "null";
    w.out += ",\n";

    w.pad(); w.str("events"); w.out += ": [";
    if (events.empty()) w.out += "]";
    else {
        w.out += "\n"; w.indent++;
        for (size_t i = 0; i < events.size(); ++i) {
            w.pad(); writeEvent(w, events[i]);
            w.out += (i + 1 == events.size()) ? "\n" : ",\n";
        }
        w.indent--; w.pad(); w.out += "]";
    }
    w.out += ",\n";

    w.pad(); w.str("checkpoints"); w.out += ": [";
    if (checkpoints.empty()) w.out += "]";
    else {
        w.out += "\n"; w.indent++;
        for (size_t i = 0; i < checkpoints.size(); ++i) {
            w.pad(); w.out += "{ ";
            w.str("frame"); w.out += ": "; w.unum(checkpoints[i].frame);
            w.out += ", "; w.str("hash"); w.out += ": "; w.hex(checkpoints[i].hash);
            w.out += " }";
            w.out += (i + 1 == checkpoints.size()) ? "\n" : ",\n";
        }
        w.indent--; w.pad(); w.out += "]";
    }
    w.out += "\n";

    w.indent--; w.out += "}\n";
    return w.out;
}

bool Replay::fromJson(const std::string& json, std::string* err) {
    *this = {};
    P p(json);
    bool ok = p.readObject([&](const std::string& k) {
        if (k == "format") { std::string s; return p.readString(s); }
        if (k == "version") { long long n; if (!p.readNumber(n)) return true; version = (int)n; return true; }
        if (k == "rom_bytes_hex") {
            std::string s;
            if (!p.readString(s)) return true;
            return parseHex(s, rom_bytes);
        }
        if (k == "isa")    { return p.readString(isa); }
        if (k == "quirks") { return readQuirksObject(p, quirks); }
        if (k == "seed") {
            // null OR number
            if (p.readNull()) { have_seed = false; return true; }
            long long n; if (!p.readNumber(n)) return true;
            seed = n; have_seed = true; return true;
        }
        if (k == "events") { return readEventsArray(p, events); }
        if (k == "checkpoints") {
            return p.readArray([&]() {
                ReplayCheckpoint cp;
                if (!p.readObject([&](const std::string& ck) {
                    if (ck == "frame") { long long n; if (!p.readNumber(n)) return true; cp.frame = (uint64_t)n; return true; }
                    if (ck == "hash")  {
                        std::string s; if (!p.readString(s)) return true;
                        cp.hash = std::strtoull(s.c_str(), nullptr, 0);
                        return true;
                    }
                    return false;
                })) return false;
                checkpoints.push_back(cp);
                return true;
            });
        }
        return false;
    });
    if (!ok && err) *err = p.err;
    return ok;
}

bool Replay::save(const std::string& path) const {
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "Replay::save: cannot open '%s'\n", path.c_str());
        return false;
    }
    f << toJson();
    return f.good();
}

bool Replay::load(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        std::fprintf(stderr, "Replay::load: cannot open '%s'\n", path.c_str());
        return false;
    }
    auto sz = f.tellg();
    f.seekg(0, std::ios::beg);
    std::string buf(static_cast<size_t>(sz), '\0');
    f.read(buf.data(), sz);
    std::string err;
    if (!fromJson(buf, &err)) {
        std::fprintf(stderr, "Replay::load: parse error: %s\n", err.c_str());
        return false;
    }
    if (version != VERSION) {
        std::fprintf(stderr,
            "Replay::load: warning, file version %d differs from current %d. "
            "Forward-compat best effort only.\n",
            version, VERSION);
    }
    return true;
}
