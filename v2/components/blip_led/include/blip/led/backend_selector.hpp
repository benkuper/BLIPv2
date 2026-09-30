#pragma once

#include "blip/led/engine.hpp"

#include <cstdint>
#include <span>
#include <string_view>

namespace blip::led {

enum class BackendKind : std::uint8_t {
    automatic,
    rmt,
    rmt_dma,
    spi_encoded_one_wire,
    parallel_wave,
    fastled_compatibility,
    spi_clocked,
};

struct BackendCandidate {
    BackendKind kind{BackendKind::rmt};
    const DriverCapabilities* capabilities{};
    bool compiled{};
    bool resources_available{};
};

struct BackendRequest {
    BackendKind requested{BackendKind::automatic};
    PixelProtocol protocol{PixelProtocol::ws2812};
    std::uint16_t lanes{1U};
    std::uint16_t pixels_per_lane{1U};
    std::size_t staging_budget_bytes{};
};

struct BackendSelection {
    const BackendCandidate* candidate{};
    std::string_view reason{};
    [[nodiscard]] explicit operator bool() const noexcept { return candidate != nullptr; }
};

// Selection is independent of candidate registration order. Unqualified
// backends are never selected, including when explicitly forced.
[[nodiscard]] core::Result<BackendSelection>
select_backend(const BackendRequest& request,
               std::span<const BackendCandidate> candidates) noexcept;

} // namespace blip::led
