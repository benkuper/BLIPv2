#include "blip/led/engine.hpp"

#include <algorithm>
#include <limits>

namespace blip::led {
namespace {
[[nodiscard]] core::Status failure(core::ErrorCode code, std::string_view detail) noexcept {
    return core::Status::failure(
        {core::ErrorDomain::transport, code, "blip.led.engine", "configure", detail});
}
} // namespace

bool DriverCapabilities::supports(PixelProtocol protocol) const noexcept {
    for (const auto value : protocols) {
        if (value == protocol) {
            return true;
        }
    }
    return false;
}

std::size_t DriverCapabilities::required_staging_bytes(std::size_t pixels) const noexcept {
    if (staging_bytes_per_pixel != 0U &&
        pixels > (std::numeric_limits<std::size_t>::max() - fixed_staging_bytes) /
                     staging_bytes_per_pixel) {
        return std::numeric_limits<std::size_t>::max();
    }
    return fixed_staging_bytes + pixels * staging_bytes_per_pixel;
}

std::uint64_t DriverCapabilities::physical_frame_period_us(PixelProtocol protocol,
                                                           std::uint32_t pixels,
                                                           std::uint32_t clock_hz) const noexcept {
    if (protocol == PixelProtocol::ws2812 || protocol == PixelProtocol::sk6812) {
        const auto channels = protocol == PixelProtocol::sk6812 ? 4ULL : 3ULL;
        return pixels * channels * 8ULL * 1250ULL / 1000ULL + reset_time_us;
    }
    if (clock_hz == 0U) {
        return 0U;
    }
    const auto bits = protocol == PixelProtocol::hd108
                          ? 128ULL + static_cast<std::uint64_t>(pixels) * 64ULL +
                                static_cast<std::uint64_t>(
                                    std::max<std::uint32_t>(8U, (pixels + 7U) / 8U)) * 8ULL
                          : 32ULL + static_cast<std::uint64_t>(pixels) * 32ULL +
                                static_cast<std::uint64_t>((pixels + 15U) / 16U) * 8ULL;
    return (bits * 1000000ULL + clock_hz - 1ULL) / clock_hz;
}

core::Status validate_output_config(const DriverCapabilities& capabilities,
                                    const OutputConfig& config) noexcept {
    if (!capabilities.supports(config.protocol)) {
        return failure(core::ErrorCode::validation_failed, "unsupported-protocol");
    }
    bool format_supported = false;
    for (const auto format : capabilities.formats) {
        format_supported = format_supported || format == config.format;
    }
    if (!format_supported) {
        return failure(core::ErrorCode::validation_failed, "unsupported-format");
    }
    if (config.lanes < capabilities.minimum_lanes || config.lanes > capabilities.maximum_lanes ||
        config.pixels_per_lane == 0U ||
        config.pixels_per_lane > capabilities.maximum_pixels_per_lane) {
        return failure(core::ErrorCode::validation_failed, "lane-or-pixel-limit");
    }
    if (config.clock_hz != 0U && (config.clock_hz < capabilities.minimum_clock_hz ||
                                  config.clock_hz > capabilities.maximum_clock_hz)) {
        return failure(core::ErrorCode::validation_failed, "clock-limit");
    }
    return core::Status::success();
}

} // namespace blip::led
