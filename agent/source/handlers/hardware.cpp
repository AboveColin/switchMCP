// Low-level hardware buses: i2c (sensors/PMIC) and gpio. Gated behind
// allow_hardware in config — poking these can affect or damage hardware.
// Note: pwm/fan control is not exposed by a public service in this libnx, so
// fan-curve control isn't available here.
#include "../protocol.hpp"

#include <switch.h>

#include <string>
#include <vector>

#include "../log.hpp"

namespace agent {
namespace handlers {

bool I2cRead(const Request& req, Reply& reply) {
    if (!req.cfg.allow_hardware)
        return Fail(reply, "disabled", "i2c requires allow_hardware=true in config");
    I2cDevice dev = (I2cDevice)req["device"].as_int(I2cDevice_Tmp451);  // temp sensor
    int64_t len = req["len"].as_int(1);
    if (len <= 0 || len > 256) len = 1;

    I2cSession s;
    if (R_FAILED(i2cOpenSession(&s, dev)))
        return Fail(reply, "i2c_failed", "cannot open i2c device");
    // If a register was given, write it first (repeated-start read).
    if (req.msg.has("reg")) {
        uint8_t reg = (uint8_t)req["reg"].as_int(0);
        i2csessionSendAuto(&s, &reg, 1, I2cTransactionOption_Start);
    }
    std::vector<uint8_t> buf(len);
    Result rc = i2csessionReceiveAuto(&s, buf.data(), len, I2cTransactionOption_All);
    i2csessionClose(&s);
    if (R_FAILED(rc)) return Fail(reply, "i2c_failed", "i2c receive failed");
    reply.out = std::move(buf);
    reply.json.set("device", (int64_t)dev);
    return true;
}

bool I2cWrite(const Request& req, Reply& reply) {
    if (!req.cfg.allow_hardware)
        return Fail(reply, "disabled", "i2c requires allow_hardware=true in config");
    if (req.payload.empty()) return Fail(reply, "bad_arg", "no bytes to write");
    I2cDevice dev = (I2cDevice)req["device"].as_int(0);
    I2cSession s;
    if (R_FAILED(i2cOpenSession(&s, dev)))
        return Fail(reply, "i2c_failed", "cannot open i2c device");
    Result rc = i2csessionSendAuto(&s, req.payload.data(), req.payload.size(),
                                   I2cTransactionOption_All);
    i2csessionClose(&s);
    if (R_FAILED(rc)) return Fail(reply, "i2c_failed", "i2c send failed");
    reply.json.set("ok", true);
    reply.json.set("written", (int64_t)req.payload.size());
    return true;
}

bool GpioRead(const Request& req, Reply& reply) {
    if (!req.cfg.allow_hardware)
        return Fail(reply, "disabled", "gpio requires allow_hardware=true in config");
    GpioPadName pad = (GpioPadName)req["pad"].as_int(GpioPadName_ButtonVolUp);
    GpioPadSession s;
    if (R_FAILED(gpioOpenSession(&s, pad)))
        return Fail(reply, "gpio_failed", "cannot open gpio pad");
    GpioValue v = GpioValue_Low;
    Result rc = gpioPadGetValue(&s, &v);
    gpioPadClose(&s);
    if (R_FAILED(rc)) return Fail(reply, "gpio_failed", "gpio read failed");
    reply.json.set("pad", (int64_t)pad);
    reply.json.set("value", (int64_t)v);
    reply.json.set("high", v == GpioValue_High);
    return true;
}

}  // namespace handlers
}  // namespace agent
