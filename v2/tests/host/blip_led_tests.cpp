#include "blip/led/strip_config.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>

namespace {

using namespace blip::led;

#define BLIP_CHECK(condition)                                                                      \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            std::cerr << "FAIL " << __func__ << ':' << __LINE__ << " " #condition << '\n';         \
            return false;                                                                          \
        }                                                                                          \
    } while (false)

bool settings_round_trip_and_corruption_fail_closed() {
    StripConfig config{};
    config.enabled = true;
    config.protocol = StripProtocol::sk6812_rgbw;
    config.gpio = 20U;
    config.pixel_count = 1024U;
    config.brightness = 128U;
    config.red = 1U;
    config.green = 2U;
    config.blue = 3U;
    config.white = 4U;
    std::array<std::byte, kStripSettingsBytes> encoded{};
    const auto size = encode_strip_config(config, encoded);
    BLIP_CHECK(size && size.value() == kStripSettingsBytes);
    const auto decoded = decode_strip_config(encoded);
    BLIP_CHECK(decoded && decoded.value() == config);
    encoded[24] = std::byte{1};
    BLIP_CHECK(!decode_strip_config(encoded));
    return true;
}

bool invalid_settings_are_rejected() {
    StripConfig config{};
    config.pixel_count = 0U;
    BLIP_CHECK(!validate_strip_config(config));
    config.pixel_count = 1U;
    config.gpio = 64U;
    BLIP_CHECK(!validate_strip_config(config));
    config.gpio = 2U;
    config.protocol = static_cast<StripProtocol>(3U);
    BLIP_CHECK(!validate_strip_config(config));
    config.protocol = StripProtocol::hd108_rgb;
    std::array<std::uint8_t, 3> unclocked{};
    BLIP_CHECK(!fill_solid_frame(config, unclocked));
    std::array<std::byte, kStripSettingsBytes - 1U> short_output{};
    config.protocol = StripProtocol::ws2812_rgb;
    BLIP_CHECK(!encode_strip_config(config, short_output));
    return true;
}

bool ws2812_frame_is_grb_and_brightness_scaled() {
    StripConfig config{};
    config.pixel_count = 2U;
    config.brightness = 255U;
    config.red = 0x11U;
    config.green = 0x22U;
    config.blue = 0x33U;
    config.white = 0x44U;
    std::array<std::uint8_t, 8> output{};
    const auto size = fill_solid_frame(config, output);
    BLIP_CHECK(size && size.value() == 6U);
    constexpr std::array<std::uint8_t, 6> expected{0x22U, 0x11U, 0x33U, 0x22U, 0x11U, 0x33U};
    for (std::size_t index = 0; index < expected.size(); ++index) {
        BLIP_CHECK(output[index] == expected[index]);
    }
    config.brightness = 127U;
    const auto dimmed = fill_solid_frame(config, output);
    BLIP_CHECK(dimmed && output[0] == 0x11U && output[1] == 0x08U && output[2] == 0x19U);
    return true;
}

bool sk6812_frame_is_grbw_at_maximum_length() {
    StripConfig config{};
    config.protocol = StripProtocol::sk6812_rgbw;
    config.pixel_count = static_cast<std::uint16_t>(kMaximumStripPixels);
    config.red = 1U;
    config.green = 2U;
    config.blue = 3U;
    config.white = 4U;
    std::array<std::uint8_t, kMaximumStripPixels * 4U> output{};
    const auto size = fill_solid_frame(config, output);
    BLIP_CHECK(size && size.value() == output.size());
    BLIP_CHECK(output[0] == 2U && output[1] == 1U && output[2] == 3U && output[3] == 4U);
    BLIP_CHECK(output[output.size() - 4U] == 2U && output.back() == 4U);
    BLIP_CHECK(
        !fill_solid_frame(config, std::span<std::uint8_t>{output}.first(output.size() - 1U)));
    return true;
}

bool protocol_timings_are_exact() {
    const auto ws = strip_timing(StripProtocol::ws2812_rgb);
    BLIP_CHECK(ws.zero_high_ticks == 4U && ws.zero_low_ticks == 9U);
    BLIP_CHECK(ws.one_high_ticks == 9U && ws.one_low_ticks == 4U && ws.reset_us == 80U);
    const auto sk = strip_timing(StripProtocol::sk6812_rgbw);
    BLIP_CHECK(sk.zero_high_ticks == 3U && sk.zero_low_ticks == 9U);
    BLIP_CHECK(sk.one_high_ticks == 6U && sk.one_low_ticks == 6U && sk.reset_us == 80U);
    return true;
}

} // namespace

int main() {
    const std::array tests{settings_round_trip_and_corruption_fail_closed,
                           invalid_settings_are_rejected, ws2812_frame_is_grb_and_brightness_scaled,
                           sk6812_frame_is_grbw_at_maximum_length, protocol_timings_are_exact};
    for (const auto test : tests) {
        if (!test()) {
            return 1;
        }
    }
    std::cout << "PASS blip_led_tests " << tests.size() << " cases\n";
    return 0;
}
