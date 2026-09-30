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
    pin("gpio.15", "GPIO15 / strip data", 15, kIoAdc,
        "3.3V; boot strap; LED data input must remain high impedance during reset",
        "blip.output.strip0:pin", "ws2812b-data-boot-strap", {}, false),
    pin("gpio.32", "GPIO32", 32, kIoAdc), pin("gpio.14", "GPIO14", 14, kIoAdc),
    pin("gpio.35", "A13 / battery monitor", 35, kGpioInput | kGpioAdc,
        "3.3V input-only; divided VBAT", "system.battery", "onboard-battery-divider", {},
        false),
};

constexpr std::array kM5DialPins{
    pin("gpio.13", "PORT.A SDA / GPIO13", 13, kI2c | kGpioPwm | kGpioRmt),
    pin("gpio.15", "PORT.A SCL / GPIO15", 15, kI2c | kGpioPwm | kGpioRmt),
    pin("gpio.2", "PORT.B / GPIO2", 2, kIoAdc), pin("gpio.1", "PORT.B / GPIO1", 1, kIoAdc),
    pin("gpio.21", "Stamp S3A status LED / GPIO21", 21, kGpioOutput | kGpioPwm | kGpioRmt,
        "3.3V; single WS2812B-2020 on controller module"),
    pin("gpio.38", "Stamp S3A status LED power / GPIO38", 38, kGpioOutput,
        "controller-module status LED power switch; high enables", "system.led-power", "led-enable", {},
        false),
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

// V1 creatorsballv2 wiring. These pins are physically connected to onboard
// devices, so they must not inherit the XIAO C6 RF-switch reservations.
constexpr std::array kCreatorsBallV2Pins{
    pin("gpio.0", "IMU SDA / GPIO0", 0, kI2c, "3.3V I2C", "bus.i2c.imu",
        "onboard-imu-bus", "i2c.imu", false),
    pin("gpio.1", "IMU SCL / GPIO1", 1, kI2c, "3.3V I2C", "bus.i2c.imu",
        "onboard-imu-bus", "i2c.imu", false),
    pin("gpio.2", "HD108 clock / GPIO2", 2, kGpioOutput, "onboard LED clock",
        "blip.output.strip0:clock", "hd108-clock", {}, false),
    pin("gpio.3", "HD108 data / GPIO3", 3, kGpioOutput, "onboard LED data",
        "blip.output.strip0:pin", "hd108-data", {}, false),
    pin("gpio.4", "Flash MOSI / GPIO4", 4, kGpioOutput, "onboard storage",
        "system.storage", "storage-spi", {}, false),
    pin("gpio.5", "Flash MISO / GPIO5", 5, kGpioInput, "onboard storage",
        "system.storage", "storage-spi", {}, false),
    pin("gpio.6", "Battery sense / GPIO6", 6, kGpioInput | kGpioAdc, "battery divider",
        "system.battery", "battery-monitor", {}, false),
    pin("gpio.7", "IMU interrupt / GPIO7", 7, kGpioInput | kGpioInterrupt, "onboard IMU",
        "system.imu", "imu-interrupt", {}, false),
    pin("gpio.8", "Flash SCK / GPIO8", 8, kGpioOutput, "onboard storage",
        "system.storage", "storage-spi", {}, false),
    pin("gpio.9", "Charge feedback LED / GPIO9", 9, kGpioOutput | kGpioPwm,
        "onboard LED; boot strap", "system.battery", "charge-feedback", {}, false),
    pin("gpio.14", "Charge sense / GPIO14", 14, kGpioInput, "charger input",
        "system.battery", "battery-charge-input", {}, false),
    pin("gpio.15", "Flash CS / GPIO15", 15, kGpioOutput, "onboard storage",
        "system.storage", "storage-spi", {}, false),
    pin("gpio.16", "HD108 backup data / GPIO16", 16, kGpioOutput,
        "onboard backup data", "system.led-backup", "hd108-backup-data", {}, false),
    pin("gpio.17", "HD108 backup clock / GPIO17", 17, kGpioOutput,
        "onboard backup clock", "system.led-backup", "hd108-backup-clock", {}, false),
    pin("gpio.18", "IR input 1 / GPIO18", 18, kGpioInput | kGpioInterrupt,
        "onboard IR input", "system.ir", "ir-input", {}, false),
    pin("gpio.19", "Power/wake button / GPIO19", 19, kGpioInput | kGpioInterrupt,
        "onboard button", "system.power", "wake-button", {}, false),
    pin("gpio.20", "IR input 2 / GPIO20", 20, kGpioInput | kGpioInterrupt,
        "onboard IR input", "system.ir", "ir-input", {}, false),
    pin("gpio.21", "HD108 enable / GPIO21", 21, kGpioOutput, "onboard LED power",
        "system.led-power", "led-enable", {}, false),
    pin("gpio.22", "Power hold / GPIO22", 22, kGpioOutput, "power latch",
        "system.power", "power-hold-critical", {}, false),
    pin("gpio.23", "IMU reset / GPIO23", 23, kGpioOutput, "onboard IMU",
        "system.imu", "imu-reset", {}, false),
};

constexpr std::array kCreatorsTabPins{
    pin("gpio.0", "Boot / GPIO0", 0, kGpioInput, "boot strap", "system.boot",
        "boot-strap-pin", {}, false),
    pin("gpio.1", "UART0 TX / GPIO1", 1, kGpioOutput, "USB serial bridge",
        "system.serial", "serial-console", {}, false),
    pin("gpio.3", "UART0 RX / GPIO3", 3, kGpioInput, "USB serial bridge",
        "system.serial", "serial-console", {}, false),
    pin("gpio.12", "Power hold / GPIO12", 12, kGpioOutput, "power latch; boot strap",
        "system.power", "power-hold-critical", {}, false),
    pin("gpio.13", "SD MOSI / GPIO13", 13, kGpioOutput, "onboard SD",
        "system.storage", "sd-spi", {}, false),
    pin("gpio.14", "SD SCK / GPIO14", 14, kGpioOutput, "onboard SD",
        "system.storage", "sd-spi", {}, false),
    pin("gpio.15", "SD CS / GPIO15", 15, kGpioOutput, "onboard SD; boot strap",
        "system.storage", "sd-spi", {}, false),
    pin("gpio.19", "SD MISO / GPIO19", 19, kGpioInput, "onboard SD",
        "system.storage", "sd-spi", {}, false),
    pin("gpio.22", "IMU SCL / GPIO22", 22, kI2c, "3.3V I2C", "bus.i2c.imu",
        "onboard-imu-bus", "i2c.imu", false),
    pin("gpio.23", "IMU SDA / GPIO23", 23, kI2c, "3.3V I2C", "bus.i2c.imu",
        "onboard-imu-bus", "i2c.imu", false),
    pin("gpio.25", "WS2812B data / GPIO25", 25, kGpioOutput | kGpioRmt,
        "onboard LED connector", "blip.output.strip0:pin", "ws2812b-data", {}, false),
    pin("gpio.27", "LED strip enable / GPIO27", 27, kGpioOutput,
        "strip power enable", "system.led-power", "led-enable", {}, false),
    pin("gpio.32", "Button / GPIO32", 32, kGpioInput | kGpioInterrupt,
        "onboard button", "system.button", "button-input", {}, false),
    pin("gpio.33", "SD enable / GPIO33", 33, kGpioOutput, "onboard SD power",
        "system.storage", "sd-enable", {}, false),
    pin("gpio.34", "IMU interrupt / GPIO34", 34, kGpioInput | kGpioInterrupt,
        "input only", "system.imu", "imu-interrupt", {}, false),
    pin("gpio.35", "Battery sense / GPIO35", 35, kGpioInput | kGpioAdc,
        "input only; battery divider", "system.battery", "battery-monitor", {}, false),
    pin("gpio.36", "Power/wake button / GPIO36", 36, kGpioInput | kGpioInterrupt,
        "input only", "system.power", "wake-button", {}, false),
    pin("gpio.39", "Charge sense / GPIO39", 39, kGpioInput | kGpioAdc,
        "input only; charger input", "system.battery", "battery-charge-input", {}, false),
};

constexpr std::array kM5StickCPins{
    pin("gpio.0", "Microphone clock / HAT GPIO0", 0, kGpioOutput,
        "microphone clock; boot strap", "system.microphone", "onboard-microphone-clock", {}, false),
    pin("gpio.1", "UART0 TX / GPIO1", 1, kGpioOutput, "USB serial bridge",
        "system.serial", "serial-console", {}, false),
    pin("gpio.3", "UART0 RX / GPIO3", 3, kGpioInput, "USB serial bridge",
        "system.serial", "serial-console", {}, false),
    pin("gpio.5", "TFT CS / GPIO5", 5, kGpioOutput, "onboard display",
        "system.display", "tft-chip-select", {}, false),
    pin("gpio.9", "IR transmitter / GPIO9", 9, kGpioOutput, "onboard IR transmitter",
        "system.ir", "ir-output", {}, false),
    pin("gpio.10", "Red LED / GPIO10", 10, kGpioOutput, "onboard red LED",
        "system.led-feedback", "feedback-led", {}, false),
    pin("gpio.13", "TFT clock / GPIO13", 13, kGpioOutput, "onboard display",
        "system.display", "tft-clock", {}, false),
    pin("gpio.15", "TFT MOSI / GPIO15", 15, kGpioOutput, "onboard display; boot strap",
        "system.display", "tft-mosi", {}, false),
    pin("gpio.18", "TFT reset / GPIO18", 18, kGpioOutput, "onboard display",
        "system.display", "tft-reset", {}, false),
    pin("gpio.21", "Internal I2C SDA / GPIO21", 21, kI2c, "3.3V I2C",
        "bus.i2c.internal", "internal-i2c-bus", "i2c.internal", false),
    pin("gpio.22", "Internal I2C SCL / GPIO22", 22, kI2c, "3.3V I2C",
        "bus.i2c.internal", "internal-i2c-bus", "i2c.internal", false),
    pin("gpio.23", "TFT DC / GPIO23", 23, kGpioOutput, "onboard display",
        "system.display", "tft-data-command", {}, false),
    pin("gpio.26", "HAT GPIO26", 26, kIoAdc, "3.3V HAT pin"),
    pin("gpio.32", "Grove GPIO32", 32, kIoAdc | kGpioOpenDrain, "3.3V Grove pin"),
    pin("gpio.33", "Grove GPIO33", 33, kIoAdc | kGpioOpenDrain, "3.3V Grove pin"),
    pin("gpio.34", "Microphone data / GPIO34", 34, kGpioInput, "input only",
        "system.microphone", "onboard-microphone-data", {}, false),
    pin("gpio.35", "PMU/IMU/RTC interrupt / GPIO35", 35, kGpioInput | kGpioInterrupt,
        "input only", "system.irq", "onboard-shared-interrupt", {}, false),
    pin("gpio.36", "HAT GPIO36", 36, kGpioInput | kGpioAdc | kGpioInterrupt,
        "3.3V input-only HAT pin"),
    pin("gpio.37", "Button A / GPIO37", 37, kGpioInput | kGpioInterrupt,
        "input only", "system.button", "button-a", {}, false),
    pin("gpio.39", "Button B / GPIO39", 39, kGpioInput | kGpioInterrupt,
        "input only", "system.button", "button-b", {}, false),
};
} // namespace detail

