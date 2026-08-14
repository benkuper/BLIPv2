#include "blip/transport/legacy_serial_v1.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>

namespace blip::transport {
namespace {

[[nodiscard]] core::Error legacy_error(core::ErrorCode code, std::string_view operation,
                                       std::string_view detail) noexcept {
    return {core::ErrorDomain::transport, code, "blip.transport.serial.legacy-v1", operation,
            detail};
}

[[nodiscard]] bool valid_name(std::string_view value, bool dots) noexcept {
    if (value.empty() || value.size() > 96U) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [dots](char character) {
        return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == '_' || character == '-' ||
               (dots && character == '.');
    });
}

[[nodiscard]] core::Result<LegacySerialValue> parse_value(std::string_view token) noexcept {
    if (token.empty() || token.size() > 128U) {
        return core::Result<LegacySerialValue>::failure(
            legacy_error(core::ErrorCode::capacity_exceeded, "parse-value", "invalid-size"));
    }
    LegacySerialValue value{};
    if (token == "true" || token == "false") {
        value.type = LegacySerialValueType::boolean;
        value.boolean = token == "true";
        return core::Result<LegacySerialValue>::success(value);
    }
    // Preserve V1's documented quirk: a leading '-' token was treated as text.
    if (token.front() != '-') {
        if (token.find_first_of(".eE") != std::string_view::npos) {
            double number{};
            const auto converted = std::from_chars(token.data(), token.data() + token.size(),
                                                   number, std::chars_format::general);
            if (converted.ec == std::errc{} && converted.ptr == token.data() + token.size() &&
                std::isfinite(number)) {
                value.type = LegacySerialValueType::number;
                value.number = number;
                return core::Result<LegacySerialValue>::success(value);
            }
        } else {
            std::int64_t integer{};
            const auto converted =
                std::from_chars(token.data(), token.data() + token.size(), integer);
            if (converted.ec == std::errc{} && converted.ptr == token.data() + token.size()) {
                value.type = LegacySerialValueType::integer;
                value.integer = integer;
                return core::Result<LegacySerialValue>::success(value);
            }
        }
    }
    if (token.find_first_of("\r\n,") != std::string_view::npos) {
        return core::Result<LegacySerialValue>::failure(
            legacy_error(core::ErrorCode::corrupt_data, "parse-value", "invalid-text"));
    }
    value.type = LegacySerialValueType::string;
    value.text = token;
    return core::Result<LegacySerialValue>::success(value);
}

[[nodiscard]] bool safe_quoted(std::string_view value) noexcept {
    return value.find_first_of("\"\r\n") == std::string_view::npos;
}

} // namespace

core::Result<LegacySerialRequest>
LegacySerialV1Parser::parse(std::string_view line) const noexcept {
    if (line.empty() || line.size() > kMaxLegacySerialLineBytes) {
        return core::Result<LegacySerialRequest>::failure(
            legacy_error(core::ErrorCode::capacity_exceeded, "parse-line", "invalid-size"));
    }
    if (line.ends_with('\n')) {
        line.remove_suffix(1);
    }
    if (line.ends_with('\r')) {
        line.remove_suffix(1);
    }
    if (line.empty() || line.find_first_of("\r\n\0") != std::string_view::npos) {
        return core::Result<LegacySerialRequest>::failure(
            legacy_error(core::ErrorCode::corrupt_data, "parse-line", "invalid-line"));
    }
    if (line == "yo") {
        LegacySerialRequest request{};
        request.discovery = true;
        return core::Result<LegacySerialRequest>::success(request);
    }
    const std::size_t separator = line.find(' ');
    const std::string_view target = line.substr(0, separator);
    const std::size_t dot = target.rfind('.');
    if (dot == std::string_view::npos || dot == 0U || dot + 1U == target.size()) {
        return core::Result<LegacySerialRequest>::failure(
            legacy_error(core::ErrorCode::corrupt_data, "parse-line", "invalid-target"));
    }
    LegacySerialRequest request{};
    request.component_path = target.substr(0, dot);
    request.command = target.substr(dot + 1U);
    if (!valid_name(request.component_path, true) || !valid_name(request.command, false)) {
        return core::Result<LegacySerialRequest>::failure(
            legacy_error(core::ErrorCode::corrupt_data, "parse-line", "invalid-name"));
    }
    if (separator == std::string_view::npos) {
        return core::Result<LegacySerialRequest>::success(request);
    }
    std::string_view remaining = line.substr(separator + 1U);
    if (remaining.empty()) {
        return core::Result<LegacySerialRequest>::failure(
            legacy_error(core::ErrorCode::corrupt_data, "parse-line", "empty-values"));
    }
    while (!remaining.empty()) {
        if (request.value_count == request.values.size()) {
            return core::Result<LegacySerialRequest>::failure(
                legacy_error(core::ErrorCode::capacity_exceeded, "parse-line", "too-many-values"));
        }
        const std::size_t comma = remaining.find(',');
        const std::string_view token = remaining.substr(0, comma);
        const auto value = parse_value(token);
        if (!value) {
            return core::Result<LegacySerialRequest>::failure(value.error());
        }
        request.values[request.value_count++] = value.value();
        if (comma == std::string_view::npos) {
            break;
        }
        remaining.remove_prefix(comma + 1U);
        if (remaining.empty()) {
            return core::Result<LegacySerialRequest>::failure(
                legacy_error(core::ErrorCode::corrupt_data, "parse-line", "empty-value"));
        }
    }
    return core::Result<LegacySerialRequest>::success(request);
}

core::Result<std::size_t> format_legacy_discovery(std::string_view device_id,
                                                  std::string_view device_type,
                                                  std::string_view device_name,
                                                  std::string_view version,
                                                  std::span<char> output) noexcept {
    if (device_id.empty() || device_id.size() > 32U || device_type.empty() ||
        device_type.size() > 64U || device_name.empty() || device_name.size() > 64U ||
        version.empty() || version.size() > 24U || !safe_quoted(device_id) ||
        !safe_quoted(device_type) || !safe_quoted(device_name) || !safe_quoted(version)) {
        return core::Result<std::size_t>::failure(
            legacy_error(core::ErrorCode::invalid_argument, "format-discovery", "invalid-field"));
    }
    const std::size_t required = 7U + device_id.size() + 2U + device_type.size() + 3U +
                                 device_name.size() + 3U + version.size() + 2U;
    if (output.size() <= required) {
        return core::Result<std::size_t>::failure(legacy_error(
            core::ErrorCode::serialization_overflow, "format-discovery", "output-too-small"));
    }
    std::size_t offset{};
    const auto append = [&output, &offset](std::string_view value) {
        std::memcpy(output.data() + offset, value.data(), value.size());
        offset += value.size();
    };
    append("wassup ");
    append(device_id);
    append(" \"");
    append(device_type);
    append("\" \"");
    append(device_name);
    append("\" \"");
    append(version);
    append("\"\n");
    output[offset] = '\0';
    return core::Result<std::size_t>::success(offset);
}

} // namespace blip::transport
