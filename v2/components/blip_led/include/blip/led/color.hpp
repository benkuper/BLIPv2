#pragma once

#include "blip/led/engine.hpp"

#include <array>
#include <cstdint>
#include <span>

namespace blip::led {

enum class ColorChannel : std::uint8_t { red, green, blue, white };
enum class TransferFunction : std::uint8_t { linear, srgb, gamma22 };

struct ColorTransform {
    // Signed Q16.16 RGB matrix, followed by per-channel Q0.16 calibration.
    std::array<std::int32_t, 9> matrix{{65536, 0, 0, 0, 65536, 0, 0, 0, 65536}};
    std::array<std::uint16_t, 4> calibration{{65535U, 65535U, 65535U, 65535U}};
    std::uint16_t brightness{65535U};
    TransferFunction transfer{TransferFunction::srgb};
    std::array<ColorChannel, 4> order{
        {ColorChannel::red, ColorChannel::green, ColorChannel::blue, ColorChannel::white}};
};

[[nodiscard]] LinearPixel apply_color_transform(LinearPixel input,
                                                const ColorTransform& transform) noexcept;
[[nodiscard]] std::uint8_t quantize_channel(std::uint16_t linear,
                                            TransferFunction transfer) noexcept;
[[nodiscard]] std::uint16_t quantize_channel16(std::uint16_t linear,
                                               TransferFunction transfer) noexcept;
[[nodiscard]] core::Result<std::size_t> encode_color(const LinearPixel& input, PixelFormat format,
                                                     const ColorTransform& transform,
                                                     std::span<std::byte> output) noexcept;

} // namespace blip::led
