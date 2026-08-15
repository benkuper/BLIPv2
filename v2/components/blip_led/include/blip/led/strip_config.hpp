#pragma once

#include "blip/core/error.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace blip::led {

inline constexpr std::uint16_t kStripSettingsFormatVersion = 1U;
inline constexpr std::size_t kStripSettingsBytes = 32U;
inline constexpr std::size_t kMaximumStripPixels = 1024U;

enum class StripProtocol : std::uint8_t { ws2812_rgb = 0, sk6812_rgbw = 1 };

struct StripConfig {
    bool enabled{};
    StripProtocol protocol{StripProtocol::ws2812_rgb};
    std::uint8_t gpio{2U};
    std::uint16_t pixel_count{1U};
    std::uint8_t brightness{255U};
    std::uint8_t red{};
    std::uint8_t green{};
    std::uint8_t blue{};
    std::uint8_t white{};

    [[nodiscard]] bool operator==(const StripConfig&) const noexcept = default;
};

struct StripTiming {
    std::uint16_t zero_high_ticks{};
    std::uint16_t zero_low_ticks{};
    std::uint16_t one_high_ticks{};
    std::uint16_t one_low_ticks{};
    std::uint16_t reset_us{};
};

[[nodiscard]] constexpr std::size_t channel_count(StripProtocol protocol) noexcept {
    return protocol == StripProtocol::sk6812_rgbw ? 4U : 3U;
}

[[nodiscard]] constexpr StripTiming strip_timing(StripProtocol protocol) noexcept {
    return protocol == StripProtocol::sk6812_rgbw ? StripTiming{3U, 9U, 6U, 6U, 80U}
                                                  : StripTiming{4U, 9U, 9U, 4U, 80U};
}

[[nodiscard]] core::Status validate_strip_config(const StripConfig& config) noexcept;
[[nodiscard]] core::Result<std::size_t> encode_strip_config(const StripConfig& config,
                                                            std::span<std::byte> output) noexcept;
[[nodiscard]] core::Result<StripConfig>
decode_strip_config(std::span<const std::byte> input) noexcept;
[[nodiscard]] core::Result<std::size_t> fill_solid_frame(const StripConfig& config,
                                                         std::span<std::uint8_t> output) noexcept;

} // namespace blip::led
