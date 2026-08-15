#pragma once

#include "blip/resources/device_broker.hpp"

#include <array>
#include <span>
#include <string_view>

namespace blip::resources {

struct BoardManifest {
    std::string_view id{};
    std::string_view target{};
    std::span<const ResourceSpec> pins{};
    std::int16_t default_led_gpio{};
    std::string_view antenna{};
};

namespace detail {
constexpr auto kIo = kGpioInput | kGpioOutput | kGpioPwm | kGpioInterrupt | kGpioRmt;
constexpr auto kIoAdc = kIo | kGpioAdc;
constexpr auto kI2c = kGpioInput | kGpioOutput | kGpioInterrupt | kGpioOpenDrain;

constexpr ResourceSpec pin(std::string_view id, std::string_view label, std::int16_t gpio,
                           std::uint32_t capabilities, std::string_view electrical = "3.3V",
                           std::string_view owner = {}, std::string_view reason = {},
                           std::string_view bus = {}, bool selectable = true) noexcept {
    return {core::ResourceClass::gpio, id, capabilities, 1U, owner, label, gpio, electrical,
            reason, bus, selectable};
}

constexpr std::array kHuzzah32Pins{
    pin("gpio.26", "A0 / GPIO26", 26, kIoAdc), pin("gpio.25", "A1 / GPIO25", 25, kIoAdc),
    pin("gpio.34", "A2 / GPIO34", 34, kGpioInput | kGpioAdc | kGpioInterrupt,
        "3.3V input-only"),
    pin("gpio.39", "A3 / GPIO39", 39, kGpioInput | kGpioAdc | kGpioInterrupt,
        "3.3V input-only"),
    pin("gpio.36", "A4 / GPIO36", 36, kGpioInput | kGpioAdc | kGpioInterrupt,
        "3.3V input-only"),
    pin("gpio.4", "A5 / GPIO4", 4, kIoAdc),
    pin("gpio.21", "GPIO21", 21, kIo | kGpioOpenDrain),
    pin("gpio.13", "GPIO13 / red LED", 13, kIo, "3.3V; onboard LED load"),
    pin("gpio.12", "GPIO12", 12, kIoAdc, "3.3V; boot strap", "system.boot",
        "boot-strap-pin", {}, false),
    pin("gpio.27", "GPIO27", 27, kIoAdc), pin("gpio.33", "GPIO33", 33, kIoAdc),
    pin("gpio.15", "GPIO15", 15, kIoAdc, "3.3V; boot strap", "system.boot",
        "boot-strap-pin", {}, false),
    pin("gpio.32", "GPIO32", 32, kIoAdc), pin("gpio.14", "GPIO14", 14, kIoAdc),
    pin("gpio.35", "A13 / battery monitor", 35, kGpioInput | kGpioAdc,
        "3.3V input-only; divided VBAT", "system.battery", "onboard-battery-divider", {},
        false),
};

constexpr std::array kM5DialPins{
    pin("gpio.13", "PORT.A SDA / GPIO13", 13, kI2c | kGpioPwm | kGpioRmt),
    pin("gpio.15", "PORT.A SCL / GPIO15", 15, kI2c | kGpioPwm | kGpioRmt),
    pin("gpio.2", "PORT.B / GPIO2", 2, kIoAdc), pin("gpio.1", "PORT.B / GPIO1", 1, kIoAdc),
    pin("gpio.21", "RGB LED / GPIO21", 21, kGpioOutput | kGpioPwm | kGpioRmt,
        "3.3V; onboard RGB LED"),
    pin("gpio.3", "Buzzer / GPIO3", 3, kGpioOutput | kGpioPwm, "onboard buzzer",
        "system.buzzer", "onboard-buzzer", {}, false),
    pin("gpio.4", "LCD RS / GPIO4", 4, kGpioOutput, "onboard display", "system.display",
        "onboard-display", {}, false),
    pin("gpio.5", "LCD MOSI / GPIO5", 5, kGpioOutput, "onboard display", "system.display",
        "onboard-display", {}, false),
    pin("gpio.6", "LCD SCK / GPIO6", 6, kGpioOutput, "onboard display", "system.display",
        "onboard-display", {}, false),
    pin("gpio.7", "LCD CS / GPIO7", 7, kGpioOutput, "onboard display", "system.display",
        "onboard-display", {}, false),
    pin("gpio.8", "LCD reset / RFID reset / GPIO8", 8, kGpioOutput,
        "onboard display and RFID", "system.display-rfid", "onboard-shared-function", {}, false),
    pin("gpio.9", "LCD backlight / GPIO9", 9, kGpioOutput | kGpioPwm, "onboard display",
        "system.display", "onboard-display", {}, false),
    pin("gpio.11", "Internal I2C SDA / GPIO11", 11, kI2c, "3.3V I2C", "bus.i2c.internal",
        "internal-i2c-bus", "i2c.internal", false),
    pin("gpio.12", "Internal I2C SCL / GPIO12", 12, kI2c, "3.3V I2C", "bus.i2c.internal",
        "internal-i2c-bus", "i2c.internal", false),
    pin("gpio.14", "Touch interrupt / GPIO14", 14, kGpioInput | kGpioInterrupt,
        "onboard touch", "system.touch", "onboard-touch", {}, false),
    pin("gpio.40", "Encoder B / GPIO40", 40, kGpioInput | kGpioInterrupt,
        "onboard encoder", "system.encoder", "onboard-encoder", {}, false),
    pin("gpio.41", "Encoder A / GPIO41", 41, kGpioInput | kGpioInterrupt,
        "onboard encoder", "system.encoder", "onboard-encoder", {}, false),
    pin("gpio.46", "Power hold / GPIO46", 46, kGpioOutput, "power latch", "system.power",
        "power-hold-critical", {}, false),
    pin("gpio.0", "Boot button / GPIO0", 0, kGpioInput | kGpioInterrupt, "boot strap",
        "system.boot", "boot-strap-pin", {}, false),
    pin("gpio.19", "USB D- / GPIO19", 19, 0U, "USB", "system.usb", "native-usb", {}, false),
    pin("gpio.20", "USB D+ / GPIO20", 20, 0U, "USB", "system.usb", "native-usb", {}, false),
};

constexpr std::array kXiaoC6Pins{
    pin("gpio.0", "D0 / GPIO0", 0, kIoAdc), pin("gpio.1", "D1 / GPIO1", 1, kIoAdc),
    pin("gpio.2", "D2 / GPIO2", 2, kIoAdc), pin("gpio.21", "D3 / GPIO21", 21, kIo),
    pin("gpio.22", "D4 / SDA / GPIO22", 22, kI2c | kGpioPwm | kGpioRmt),
    pin("gpio.23", "D5 / SCL / GPIO23", 23, kI2c | kGpioPwm | kGpioRmt),
    pin("gpio.16", "D6 / TX / GPIO16", 16, kIo), pin("gpio.17", "D7 / RX / GPIO17", 17, kIo),
    pin("gpio.19", "D8 / SCK / GPIO19", 19, kIo), pin("gpio.20", "D9 / MISO / GPIO20", 20, kIo),
    pin("gpio.18", "D10 / MOSI / GPIO18", 18, kIo),
    pin("gpio.15", "User LED / GPIO15", 15, kGpioOutput | kGpioPwm | kGpioRmt,
        "3.3V; active-low onboard LED"),
    pin("gpio.3", "RF switch power / GPIO3", 3, kGpioOutput, "onboard RF switch; low enables",
        "system.radio", "onboard-antenna-rf-switch-power", {}, false),
    pin("gpio.14", "RF antenna select / GPIO14", 14, kGpioOutput,
        "low=onboard, high=external", "system.radio", "onboard-antenna-selected", {}, false),
    pin("gpio.9", "Boot button / GPIO9", 9, kGpioInput | kGpioInterrupt, "boot strap",
        "system.boot", "boot-strap-pin", {}, false),
    pin("gpio.7", "JTAG MTDO / GPIO7", 7, 0U, "debug", "system.debug", "jtag-debug", {}, false),
    pin("gpio.5", "JTAG MTDI / GPIO5", 5, 0U, "debug", "system.debug", "jtag-debug", {}, false),
    pin("gpio.6", "JTAG MTCK / GPIO6", 6, 0U, "debug", "system.debug", "jtag-debug", {}, false),
    pin("gpio.4", "JTAG MTMS / GPIO4", 4, 0U, "debug", "system.debug", "jtag-debug", {}, false),
};
} // namespace detail

[[nodiscard]] constexpr BoardManifest selected_board_manifest() noexcept {
#if defined(CONFIG_IDF_TARGET_ESP32S3)
    return {"m5stack-m5dial", "esp32s3", detail::kM5DialPins, 21, "pcb"};
#elif defined(CONFIG_IDF_TARGET_ESP32C6)
    return {"seeed-xiao-esp32c6-chip-antenna", "esp32c6", detail::kXiaoC6Pins, 20, "onboard"};
#else
    return {"adafruit-huzzah32", "esp32", detail::kHuzzah32Pins, 23, "pcb"};
#endif
}

} // namespace blip::resources
