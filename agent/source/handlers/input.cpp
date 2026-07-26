// Input injection via hiddbg HDLS (virtual Pro Controller) and touchscreen
// auto-pilot. NOTE: the hiddbg HDLS API surface shifts between libnx releases;
// this targets libnx 4.x (the same calls sys-botbase uses). Adjust struct field
// names if your toolchain differs.
#include "../protocol.hpp"

#include <switch.h>

#include <cstring>
#include <map>
#include <string>

#include "../log.hpp"

namespace agent {
namespace vpad {

namespace {
bool g_attached = false;
HiddbgHdlsSessionId g_session = {0};
HiddbgHdlsHandle g_handle = {0};
HiddbgHdlsState g_state = {0};
HiddbgHdlsDeviceInfo g_device = {0};
// HDLS work buffer must be page-aligned; one page is plenty for a single pad.
alignas(0x1000) u8 g_workbuf[0x1000];
}  // namespace

bool Ensure() {
    if (g_attached) return true;
    if (R_FAILED(hiddbgAttachHdlsWorkBuffer(&g_session, g_workbuf, sizeof(g_workbuf)))) {
        LOG_ERROR("hiddbgAttachHdlsWorkBuffer failed");
        return false;
    }
    g_device.deviceType = HidDeviceType_FullKey3;
    g_device.npadInterfaceType = HidNpadInterfaceType_Bluetooth;
    g_device.singleColorBody = 0xFFFFFFFF;
    g_device.singleColorButtons = 0xFF000000;
    g_device.colorLeftGrip = 0xFF666666;
    g_device.colorRightGrip = 0xFF666666;

    g_state.battery_level = 4;  // full
    g_state.flags = 0;

    if (R_FAILED(hiddbgAttachHdlsVirtualDevice(&g_handle, &g_device))) {
        LOG_ERROR("hiddbgAttachHdlsVirtualDevice failed");
        hiddbgReleaseHdlsWorkBuffer(g_session);
        return false;
    }
    g_attached = true;
    LOG_INFO("virtual controller attached");
    return true;
}

void Release() {
    if (!g_attached) return;
    // Zero the state first so no phantom buttons are left held.
    std::memset(&g_state.buttons, 0, sizeof(g_state.buttons));
    hiddbgSetHdlsState(g_handle, &g_state);
    hiddbgDetachHdlsVirtualDevice(g_handle);
    hiddbgReleaseHdlsWorkBuffer(g_session);
    g_attached = false;
    LOG_INFO("virtual controller released");
}

bool SetButtons(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    if (!Ensure()) return false;
    g_state.buttons = buttons;
    g_state.analog_stick_l.x = lx;
    g_state.analog_stick_l.y = ly;
    g_state.analog_stick_r.x = rx;
    g_state.analog_stick_r.y = ry;
    return R_SUCCEEDED(hiddbgSetHdlsState(g_handle, &g_state));
}

}  // namespace vpad

namespace handlers {

namespace {

uint64_t ButtonMask(const std::string& name) {
    static const std::map<std::string, uint64_t> map = {
        {"A", HidNpadButton_A},         {"B", HidNpadButton_B},
        {"X", HidNpadButton_X},         {"Y", HidNpadButton_Y},
        {"L", HidNpadButton_L},         {"R", HidNpadButton_R},
        {"ZL", HidNpadButton_ZL},       {"ZR", HidNpadButton_ZR},
        {"PLUS", HidNpadButton_Plus},   {"MINUS", HidNpadButton_Minus},
        {"DUP", HidNpadButton_Up},      {"DDOWN", HidNpadButton_Down},
        {"DLEFT", HidNpadButton_Left},  {"DRIGHT", HidNpadButton_Right},
        {"LSTICK", HidNpadButton_StickL}, {"RSTICK", HidNpadButton_StickR},
    };
    auto it = map.find(name);
    return it == map.end() ? 0 : it->second;
}

int32_t StickAxis(double v) {
    if (v > 1.0) v = 1.0;
    if (v < -1.0) v = -1.0;
    return (int32_t)(v * 32767.0);
}

}  // namespace

bool Input(const Request& req, Reply& reply) {
    if (!vpad::Ensure()) return Fail(reply, "hdls_failed", "cannot attach virtual pad");

    uint64_t mask = 0;
    for (const auto& b : req["buttons"].as_array())
        mask |= ButtonMask(b.as_string());

    const json::Value& sticks = req["sticks"];
    int32_t lx = StickAxis(sticks["lx"].as_double(0));
    int32_t ly = StickAxis(sticks["ly"].as_double(0));
    int32_t rx = StickAxis(sticks["rx"].as_double(0));
    int32_t ry = StickAxis(sticks["ry"].as_double(0));

    int64_t hold_ms = req["hold_ms"].as_int(100);
    if (hold_ms < 0) hold_ms = 0;
    if (hold_ms > 10000) hold_ms = 10000;  // cap held input at 10 s

    // Press, hold, release.
    vpad::SetButtons(mask, lx, ly, rx, ry);
    svcSleepThread((uint64_t)hold_ms * 1'000'000ULL);
    vpad::SetButtons(0, 0, 0, 0, 0);

    reply.json.set("ok", true);
    return true;
}

namespace {
// Map an ASCII char to a USB HID keyboard usage id + whether Shift is needed.
// Covers the printable ASCII the software keyboard accepts.
bool CharToKey(char c, uint8_t& key, bool& shift) {
    shift = false;
    if (c >= 'a' && c <= 'z') { key = 4 + (c - 'a'); return true; }
    if (c >= 'A' && c <= 'Z') { key = 4 + (c - 'A'); shift = true; return true; }
    if (c >= '1' && c <= '9') { key = 30 + (c - '1'); return true; }
    if (c == '0') { key = 39; return true; }
    switch (c) {
        case ' ': key = 44; return true;
        case '\n': key = 40; return true;  // Enter
        case '-': key = 45; return true;
        case '_': key = 45; shift = true; return true;
        case '=': key = 46; return true;
        case '+': key = 46; shift = true; return true;
        case '[': key = 47; return true;
        case ']': key = 48; return true;
        case '\\': key = 49; return true;
        case ';': key = 51; return true;
        case ':': key = 51; shift = true; return true;
        case '\'': key = 52; return true;
        case '"': key = 52; shift = true; return true;
        case ',': key = 54; return true;
        case '<': key = 54; shift = true; return true;
        case '.': key = 55; return true;
        case '>': key = 55; shift = true; return true;
        case '/': key = 56; return true;
        case '?': key = 56; shift = true; return true;
        case '!': key = 30; shift = true; return true;
        case '@': key = 31; shift = true; return true;
        case '#': key = 32; shift = true; return true;
        case '$': key = 33; shift = true; return true;
        case '%': key = 34; shift = true; return true;
        case '^': key = 35; shift = true; return true;
        case '&': key = 36; shift = true; return true;
        case '*': key = 37; shift = true; return true;
        case '(': key = 38; shift = true; return true;
        case ')': key = 39; shift = true; return true;
    }
    return false;
}
}  // namespace

// Type a UTF-8/ASCII string via the virtual keyboard (hiddbg keyboard
// autopilot). Works wherever the system software keyboard accepts USB input.
bool TypeText(const Request& req, Reply& reply) {
    const std::string& text = req["text"].as_string();
    if (text.empty()) return Fail(reply, "bad_arg", "missing text");
    int64_t key_ms = req["key_ms"].as_int(40);
    if (key_ms < 10) key_ms = 10;
    if (key_ms > 500) key_ms = 500;

    size_t typed = 0;
    for (char c : text) {
        uint8_t key;
        bool shift;
        if (!CharToKey(c, key, shift)) continue;  // skip unsupported chars
        HiddbgKeyboardAutoPilotState st = {0};
        st.modifiers = shift ? HidKeyboardModifier_Shift : 0;
        st.keys[key / 64] |= (1ULL << (key % 64));
        if (R_FAILED(hiddbgSetKeyboardAutoPilotState(&st)))
            return Fail(reply, "kbd_failed", "keyboard auto-pilot state failed");
        svcSleepThread((uint64_t)key_ms * 1'000'000ULL);
        HiddbgKeyboardAutoPilotState clear = {0};
        hiddbgSetKeyboardAutoPilotState(&clear);
        svcSleepThread((uint64_t)key_ms * 1'000'000ULL);
        typed++;
    }
    hiddbgUnsetKeyboardAutoPilotState();
    reply.json.set("ok", true);
    reply.json.set("typed", (int64_t)typed);
    return true;
}

bool Touch(const Request& req, Reply& reply) {
    // Single tap or swipe. Coordinates are in screen pixels (1280x720 handheld).
    int32_t x = (int32_t)req["x"].as_int(0);
    int32_t y = (int32_t)req["y"].as_int(0);
    int32_t x2 = (int32_t)req["x2"].as_int(x);
    int32_t y2 = (int32_t)req["y2"].as_int(y);
    int64_t duration_ms = req["duration_ms"].as_int(50);
    if (duration_ms < 10) duration_ms = 10;
    if (duration_ms > 5000) duration_ms = 5000;

    const int steps = 10;
    for (int i = 0; i <= steps; i++) {
        HidTouchState touch = {0};
        touch.x = x + (x2 - x) * i / steps;
        touch.y = y + (y2 - y) * i / steps;
        touch.diameter_x = 15;
        touch.diameter_y = 15;
        if (R_FAILED(hiddbgSetTouchScreenAutoPilotState(&touch, 1)))
            return Fail(reply, "touch_failed", "auto-pilot state failed");
        svcSleepThread((uint64_t)duration_ms * 1'000'000ULL / steps);
    }
    hiddbgUnsetTouchScreenAutoPilotState();
    reply.json.set("ok", true);
    return true;
}

}  // namespace handlers
}  // namespace agent
