// Event waiting and macro recording.
//
// Both exist to remove polling loops from the client, which on this transport
// are expensive in a way that is easy to underestimate: every poll is a Wi-Fi
// round-trip, and for an LLM operator every poll is also a tool call and a
// context entry. "Wait until the game finishes loading" as 40 screenshots costs
// far more than the answer is worth.
//
// So the waiting happens here, on the device, and one request returns one
// answer.
#include "../protocol.hpp"

#include <switch.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../log.hpp"

namespace agent {
namespace handlers {

namespace {

// The service thread is single-threaded, so a wait blocks every other command
// for its duration. Bounded well below the client's per-command deadline so a
// wait always returns an answer rather than tripping a timeout.
constexpr int kMaxWaitMs = 20000;
constexpr u64 kPollNs = 200'000'000ULL;  // 200 ms

bool AppRunning(u64* out_pid) {
    u64 pid = 0;
    bool ok = R_SUCCEEDED(pmdmntGetApplicationProcessId(&pid)) && pid != 0;
    if (out_pid) *out_pid = pid;
    return ok;
}

// Read the real controller — the physical one, or a virtual pad if attached.
// Handheld first because that is how this console is usually used.
bool ReadPad(u64* buttons, HidAnalogStickState* l, HidAnalogStickState* r) {
    HidNpadCommonState st{};
    if (hidGetNpadStatesHandheld(HidNpadIdType_Handheld, &st, 1) > 0 &&
        st.sampling_number) {
        *buttons = st.buttons;
        *l = st.analog_stick_l;
        *r = st.analog_stick_r;
        return true;
    }
    if (hidGetNpadStatesFullKey(HidNpadIdType_No1, &st, 1) > 0) {
        *buttons = st.buttons;
        *l = st.analog_stick_l;
        *r = st.analog_stick_r;
        return true;
    }
    return false;
}

const char* kButtonNames[] = {
    "A", "B", "X", "Y", "LSTICK", "RSTICK", "L", "R", "ZL", "ZR",
    "PLUS", "MINUS", "DLEFT", "DUP", "DRIGHT", "DDOWN",
};

json::Value ButtonsToArray(u64 buttons) {
    json::Value arr = json::Value::array();
    for (int i = 0; i < 16; i++)
        if (buttons & (1ULL << i)) arr.push(json::Value(kButtonNames[i]));
    return arr;
}

}  // namespace

// Block until something happens, or the timeout expires.
//
// One call replaces a polling loop. Which event to wait for is explicit so the
// reply is unambiguous — "something changed" would just move the guessing to
// the caller.
bool WaitEvent(const Request& req, Reply& reply) {
    std::string what = req["event"].as_string("app_change");
    int timeout_ms = (int)req["timeout_ms"].as_int(10000);
    if (timeout_ms < 100) timeout_ms = 100;
    if (timeout_ms > kMaxWaitMs) timeout_ms = kMaxWaitMs;

    u64 start_pid = 0;
    bool start_running = AppRunning(&start_pid);
    u64 start_buttons = 0;
    HidAnalogStickState sl{}, sr{};
    ReadPad(&start_buttons, &sl, &sr);

    if (what == "app_start" && start_running) {
        reply.json.set("fired", true);
        reply.json.set("event", what);
        reply.json.set("waited_ms", (int64_t)0);
        reply.json.set("running", true);
        reply.json.set("pid", (int64_t)start_pid);
        reply.json.set("note", "condition already held; returned immediately");
        return true;
    }
    if (what == "app_exit" && !start_running) {
        reply.json.set("fired", true);
        reply.json.set("event", what);
        reply.json.set("waited_ms", (int64_t)0);
        reply.json.set("running", false);
        reply.json.set("note", "condition already held; returned immediately");
        return true;
    }

    int waited = 0;
    while (waited < timeout_ms) {
        svcSleepThread(kPollNs);
        waited += (int)(kPollNs / 1'000'000ULL);

        if (what == "app_start" || what == "app_exit" || what == "app_change") {
            u64 pid = 0;
            bool running = AppRunning(&pid);
            bool fired = false;
            // app_start/app_exit are LEVEL-triggered: they answer "is an app
            // running?", not "did one just start?". Edge semantics are unusable
            // here, because the agent serves one command at a time — you cannot
            // trigger a transition while a wait is blocking the only connection,
            // so the caller must launch first and then wait, by which point the
            // edge has already passed. Level-triggered returns immediately if
            // the condition already holds, which is what callers actually want.
            if (what == "app_start") fired = running;
            else if (what == "app_exit") fired = !running;
            else fired = (running != start_running) || (running && pid != start_pid);
            if (fired) {
                reply.json.set("fired", true);
                reply.json.set("event", what);
                reply.json.set("waited_ms", (int64_t)waited);
                reply.json.set("running", running);
                if (running) reply.json.set("pid", (int64_t)pid);
                return true;
            }
        } else if (what == "button") {
            u64 b = 0;
            HidAnalogStickState l{}, r{};
            if (ReadPad(&b, &l, &r) && b && b != start_buttons) {
                reply.json.set("fired", true);
                reply.json.set("event", what);
                reply.json.set("waited_ms", (int64_t)waited);
                reply.json.set("buttons", ButtonsToArray(b));
                return true;
            }
        } else {
            return Fail(reply, "bad_arg",
                        "event must be app_start, app_exit, app_change or button");
        }
    }

    // Not firing is a real answer, not a failure: it is how a caller learns an
    // action did not take effect.
    reply.json.set("fired", false);
    reply.json.set("event", what);
    reply.json.set("waited_ms", (int64_t)waited);
    reply.json.set("note", "timed out; nothing happened within the window");
    return true;
}

// Record real controller input into a replayable sequence.
//
// Reading the physical pad turns "describe the button presses you want" into
// "do it once and keep it". The output is the exact shape send_input_sequence
// consumes, so a recording can be replayed without editing.
bool RecordInput(const Request& req, Reply& reply) {
    int duration_ms = (int)req["duration_ms"].as_int(5000);
    if (duration_ms < 200) duration_ms = 200;
    if (duration_ms > kMaxWaitMs) duration_ms = kMaxWaitMs;
    // 50 ms sampling: fast enough to catch a deliberate press, slow enough that
    // a 20 s recording stays a few hundred samples rather than thousands.
    const int step_ms = 50;

    json::Value steps = json::Value::array();
    u64 prev = 0;
    int idle_ms = 0, elapsed = 0, count = 0;
    while (elapsed < duration_ms) {
        svcSleepThread((u64)step_ms * 1'000'000ULL);
        elapsed += step_ms;

        u64 b = 0;
        HidAnalogStickState l{}, r{};
        if (!ReadPad(&b, &l, &r)) { idle_ms += step_ms; continue; }

        if (b == prev) { idle_ms += step_ms; continue; }
        if (b) {
            // Only transitions to a pressed state become steps; the gap since
            // the previous one is carried as wait_ms so timing is preserved.
            json::Value s = json::Value::object();
            s.set("buttons", ButtonsToArray(b));
            s.set("hold_ms", (int64_t)step_ms * 2);
            s.set("wait_ms", (int64_t)idle_ms);
            steps.push(std::move(s));
            count++;
            idle_ms = 0;
        }
        prev = b;
    }

    reply.json.set("recorded_ms", (int64_t)elapsed);
    reply.json.set("steps", (int64_t)count);
    reply.json.set("sequence", std::move(steps));
    reply.json.set("note",
                   "pass 'sequence' straight to send_input_sequence to replay it");
    return true;
}

// Current physical controller state — what the human is holding right now.
bool ReadInput(const Request& req, Reply& reply) {
    (void)req;
    u64 b = 0;
    HidAnalogStickState l{}, r{};
    if (!ReadPad(&b, &l, &r))
        return Fail(reply, "no_pad", "no controller state available");
    reply.json.set("buttons", ButtonsToArray(b));
    reply.json.set("raw", (int64_t)b);
    json::Value ls = json::Value::object();
    ls.set("x", (int64_t)l.x);
    ls.set("y", (int64_t)l.y);
    json::Value rs = json::Value::object();
    rs.set("x", (int64_t)r.x);
    rs.set("y", (int64_t)r.y);
    reply.json.set("left_stick", std::move(ls));
    reply.json.set("right_stick", std::move(rs));
    return true;
}

}  // namespace handlers
}  // namespace agent
