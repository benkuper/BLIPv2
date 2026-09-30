#pragma once

#include "blip/led/color.hpp"

namespace blip::led {

struct EncoderOptions {
    ColorTransform color{};
    std::uint8_t global_brightness{31U};
};

[[nodiscard]] std::size_t encoded_frame_size(PixelProtocol protocol, std::size_t pixels) noexcept;
[[nodiscard]] core::Result<std::size_t> encode_frame(ConstPixelSurface surface,
                                                     PixelProtocol protocol,
                                                     const EncoderOptions& options,
                                                     std::span<std::byte> output) noexcept;

// Expands a byte stream to the common 3-SPI-bit one-wire representation
// (0 -> 100, 1 -> 110), exactly three output bytes for every input byte.
[[nodiscard]] core::Result<std::size_t> encode_spi_one_wire(std::span<const std::byte> input,
                                                            std::span<std::byte> output) noexcept;

} // namespace blip::led
