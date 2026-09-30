#include "network_lighting_features.hpp"

#include "blip/ddp/esp_ddp_component.hpp"

namespace blip::firmware {

core::Component& ddp_feature(led::EspRmtStripComponent& output) noexcept {
    static ddp::EspDdpComponent component{output};
    return component;
}

} // namespace blip::firmware
