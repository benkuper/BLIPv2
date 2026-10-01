#include "blip/pm/esp_power_manager_component.hpp"

#include "sdkconfig.h"

#include <array>
#include <string_view>

#ifndef CONFIG_PM_ENABLE
#error "BLIP V2 power profiles require CONFIG_PM_ENABLE"
#endif

namespace blip::pm {
namespace {
constexpr std::array<std::string_view, 1> kServices{"power.cpu"};
constexpr std::array<core::ParameterDescriptor, 6> kParameters{{
    {"max_cpu_mhz", "Maximum CPU frequency", core::ValueType::integer,
     core::Access::read_only, false,
     core::ScalarValue::from_integer(CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ), {}, "MHz"},
    {"min_cpu_mhz", "Idle CPU frequency", core::ValueType::integer,
     core::Access::read_only, false, core::ScalarValue::from_integer(80), {}, "MHz"},
    {"auto_light_sleep", "Automatic light sleep", core::ValueType::boolean,
     core::Access::read_only, false, core::ScalarValue::from_bool(false), {}, ""},
    {"active_frame_locks", "Active pixel-frame locks", core::ValueType::integer,
     core::Access::read_only, false, core::ScalarValue::from_integer(0), {}, "locks"},
    {"acquired_frames", "Pixel frames protected by CPU lock", core::ValueType::integer,
     core::Access::read_only, false, core::ScalarValue::from_integer(0), {}, "frames"},
    {"release_errors", "CPU lock release errors", core::ValueType::integer,
     core::Access::read_only, false, core::ScalarValue::from_integer(0), {}, "errors"},
}};

[[nodiscard]] constexpr core::ComponentDescriptor pm_descriptor() noexcept {
    core::ComponentDescriptor descriptor{};
    descriptor.schema_version = 1;
    descriptor.id = "blip.power.manager";
    descriptor.display_name = "ESP power management";
    descriptor.description = "80 MHz idle / board maximum dynamic scaling and pixel-frame CPU lock";
    descriptor.provided_services = kServices;
    descriptor.parameters = kParameters;
    descriptor.settings = {1, 1};
    descriptor.disable_policy = core::DisablePolicy::reboot_required;
    descriptor.cost = {4096, 512, 0};
    return descriptor;
}

[[nodiscard]] core::Status pm_error(core::ErrorCode code, std::string_view operation,
                                     std::string_view detail) noexcept {
    return core::Status::failure(
        {core::ErrorDomain::diagnostics, code, "blip.power.manager", operation, detail});
}
} // namespace

const core::ComponentDescriptor EspPowerManagerComponent::descriptor_{pm_descriptor()};

const core::ComponentDescriptor& EspPowerManagerComponent::descriptor() const noexcept {
    return descriptor_;
}

core::Status EspPowerManagerComponent::start(const core::StartContext&) noexcept {
    if (started_) {
        return core::Status::success();
    }
    esp_pm_config_t config{};
    config.max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    config.min_freq_mhz = 80;
    config.light_sleep_enable = false;
    if (esp_pm_configure(&config) != ESP_OK) {
        return pm_error(core::ErrorCode::start_failed, "start", "pm-configure-failed");
    }
    if (esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "blip_led_frame", &frame_lock_) != ESP_OK) {
        return pm_error(core::ErrorCode::start_failed, "start", "frame-lock-create-failed");
    }
    started_ = true;
    return core::Status::success();
}

core::Status EspPowerManagerComponent::stop() noexcept {
    if (active_frame_locks_.load() != 0U) {
        return pm_error(core::ErrorCode::stop_failed, "stop", "frame-lock-held");
    }
    if (frame_lock_ != nullptr) {
        if (esp_pm_lock_delete(frame_lock_) != ESP_OK) {
            return pm_error(core::ErrorCode::stop_failed, "stop", "frame-lock-delete-failed");
        }
        frame_lock_ = nullptr;
    }
    started_ = false;
    return core::Status::success();
}

bool EspPowerManagerComponent::acquire_frame_lock() noexcept {
    if (!started_ || frame_lock_ == nullptr || esp_pm_lock_acquire(frame_lock_) != ESP_OK) {
        return false;
    }
    active_frame_locks_.fetch_add(1U);
    acquired_frames_.fetch_add(1U);
    return true;
}

void EspPowerManagerComponent::release_frame_lock() noexcept {
    if (esp_pm_lock_release(frame_lock_) == ESP_OK) {
        active_frame_locks_.fetch_sub(1U);
    } else {
        release_errors_.fetch_add(1U);
    }
}

core::Status EspPowerManagerComponent::read_parameter(std::string_view id,
                                                       core::ScalarValue& output) noexcept {
    if (!started_) {
        return pm_error(core::ErrorCode::invalid_state, "read-parameter", "not-started");
    }
    if (id == "max_cpu_mhz") {
        output = core::ScalarValue::from_integer(CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    } else if (id == "min_cpu_mhz") {
        output = core::ScalarValue::from_integer(80);
    } else if (id == "auto_light_sleep") {
        output = core::ScalarValue::from_bool(false);
    } else if (id == "active_frame_locks") {
        output = core::ScalarValue::from_integer(active_frame_locks_.load());
    } else if (id == "acquired_frames") {
        output = core::ScalarValue::from_integer(acquired_frames_.load());
    } else if (id == "release_errors") {
        output = core::ScalarValue::from_integer(release_errors_.load());
    } else {
        return pm_error(core::ErrorCode::not_found, "read-parameter", "parameter-not-found");
    }
    return core::Status::success();
}

} // namespace blip::pm
