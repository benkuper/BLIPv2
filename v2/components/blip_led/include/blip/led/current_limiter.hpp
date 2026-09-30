#pragma once

#include "blip/core/error.hpp"
#include "blip/led/engine.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace blip::led {

// This is a calculated LED-channel budget, not a measurement of the power rail.
struct CurrentModel {
    std::uint16_t fixed_ma{};
    std::uint16_t idle_ma_per_pixel{1U};
    std::array<std::uint16_t, 4> full_channel_ma{20U, 20U, 20U, 20U};
};

struct CurrentLimitResult {
    std::uint32_t estimated_before_ma{};
    std::uint32_t estimated_after_ma{};
    std::uint16_t applied_scale_q16{65535U};
    bool limited{};
};

inline constexpr std::uint16_t kDefaultPowerBudgetMa = 1500U;
inline constexpr std::uint16_t kHardPowerBudgetMa = 5000U;

[[nodiscard]] bool valid_power_budget(std::uint16_t budget_ma, std::size_t pixels,
                                      const CurrentModel& model = {}) noexcept;

class CurrentLimiter {
  public:
    explicit CurrentLimiter(CurrentModel model = {},
                            std::uint16_t rise_step_q16 = 1024U) noexcept
        : model_(model), rise_step_q16_(rise_step_q16) {}

    [[nodiscard]] core::Result<CurrentLimitResult>
    limit(std::span<std::byte> encoded_frame, PixelProtocol protocol,
          std::size_t pixels, std::uint16_t budget_ma) noexcept;
    void reset() noexcept { previous_scale_q16_ = 65535U; }

  private:
    CurrentModel model_{};
    std::uint16_t rise_step_q16_{};
    std::uint16_t previous_scale_q16_{65535U};
};

} // namespace blip::led
