#include "blip/led/strip_config.hpp"
#include "blip/led/current_limiter.hpp"

#include <algorithm>
#include <array>

namespace blip::led {
namespace {

constexpr std::uint32_t kMagic = 0x54534c42U; // "BLST".
constexpr std::size_t kCrcOffset = 8U;

[[nodiscard]] core::Error strip_error(core::ErrorCode code, std::string_view operation,
                                      std::string_view detail) noexcept {
    return {core::ErrorDomain::storage, code, "blip.output.strip0", operation, detail};
}

void write_u16(std::span<std::byte> output, std::size_t offset, std::uint16_t value) noexcept {
    output[offset] = static_cast<std::byte>(value & 0xffU);
    output[offset + 1U] = static_cast<std::byte>(value >> 8U);
}

void write_u32(std::span<std::byte> output, std::size_t offset, std::uint32_t value) noexcept {
    for (std::size_t index = 0; index < 4U; ++index) {
        output[offset + index] = static_cast<std::byte>(value >> (index * 8U));
    }
}

[[nodiscard]] std::uint16_t read_u16(std::span<const std::byte> input,
                                     std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(input[offset])) |
           static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(input[offset + 1U])) << 8U;
}

[[nodiscard]] std::uint32_t read_u32(std::span<const std::byte> input,
                                     std::size_t offset) noexcept {
    std::uint32_t value{};
    for (std::size_t index = 0; index < 4U; ++index) {
        value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(input[offset + index]))
                 << (index * 8U);
    }
    return value;
}

[[nodiscard]] std::uint32_t crc32(std::span<const std::byte> input) noexcept {
    std::uint32_t crc = 0xffffffffU;
    for (std::size_t index = 0; index < input.size(); ++index) {
        const auto value = index >= kCrcOffset && index < kCrcOffset + 4U
                               ? 0U
                               : std::to_integer<std::uint8_t>(input[index]);
        crc ^= value;
        for (std::uint8_t bit = 0; bit < 8U; ++bit) {
            const std::uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return ~crc;
}

[[nodiscard]] constexpr std::uint8_t scale(std::uint8_t value, std::uint8_t brightness) noexcept {
    return static_cast<std::uint8_t>(
        (static_cast<std::uint16_t>(value) * (static_cast<std::uint16_t>(brightness) + 1U)) >> 8U);
}

} // namespace

core::Status validate_strip_config(const StripConfig& config) noexcept {
    if (static_cast<std::uint8_t>(config.protocol) > 3U || config.gpio > 63U ||
        config.pixel_count == 0U || config.pixel_count > kMaximumStripPixels ||
        !valid_power_budget(config.power_budget_ma, config.pixel_count)) {
        return core::Status::failure(
            strip_error(core::ErrorCode::validation_failed, "validate", "invalid-strip-settings"));
    }
    return core::Status::success();
}

core::Result<std::size_t> encode_strip_config(const StripConfig& config,
                                              std::span<std::byte> output) noexcept {
    const auto valid = validate_strip_config(config);
    if (!valid) {
        return core::Result<std::size_t>::failure(valid.error());
    }
    if (output.size() < kStripSettingsBytes) {
        return core::Result<std::size_t>::failure(
            strip_error(core::ErrorCode::serialization_overflow, "encode", "output-too-small"));
    }
    std::fill(output.begin(), output.begin() + static_cast<std::ptrdiff_t>(kStripSettingsBytes),
              std::byte{0});
    write_u32(output, 0U, kMagic);
    write_u16(output, 4U, kStripSettingsFormatVersion);
    write_u16(output, 6U, kStripSettingsBytes);
    output[12] = config.enabled ? std::byte{1} : std::byte{0};
    output[13] = static_cast<std::byte>(config.protocol);
    output[14] = static_cast<std::byte>(config.gpio);
    output[15] = static_cast<std::byte>(config.brightness);
    write_u16(output, 16U, config.pixel_count);
    output[18] = static_cast<std::byte>(config.red);
    output[19] = static_cast<std::byte>(config.green);
    output[20] = static_cast<std::byte>(config.blue);
    output[21] = static_cast<std::byte>(config.white);
    write_u16(output, 22U, config.power_budget_ma);
    write_u32(output, kCrcOffset, crc32(output.first(kStripSettingsBytes)));
    return core::Result<std::size_t>::success(kStripSettingsBytes);
}

core::Result<StripConfig> decode_strip_config(std::span<const std::byte> input) noexcept {
    if (input.size() != kStripSettingsBytes || read_u32(input, 0U) != kMagic ||
        (read_u16(input, 4U) != 1U &&
         read_u16(input, 4U) != kStripSettingsFormatVersion) ||
        read_u16(input, 6U) != kStripSettingsBytes || input[12] > std::byte{1} ||
        input[13] > std::byte{3} ||
        !std::all_of(input.begin() +
                         (read_u16(input, 4U) == 1U ? 22 : 24), input.end(),
                     [](std::byte value) { return value == std::byte{0}; }) ||
        (read_u16(input, 4U) == 1U &&
         (input[22] != std::byte{0} || input[23] != std::byte{0})) ||
        read_u32(input, kCrcOffset) != crc32(input)) {
        return core::Result<StripConfig>::failure(
            strip_error(core::ErrorCode::corrupt_data, "decode", "invalid-strip-record"));
    }
    StripConfig config{};
    config.enabled = input[12] == std::byte{1};
    config.protocol = static_cast<StripProtocol>(std::to_integer<std::uint8_t>(input[13]));
    config.gpio = std::to_integer<std::uint8_t>(input[14]);
    config.brightness = std::to_integer<std::uint8_t>(input[15]);
    config.pixel_count = read_u16(input, 16U);
    config.red = std::to_integer<std::uint8_t>(input[18]);
    config.green = std::to_integer<std::uint8_t>(input[19]);
    config.blue = std::to_integer<std::uint8_t>(input[20]);
    config.white = std::to_integer<std::uint8_t>(input[21]);
    if (read_u16(input, 4U) == kStripSettingsFormatVersion) {
        config.power_budget_ma = read_u16(input, 22U);
    }
    const auto valid = validate_strip_config(config);
    return valid ? core::Result<StripConfig>::success(config)
                 : core::Result<StripConfig>::failure(valid.error());
}

core::Result<std::size_t> fill_solid_frame(const StripConfig& config,
                                           std::span<std::uint8_t> output) noexcept {
    const auto valid = validate_strip_config(config);
    if (!valid) {
        return core::Result<std::size_t>::failure(valid.error());
    }
    if (config.protocol == StripProtocol::hd108_rgb ||
        config.protocol == StripProtocol::sk9822_rgb) {
        return core::Result<std::size_t>::failure(
            strip_error(core::ErrorCode::validation_failed, "fill-frame",
                        "clocked-encoder-required"));
    }
    const std::size_t channels = channel_count(config.protocol);
    const std::size_t required = config.pixel_count * channels;
    if (output.size() < required) {
        return core::Result<std::size_t>::failure(
            strip_error(core::ErrorCode::serialization_overflow, "fill-frame", "output-too-small"));
    }
    const std::array values{
        scale(config.green, config.brightness), scale(config.red, config.brightness),
        scale(config.blue, config.brightness), scale(config.white, config.brightness)};
    for (std::size_t pixel = 0; pixel < config.pixel_count; ++pixel) {
        std::copy_n(values.begin(), channels,
                    output.begin() + static_cast<std::ptrdiff_t>(pixel * channels));
    }
    return core::Result<std::size_t>::success(required);
}

} // namespace blip::led
