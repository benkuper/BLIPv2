#include "blip/oscquery/osc.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace blip::oscquery {
namespace {

[[nodiscard]] core::Error osc_error(core::ErrorCode code, std::string_view operation,
                                    std::string_view detail) noexcept {
    return {core::ErrorDomain::transport, code, "blip.oscquery", operation, detail};
}

[[nodiscard]] constexpr std::size_t padded_size(std::size_t text_size) noexcept {
    return (text_size + 4U) & ~std::size_t{3U};
}

[[nodiscard]] bool valid_utf8(std::string_view text) noexcept {
    std::size_t index{};
    while (index < text.size()) {
        const auto first = static_cast<std::uint8_t>(text[index]);
        std::size_t continuation{};
        std::uint32_t value{};
        if (first <= 0x7fU) {
            ++index;
            continue;
        }
        if (first >= 0xc2U && first <= 0xdfU) {
            continuation = 1;
            value = first & 0x1fU;
        } else if (first >= 0xe0U && first <= 0xefU) {
            continuation = 2;
            value = first & 0x0fU;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            continuation = 3;
            value = first & 0x07U;
        } else {
            return false;
        }
        if (index + continuation >= text.size()) {
            return false;
        }
        for (std::size_t offset = 1; offset <= continuation; ++offset) {
            const auto next = static_cast<std::uint8_t>(text[index + offset]);
            if ((next & 0xc0U) != 0x80U) {
                return false;
            }
            value = (value << 6U) | (next & 0x3fU);
        }
        if ((continuation == 2U && value < 0x800U) || (continuation == 3U && value < 0x10000U) ||
            value > 0x10ffffU || (value >= 0xd800U && value <= 0xdfffU)) {
            return false;
        }
        index += continuation + 1U;
    }
    return true;
}

[[nodiscard]] bool valid_address(std::string_view address) noexcept {
    if (address.empty() || address.size() > kMaxOscAddressBytes || address.front() != '/' ||
        address.find('\0') != std::string_view::npos) {
        return false;
    }
    for (const char character : address) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < 0x21U || byte > 0x7eU || character == '#' || character == '*' ||
            character == ',' || character == '?' || character == '[' || character == ']' ||
            character == '{' || character == '}') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool read_string(std::span<const std::byte> packet, std::size_t& position,
                               std::size_t maximum, std::string_view& output) noexcept {
    if (position >= packet.size()) {
        return false;
    }
    std::size_t end = position;
    while (end < packet.size() && packet[end] != std::byte{0}) {
        ++end;
    }
    if (end == packet.size() || end - position > maximum) {
        return false;
    }
    const std::size_t encoded_size = padded_size(end - position);
    if (position + encoded_size > packet.size()) {
        return false;
    }
    for (std::size_t index = end; index < position + encoded_size; ++index) {
        if (packet[index] != std::byte{0}) {
            return false;
        }
    }
    output = {reinterpret_cast<const char*>(packet.data() + position), end - position};
    position += encoded_size;
    return valid_utf8(output);
}

[[nodiscard]] std::uint32_t read_u32(std::span<const std::byte> input,
                                     std::size_t position) noexcept {
    return (std::to_integer<std::uint32_t>(input[position]) << 24U) |
           (std::to_integer<std::uint32_t>(input[position + 1U]) << 16U) |
           (std::to_integer<std::uint32_t>(input[position + 2U]) << 8U) |
           std::to_integer<std::uint32_t>(input[position + 3U]);
}

void write_u32(std::span<std::byte> output, std::size_t position, std::uint32_t value) noexcept {
    output[position] = static_cast<std::byte>((value >> 24U) & 0xffU);
    output[position + 1U] = static_cast<std::byte>((value >> 16U) & 0xffU);
    output[position + 2U] = static_cast<std::byte>((value >> 8U) & 0xffU);
    output[position + 3U] = static_cast<std::byte>(value & 0xffU);
}

[[nodiscard]] bool write_string(std::string_view text, std::span<std::byte> output,
                                std::size_t& position, std::size_t maximum) noexcept {
    const std::size_t encoded_size = padded_size(text.size());
    if (text.size() > maximum || text.find('\0') != std::string_view::npos || !valid_utf8(text) ||
        position + encoded_size > output.size()) {
        return false;
    }
    for (const char character : text) {
        output[position++] = static_cast<std::byte>(static_cast<unsigned char>(character));
    }
    const std::size_t end = position - text.size() + encoded_size;
    while (position < end) {
        output[position++] = std::byte{0};
    }
    return true;
}

} // namespace

