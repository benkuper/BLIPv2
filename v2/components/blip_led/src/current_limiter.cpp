#include "blip/led/current_limiter.hpp"
#include "blip/led/protocol_encoder.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace blip::led {
namespace {
struct Layout {
    std::size_t first{};
    std::size_t stride{};
    std::uint8_t channels{};
    std::uint8_t bytes_per_channel{};
    std::uint32_t maximum_code{};
    std::array<std::uint8_t, 4> model_channel{};
};

[[nodiscard]] Layout layout(PixelProtocol protocol) noexcept {
    switch (protocol) {
    case PixelProtocol::ws2812:
        return {0U, 3U, 3U, 1U, 255U, {1U, 0U, 2U, 3U}};
    case PixelProtocol::sk6812:
        return {0U, 4U, 4U, 1U, 255U, {1U, 0U, 2U, 3U}};
    case PixelProtocol::apa102:
    case PixelProtocol::sk9822:
        return {5U, 4U, 3U, 1U, 255U, {2U, 1U, 0U, 3U}};
    case PixelProtocol::hd108:
        return {18U, 8U, 3U, 2U, 65535U, {0U, 1U, 2U, 3U}};
    }
    return {};
}

[[nodiscard]] std::uint32_t read_code(std::span<const std::byte> frame,
                                      std::size_t offset, std::uint8_t width) noexcept {
    const auto high = std::to_integer<std::uint8_t>(frame[offset]);
    return width == 1U ? high
                       : (static_cast<std::uint32_t>(high) << 8U) |
                             std::to_integer<std::uint8_t>(frame[offset + 1U]);
}

void write_code(std::span<std::byte> frame, std::size_t offset,
                std::uint8_t width, std::uint32_t value) noexcept {
    if (width == 1U) {
        frame[offset] = static_cast<std::byte>(value);
    } else {
        frame[offset] = static_cast<std::byte>(value >> 8U);
        frame[offset + 1U] = static_cast<std::byte>(value & 0xffU);
    }
}

[[nodiscard]] std::uint64_t weighted_channels(std::span<const std::byte> frame,
                                               Layout wire, std::size_t pixels,
                                               const CurrentModel& model) noexcept {
    std::uint64_t weighted{};
    for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
        for (std::size_t channel = 0; channel < wire.channels; ++channel) {
            const auto offset = wire.first + pixel * wire.stride +
                                channel * wire.bytes_per_channel;
            weighted += static_cast<std::uint64_t>(
                            read_code(frame, offset, wire.bytes_per_channel)) *
                        model.full_channel_ma[wire.model_channel[channel]];
        }
    }
    return weighted;
}

[[nodiscard]] std::uint32_t estimate(std::uint64_t weighted, std::uint32_t baseline,
                                     std::uint32_t maximum_code) noexcept {
    return baseline + static_cast<std::uint32_t>(
                          (weighted + maximum_code - 1U) / maximum_code);
}

[[nodiscard]] core::Result<CurrentLimitResult> limit_error(std::string_view detail) noexcept {
    return core::Result<CurrentLimitResult>::failure(
        {core::ErrorDomain::transport, core::ErrorCode::validation_failed,
         "blip.led.current", "limit", detail});
}
} // namespace

bool valid_power_budget(std::uint16_t budget_ma, std::size_t pixels,
                        const CurrentModel& model) noexcept {
    return budget_ma >= 1U && budget_ma <= kHardPowerBudgetMa &&
           pixels > 0U && pixels <= kMaximumPixels &&
           static_cast<std::uint32_t>(model.fixed_ma) +
                   pixels * model.idle_ma_per_pixel <=
               budget_ma;
}

core::Result<CurrentLimitResult>
CurrentLimiter::limit(std::span<std::byte> encoded_frame, PixelProtocol protocol,
                      std::size_t pixels, std::uint16_t budget_ma) noexcept {
    const Layout wire = layout(protocol);
    if (wire.channels == 0U || pixels == 0U || pixels > kMaximumPixels ||
        encoded_frame.size() != encoded_frame_size(protocol, pixels) ||
        !valid_power_budget(budget_ma, pixels, model_)) {
        return limit_error("invalid-frame-or-budget");
    }
    const auto baseline = static_cast<std::uint32_t>(model_.fixed_ma) +
                          static_cast<std::uint32_t>(pixels * model_.idle_ma_per_pixel);
    const auto weighted = weighted_channels(encoded_frame, wire, pixels, model_);
    const auto available = static_cast<std::uint64_t>(budget_ma - baseline) *
                           wire.maximum_code;
    const auto target = weighted <= available || weighted == 0U
                            ? std::uint16_t{65535U}
                            : static_cast<std::uint16_t>((available * 65535U) / weighted);
    const auto rising = std::min<std::uint32_t>(
        65535U, static_cast<std::uint32_t>(previous_scale_q16_) + rise_step_q16_);
    const auto scale = target < previous_scale_q16_
                           ? target
                           : static_cast<std::uint16_t>(std::min<std::uint32_t>(target, rising));
    CurrentLimitResult result{};
    result.estimated_before_ma = estimate(weighted, baseline, wire.maximum_code);
    result.applied_scale_q16 = scale;
    result.limited = scale < 65535U;
    if (result.limited) {
        for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
            for (std::size_t channel = 0; channel < wire.channels; ++channel) {
                const auto offset = wire.first + pixel * wire.stride +
                                    channel * wire.bytes_per_channel;
                const auto code = read_code(encoded_frame, offset, wire.bytes_per_channel);
                write_code(encoded_frame, offset, wire.bytes_per_channel,
                           (static_cast<std::uint64_t>(code) * scale) / 65535U);
            }
        }
    }
    result.estimated_after_ma = estimate(
        weighted_channels(encoded_frame, wire, pixels, model_), baseline, wire.maximum_code);
    if (result.estimated_after_ma > budget_ma) {
        return limit_error("calculated-budget-exceeded");
    }
    previous_scale_q16_ = scale;
    return core::Result<CurrentLimitResult>::success(result);
}

} // namespace blip::led
