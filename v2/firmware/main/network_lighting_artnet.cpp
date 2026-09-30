#include "network_lighting_features.hpp"

#include "blip/artnet/esp_artnet_component.hpp"

namespace blip::firmware {

core::Component& artnet_feature(network::EspWifiComponent& wifi,
                                led::EspRmtStripComponent& output) noexcept {
    static artnet::EspArtNetComponent component{wifi, output};
    return component;
}

} // namespace blip::firmware
