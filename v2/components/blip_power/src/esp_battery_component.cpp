#include "blip/power/esp_battery_component.hpp"

#include "esp_adc/adc_cali_scheme.h"
#include "esp_timer.h"

#include <array>
#include <string_view>

namespace blip::power {
namespace {
constexpr std::array<std::string_view, 1> kServices{"power.battery"};
constexpr std::array<core::MetadataEntry, 3> kMetadata{{
    {"ui_topic", "Power & sensors"}, {"ui_primary", "filtered_mv,soc_per_mille,low,valid"},
    {"ui_gauges", "soc_per_mille"}}};
constexpr std::array<core::ParameterDescriptor, 8> kParameters{{
    {"pin_mv", "ADC pin voltage", core::ValueType::integer, core::Access::read_only,
     false, core::ScalarValue::from_integer(0), {}, "mV"},
    {"battery_mv", "Battery voltage", core::ValueType::integer, core::Access::read_only,
     false, core::ScalarValue::from_integer(0), {}, "mV"},
    {"filtered_mv", "Filtered battery voltage", core::ValueType::integer,
     core::Access::read_only, false, core::ScalarValue::from_integer(0), {}, "mV"},
    {"soc_per_mille", "Voltage-based charge estimate", core::ValueType::integer,
     core::Access::read_only, false, core::ScalarValue::from_integer(0),
     {true, 0, 1000, 1}, "per-mille"},
    {"low", "Low battery", core::ValueType::boolean, core::Access::read_only,
     false, core::ScalarValue::from_bool(false), {}, ""},
    {"valid", "Battery reading valid", core::ValueType::boolean,
     core::Access::read_only, false, core::ScalarValue::from_bool(false), {}, ""},
    {"sample_errors", "ADC sample errors", core::ValueType::integer,
     core::Access::read_only, false, core::ScalarValue::from_integer(0), {}, "samples"},
    {"divider_numerator", "Board voltage divider numerator", core::ValueType::integer,
     core::Access::read_only, false, core::ScalarValue::from_integer(2), {}, ""},
}};

[[nodiscard]] constexpr core::ComponentDescriptor battery_descriptor() noexcept {
    core::ComponentDescriptor descriptor{};
    descriptor.schema_version = 1U;
    descriptor.id = "blip.power.battery";
    descriptor.display_name = "HUZZAH32 battery";
    descriptor.description = "Calibrated ADC1 battery voltage and filtered low-battery state";
    descriptor.provided_services = kServices;
    descriptor.metadata = kMetadata;
    descriptor.parameters = kParameters;
    descriptor.settings = {1U, 1U};
    descriptor.disable_policy = core::DisablePolicy::reboot_required;
    descriptor.cost = {4096U, 2048U, 0U};
    return descriptor;
}

[[nodiscard]] core::Status battery_error(core::ErrorCode code, std::string_view operation,
                                         std::string_view detail) noexcept {
    return core::Status::failure(
        {core::ErrorDomain::diagnostics, code, "blip.power.battery", operation, detail});
}
} // namespace

const core::ComponentDescriptor EspBatteryComponent::descriptor_{battery_descriptor()};

const core::ComponentDescriptor& EspBatteryComponent::descriptor() const noexcept {
    return descriptor_;
}

core::Status EspBatteryComponent::start(const core::StartContext&) noexcept {
    if (started_) {
        return core::Status::success();
    }
    if (!model_.configure(huzzah32_battery_calibration())) {
        return battery_error(core::ErrorCode::validation_failed, "start", "invalid-calibration");
    }
    adc_unit_t unit_id{};
    if (adc_oneshot_io_to_channel(35, &unit_id, &channel_) != ESP_OK ||
        unit_id != ADC_UNIT_1) {
        return battery_error(core::ErrorCode::start_failed, "start", "gpio35-not-adc1");
    }
    adc_oneshot_unit_init_cfg_t unit_config{};
    unit_config.unit_id = unit_id;
    if (adc_oneshot_new_unit(&unit_config, &unit_) != ESP_OK) {
        return battery_error(core::ErrorCode::start_failed, "start", "adc-unit-create");
    }
    adc_oneshot_chan_cfg_t channel_config{};
    channel_config.atten = ADC_ATTEN_DB_12;
    channel_config.bitwidth = ADC_BITWIDTH_DEFAULT;
    if (adc_oneshot_config_channel(unit_, channel_, &channel_config) != ESP_OK) {
        static_cast<void>(adc_oneshot_del_unit(unit_));
        unit_ = nullptr;
        return battery_error(core::ErrorCode::start_failed, "start", "adc-channel-config");
    }
    adc_cali_line_fitting_config_t calibration_config{};
    calibration_config.unit_id = unit_id;
    calibration_config.atten = channel_config.atten;
    calibration_config.bitwidth = channel_config.bitwidth;
    calibration_config.default_vref = 1100;
    if (adc_cali_create_scheme_line_fitting(&calibration_config, &calibration_) != ESP_OK) {
        static_cast<void>(adc_oneshot_del_unit(unit_));
        unit_ = nullptr;
        return battery_error(core::ErrorCode::start_failed, "start", "adc-calibration");
    }
    started_ = true;
    static_cast<void>(sample());
    return core::Status::success();
}

core::Status EspBatteryComponent::stop() noexcept {
    started_ = false;
    if (calibration_ != nullptr) {
        static_cast<void>(adc_cali_delete_scheme_line_fitting(calibration_));
        calibration_ = nullptr;
    }
    if (unit_ != nullptr) {
        static_cast<void>(adc_oneshot_del_unit(unit_));
        unit_ = nullptr;
    }
    return core::Status::success();
}

bool EspBatteryComponent::sample() noexcept {
    std::uint32_t total_mv{};
    for (int index = 0; index < 4; ++index) {
        int pin_mv{};
        if (adc_oneshot_get_calibrated_result(unit_, calibration_, channel_, &pin_mv) != ESP_OK ||
            pin_mv < 0) {
            ++sample_errors_;
            return false;
        }
        total_mv += static_cast<std::uint32_t>(pin_mv);
    }
    if (!model_.sample(total_mv / 4U, false)) {
        ++sample_errors_;
        return false;
    }
    last_sample_us_ = esp_timer_get_time();
    return true;
}

core::Status EspBatteryComponent::read_parameter(std::string_view id,
                                                  core::ScalarValue& output) noexcept {
    if (!started_) {
        return battery_error(core::ErrorCode::invalid_state, "read-parameter", "not-started");
    }
    if (esp_timer_get_time() - last_sample_us_ >= 1'000'000) {
        static_cast<void>(sample());
    }
    const auto& state = model_.state();
    if (id == "pin_mv") {
        output = core::ScalarValue::from_integer(state.pin_mv);
    } else if (id == "battery_mv") {
        output = core::ScalarValue::from_integer(state.battery_mv);
    } else if (id == "filtered_mv") {
        output = core::ScalarValue::from_integer(state.filtered_mv);
    } else if (id == "soc_per_mille") {
        output = core::ScalarValue::from_integer(state.state_of_charge_per_mille);
    } else if (id == "low") {
        output = core::ScalarValue::from_bool(state.low);
    } else if (id == "valid") {
        output = core::ScalarValue::from_bool(state.valid);
    } else if (id == "sample_errors") {
        output = core::ScalarValue::from_integer(sample_errors_);
    } else if (id == "divider_numerator") {
        output = core::ScalarValue::from_integer(2);
    } else {
        return battery_error(core::ErrorCode::not_found, "read-parameter", id);
    }
    return core::Status::success();
}

core::Status EspBatteryComponent::write_parameter(std::string_view id,
                                                   const core::ScalarValue&) noexcept {
    return battery_error(core::ErrorCode::not_found, "write-parameter", id);
}

} // namespace blip::power
