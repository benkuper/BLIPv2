#include "blip/led/parallel_encoder.hpp"

namespace blip::led {

core::Result<std::size_t> encode_parallel_wave(std::span<const std::byte> lane_major,
                                               std::uint8_t lane_count, std::size_t bytes_per_lane,
                                               std::span<std::uint16_t> output) noexcept {
    if (lane_count == 0U || lane_count > 16U || bytes_per_lane == 0U ||
        lane_major.size() != static_cast<std::size_t>(lane_count) * bytes_per_lane) {
        return core::Result<std::size_t>::failure(
            {core::ErrorDomain::transport, core::ErrorCode::invalid_argument, "blip.led.parallel",
             "encode", "invalid-lane-layout"});
    }
    const auto required = parallel_sample_count(bytes_per_lane);
    if (required == std::numeric_limits<std::size_t>::max() || output.size() < required) {
        return core::Result<std::size_t>::failure(
            {core::ErrorDomain::transport, core::ErrorCode::serialization_overflow,
             "blip.led.parallel", "encode", "output-too-small"});
    }
    const auto all_lanes =
        static_cast<std::uint16_t>(lane_count == 16U ? 0xffffU : (1U << lane_count) - 1U);
    std::size_t sample{};
    for (std::size_t byte = 0; byte < bytes_per_lane; ++byte) {
        for (std::uint8_t bit = 0; bit < 8U; ++bit) {
            std::uint16_t one_lanes{};
            for (std::uint8_t lane = 0; lane < lane_count; ++lane) {
                const auto value =
                    std::to_integer<std::uint8_t>(lane_major[lane * bytes_per_lane + byte]);
                if ((value & (0x80U >> bit)) != 0U)
                    one_lanes |= static_cast<std::uint16_t>(1U << lane);
            }
            output[sample++] = all_lanes;
            output[sample++] = one_lanes;
            output[sample++] = 0U;
        }
    }
    return core::Result<std::size_t>::success(required);
}

} // namespace blip::led
