#include "blip/power/battery_model.hpp"

#include <algorithm>
#include <cstdint>

namespace blip::power {

bool valid_calibration(const BatteryCalibration& calibration) noexcept {
    return calibration.divider_numerator > 0U && calibration.divider_numerator <= 32U &&
           calibration.divider_denominator > 0U && calibration.divider_denominator <= 32U &&
           calibration.empty_mv >= 2500U && calibration.empty_mv < calibration.full_mv &&
           calibration.full_mv <= 5000U && calibration.low_mv >= calibration.empty_mv &&
           calibration.low_mv <= calibration.recover_mv &&
           calibration.recover_mv <= calibration.full_mv &&
           calibration.filter_shift >= 1U && calibration.filter_shift <= 8U;
}

BatteryCalibration huzzah32_battery_calibration() noexcept {
    return {2U, 1U, 3300U, 4200U, 3500U, 3600U, 3U};
}

bool BatteryModel::configure(const BatteryCalibration& calibration) noexcept {
    if (!valid_calibration(calibration)) {
        return false;
    }
    calibration_ = calibration;
    state_ = {};
    configured_ = true;
    return true;
}

bool BatteryModel::sample(std::uint32_t calibrated_pin_mv, bool charging) noexcept {
    if (!configured_ || calibrated_pin_mv > 5000U) {
        return false;
    }
    const auto numerator = static_cast<std::uint64_t>(calibrated_pin_mv) *
                               calibration_.divider_numerator +
                           calibration_.divider_denominator / 2U;
    const auto battery_mv = static_cast<std::uint32_t>(
        numerator / calibration_.divider_denominator);
    if (battery_mv > 10000U) {
        return false;
    }
    const std::uint32_t filtered = [&]() {
        if (!state_.valid) {
            return battery_mv;
        }
        const auto old = static_cast<std::int64_t>(state_.filtered_mv);
        const auto difference = static_cast<std::int64_t>(battery_mv) - old;
        const auto magnitude = static_cast<std::uint64_t>(difference < 0 ? -difference : difference);
        const auto step = (magnitude + (1ULL << calibration_.filter_shift) - 1U) >>
                          calibration_.filter_shift;
        return static_cast<std::uint32_t>(old + (difference < 0 ? -static_cast<std::int64_t>(step)
                                                               : static_cast<std::int64_t>(step)));
    }();
    const std::uint32_t clamped =
        std::clamp<std::uint32_t>(filtered, calibration_.empty_mv, calibration_.full_mv);
    const auto soc = static_cast<std::uint16_t>(
        (static_cast<std::uint64_t>(clamped - calibration_.empty_mv) * 1000U) /
        (calibration_.full_mv - calibration_.empty_mv));
    const bool low = state_.valid && state_.low ? filtered < calibration_.recover_mv
                                                : filtered <= calibration_.low_mv;
    state_ = {calibrated_pin_mv, battery_mv, filtered, soc, charging, low, true};
    return true;
}

} // namespace blip::power
