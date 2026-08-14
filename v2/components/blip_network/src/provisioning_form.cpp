#include "blip/network/provisioning_form.hpp"

#include <array>
#include <cstdint>

namespace blip::network {
namespace {

[[nodiscard]] core::Error form_error(core::ErrorCode code, std::string_view detail) noexcept {
    return {core::ErrorDomain::transport, code, "blip.transport.wifi", "provisioning-form", detail};
}

[[nodiscard]] int hex_value(char character) noexcept {
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

template <std::size_t Maximum>
[[nodiscard]] bool decode_text(std::string_view encoded, BoundedText<Maximum>& output) noexcept {
    std::array<char, Maximum> decoded{};
    std::size_t size{};
    for (std::size_t index = 0; index < encoded.size(); ++index) {
        if (size == decoded.size()) {
            return false;
        }
        const char character = encoded[index];
        if (character == '+') {
            decoded[size++] = ' ';
            continue;
        }
        if (character != '%') {
            decoded[size++] = character;
            continue;
        }
        if (encoded.size() - index < 3U) {
            return false;
        }
        const int high = hex_value(encoded[index + 1U]);
        const int low = hex_value(encoded[index + 2U]);
        if (high < 0 || low < 0) {
            return false;
        }
        decoded[size++] = static_cast<char>((high << 4U) | low);
        index += 2U;
    }
    return output.assign({decoded.data(), size});
}

} // namespace

core::Result<ProvisioningSubmission> parse_provisioning_form(std::string_view body) noexcept {
    if (body.empty() || body.size() > kMaxProvisioningFormBytes) {
        return core::Result<ProvisioningSubmission>::failure(
            form_error(core::ErrorCode::capacity_exceeded, "invalid-body-size"));
    }
    ProvisioningSubmission result{};
    bool has_ssid{};
    bool has_password{};
    while (!body.empty()) {
        const std::size_t separator = body.find('&');
        const std::string_view field = body.substr(0, separator);
        const std::size_t equals = field.find('=');
        if (equals == std::string_view::npos || equals == 0U) {
            return core::Result<ProvisioningSubmission>::failure(
                form_error(core::ErrorCode::corrupt_data, "invalid-field"));
        }
        BoundedText<16> key{};
        if (!decode_text(field.substr(0, equals), key)) {
            return core::Result<ProvisioningSubmission>::failure(
                form_error(core::ErrorCode::corrupt_data, "invalid-key"));
        }
        const auto value = field.substr(equals + 1U);
        if (key.view() == "ssid" && !has_ssid) {
            has_ssid = decode_text(value, result.ssid);
            if (!has_ssid) {
                return core::Result<ProvisioningSubmission>::failure(
                    form_error(core::ErrorCode::validation_failed, "invalid-ssid"));
            }
        } else if (key.view() == "password" && !has_password) {
            has_password = decode_text(value, result.password);
            if (!has_password) {
                return core::Result<ProvisioningSubmission>::failure(
                    form_error(core::ErrorCode::validation_failed, "invalid-password"));
            }
        } else {
            return core::Result<ProvisioningSubmission>::failure(
                form_error(core::ErrorCode::invalid_argument, "unknown-or-duplicate-field"));
        }
        if (separator == std::string_view::npos) {
            body = {};
        } else {
            body.remove_prefix(separator + 1U);
            if (body.empty()) {
                return core::Result<ProvisioningSubmission>::failure(
                    form_error(core::ErrorCode::corrupt_data, "empty-field"));
            }
        }
    }
    WifiConfig candidate{};
    candidate.ssid = result.ssid;
    candidate.password = result.password;
    const auto valid = validate_wifi_config(candidate, true);
    if (!has_ssid || !has_password || !valid) {
        return core::Result<ProvisioningSubmission>::failure(
            form_error(core::ErrorCode::validation_failed, "invalid-credentials"));
    }
    return core::Result<ProvisioningSubmission>::success(result);
}

} // namespace blip::network
