#include "blip/led/protocol_encoder.hpp"

#include <algorithm>

namespace blip::led {
namespace {
[[nodiscard]] core::Result<std::size_t> overflow() noexcept {
    return core::Result<std::size_t>::failure({core::ErrorDomain::transport,
                                               core::ErrorCode::serialization_overflow,
                                               "blip.led.encoder", "encode", "output-too-small"});
}
void write16(std::span<std::byte> output, std::size_t offset, std::uint16_t value) noexcept {
    output[offset] = static_cast<std::byte>(value >> 8U);
    output[offset + 1U] = static_cast<std::byte>(value & 0xffU);
}
} // namespace

std::size_t encoded_frame_size(PixelProtocol protocol, std::size_t pixels) noexcept {
    switch (protocol) {
    case PixelProtocol::ws2812:
        return pixels * 3U;
    case PixelProtocol::sk6812:
        return pixels * 4U;
    case PixelProtocol::apa102:
    case PixelProtocol::sk9822:
        return 4U + pixels * 4U + (pixels + 15U) / 16U;
    case PixelProtocol::hd108:
        return 16U + pixels * 8U + std::max<std::size_t>(8U, (pixels + 7U) / 8U);
    }
    return 0U;
}

core::Result<std::size_t> encode_frame(ConstPixelSurface surface, PixelProtocol protocol,
                                       const EncoderOptions& options,
                                       std::span<std::byte> output) noexcept {
    if (!surface.valid() || options.global_brightness > 31U) {
        return core::Result<std::size_t>::failure(
            {core::ErrorDomain::transport, core::ErrorCode::invalid_argument, "blip.led.encoder",
             "encode", "invalid-surface-or-brightness"});
    }
    const auto required = encoded_frame_size(protocol, surface.pixels.size());
    if (output.size() < required) {
        return overflow();
    }
    std::fill(output.begin(), output.begin() + static_cast<std::ptrdiff_t>(required), std::byte{0});
    auto transform = options.color;
    std::size_t offset = 0U;
    if (protocol == PixelProtocol::apa102 || protocol == PixelProtocol::sk9822) {
        offset = 4U;
        transform.order = {ColorChannel::blue, ColorChannel::green, ColorChannel::red,
                           ColorChannel::white};
        for (const auto& pixel : surface.pixels) {
            output[offset++] = static_cast<std::byte>(0xe0U | options.global_brightness);
            const auto result =
                encode_color(pixel, PixelFormat::rgb8, transform, output.subspan(offset, 3U));
            if (!result) {
                return result;
            }
            offset += 3U;
        }
        std::fill(output.begin() + static_cast<std::ptrdiff_t>(offset),
                  output.begin() + static_cast<std::ptrdiff_t>(required), std::byte{0xff});
        return core::Result<std::size_t>::success(required);
    }
    if (protocol == PixelProtocol::hd108) {
        offset = 16U; // HD108 v1.2 start frame is 128 zero bits.
        transform.order = {ColorChannel::red, ColorChannel::green, ColorChannel::blue,
                           ColorChannel::white};
        for (const auto& pixel : surface.pixels) {
            const auto header = static_cast<std::uint16_t>(
                0x8000U | (options.global_brightness << 10U) | (options.global_brightness << 5U) |
                options.global_brightness);
            write16(output, offset, header);
            const auto result =
                encode_color(pixel, PixelFormat::rgb16, transform, output.subspan(offset + 2U, 6U));
            if (!result) {
                return result;
            }
            offset += 8U;
        }
        return core::Result<std::size_t>::success(required);
    }
    transform.order = protocol == PixelProtocol::sk6812
                          ? std::array{ColorChannel::green, ColorChannel::red, ColorChannel::blue,
                                       ColorChannel::white}
                          : std::array{ColorChannel::green, ColorChannel::red, ColorChannel::blue,
                                       ColorChannel::white};
    const auto format = protocol == PixelProtocol::sk6812 ? PixelFormat::rgbw8 : PixelFormat::rgb8;
    for (const auto& pixel : surface.pixels) {
        const auto result = encode_color(pixel, format, transform, output.subspan(offset));
        if (!result) {
            return result;
        }
        offset += result.value();
    }
    return core::Result<std::size_t>::success(offset);
}

core::Result<std::size_t> encode_spi_one_wire(std::span<const std::byte> input,
                                              std::span<std::byte> output) noexcept {
    if (output.size() < input.size() * 3U) {
        return overflow();
    }
    std::size_t out_bit = 0U;
    std::fill(output.begin(), output.begin() + static_cast<std::ptrdiff_t>(input.size() * 3U),
              std::byte{0});
    for (const auto source_byte : input) {
        const auto value = std::to_integer<std::uint8_t>(source_byte);
        for (std::uint8_t source_bit = 0U; source_bit < 8U; ++source_bit) {
            const bool one = (value & (0x80U >> source_bit)) != 0U;
            const std::uint8_t symbol = one ? 0b110U : 0b100U;
            for (std::uint8_t symbol_bit = 0U; symbol_bit < 3U; ++symbol_bit, ++out_bit) {
                if ((symbol & (0b100U >> symbol_bit)) != 0U) {
                    output[out_bit / 8U] |= static_cast<std::byte>(0x80U >> (out_bit % 8U));
                }
            }
        }
    }
    return core::Result<std::size_t>::success(input.size() * 3U);
}

} // namespace blip::led
