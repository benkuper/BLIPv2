#include "blip/power/battery_model.hpp"

#include <iostream>

namespace {
using namespace blip::power;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            std::cerr << "FAIL " << __func__ << ':' << __LINE__ << " " #x "\n";                    \
            return false;                                                                          \
        }                                                                                          \
    } while (false)

bool known_divider_and_voltage_limits() {
    const auto calibration = huzzah32_battery_calibration();
    CHECK(valid_calibration(calibration));
    CHECK(calibration.divider_numerator == 2U);
    CHECK(calibration.divider_denominator == 1U);
    BatteryModel monitor;
    CHECK(monitor.configure(calibration));
    CHECK(monitor.sample(2100U, true));
    CHECK(monitor.state().battery_mv == 4200U);
    CHECK(monitor.state().filtered_mv == 4200U);
    CHECK(monitor.state().state_of_charge_per_mille == 1000U);
    CHECK(monitor.state().charging && !monitor.state().low);
    CHECK(monitor.configure(calibration));
    CHECK(monitor.sample(1600U, false));
    CHECK(monitor.state().battery_mv == 3200U);
    CHECK(monitor.state().state_of_charge_per_mille == 0U);
    CHECK(monitor.state().low);
    return true;
}

bool filtering_and_low_state_hysteresis() {
    BatteryModel monitor;
    auto calibration = huzzah32_battery_calibration();
    calibration.filter_shift = 1U;
    CHECK(monitor.configure(calibration));
    CHECK(monitor.sample(1750U, false));
    CHECK(monitor.state().low);
    CHECK(monitor.sample(1760U, false));
    CHECK(monitor.state().filtered_mv == 3510U);
    CHECK(monitor.state().low); // Recovery needs 3600 mV, not one noisy reading.
    for (int index = 0; index < 8; ++index) {
        CHECK(monitor.sample(1900U, true));
    }
    CHECK(monitor.state().filtered_mv >= 3600U);
    CHECK(!monitor.state().low);
    CHECK(monitor.sample(1740U, false));
    CHECK(!monitor.state().low); // Entry needs the filtered value at 3500 mV.
    return true;
}

bool invalid_input_preserves_last_good_state() {
    BatteryModel monitor;
    CHECK(!monitor.sample(2000U, false));
    auto calibration = huzzah32_battery_calibration();
    calibration.divider_denominator = 0U;
    CHECK(!monitor.configure(calibration));
    CHECK(!monitor.state().valid);
    CHECK(monitor.configure(huzzah32_battery_calibration()));
    CHECK(monitor.sample(2000U, false));
    const auto before = monitor.state();
    CHECK(!monitor.sample(5001U, true));
    CHECK(monitor.state().battery_mv == before.battery_mv);
    CHECK(monitor.state().charging == before.charging);
    CHECK(monitor.state().valid);
    return true;
}
} // namespace

int main() {
    const bool success = known_divider_and_voltage_limits() &&
                         filtering_and_low_state_hysteresis() &&
                         invalid_input_preserves_last_good_state();
    return success ? 0 : 1;
}
