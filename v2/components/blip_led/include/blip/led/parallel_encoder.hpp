#pragma once

#include "blip/core/error.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace blip::led {

// Converts lane-major protocol bytes into three-phase parallel samples. Each
// bit in a sample word is one output lane: every bit starts high, only logical
// ones remain high in phase two, and all lanes are low in phase three.
[[nodiscard]] constexpr std::size_t parallel_sample_count(std::size_t bytes_per_lane) noexcept {
    return bytes_per_lane > std::numeric_limits<std::size_t>::max() / 24U
               ? std::numeric_limits<std::size_t>::max()
               : bytes_per_lane * 24U;
}
[[nodiscard]] core::Result<std::size_t>
encode_parallel_wave(std::span<const std::byte> lane_major, std::uint8_t lane_count,
                     std::size_t bytes_per_lane, std::span<std::uint16_t> output) noexcept;

} // namespace blip::led