core::Result<OscMessage> decode_osc_message(std::span<const std::byte> packet) noexcept {
    if (packet.empty() || packet.size() > kMaxOscPacketBytes || (packet.size() & 3U) != 0U) {
        return core::Result<OscMessage>::failure(
            osc_error(core::ErrorCode::invalid_argument, "decode", "packet-size"));
    }
    OscMessage message{};
    std::size_t position{};
    if (!read_string(packet, position, kMaxOscAddressBytes, message.address) ||
        !valid_address(message.address)) {
        return core::Result<OscMessage>::failure(
            osc_error(core::ErrorCode::invalid_argument, "decode", "address"));
    }
    std::string_view tags{};
    if (!read_string(packet, position, kMaxOscArguments + 1U, tags) || tags.empty() ||
        tags.front() != ',' || tags.size() - 1U > kMaxOscArguments) {
        return core::Result<OscMessage>::failure(
            osc_error(core::ErrorCode::invalid_argument, "decode", "type-tags"));
    }
    message.argument_count = tags.size() - 1U;
    for (std::size_t index = 0; index < message.argument_count; ++index) {
        auto& argument = message.arguments[index];
        const char tag = tags[index + 1U];
        if (tag == 'T' || tag == 'F') {
            argument = OscValue::from_bool(tag == 'T');
            continue;
        }
        if (tag == 's') {
            std::string_view value{};
            if (!read_string(packet, position, kMaxOscStringBytes, value)) {
                return core::Result<OscMessage>::failure(
                    osc_error(core::ErrorCode::invalid_argument, "decode", "string"));
            }
            argument = OscValue::from_string(value);
            continue;
        }
        const std::size_t required = tag == 't' ? 8U : 4U;
        if (position + required > packet.size()) {
            return core::Result<OscMessage>::failure(
                osc_error(core::ErrorCode::invalid_argument, "decode", "argument-size"));
        }
        const std::uint32_t first = read_u32(packet, position);
        position += 4U;
        switch (tag) {
        case 'i':
            argument = OscValue::from_integer(std::bit_cast<std::int32_t>(first));
            break;
        case 'f':
            argument = OscValue::from_number(std::bit_cast<float>(first));
            break;
        case 'r':
            argument.type = OscValueType::rgba;
            argument.word = first;
            break;
        case 'm':
            argument.type = OscValueType::midi;
            argument.word = first;
            break;
        case 't':
            argument.type = OscValueType::timetag;
            argument.timetag =
                (static_cast<std::uint64_t>(first) << 32U) | read_u32(packet, position);
            position += 4U;
            break;
        default:
            return core::Result<OscMessage>::failure(
                osc_error(core::ErrorCode::invalid_argument, "decode", "unsupported-type"));
        }
    }
    if (position != packet.size()) {
        return core::Result<OscMessage>::failure(
            osc_error(core::ErrorCode::invalid_argument, "decode", "trailing-bytes"));
    }
    return core::Result<OscMessage>::success(message);
}

core::Result<std::size_t> encode_osc_message(const OscMessage& message,
                                             std::span<std::byte> output) noexcept {
    if (!valid_address(message.address) || message.argument_count > kMaxOscArguments ||
        output.empty()) {
        return core::Result<std::size_t>::failure(
            osc_error(core::ErrorCode::invalid_argument, "encode", "message"));
    }
    output = output.first(std::min(output.size(), kMaxOscPacketBytes));
    std::size_t position{};
    if (!write_string(message.address, output, position, kMaxOscAddressBytes)) {
        return core::Result<std::size_t>::failure(
            osc_error(core::ErrorCode::serialization_overflow, "encode", "address"));
    }
    std::array<char, kMaxOscArguments + 1U> tags{};
    tags[0] = ',';
    for (std::size_t index = 0; index < message.argument_count; ++index) {
        const auto& argument = message.arguments[index];
        switch (argument.type) {
        case OscValueType::boolean:
            tags[index + 1U] = argument.boolean ? 'T' : 'F';
            break;
        case OscValueType::int32:
            tags[index + 1U] = 'i';
            break;
        case OscValueType::float32:
            tags[index + 1U] = 'f';
            break;
        case OscValueType::string:
            tags[index + 1U] = 's';
            break;
        case OscValueType::rgba:
            tags[index + 1U] = 'r';
            break;
        case OscValueType::midi:
            tags[index + 1U] = 'm';
            break;
        case OscValueType::timetag:
            tags[index + 1U] = 't';
            break;
        }
    }
    if (!write_string({tags.data(), message.argument_count + 1U}, output, position,
                      kMaxOscArguments + 1U)) {
        return core::Result<std::size_t>::failure(
            osc_error(core::ErrorCode::serialization_overflow, "encode", "type-tags"));
    }
    for (std::size_t index = 0; index < message.argument_count; ++index) {
        const auto& argument = message.arguments[index];
        if (argument.type == OscValueType::boolean) {
            continue;
        }
        if (argument.type == OscValueType::string) {
            if (!write_string(argument.string, output, position, kMaxOscStringBytes)) {
                return core::Result<std::size_t>::failure(
                    osc_error(core::ErrorCode::serialization_overflow, "encode", "string"));
            }
            continue;
        }
        const std::size_t required = argument.type == OscValueType::timetag ? 8U : 4U;
        if (position + required > output.size()) {
            return core::Result<std::size_t>::failure(
                osc_error(core::ErrorCode::serialization_overflow, "encode", "argument"));
        }
        std::uint32_t first{};
        switch (argument.type) {
        case OscValueType::int32:
            first = std::bit_cast<std::uint32_t>(argument.integer);
            break;
        case OscValueType::float32:
            first = std::bit_cast<std::uint32_t>(argument.number);
            break;
        case OscValueType::rgba:
        case OscValueType::midi:
            first = argument.word;
            break;
        case OscValueType::timetag:
            first = static_cast<std::uint32_t>(argument.timetag >> 32U);
            break;
        case OscValueType::boolean:
        case OscValueType::string:
            break;
        }
        write_u32(output, position, first);
        position += 4U;
        if (argument.type == OscValueType::timetag) {
            write_u32(output, position, static_cast<std::uint32_t>(argument.timetag));
            position += 4U;
        }
    }
    return core::Result<std::size_t>::success(position);
}

} // namespace blip::oscquery
