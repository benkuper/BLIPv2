#pragma once

#include "blip/resources/broker.hpp"

namespace blip::resources {

inline constexpr std::uint32_t kGpioInput = 1U << 0U;
inline constexpr std::uint32_t kGpioOutput = 1U << 1U;
inline constexpr std::uint32_t kGpioAdc = 1U << 2U;
inline constexpr std::uint32_t kGpioPwm = 1U << 3U;
inline constexpr std::uint32_t kGpioInterrupt = 1U << 4U;
inline constexpr std::uint32_t kGpioOpenDrain = 1U << 5U;
inline constexpr std::uint32_t kGpioRmt = 1U << 6U;

using DeviceBroker = Broker<64, 32>;

} // namespace blip::resources
