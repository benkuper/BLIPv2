#pragma once

#include "blip/network/wifi_config.hpp"

#include <string_view>

namespace blip::network {

inline constexpr std::size_t kMaxProvisioningFormBytes = 384;

struct ProvisioningSubmission {
    WifiSsid ssid{};
    WifiPassword password{};
};

[[nodiscard]] core::Result<ProvisioningSubmission>
parse_provisioning_form(std::string_view body) noexcept;

} // namespace blip::network
