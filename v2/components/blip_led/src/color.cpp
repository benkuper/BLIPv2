#include "blip/led/color.hpp"

#include <algorithm>
#include <cmath>

namespace blip::led {
namespace {
[[nodiscard]] constexpr std::uint16_t clamp16(std::int64_t value) noexcept {
    return static_cast<std::uint16_t>(std::clamp<std::int64_t>(value, 0, 65535));
}
[[nodiscard]] constexpr std::uint16_t calibrated(std::uint16_t value, std::uint16_t gain,
                                                 std::uint16_t brightness) noexcept {
    const auto first = (static_cast<std::uint64_t>(value) * gain + 32767ULL) / 65535ULL;
    return static_cast<std::uint16_t>((first * brightness + 32767ULL) / 65535ULL);
}
[[nodiscard]] constexpr std::uint16_t channel(const LinearPixel& pixel,
                                              ColorChannel selected) noexcept {
    switch (selected) {
    case ColorChannel::red:
        return pixel.red;
    case ColorChannel::green:
        return pixel.green;
    case ColorChannel::blue:
        return pixel.blue;
    case ColorChannel::white:
        return pixel.white;
    }
    return 0U;
}
} // namespace

LinearPixel apply_color_transform(LinearPixel input, const ColorTransform& transform) noexcept {
    const std::array<std::uint16_t, 3> source{input.red, input.green, input.blue};
    std::array<std::uint16_t, 3> rgb{};
    for (std::size_t row = 0; row < 3U; ++row) {
        std::int64_t sum = 32768;
        for (std::size_t column = 0; column < 3U; ++column) {
            sum += static_cast<std::int64_t>(transform.matrix[row * 3U + column]) * source[column];
        }
        rgb[row] = clamp16(sum >> 16U);
    }
    return {calibrated(rgb[0], transform.calibration[0], transform.brightness),
            calibrated(rgb[1], transform.calibration[1], transform.brightness),
            calibrated(rgb[2], transform.calibration[2], transform.brightness),
            calibrated(input.white, transform.calibration[3], transform.brightness), input.alpha};
}

std::uint8_t quantize_channel(std::uint16_t linear, TransferFunction transfer) noexcept {
    const double normalized = static_cast<double>(linear) / 65535.0;
    double encoded = normalized;
    if (transfer == TransferFunction::srgb) {
        encoded = normalized <= 0.0031308 ? normalized * 12.92
                                          : 1.055 * std::pow(normalized, 1.0 / 2.4) - 0.055;
    } else if (transfer == TransferFunction::gamma22) {
        encoded = std::pow(normalized, 1.0 / 2.2);
    }
    return static_cast<std::uint8_t>(std::clamp(std::lround(encoded * 255.0), 0L, 255L));
}

std::uint16_t quantize_channel16(std::uint16_t linear, TransferFunction transfer) noexcept {
    if (transfer == TransferFunction::linear) {
        return linear;
    }
    const double normalized = static_cast<double>(linear) / 65535.0;
    const double encoded =
        transfer == TransferFunction::srgb
            ? (normalized <= 0.0031308 ? normalized * 12.92
                                       : 1.055 * std::pow(normalized, 1.0 / 2.4) - 0.055)
            : std::pow(normalized, 1.0 / 2.2);
    return static_cast<std::uint16_t>(std::clamp(std::lround(encoded * 65535.0), 0L, 65535L));
}

core::Result<std::size_t> encode_color(const LinearPixel& input, PixelFormat format,
                                       const ColorTransform& transform,
                                       std::span<std::byte> output) noexcept {
    const auto channels = format == PixelFormat::rgbw8 || format == PixelFormat::rgbw16 ? 4U : 3U;
    const bool sixteen_bit = format == PixelFormat::rgb16 || format == PixelFormat::rgbw16;
    const auto required = channels * (sixteen_bit ? 2U : 1U);
    if (output.size() < required) {
        return core::Result<std::size_t>::failure({core::ErrorDomain::transport,
                                                   core::ErrorCode::serialization_overflow,
                                                   "blip.led.color", "encode", "output-too-small"});
    }
    const auto pixel = apply_color_transform(input, transform);
    for (std::size_t index = 0; index < channels; ++index) {
        const auto value = channel(pixel, transform.order[index]);
        if (sixteen_bit) {
            const auto encoded = quantize_channel16(value, transform.transfer);
            output[index * 2U] = static_cast<std::byte>(encoded >> 8U);
            output[index * 2U + 1U] = static_cast<std::byte>(encoded & 0xffU);
        } else {
            output[index] = static_cast<std::byte>(quantize_channel(value, transform.transfer));
        }
    }
    return core::Result<std::size_t>::success(required);
}

} // namespace blip::led
