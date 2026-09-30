#pragma once

#include "blip/led/engine.hpp"

#include <array>
#include <cstdint>
#include <span>

namespace blip::led {

enum class LayerId : std::uint8_t { stream, playback, script, system };
enum class BlendMode : std::uint8_t { replace, alpha_over, add, multiply };

struct LayerView {
    LayerId id{LayerId::stream};
    std::span<const LinearPixel> pixels{};
    BlendMode blend{BlendMode::alpha_over};
    std::uint16_t opacity{65535U};
    bool enabled{};
};

// Priority is fixed and independent of registration order:
// stream < playback < script < system.
class Compositor {
  public:
    [[nodiscard]] core::Status set_layer(LayerView layer) noexcept;
    void clear_layer(LayerId id) noexcept;
    [[nodiscard]] core::Status compose(std::span<LinearPixel> output) const noexcept;
    [[nodiscard]] const LayerView& layer(LayerId id) const noexcept;

  private:
    std::array<LayerView, 4> layers_{
        {{LayerId::stream}, {LayerId::playback}, {LayerId::script}, {LayerId::system}}};
};

} // namespace blip::led
