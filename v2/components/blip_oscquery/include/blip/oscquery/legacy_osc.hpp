#pragma once

#include "blip/core/control.hpp"
#include "blip/core/registry.hpp"
#include "blip/oscquery/osc.hpp"

#include <array>
#include <string_view>

namespace blip::oscquery {

struct DeviceIdentity {
    std::string_view id{};
    std::string_view type{};
    std::string_view name{};
    std::string_view version{};
    std::uint16_t osc_port{9000};
};

class LegacyOscEndpoint {
  public:
    LegacyOscEndpoint(const core::RegistryView& registry, core::ControlService& controls,
                      DeviceIdentity identity) noexcept;
    void set_identity(DeviceIdentity identity) noexcept { identity_ = identity; }

    [[nodiscard]] core::Status handle(const OscMessage& request, std::string_view local_ip,
                                      bool udp_feedback, OscMessage& response,
                                      bool& should_reply) noexcept;

  private:
    [[nodiscard]] core::Status handle_control(const OscMessage& request, bool udp_feedback,
                                              OscMessage& response) noexcept;

    const core::RegistryView* registry_{};
    core::ControlService* controls_{};
    DeviceIdentity identity_{};
    std::array<char, kMaxOscAddressBytes + 1U> response_address_{};
    std::array<char, core::kControlResponseStringBytes> response_strings_{};
};

} // namespace blip::oscquery