[[nodiscard]] constexpr BoardManifest selected_board_manifest() noexcept {
#if defined(BLIP_BOARD_CREATORS_BALL_V2)
    static_assert(
#if defined(CONFIG_IDF_TARGET_ESP32C6)
        true,
#else
        false,
#endif
        "Creators Ball V2 requires ESP32-C6");
    return {"creators-ball-v2", "esp32c6", detail::kCreatorsBallV2Pins, 3, "pcb"};
#elif defined(BLIP_BOARD_CREATORS_TAB)
    static_assert(
#if defined(CONFIG_IDF_TARGET_ESP32)
        true,
#else
        false,
#endif
        "Creators Tab requires ESP32");
    return {"creators-tab", "esp32", detail::kCreatorsTabPins, 25, "pcb"};
#elif defined(BLIP_BOARD_M5STICKC)
    static_assert(
#if defined(CONFIG_IDF_TARGET_ESP32)
        true,
#else
        false,
#endif
        "M5StickC requires ESP32");
    return {"m5stack-m5stickc", "esp32", detail::kM5StickCPins, 26, "pcb"};
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
    return {"m5stack-m5dial", "esp32s3", detail::kM5DialPins, 21, "pcb"};
#elif defined(CONFIG_IDF_TARGET_ESP32C6)
    return {"seeed-xiao-esp32c6-chip-antenna", "esp32c6", detail::kXiaoC6Pins, 20, "onboard"};
#else
    return {"adafruit-huzzah32", "esp32", detail::kHuzzah32Pins, 15, "pcb"};
#endif
}

} // namespace blip::resources
