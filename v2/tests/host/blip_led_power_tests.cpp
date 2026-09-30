#include "blip/led/current_limiter.hpp"
#include "blip/led/protocol_encoder.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>

namespace {
using namespace blip::led;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            std::cerr << "FAIL " << __func__ << ':' << __LINE__ << " " #x "\n";                    \
            return false;                                                                          \
        }                                                                                          \
    } while (false)

std::uint8_t value(std::byte byte) { return std::to_integer<std::uint8_t>(byte); }

bool budget_caps_actual_one_wire_bytes() {
    std::array<std::byte, 3> frame{std::byte{255}, std::byte{255}, std::byte{255}};
    CurrentLimiter limiter;
    const auto limited = limiter.limit(frame, PixelProtocol::ws2812, 1U, 31U);
    CHECK(limited);
    CHECK(limited.value().estimated_before_ma == 61U);
    CHECK(limited.value().estimated_after_ma <= 31U);
    CHECK(limited.value().limited);
    const auto encoded_sum = value(frame[0]) + value(frame[1]) + value(frame[2]);
    CHECK(1U + (encoded_sum * 20U + 254U) / 255U <= 31U);
    CHECK(!valid_power_budget(0U, 1U));
    CHECK(!valid_power_budget(5001U, 1U));
    CHECK(!valid_power_budget(1U, 2U));
    return true;
}

bool protocol_layout_and_channel_costs() {
    CurrentModel asymmetric{};
    asymmetric.idle_ma_per_pixel = 0U;
    asymmetric.full_channel_ma = {10U, 20U, 30U, 40U};
    CurrentLimiter limiter{asymmetric};
    std::array<std::byte, 4> grbw{std::byte{255}, std::byte{0}, std::byte{0}, std::byte{0}};
    const auto green = limiter.limit(grbw, PixelProtocol::sk6812, 1U, 100U);
    CHECK(green);
    CHECK(green.value().estimated_before_ma == 20U);
    std::array<std::byte, 9> apa{std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
                                 std::byte{0xff}, std::byte{255}, std::byte{0},
                                 std::byte{0}, std::byte{0xff}};
    const auto blue = limiter.limit(apa, PixelProtocol::apa102, 1U, 100U);
    CHECK(blue);
    CHECK(blue.value().estimated_before_ma == 30U);
    return true;
}

bool hd108_framing_stays_intact() {
    std::array<LinearPixel, 2> pixels{{{65535U, 65535U, 65535U, 0U, 65535U},
                                       {65535U, 65535U, 65535U, 0U, 65535U}}};
    std::array<std::byte, 40> frame{};
    EncoderOptions options{};
    options.global_brightness = 31U;
    options.color.transfer = TransferFunction::linear;
    const auto encoded = encode_frame({pixels, 1U, 2U}, PixelProtocol::hd108, options, frame);
    CHECK(encoded && encoded.value() == frame.size());
    const auto header = frame[16];
    const auto trailer = frame[39];
    CurrentLimiter limiter;
    const auto limited = limiter.limit(frame, PixelProtocol::hd108, 2U, 60U);
    CHECK(limited && limited.value().estimated_after_ma <= 60U);
    CHECK(frame[16] == header && frame[39] == trailer);
    CHECK(value(frame[18]) < 255U);
    return true;
}

bool overload_drops_immediately_and_recovers_smoothly() {
    CurrentLimiter limiter{{}, 1000U};
    std::array<std::byte, 3> saturated{std::byte{255}, std::byte{255}, std::byte{255}};
    const auto first = limiter.limit(saturated, PixelProtocol::ws2812, 1U, 31U);
    CHECK(first && first.value().limited);
    std::array<std::byte, 3> dark{};
    const auto recovering = limiter.limit(dark, PixelProtocol::ws2812, 1U, 31U);
    CHECK(recovering);
    CHECK(recovering.value().applied_scale_q16 ==
          first.value().applied_scale_q16 + 1000U);
    std::array<std::byte, 3> saturated_again{std::byte{255}, std::byte{255}, std::byte{255}};
    const auto second = limiter.limit(saturated_again, PixelProtocol::ws2812, 1U, 31U);
    CHECK(second);
    CHECK(second.value().applied_scale_q16 == first.value().applied_scale_q16);
    std::array<std::byte, 3> invalid{std::byte{255}, std::byte{255}, std::byte{255}};
    CHECK(!limiter.limit(invalid, PixelProtocol::ws2812, 1U, 0U));
    CHECK(value(invalid[0]) == 255U);
    return true;
}
} // namespace

int main() {
    return budget_caps_actual_one_wire_bytes() && protocol_layout_and_channel_costs() &&
                   hd108_framing_stays_intact() &&
                   overload_drops_immediately_and_recovers_smoothly()
               ? 0
               : 1;
}
