# Agent tests

Host-side tests that don't need a console or devkitPro (plain clang++).

## JSON parser fuzz

The parser runs on every inbound frame *before* auth, so it's the main
attacker-facing surface. Fuzz it under sanitizers:

```sh
clang++ -std=gnu++17 -fno-exceptions -fno-rtti \
    -fsanitize=address,undefined -O1 -I../source \
    ../source/json.cpp fuzz_json.cpp -o /tmp/fuzz_json && /tmp/fuzz_json
```

Deterministic (seeded PRNG, no wall clock), so failures reproduce. Must print
`fuzz ok: …` and exit 0 — any ASan/UBSan report is a real bug.

On-hardware runtime checks live in `../../server/tests/live_smoke.py` and
`soak.py` (they drive the device through the MCP client).
