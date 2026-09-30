#pragma once

#include <cstdint>

namespace blip::power {

// The divider ratio converts a calibrated ADC-pin voltage to battery voltage.
// A voltage-based percentage is only an estimate, especially under LED load.
struct BatteryCalibration {
    std::uint16_t divider_numerator{2U};
    std::uint16_t divider_denominator{1U};
    std::uint16_t empty_mv{3300U};
    std::uint16_t full_mv{4200U};
    std::uint16_t low_mv{3500U};
    std::uint16_t recover_mv{3600U};
    std::uint8_t filter_shift{3U};
};

struct BatteryState {
    std::uint32_t pin_mv{};
    std::uint32_t battery_mv{};
    std::uint32_t filtered_mv{};
    std::uint16_t state_of_charge_per_mille{};
    bool charging{};
    bool low{};
    bool valid{};
};

[[nodiscard]] bool valid_calibration(const BatteryCalibration& calibration) noexcept;
[[nodiscard]] BatteryCalibration huzzah32_battery_calibration() noexcept;

class BatteryModel {
  public:
    [[nodiscard]] bool configure(const BatteryCalibration& calibration) noexcept;
    [[nodiscard]] bool sample(std::uint32_t calibrated_pin_mv, bool charging) noexcept;
    [[nodiscard]] const BatteryState& state() const noexcept { return state_; }

  private:
    BatteryCalibration calibration_{};
    BatteryState state_{};
    bool configured_{};
};

} // namespace blip::power
