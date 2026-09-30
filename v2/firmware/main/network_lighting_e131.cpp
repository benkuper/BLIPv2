#include "network_lighting_features.hpp"

#include "blip/e131/esp_e131_component.hpp"

namespace blip::firmware {

core::Component& e131_feature(network::EspWifiComponent& wifi,
                              led::EspRmtStripComponent& output) noexcept {
    static e131::EspE131Component component{wifi, output};
    return component;
}

} // namespace blip::firmware
