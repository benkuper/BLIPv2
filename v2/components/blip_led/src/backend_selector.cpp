#include "blip/led/backend_selector.hpp"

#include <limits>

namespace blip::led {
namespace {
[[nodiscard]] bool clocked(PixelProtocol protocol) noexcept {
    return protocol == PixelProtocol::apa102 || protocol == PixelProtocol::sk9822 ||
           protocol == PixelProtocol::hd108;
}
[[nodiscard]] unsigned rank(BackendKind kind, const BackendRequest& request) noexcept {
    if (clocked(request.protocol)) {
        return kind == BackendKind::spi_clocked ? 0U : 100U;
    }
    if (request.lanes > 1U) {
        if (kind == BackendKind::parallel_wave)
            return 0U;
        if (kind == BackendKind::rmt_dma)
            return 1U;
        if (kind == BackendKind::rmt)
            return 2U;
        if (kind == BackendKind::spi_encoded_one_wire)
            return 3U;
    }
    if (request.pixels_per_lane >= 512U) {
        if (kind == BackendKind::spi_encoded_one_wire)
            return 0U;
        if (kind == BackendKind::rmt_dma)
            return 1U;
        if (kind == BackendKind::rmt)
            return 2U;
    }
    if (kind == BackendKind::rmt)
        return 0U;
    if (kind == BackendKind::rmt_dma)
        return 1U;
    if (kind == BackendKind::spi_encoded_one_wire)
        return 2U;
    if (kind == BackendKind::parallel_wave)
        return 3U;
    if (kind == BackendKind::fastled_compatibility)
        return 4U;
    return 100U;
}
[[nodiscard]] bool eligible(const BackendCandidate& candidate,
                            const BackendRequest& request) noexcept {
    if (!candidate.compiled || !candidate.resources_available ||
        candidate.capabilities == nullptr || !candidate.capabilities->qualified ||
        !candidate.capabilities->supports(request.protocol) ||
        request.lanes < candidate.capabilities->minimum_lanes ||
        request.lanes > candidate.capabilities->maximum_lanes ||
        request.pixels_per_lane > candidate.capabilities->maximum_pixels_per_lane) {
        return false;
    }
    const auto pixels = static_cast<std::size_t>(request.lanes) * request.pixels_per_lane;
    return request.staging_budget_bytes == 0U ||
           candidate.capabilities->required_staging_bytes(pixels) <= request.staging_budget_bytes;
}
[[nodiscard]] core::Result<BackendSelection> failed(std::string_view detail) noexcept {
    return core::Result<BackendSelection>::failure({core::ErrorDomain::resource,
                                                    core::ErrorCode::resource_unavailable,
                                                    "blip.led.selector", "select", detail});
}
} // namespace

core::Result<BackendSelection>
select_backend(const BackendRequest& request,
               std::span<const BackendCandidate> candidates) noexcept {
    if (request.lanes == 0U || request.pixels_per_lane == 0U) {
        return failed("invalid-request");
    }
    if (request.requested != BackendKind::automatic) {
        for (const auto& candidate : candidates) {
            if (candidate.kind == request.requested) {
                return eligible(candidate, request)
                           ? core::Result<BackendSelection>::success(
                                 {&candidate, "forced-qualified-backend"})
                           : failed("forced-backend-unavailable-or-unqualified");
            }
        }
        return failed("forced-backend-not-compiled");
    }
    const BackendCandidate* selected = nullptr;
    unsigned selected_rank = std::numeric_limits<unsigned>::max();
    for (const auto& candidate : candidates) {
        if (!eligible(candidate, request)) {
            continue;
        }
        const auto candidate_rank = rank(candidate.kind, request);
        if (selected == nullptr || candidate_rank < selected_rank ||
            (candidate_rank == selected_rank &&
             candidate.capabilities->id < selected->capabilities->id)) {
            selected = &candidate;
            selected_rank = candidate_rank;
        }
    }
    if (selected == nullptr) {
        return failed("no-qualified-backend-fits-limits-and-resources");
    }
    const auto reason = clocked(request.protocol)         ? "auto:clocked-protocol-native-spi"
                        : request.lanes > 1U              ? "auto:multi-lane-qualified-parallel"
                        : request.pixels_per_lane >= 512U ? "auto:long-strip-staging-throughput"
                                                          : "auto:short-strip-low-staging";
    return core::Result<BackendSelection>::success({selected, reason});
}

} // namespace blip::led
