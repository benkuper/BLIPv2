#include "blip/transport/envelope.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

namespace blip::transport {
namespace {

constexpr std::array<std::byte, 4> kMagic{std::byte{'B'}, std::byte{'L'}, std::byte{'I'},
                                          std::byte{'P'}};
constexpr std::size_t kEnvelopeCrcOffset = 20;

[[nodiscard]] core::Error wire_error(core::ErrorCode code, std::string_view operation,
                                     std::string_view detail) noexcept {
    return {core::ErrorDomain::transport, code, "blip.transport", operation, detail};
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

void write_u64(std::span<std::byte> output, std::size_t offset, std::uint64_t value) noexcept {
    for (std::size_t index = 0; index < 8U; ++index) {
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

[[nodiscard]] std::uint64_t read_u64(std::span<const std::byte> input,
                                     std::size_t offset) noexcept {
    std::uint64_t value{};
    for (std::size_t index = 0; index < 8U; ++index) {
        value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(input[offset + index]))
                 << (index * 8U);
    }
    return value;
}

[[nodiscard]] std::uint32_t crc32(std::span<const std::byte> input,
                                  std::size_t zero_offset = std::numeric_limits<std::size_t>::max(),
                                  std::size_t zero_size = 0) noexcept {
    std::uint32_t crc = 0xffffffffU;
    for (std::size_t index = 0; index < input.size(); ++index) {
        const std::uint8_t value = index >= zero_offset && index < zero_offset + zero_size
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

[[nodiscard]] constexpr bool valid_envelope_kind(EnvelopeKind kind) noexcept {
    return static_cast<std::uint8_t>(kind) >= static_cast<std::uint8_t>(EnvelopeKind::request) &&
           static_cast<std::uint8_t>(kind) <= static_cast<std::uint8_t>(EnvelopeKind::error);
}

[[nodiscard]] constexpr bool valid_payload_type(PayloadType type) noexcept {
    return static_cast<std::uint16_t>(type) >= static_cast<std::uint16_t>(PayloadType::control) &&
           static_cast<std::uint16_t>(type) <=
               static_cast<std::uint16_t>(PayloadType::provisioning);
}

[[nodiscard]] constexpr bool valid_control_operation(core::ControlOperation operation) noexcept {
    return static_cast<std::uint8_t>(operation) >=
               static_cast<std::uint8_t>(core::ControlOperation::read_parameter) &&
           static_cast<std::uint8_t>(operation) <=
               static_cast<std::uint8_t>(core::ControlOperation::invoke_action);
}

[[nodiscard]] constexpr bool valid_value_type(core::ValueType type) noexcept {
    return static_cast<std::uint8_t>(type) <= static_cast<std::uint8_t>(core::ValueType::string);
}

[[nodiscard]] constexpr bool valid_error_domain(core::ErrorDomain domain) noexcept {
    return static_cast<std::uint8_t>(domain) <=
           static_cast<std::uint8_t>(core::ErrorDomain::transport);
}

[[nodiscard]] constexpr bool valid_error_code(core::ErrorCode code) noexcept {
    return static_cast<std::uint16_t>(code) <=
           static_cast<std::uint16_t>(core::ErrorCode::generation_exhausted);
}

[[nodiscard]] bool valid_name(std::string_view value, std::size_t maximum,
                              bool require_dot) noexcept {
    if (value.empty() || value.size() > maximum ||
        (require_dot && value.find('.') == std::string_view::npos)) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](char character) {
        return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
               character == '_' || character == '-' || character == '.';
    });
}

[[nodiscard]] bool valid_detail(std::string_view value) noexcept {
    return value.size() <= kMaxControlDetailBytes &&
           std::all_of(value.begin(), value.end(), [](char character) {
               const auto byte = static_cast<std::uint8_t>(character);
               return byte >= 0x20U && byte <= 0x7eU;
           });
}

[[nodiscard]] bool valid_utf8(std::string_view value) noexcept {
    const auto byte_at = [&value](std::size_t index) {
        return static_cast<std::uint8_t>(value[index]);
    };
    std::size_t index{};
    while (index < value.size()) {
        const std::uint8_t lead = byte_at(index++);
        if (lead <= 0x7fU) {
            continue;
        }
        std::size_t continuation_count{};
        std::uint8_t first_min{0x80U};
        std::uint8_t first_max{0xbfU};
        if (lead >= 0xc2U && lead <= 0xdfU) {
            continuation_count = 1;
        } else if (lead >= 0xe0U && lead <= 0xefU) {
            continuation_count = 2;
            first_min = lead == 0xe0U ? 0xa0U : 0x80U;
            first_max = lead == 0xedU ? 0x9fU : 0xbfU;
        } else if (lead >= 0xf0U && lead <= 0xf4U) {
            continuation_count = 3;
            first_min = lead == 0xf0U ? 0x90U : 0x80U;
            first_max = lead == 0xf4U ? 0x8fU : 0xbfU;
        } else {
            return false;
        }
        if (continuation_count > value.size() - index) {
            return false;
        }
        const std::uint8_t first = byte_at(index++);
        if (first < first_min || first > first_max) {
            return false;
        }
        for (std::size_t continuation = 1; continuation < continuation_count; ++continuation) {
            const std::uint8_t byte = byte_at(index++);
            if (byte < 0x80U || byte > 0xbfU) {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] std::size_t encoded_value_size(const core::ScalarValue& value) noexcept {
    switch (value.type) {
    case core::ValueType::boolean:
        return 1;
    case core::ValueType::integer:
    case core::ValueType::number:
        return 8;
    case core::ValueType::string:
        return value.string.size();
    }
    return 0;
}

} // namespace

core::Result<std::size_t> encode_envelope(const EnvelopeInput& input,
                                          std::span<std::byte> output) noexcept {
    if (!valid_envelope_kind(input.kind) || !valid_payload_type(input.payload_type) ||
        input.flags != 0U || input.payload.size() > kMaxEnvelopePayloadBytes ||
        output.size() < kEnvelopeHeaderBytes + input.payload.size()) {
        return core::Result<std::size_t>::failure(
            wire_error(output.size() < kEnvelopeHeaderBytes + input.payload.size()
                           ? core::ErrorCode::serialization_overflow
                           : core::ErrorCode::invalid_argument,
                       "encode-envelope", "invalid-envelope-or-output"));
    }
    const std::size_t total = kEnvelopeHeaderBytes + input.payload.size();
    std::fill(output.begin(), output.begin() + static_cast<std::ptrdiff_t>(total), std::byte{0});
    std::copy(kMagic.begin(), kMagic.end(), output.begin());
    output[4] = static_cast<std::byte>(kEnvelopeFormatVersion);
    output[5] = static_cast<std::byte>(kEnvelopeHeaderBytes);
    output[6] = static_cast<std::byte>(input.kind);
    output[7] = static_cast<std::byte>(input.flags);
    write_u32(output, 8, input.request_id);
    write_u16(output, 12, static_cast<std::uint16_t>(input.payload_type));
    write_u16(output, 14, static_cast<std::uint16_t>(input.payload.size()));
    if (!input.payload.empty()) {
        std::memcpy(output.data() + kEnvelopeHeaderBytes, input.payload.data(),
                    input.payload.size());
    }
    write_u32(output, kEnvelopeCrcOffset,
              crc32(output.first(total), kEnvelopeCrcOffset, sizeof(std::uint32_t)));
    return core::Result<std::size_t>::success(total);
}

core::Result<EnvelopeView> decode_envelope(std::span<const std::byte> input) noexcept {
    if (input.size() < kEnvelopeHeaderBytes ||
        !std::equal(kMagic.begin(), kMagic.end(), input.begin()) ||
        std::to_integer<std::uint8_t>(input[4]) != kEnvelopeFormatVersion ||
        std::to_integer<std::uint8_t>(input[5]) != kEnvelopeHeaderBytes) {
        return core::Result<EnvelopeView>::failure(
            wire_error(core::ErrorCode::incompatible_version, "decode-envelope", "invalid-header"));
    }
    const auto kind = static_cast<EnvelopeKind>(std::to_integer<std::uint8_t>(input[6]));
    const auto type = static_cast<PayloadType>(read_u16(input, 12));
    const std::uint16_t payload_size = read_u16(input, 14);
    if (!valid_envelope_kind(kind) || !valid_payload_type(type) || input[7] != std::byte{0} ||
        read_u32(input, 16) != 0U || payload_size > kMaxEnvelopePayloadBytes ||
        input.size() != kEnvelopeHeaderBytes + payload_size ||
        read_u32(input, kEnvelopeCrcOffset) !=
            crc32(input, kEnvelopeCrcOffset, sizeof(std::uint32_t))) {
        return core::Result<EnvelopeView>::failure(
            wire_error(core::ErrorCode::corrupt_data, "decode-envelope", "invalid-envelope"));
    }
    return core::Result<EnvelopeView>::success(
        {kind, type, read_u32(input, 8), 0, input.subspan(kEnvelopeHeaderBytes, payload_size)});
}

core::Result<std::size_t> encode_control_message(const ControlMessage& message,
                                                 std::span<std::byte> output) noexcept {
    if (!valid_control_operation(message.operation) || !valid_error_domain(message.error_domain) ||
        !valid_error_code(message.error_code) ||
        (message.error_code == core::ErrorCode::none) !=
            (message.error_domain == core::ErrorDomain::none) ||
        !valid_name(message.component_id, kMaxControlComponentBytes, true) ||
        !valid_name(message.control_id, kMaxControlIdBytes, false) ||
        !valid_detail(message.detail) || message.values.size() > core::kMaxControlValues) {
        return core::Result<std::size_t>::failure(
            wire_error(core::ErrorCode::invalid_argument, "encode-control", "invalid-message"));
    }
    std::size_t required = kControlHeaderBytes + message.component_id.size() +
                           message.control_id.size() + message.detail.size();
    for (const auto& value : message.values) {
        const std::size_t value_size = encoded_value_size(value);
        if (!valid_value_type(value.type) ||
            (value.type == core::ValueType::boolean && value_size != 1U) ||
            (value.type == core::ValueType::number && !std::isfinite(value.number)) ||
            (value.type == core::ValueType::string &&
             (value.string.size() > kMaxControlStringBytes || !valid_utf8(value.string)))) {
            return core::Result<std::size_t>::failure(
                wire_error(core::ErrorCode::invalid_argument, "encode-control", "invalid-value"));
        }
        required += 4U + value_size;
    }
    if (required > kMaxControlPayloadBytes || output.size() < required) {
        return core::Result<std::size_t>::failure(wire_error(
            core::ErrorCode::serialization_overflow, "encode-control", "output-too-small"));
    }
    std::fill(output.begin(), output.begin() + static_cast<std::ptrdiff_t>(required), std::byte{0});
    output[0] = static_cast<std::byte>(kControlPayloadFormatVersion);
    output[1] = static_cast<std::byte>(message.operation);
    output[2] = static_cast<std::byte>(message.values.size());
    output[4] = static_cast<std::byte>(message.error_domain);
    write_u16(output, 6, static_cast<std::uint16_t>(message.error_code));
    output[8] = static_cast<std::byte>(message.component_id.size());
    output[9] = static_cast<std::byte>(message.control_id.size());
    output[10] = static_cast<std::byte>(message.detail.size());
    std::size_t offset = kControlHeaderBytes;
    const auto append_text = [&output, &offset](std::string_view value) {
        if (!value.empty()) {
            std::memcpy(output.data() + offset, value.data(), value.size());
            offset += value.size();
        }
    };
    append_text(message.component_id);
    append_text(message.control_id);
    append_text(message.detail);
    for (const auto& value : message.values) {
        const auto value_size = static_cast<std::uint16_t>(encoded_value_size(value));
        output[offset] = static_cast<std::byte>(value.type);
        write_u16(output, offset + 2U, value_size);
        offset += 4U;
        switch (value.type) {
        case core::ValueType::boolean:
            output[offset] = value.boolean ? std::byte{1} : std::byte{0};
            break;
        case core::ValueType::integer:
            write_u64(output, offset, std::bit_cast<std::uint64_t>(value.integer));
            break;
        case core::ValueType::number:
            write_u64(output, offset, std::bit_cast<std::uint64_t>(value.number));
            break;
        case core::ValueType::string:
            if (!value.string.empty()) {
                std::memcpy(output.data() + offset, value.string.data(), value.string.size());
            }
            break;
        }
        offset += value_size;
    }
    return core::Result<std::size_t>::success(offset);
}

core::Result<DecodedControlMessage>
decode_control_message(std::span<const std::byte> input) noexcept {
    if (input.size() < kControlHeaderBytes ||
        std::to_integer<std::uint8_t>(input[0]) != kControlPayloadFormatVersion ||
        input[3] != std::byte{0} || input[5] != std::byte{0} || input[11] != std::byte{0}) {
        return core::Result<DecodedControlMessage>::failure(
            wire_error(core::ErrorCode::incompatible_version, "decode-control", "invalid-header"));
    }
    DecodedControlMessage result{};
    result.operation = static_cast<core::ControlOperation>(std::to_integer<std::uint8_t>(input[1]));
    result.value_count = std::to_integer<std::uint8_t>(input[2]);
    result.error_domain = static_cast<core::ErrorDomain>(std::to_integer<std::uint8_t>(input[4]));
    result.error_code = static_cast<core::ErrorCode>(read_u16(input, 6));
    const std::size_t component_size = std::to_integer<std::uint8_t>(input[8]);
    const std::size_t control_size = std::to_integer<std::uint8_t>(input[9]);
    const std::size_t detail_size = std::to_integer<std::uint8_t>(input[10]);
    if (!valid_control_operation(result.operation) || !valid_error_domain(result.error_domain) ||
        !valid_error_code(result.error_code) ||
        (result.error_code == core::ErrorCode::none) !=
            (result.error_domain == core::ErrorDomain::none) ||
        result.value_count > result.values.size() || component_size > kMaxControlComponentBytes ||
        control_size > kMaxControlIdBytes || detail_size > kMaxControlDetailBytes ||
        component_size + control_size + detail_size > input.size() - kControlHeaderBytes) {
        return core::Result<DecodedControlMessage>::failure(
            wire_error(core::ErrorCode::corrupt_data, "decode-control", "invalid-message"));
    }
    std::size_t offset = kControlHeaderBytes;
    result.component_id = {reinterpret_cast<const char*>(input.data() + offset), component_size};
    offset += component_size;
    result.control_id = {reinterpret_cast<const char*>(input.data() + offset), control_size};
    offset += control_size;
    result.detail = {reinterpret_cast<const char*>(input.data() + offset), detail_size};
    offset += detail_size;
    if (!valid_name(result.component_id, kMaxControlComponentBytes, true) ||
        !valid_name(result.control_id, kMaxControlIdBytes, false) || !valid_detail(result.detail)) {
        return core::Result<DecodedControlMessage>::failure(
            wire_error(core::ErrorCode::corrupt_data, "decode-control", "invalid-text"));
    }
    for (std::size_t index = 0; index < result.value_count; ++index) {
        if (input.size() - offset < 4U) {
            return core::Result<DecodedControlMessage>::failure(
                wire_error(core::ErrorCode::corrupt_data, "decode-control", "truncated-value"));
        }
        const auto type =
            static_cast<core::ValueType>(std::to_integer<std::uint8_t>(input[offset]));
        const std::uint16_t size = read_u16(input, offset + 2U);
        if (!valid_value_type(type) || input[offset + 1U] != std::byte{0} ||
            size > input.size() - offset - 4U) {
            return core::Result<DecodedControlMessage>::failure(
                wire_error(core::ErrorCode::corrupt_data, "decode-control", "invalid-value"));
        }
        offset += 4U;
        auto& value = result.values[index];
        value.type = type;
        switch (type) {
        case core::ValueType::boolean:
            if (size != 1U || std::to_integer<std::uint8_t>(input[offset]) > 1U) {
                return core::Result<DecodedControlMessage>::failure(
                    wire_error(core::ErrorCode::corrupt_data, "decode-control", "invalid-boolean"));
            }
            value.boolean = input[offset] == std::byte{1};
            break;
        case core::ValueType::integer:
            if (size != 8U) {
                return core::Result<DecodedControlMessage>::failure(
                    wire_error(core::ErrorCode::corrupt_data, "decode-control", "invalid-integer"));
            }
            value.integer = std::bit_cast<std::int64_t>(read_u64(input, offset));
            break;
        case core::ValueType::number:
            if (size != 8U) {
                return core::Result<DecodedControlMessage>::failure(
                    wire_error(core::ErrorCode::corrupt_data, "decode-control", "invalid-number"));
            }
            value.number = std::bit_cast<double>(read_u64(input, offset));
            if (!std::isfinite(value.number)) {
                return core::Result<DecodedControlMessage>::failure(wire_error(
                    core::ErrorCode::corrupt_data, "decode-control", "non-finite-number"));
            }
            break;
        case core::ValueType::string:
            if (size > kMaxControlStringBytes) {
                return core::Result<DecodedControlMessage>::failure(wire_error(
                    core::ErrorCode::capacity_exceeded, "decode-control", "string-too-long"));
            }
            value.string = {reinterpret_cast<const char*>(input.data() + offset), size};
            if (!valid_utf8(value.string)) {
                return core::Result<DecodedControlMessage>::failure(
                    wire_error(core::ErrorCode::corrupt_data, "decode-control", "invalid-utf8"));
            }
            break;
        }
        offset += size;
    }
    if (offset != input.size()) {
        return core::Result<DecodedControlMessage>::failure(
            wire_error(core::ErrorCode::corrupt_data, "decode-control", "trailing-data"));
    }
    return core::Result<DecodedControlMessage>::success(result);
}

} // namespace blip::transport
