#pragma once

#include "blip/core/error.hpp"
#include "blip/storage/imported_settings.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace blip::network {

inline constexpr std::uint16_t kWifiSettingsFormatVersion = 2;
inline constexpr std::size_t kWifiSettingsHeaderBytes = 24;
inline constexpr std::size_t kMaxWifiSettingsBytes = 192;

enum class WifiMode : std::uint8_t { station = 0, access_point = 1, station_and_ap = 2 };
enum class WifiProtocol : std::uint8_t { b = 0, bg = 1, bgn = 2, ax = 3 };
enum class WifiAntenna : std::uint8_t { board_default = 0, onboard = 1, external = 2 };
enum class WifiSettingsSource : std::uint8_t { native, legacy_import };

template <std::size_t Maximum> struct BoundedText {
    std::array<char, Maximum + 1U> bytes{};
    std::uint16_t size{};

    [[nodiscard]] bool assign(std::string_view value) noexcept {
        if (value.size() > Maximum) {
            return false;
        }
        bytes.fill('\0');
        if (!value.empty()) {
            std::memcpy(bytes.data(), value.data(), value.size());
        }
        size = static_cast<std::uint16_t>(value.size());
        return true;
    }
    void clear() noexcept {
        bytes.fill('\0');
        size = 0;
    }
    [[nodiscard]] std::string_view view() const noexcept { return {bytes.data(), size}; }
    [[nodiscard]] bool empty() const noexcept { return size == 0U; }
    [[nodiscard]] bool operator==(const BoundedText&) const noexcept = default;
};

using WifiSsid = BoundedText<32>;
using WifiPassword = BoundedText<63>;
using Ipv4Text = BoundedText<15>;

struct WifiConfig {
    bool enabled{true};
    WifiMode mode{WifiMode::station};
    WifiSsid ssid{};
    WifiPassword password{};
    Ipv4Text manual_ip{};
    Ipv4Text manual_gateway{};
    bool channel_scan{true};
    std::uint8_t tx_power_index{2};
    WifiProtocol protocol{WifiProtocol::bgn};
    std::uint8_t channel{};
    WifiAntenna antenna{WifiAntenna::board_default};

    [[nodiscard]] bool operator==(const WifiConfig&) const noexcept = default;
};

struct DecodedWifiConfig {
    WifiConfig config{};
    WifiSettingsSource source{WifiSettingsSource::native};
};

[[nodiscard]] bool valid_utf8(std::string_view value) noexcept;
[[nodiscard]] bool valid_ipv4(std::string_view value) noexcept;
[[nodiscard]] core::Status validate_wifi_config(const WifiConfig& config,
                                                bool require_credentials = false) noexcept;
[[nodiscard]] core::Result<std::size_t> encode_wifi_config(const WifiConfig& config,
                                                           std::span<std::byte> output) noexcept;
[[nodiscard]] core::Result<DecodedWifiConfig>
decode_wifi_config(std::span<const std::byte> input,
                   storage::ImportedSettingsDecodeWorkspace& workspace) noexcept;

} // namespace blip::network
