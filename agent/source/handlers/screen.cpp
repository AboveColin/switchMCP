// Screenshot handler. capssc captures the composited screen straight to JPEG,
// which is ideal for a memory-constrained sysmodule (no on-device re-encoding).
#include "../protocol.hpp"

#include <switch.h>

#include <vector>

#include "../log.hpp"

namespace agent {
namespace handlers {

bool Screenshot(const Request& req, Reply& reply) {
    (void)req;
    // Screen is 1280x720 RGBA-composited; JPEG is comfortably under 512 KiB.
    constexpr size_t kBufSize = 512 * 1024;
    std::vector<uint8_t> buf(kBufSize);
    uint64_t out_size = 0;

    // Different layer stacks are capturable depending on what's on screen
    // (game vs applet vs HOME). Default (all layers) captures the HOME menu and
    // applets; ApplicationForDebug captures a running game. Timeout is a SIGNED
    // s64 in ns — must be finite (UINT64_MAX becomes -1 and is rejected).
    static const ViLayerStack kStacks[] = {
        ViLayerStack_Default,
        ViLayerStack_ApplicationForDebug,
        ViLayerStack_Screenshot,
    };
    constexpr s64 kTimeoutNs = 1'000'000'000;  // 1 s

    Result rc = 1;
    for (ViLayerStack stack : kStacks) {
        rc = capsscCaptureJpegScreenShot(&out_size, buf.data(), buf.size(), stack,
                                         kTimeoutNs);
        if (R_SUCCEEDED(rc) && out_size > 0) break;
        LOG_DEBUG("capssc stack %d rc=0x%x size=%llu", (int)stack, rc,
                  (unsigned long long)out_size);
    }
    if (R_FAILED(rc) || out_size == 0) {
        LOG_WARN("screenshot failed rc=0x%x", rc);
        return Fail(reply, "capture_failed",
                    "no capturable layer (screen may be protected)");
    }

    buf.resize(out_size);
    reply.out = std::move(buf);
    reply.json.set("jpeg", true);
    reply.json.set("width", 1280);
    reply.json.set("height", 720);
    return true;
}

}  // namespace handlers
}  // namespace agent
