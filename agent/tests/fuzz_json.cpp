// Host fuzz harness for the agent's JSON parser — the attacker-facing surface
// on the device (every frame is parsed before auth on the very first frame).
// Build with sanitizers and run: it must never crash, hang, or read OOB on
// arbitrary bytes. Deterministic PRNG so runs are reproducible (no wall clock).
//
//   clang++ -std=gnu++17 -fno-exceptions -fno-rtti \
//       -fsanitize=address,undefined -I../source \
//       ../source/json.cpp fuzz_json.cpp -o /tmp/fuzz_json && /tmp/fuzz_json
#include <cstdint>
#include <cstdio>
#include <string>

#include "json.hpp"

namespace {
// xorshift64 — deterministic, no time/random syscalls.
uint64_t g_state = 0x9E3779B97F4A7C15ULL;
uint64_t Next() {
    g_state ^= g_state << 13;
    g_state ^= g_state >> 7;
    g_state ^= g_state << 17;
    return g_state;
}

const char kInteresting[] = "{}[]\":,0123456789.-+eEtfnul \t\n\\/u\x00\xff";

std::string RandomBytes(size_t n) {
    std::string s;
    s.reserve(n);
    for (size_t i = 0; i < n; i++) {
        uint64_t r = Next();
        // Bias toward JSON-significant bytes so we reach deeper parser states.
        if (r & 1)
            s += kInteresting[r % (sizeof(kInteresting) - 1)];
        else
            s += (char)(r >> 8);
    }
    return s;
}

// Mutate a valid seed to probe boundary conditions near well-formed input.
std::string Mutate(std::string s) {
    if (s.empty()) return s;
    int ops = (int)(Next() % 5) + 1;
    for (int i = 0; i < ops; i++) {
        if (s.empty()) break;
        size_t pos = Next() % s.size();
        switch (Next() % 3) {
            case 0: s[pos] = (char)(Next() & 0xff); break;                    // flip
            case 1: s.insert(pos, 1, kInteresting[Next() % (sizeof(kInteresting) - 1)]); break;
            case 2: s.erase(pos, 1); break;                                   // truncate
        }
    }
    return s;
}
}  // namespace

int main() {
    const char* seeds[] = {
        R"({"id":1,"cmd":"input","buttons":["A","B"],"sticks":{"lx":0.5}})",
        R"({"cmd":"hello","token":"abc","version":1,"bin":1048576})",
        R"([1,2,3,{"a":null,"b":true,"c":-1.5e10}])",
        R"({"deeply":{"nested":{"objects":{"go":{"here":[[[]]]}}}}})",
        "", "{", "[", "\"", "\"\\u", "\"\\uZZZZ\"", "1e", "-", "null",
    };

    size_t parsed = 0, total = 0;
    // 1) Pure random bytes of varying lengths.
    for (int i = 0; i < 200000; i++) {
        std::string s = RandomBytes(Next() % 256);
        json::Value v;
        bool ok = json::Value::parse(s, v);
        if (ok) { parsed++; (void)v.dump(); }  // round-trip must also not crash
        total++;
    }
    // 2) Mutations of valid seeds — the interesting boundary cases.
    for (const char* seed : seeds) {
        for (int i = 0; i < 20000; i++) {
            std::string s = Mutate(seed);
            json::Value v;
            if (json::Value::parse(s, v)) { parsed++; (void)v.dump(); }
            total++;
        }
    }
    std::printf("fuzz ok: %zu/%zu inputs parsed without crash/OOB\n", parsed, total);
    return 0;
}
