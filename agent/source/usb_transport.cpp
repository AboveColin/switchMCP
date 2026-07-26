// USB bulk transport (usb:ds) — a second backend for the same frame protocol.
//
// Why: measured Wi-Fi throughput on this console is 2.8 MB/s up and 4.1 MB/s
// down, and the round-trip is 22.8 ms. Profiling showed the protocol is NOT the
// bottleneck (latency is ~9% of a 4 MiB transfer) — the link is. USB bulk is
// the only change that moves that ceiling, which is why this exists and why
// chunk-pipelining was dropped as not worth doing.
//
// The wire format is byte-identical to the TCP transport, so every handler,
// every command and the whole client work unchanged; only the byte pump
// differs.
//
// ============================ UNVERIFIED =====================================
// This has NOT been tested against real hardware — it needs a USB cable and a
// host-side libusb client that does not exist yet. It is therefore:
//   * OFF by default (enable_usb in config.ini), and
//   * initialised LAZILY, never during __appInit.
// Both deliberately: a boot-time USB init that fails would stop the sysmodule
// starting, and this session has already had one change do exactly that, with
// no log, no crash report, and recovery only by physically removing the SD
// card. An unverified transport must not be able to cost that again.
// =============================================================================
#include "protocol.hpp"

#include <switch.h>

#include <cstring>
#include <vector>

#include "log.hpp"

namespace agent {
namespace usb_transport {

namespace {

UsbDsInterface* g_interface = nullptr;
UsbDsEndpoint* g_ep_in = nullptr;   // device -> host
UsbDsEndpoint* g_ep_out = nullptr;  // host -> device
bool g_ready = false;

// usb:ds requires transfer buffers to be page-aligned.
alignas(0x1000) u8 g_bounce[0x1000];

// Vendor/product reported to the host. 0x057E is Nintendo; the product id is
// arbitrary and simply has to be something a host client can match on.
constexpr u16 kVendorId = 0x057E;
constexpr u16 kProductId = 0xAE57;

bool SetupDescriptors() {
    UsbDsDeviceInfo info = {};
    info.idVendor = kVendorId;
    info.idProduct = kProductId;
    info.bcdDevice = 0x0100;
    std::strcpy(info.Manufacturer, "switch-agentd");
    std::strcpy(info.Product, "switch-agentd bulk");
    std::strcpy(info.SerialNumber, "0001");
    if (R_FAILED(usbDsSetVidPidBcd(&info))) return false;

    struct usb_interface_descriptor interface_descriptor = {};
    interface_descriptor.bLength = USB_DT_INTERFACE_SIZE;
    interface_descriptor.bDescriptorType = USB_DT_INTERFACE;
    interface_descriptor.bInterfaceNumber = 0;
    interface_descriptor.bNumEndpoints = 2;
    interface_descriptor.bInterfaceClass = USB_CLASS_VENDOR_SPEC;
    interface_descriptor.bInterfaceSubClass = USB_CLASS_VENDOR_SPEC;
    interface_descriptor.bInterfaceProtocol = USB_CLASS_VENDOR_SPEC;

    if (R_FAILED(usbDsGetDsInterface(&g_interface, &interface_descriptor, "agentd")))
        return false;

    struct usb_endpoint_descriptor in_desc = {};
    in_desc.bLength = USB_DT_ENDPOINT_SIZE;
    in_desc.bDescriptorType = USB_DT_ENDPOINT;
    in_desc.bEndpointAddress = USB_ENDPOINT_IN | 1;
    in_desc.bmAttributes = USB_TRANSFER_TYPE_BULK;
    in_desc.wMaxPacketSize = 0x200;

    struct usb_endpoint_descriptor out_desc = in_desc;
    out_desc.bEndpointAddress = USB_ENDPOINT_OUT | 1;

    if (R_FAILED(usbDsInterface_RegisterEndpoint(g_interface, &g_ep_in,
                                                 in_desc.bEndpointAddress)))
        return false;
    if (R_FAILED(usbDsInterface_RegisterEndpoint(g_interface, &g_ep_out,
                                                 out_desc.bEndpointAddress)))
        return false;
    return R_SUCCEEDED(usbDsInterface_EnableInterface(g_interface));
}

// One bulk transfer, page-aligned via a bounce buffer when needed.
bool Transfer(UsbDsEndpoint* ep, void* data, size_t size, size_t* transferred,
              u64 timeout_ns) {
    if (!ep) return false;
    size_t done = 0;
    while (done < size) {
        size_t chunk = size - done;
        if (chunk > sizeof(g_bounce)) chunk = sizeof(g_bounce);
        bool out = (ep == g_ep_in);
        if (out) std::memcpy(g_bounce, (u8*)data + done, chunk);

        u32 urb = 0;
        if (R_FAILED(usbDsEndpoint_PostBufferAsync(ep, g_bounce, chunk, &urb)))
            return false;
        if (R_FAILED(eventWait(&ep->CompletionEvent, timeout_ns))) {
            usbDsEndpoint_Cancel(ep);
            return false;
        }
        eventClear(&ep->CompletionEvent);

        UsbDsReportData report = {};
        if (R_FAILED(usbDsEndpoint_GetReportData(ep, &report))) return false;
        u32 requested = 0, moved = 0;
        if (R_FAILED(usbDsParseReportData(&report, urb, &requested, &moved)))
            return false;
        if (!out) std::memcpy((u8*)data + done, g_bounce, moved);
        done += moved;
        if (moved < chunk) break;   // short packet ends the transfer
    }
    if (transferred) *transferred = done;
    return true;
}

}  // namespace

bool Ready() { return g_ready; }

// Lazy init. Returns false — harmlessly — if USB is unavailable or disabled.
bool Init(const AgentConfig& cfg) {
    if (g_ready) return true;
    if (!cfg.enable_usb) return false;
    if (R_FAILED(usbDsInitialize())) {
        LOG_WARN("usb: usbDsInitialize failed; USB transport unavailable");
        return false;
    }
    if (!SetupDescriptors()) {
        LOG_WARN("usb: descriptor setup failed; USB transport unavailable");
        usbDsExit();
        return false;
    }
    g_ready = true;
    LOG_INFO("usb: bulk transport ready (vid %04x pid %04x)", kVendorId, kProductId);
    return true;
}

void Exit() {
    if (!g_ready) return;
    usbDsExit();
    g_interface = nullptr;
    g_ep_in = g_ep_out = nullptr;
    g_ready = false;
}

bool Read(void* buf, size_t size, size_t* got, u64 timeout_ns) {
    return g_ready && Transfer(g_ep_out, buf, size, got, timeout_ns);
}

bool Write(const void* buf, size_t size, u64 timeout_ns) {
    size_t sent = 0;
    return g_ready &&
           Transfer(g_ep_in, const_cast<void*>(buf), size, &sent, timeout_ns) &&
           sent == size;
}

}  // namespace usb_transport
}  // namespace agent
