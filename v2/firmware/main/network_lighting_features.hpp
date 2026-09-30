#pragma once

#include "blip/core/component.hpp"

namespace blip::led { class EspRmtStripComponent; }
namespace blip::network { class EspWifiComponent; }

namespace blip::firmware {

[[nodiscard]] core::Component& ddp_feature(led::EspRmtStripComponent& output) noexcept;
[[nodiscard]] core::Component& artnet_feature(network::EspWifiComponent& wifi,
                                              led::EspRmtStripComponent& output) noexcept;
[[nodiscard]] core::Component& e131_feature(network::EspWifiComponent& wifi,
                                            led::EspRmtStripComponent& output) noexcept;

} // namespace blip::firmware
