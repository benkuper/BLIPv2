#include "blip/network/wifi_config.hpp"

#include <algorithm>
#include <bit>
#include <limits>

namespace blip::network {
namespace {

constexpr std::uint32_t kWifiMagic = 0x46574c42U; // "BLWF".
constexpr std::size_t kCrcOffset = 8;

[[nodiscard]] core::Error config_error(core::ErrorCode code, std::string_view operation,
                                       std::string_view detail) noexcept {
    return {core::ErrorDomain::storage, code, "blip.transport.wifi", operation, detail};
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
        const std::uint8_t value = index >= kCrcOffset && index < kCrcOffset + 4U
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

[[nodiscard]] bool bounded_sizes(const WifiConfig& config) noexcept {
    return config.ssid.size <= 32U && config.password.size <= 63U && config.manual_ip.size <= 15U &&
           config.manual_gateway.size <= 15U;
}

[[nodiscard]] bool no_nul(std::string_view value) noexcept {
    return value.find('\0') == std::string_view::npos;
}

[[nodiscard]] bool valid_password(std::string_view password) noexcept {
    if (password.empty()) {
        return true;
    }
    return password.size() >= 8U && password.size() <= 63U &&
           std::all_of(password.begin(), password.end(), [](char character) {
               const auto value = static_cast<std::uint8_t>(character);
               return value >= 0x20U && value <= 0x7eU;
           });
}

[[nodiscard]] core::Result<std::int64_t> legacy_integer(const storage::LegacyValue& value,
                                                        std::string_view field) noexcept {
    if (value.type == storage::LegacyValueType::signed_integer) {
        return core::Result<std::int64_t>::success(value.signed_integer);
    }
    if (value.type == storage::LegacyValueType::unsigned_integer &&
        value.unsigned_integer <=
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return core::Result<std::int64_t>::success(
            static_cast<std::int64_t>(value.unsigned_integer));
    }
    return core::Result<std::int64_t>::failure(
        config_error(core::ErrorCode::corrupt_data, "decode-legacy", field));
}

struct LegacyContext {
    WifiConfig* config{};
};

core::Status apply_legacy_setting(void* opaque, const storage::ImportedSetting& setting) noexcept {
    auto& config = *static_cast<LegacyContext*>(opaque)->config;
    const auto type_error = [&setting]() {
        return core::Status::failure(
            config_error(core::ErrorCode::corrupt_data, "decode-legacy", setting.field));
    };
    if (setting.field == "enabled" || setting.field == "channelScanMode") {
        if (setting.value.type != storage::LegacyValueType::boolean) {
            return type_error();
        }
        if (setting.field == "enabled") {
            config.enabled = setting.value.boolean;
        } else {
            config.channel_scan = setting.value.boolean;
        }
        return core::Status::success();
    }
    if (setting.field == "ssid" || setting.field == "pass" || setting.field == "manualIP" ||
        setting.field == "manualGateway") {
        if (setting.value.type != storage::LegacyValueType::string) {
            return type_error();
        }
        const bool assigned =
            setting.field == "ssid"       ? config.ssid.assign(setting.value.string)
            : setting.field == "pass"     ? config.password.assign(setting.value.string)
            : setting.field == "manualIP" ? config.manual_ip.assign(setting.value.string)
                                          : config.manual_gateway.assign(setting.value.string);
        return assigned ? core::Status::success() : type_error();
    }
    if (setting.field == "mode" || setting.field == "txPower" || setting.field == "wifiProtocol" ||
        setting.field == "channel") {
        const auto integer = legacy_integer(setting.value, setting.field);
        if (!integer) {
            return core::Status::failure(integer.error());
        }
        if (setting.field == "mode") {
            if (integer.value() < 0 || integer.value() > 2) {
                return type_error();
            }
            config.mode = static_cast<WifiMode>(integer.value());
        } else if (setting.field == "txPower") {
            if (integer.value() < 0 || integer.value() > 3) {
                return type_error();
            }
            config.tx_power_index = static_cast<std::uint8_t>(integer.value());
        } else if (setting.field == "wifiProtocol") {
            if (integer.value() < 0 || integer.value() > 3) {
                return type_error();
            }
            config.protocol = static_cast<WifiProtocol>(integer.value());
        } else {
            if (integer.value() < 0 || integer.value() > 14) {
                return type_error();
            }
            config.channel = static_cast<std::uint8_t>(integer.value());
        }
    }
    return core::Status::success();
}

} // namespace

bool valid_utf8(std::string_view value) noexcept {
    std::size_t index{};
    while (index < value.size()) {
        const auto first = static_cast<std::uint8_t>(value[index]);
        if (first <= 0x7fU) {
            ++index;
            continue;
        }
        std::size_t trailing{};
        std::uint32_t code_point{};
        if (first >= 0xc2U && first <= 0xdfU) {
            trailing = 1;
            code_point = first & 0x1fU;
        } else if (first >= 0xe0U && first <= 0xefU) {
            trailing = 2;
            code_point = first & 0x0fU;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            trailing = 3;
            code_point = first & 0x07U;
        } else {
            return false;
        }
        if (index + trailing >= value.size()) {
            return false;
        }
        for (std::size_t offset = 1; offset <= trailing; ++offset) {
            const auto next = static_cast<std::uint8_t>(value[index + offset]);
            if ((next & 0xc0U) != 0x80U) {
                return false;
            }
            code_point = (code_point << 6U) | (next & 0x3fU);
        }
        if ((trailing == 2U && code_point < 0x800U) || (trailing == 3U && code_point < 0x10000U) ||
            code_point > 0x10ffffU || (code_point >= 0xd800U && code_point <= 0xdfffU)) {
            return false;
        }
        index += trailing + 1U;
    }
    return true;
}

bool valid_ipv4(std::string_view value) noexcept {
    if (value.empty() || value.size() > 15U) {
        return false;
    }
    std::size_t offset{};
    for (std::size_t part = 0; part < 4U; ++part) {
        if (offset == value.size()) {
            return false;
        }
        std::uint16_t number{};
        std::size_t digits{};
        while (offset < value.size() && value[offset] != '.') {
            const char character = value[offset++];
            if (character < '0' || character > '9' || digits == 3U) {
                return false;
            }
            number = static_cast<std::uint16_t>(number * 10U + character - '0');
            ++digits;
        }
        if (digits == 0U || number > 255U || (part < 3U && offset++ == value.size()) ||
            (part == 3U && offset != value.size())) {
            return false;
        }
    }
    return true;
}

core::Status validate_wifi_config(const WifiConfig& config, bool require_credentials) noexcept {
    if (!bounded_sizes(config) || static_cast<std::uint8_t>(config.mode) > 2U ||
        static_cast<std::uint8_t>(config.protocol) > 3U || config.tx_power_index > 3U ||
        config.channel > 14U || !valid_utf8(config.ssid.view()) || !no_nul(config.ssid.view()) ||
        !valid_password(config.password.view())) {
        return core::Status::failure(
            config_error(core::ErrorCode::validation_failed, "validate", "invalid-wifi-settings"));
    }
    if (require_credentials && config.ssid.empty()) {
        return core::Status::failure(
            config_error(core::ErrorCode::validation_failed, "validate", "ssid-required"));
    }
    const bool has_ip = !config.manual_ip.empty();
    const bool has_gateway = !config.manual_gateway.empty();
    if (has_ip != has_gateway || (has_ip && (!valid_ipv4(config.manual_ip.view()) ||
                                             !valid_ipv4(config.manual_gateway.view())))) {
        return core::Status::failure(
            config_error(core::ErrorCode::validation_failed, "validate", "invalid-static-ip"));
    }
    return core::Status::success();
}

core::Result<std::size_t> encode_wifi_config(const WifiConfig& config,
                                             std::span<std::byte> output) noexcept {
    const auto valid = validate_wifi_config(config);
    if (!valid) {
        return core::Result<std::size_t>::failure(valid.error());
    }
    const std::size_t required = kWifiSettingsHeaderBytes + config.ssid.size +
                                 config.password.size + config.manual_ip.size +
                                 config.manual_gateway.size;
    if (required > kMaxWifiSettingsBytes || output.size() < required) {
        return core::Result<std::size_t>::failure(
            config_error(core::ErrorCode::serialization_overflow, "encode", "output-too-small"));
    }
    std::fill(output.begin(), output.begin() + static_cast<std::ptrdiff_t>(required), std::byte{0});
    write_u32(output, 0, kWifiMagic);
    write_u16(output, 4, kWifiSettingsFormatVersion);
    write_u16(output, 6, kWifiSettingsHeaderBytes);
    output[12] = config.enabled ? std::byte{1} : std::byte{0};
    output[13] = static_cast<std::byte>(config.mode);
    output[14] = config.channel_scan ? std::byte{1} : std::byte{0};
    output[15] = static_cast<std::byte>(config.tx_power_index);
    output[16] = static_cast<std::byte>(config.protocol);
    output[17] = static_cast<std::byte>(config.channel);
    output[18] = static_cast<std::byte>(config.ssid.size);
    output[19] = static_cast<std::byte>(config.password.size);
    output[20] = static_cast<std::byte>(config.manual_ip.size);
    output[21] = static_cast<std::byte>(config.manual_gateway.size);
    std::size_t offset = kWifiSettingsHeaderBytes;
    const auto append = [&output, &offset](std::string_view value) {
        if (!value.empty()) {
            std::memcpy(output.data() + offset, value.data(), value.size());
            offset += value.size();
        }
    };
    append(config.ssid.view());
    append(config.password.view());
    append(config.manual_ip.view());
    append(config.manual_gateway.view());
    write_u32(output, kCrcOffset, crc32(output.first(required)));
    return core::Result<std::size_t>::success(required);
}

core::Result<DecodedWifiConfig>
decode_wifi_config(std::span<const std::byte> input,
                   storage::ImportedSettingsDecodeWorkspace& workspace) noexcept {
    if (input.size() >= kWifiSettingsHeaderBytes && read_u32(input, 0) == kWifiMagic) {
        if (read_u16(input, 4) != kWifiSettingsFormatVersion ||
            read_u16(input, 6) != kWifiSettingsHeaderBytes || input[12] > std::byte{1} ||
            input[14] > std::byte{1} || input[22] != std::byte{0} || input[23] != std::byte{0} ||
            read_u32(input, kCrcOffset) != crc32(input)) {
            return core::Result<DecodedWifiConfig>::failure(
                config_error(core::ErrorCode::corrupt_data, "decode", "invalid-header-or-crc"));
        }
        const std::size_t ssid_size = std::to_integer<std::uint8_t>(input[18]);
        const std::size_t password_size = std::to_integer<std::uint8_t>(input[19]);
        const std::size_t ip_size = std::to_integer<std::uint8_t>(input[20]);
        const std::size_t gateway_size = std::to_integer<std::uint8_t>(input[21]);
        if (ssid_size > 32U || password_size > 63U || ip_size > 15U || gateway_size > 15U ||
            input.size() !=
                kWifiSettingsHeaderBytes + ssid_size + password_size + ip_size + gateway_size) {
            return core::Result<DecodedWifiConfig>::failure(
                config_error(core::ErrorCode::corrupt_data, "decode", "invalid-length"));
        }
        WifiConfig config{};
        config.enabled = input[12] == std::byte{1};
        config.mode = static_cast<WifiMode>(std::to_integer<std::uint8_t>(input[13]));
        config.channel_scan = input[14] == std::byte{1};
        config.tx_power_index = std::to_integer<std::uint8_t>(input[15]);
        config.protocol = static_cast<WifiProtocol>(std::to_integer<std::uint8_t>(input[16]));
        config.channel = std::to_integer<std::uint8_t>(input[17]);
        std::size_t offset = kWifiSettingsHeaderBytes;
        const auto text = [&input, &offset](std::size_t size) {
            const std::string_view result{reinterpret_cast<const char*>(input.data() + offset),
                                          size};
            offset += size;
            return result;
        };
        static_cast<void>(config.ssid.assign(text(ssid_size)));
        static_cast<void>(config.password.assign(text(password_size)));
        static_cast<void>(config.manual_ip.assign(text(ip_size)));
        static_cast<void>(config.manual_gateway.assign(text(gateway_size)));
        const auto valid = validate_wifi_config(config);
        if (!valid) {
            return core::Result<DecodedWifiConfig>::failure(valid.error());
        }
        return core::Result<DecodedWifiConfig>::success({config, WifiSettingsSource::native});
    }

    WifiConfig config{};
    LegacyContext context{&config};
    storage::ImportedSettingsView imported{};
    const auto visited = imported.visit(input, workspace, apply_legacy_setting, &context);
    if (!visited) {
        return core::Result<DecodedWifiConfig>::failure(visited.error());
    }
    const auto valid = validate_wifi_config(config);
    if (!valid) {
        return core::Result<DecodedWifiConfig>::failure(valid.error());
    }
    return core::Result<DecodedWifiConfig>::success({config, WifiSettingsSource::legacy_import});
}

} // namespace blip::network
