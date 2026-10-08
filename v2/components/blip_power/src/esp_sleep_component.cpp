#include "blip/power/esp_sleep_component.hpp"

#include "driver/uart.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"

#include <array>
#include <string_view>

namespace blip::power {
namespace {
constexpr std::array<std::string_view, 1> kServices{"power.sleep"};
constexpr std::array<core::MetadataEntry, 1> kMetadata{{{"ui_topic", "Power & sensors"}}};
constexpr std::array<std::string_view, 2> kDependencies{"transport.wifi", "output.pixel-strip"};
constexpr std::array<core::ParameterDescriptor, 7> kParameters{{
    {"state", "Sleep state", core::ValueType::integer, core::Access::read_only, false,
     core::ScalarValue::from_integer(0), {true, 0, 4, 1}, ""},
    {"completed", "Completed sleeps", core::ValueType::integer, core::Access::read_only, false,
     core::ScalarValue::from_integer(0), {}, "cycles"},
    {"wake_cause", "Last wake cause", core::ValueType::integer, core::Access::read_only, false,
     core::ScalarValue::from_integer(0), {}, ""},
    {"last_error", "Last ESP-IDF error", core::ValueType::integer, core::Access::read_only, false,
     core::ScalarValue::from_integer(0), {}, ""},
    {"last_sleep_us", "Last measured sleep interval", core::ValueType::integer,
     core::Access::read_only, false, core::ScalarValue::from_integer(0), {}, "us"},
    {"boot_wake_cause", "Wake cause at boot", core::ValueType::integer,
     core::Access::read_only, false, core::ScalarValue::from_integer(0), {}, ""},
    {"booted_from_deep_sleep", "Boot followed deep sleep", core::ValueType::boolean,
     core::Access::read_only, false, core::ScalarValue::from_bool(false), {}, ""},
}};
constexpr std::array<core::FieldDescriptor, 1> kSleepArguments{{
    {"duration_ms", core::ValueType::integer, true},
}};
constexpr std::array<core::ActionDescriptor, 2> kActions{{
    {"light_sleep", "Timer-bounded light sleep (1000-30000 ms)", kSleepArguments},
    {"deep_sleep", "Timer-bounded deep sleep (1000-30000 ms)", kSleepArguments},
}};

[[nodiscard]] constexpr core::ComponentDescriptor sleep_descriptor() noexcept {
    core::ComponentDescriptor descriptor{};
    descriptor.schema_version = 1;
    descriptor.id = "blip.power.sleep";
    descriptor.display_name = "HUZZAH32 timed sleep";
    descriptor.description = "Manual light/deep sleep with mandatory timer wake and idle radio/LED preconditions";
    descriptor.provided_services = kServices;
    descriptor.metadata = kMetadata;
    descriptor.required_services = kDependencies;
    descriptor.parameters = kParameters;
    descriptor.actions = kActions;
    descriptor.settings = {1, 1};
    descriptor.disable_policy = core::DisablePolicy::reboot_required;
    descriptor.cost = {8192, 4096, 3072};
    return descriptor;
}

[[nodiscard]] core::Status sleep_error(core::ErrorCode code, std::string_view operation,
                                        std::string_view detail) noexcept {
    return core::Status::failure({core::ErrorDomain::control, code, "blip.power.sleep",
                                  operation, detail});
}
} // namespace

const core::ComponentDescriptor EspSleepComponent::descriptor_{sleep_descriptor()};

const core::ComponentDescriptor& EspSleepComponent::descriptor() const noexcept {
    return descriptor_;
}

core::Status EspSleepComponent::start(const core::StartContext&) noexcept {
    if (running_.load()) {
        return core::Status::success();
    }
    booted_from_deep_sleep_ = esp_reset_reason() == ESP_RST_DEEPSLEEP;
    boot_wake_cause_ = booted_from_deep_sleep_ ? esp_sleep_get_wakeup_causes() : 0U;
    quiesced_.store(false);
    running_.store(true);
    task_handle_ = xTaskCreateStatic(task_entry, "blip_sleep", sizeof(task_stack_), this, 4,
                                     task_stack_.data(), &task_storage_);
    if (task_handle_ == nullptr) {
        running_.store(false);
        quiesced_.store(true);
        return sleep_error(core::ErrorCode::start_failed, "start", "task-create-failed");
    }
    return core::Status::success();
}

core::Status EspSleepComponent::stop() noexcept {
    if (!running_.exchange(false)) {
        return core::Status::success();
    }
    xTaskNotifyGive(task_handle_);
    for (std::size_t attempt = 0; attempt < 6400 && !quiesced_.load(); ++attempt) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (!quiesced_.load()) {
        return sleep_error(core::ErrorCode::stop_failed, "stop", "task-timeout");
    }
    task_handle_ = nullptr;
    return core::Status::success();
}

bool EspSleepComponent::peripherals_idle() noexcept {
    core::ScalarValue wifi_enabled{};
    core::ScalarValue strip_enabled{};
    return wifi_->read_parameter("enabled", wifi_enabled) &&
           strip_->read_parameter("enabled", strip_enabled) &&
           wifi_enabled.type == core::ValueType::boolean &&
           strip_enabled.type == core::ValueType::boolean &&
           !wifi_enabled.boolean && !strip_enabled.boolean;
}

core::Status EspSleepComponent::read_parameter(std::string_view id,
                                                core::ScalarValue& output) noexcept {
    if (!running_.load()) {
        return sleep_error(core::ErrorCode::invalid_state, "read-parameter", "not-started");
    }
    if (id == "state") {
        output = core::ScalarValue::from_integer(state_.load());
    } else if (id == "completed") {
        output = core::ScalarValue::from_integer(completed_.load());
    } else if (id == "wake_cause") {
        output = core::ScalarValue::from_integer(wake_cause_.load());
    } else if (id == "last_error") {
        output = core::ScalarValue::from_integer(last_error_.load());
    } else if (id == "last_sleep_us") {
        output = core::ScalarValue::from_integer(last_sleep_us_.load());
    } else if (id == "boot_wake_cause") {
        output = core::ScalarValue::from_integer(boot_wake_cause_);
    } else if (id == "booted_from_deep_sleep") {
        output = core::ScalarValue::from_bool(booted_from_deep_sleep_);
    } else {
        return sleep_error(core::ErrorCode::not_found, "read-parameter", "parameter-not-found");
    }
    return core::Status::success();
}

core::Status EspSleepComponent::invoke_action(std::string_view id,
                                               std::span<const core::ScalarValue> arguments,
                                               std::span<core::ScalarValue>,
                                               std::size_t& output_count) noexcept {
    output_count = 0;
    if (id != "light_sleep" && id != "deep_sleep") {
        return sleep_error(core::ErrorCode::not_found, "invoke-action", "action-not-found");
    }
    if (!running_.load() || arguments.size() != 1 ||
        arguments[0].type != core::ValueType::integer || arguments[0].integer < 1000 ||
        arguments[0].integer > 30000) {
        return sleep_error(core::ErrorCode::validation_failed, "invoke-action", "duration-range");
    }
    if (!peripherals_idle()) {
        return sleep_error(core::ErrorCode::invalid_state, "invoke-action", "wifi-or-led-active");
    }
    std::uint32_t previous = state_.load();
    do {
        if (previous == 1 || previous == 2) {
            return sleep_error(core::ErrorCode::invalid_state, "invoke-action", "sleep-busy");
        }
    } while (!state_.compare_exchange_weak(previous, 1));
    requested_mode_.store(id == "deep_sleep" ? 1U : 0U);
    requested_ms_.store(static_cast<std::uint32_t>(arguments[0].integer));
    xTaskNotifyGive(task_handle_);
    return core::Status::success();
}

void EspSleepComponent::run() noexcept {
    while (running_.load()) {
        if (ulTaskNotifyTake(pdTRUE, portMAX_DELAY) == 0 || !running_.load()) {
            continue;
        }
        // Let the serial action response leave the UART before sleep begins.
        vTaskDelay(pdMS_TO_TICKS(250));
        if (!running_.load()) {
            break;
        }
        if (!peripherals_idle()) {
            last_error_.store(ESP_ERR_INVALID_STATE);
            state_.store(4);
            continue;
        }
        if (uart_wait_tx_done(UART_NUM_0, pdMS_TO_TICKS(100)) != ESP_OK) {
            last_error_.store(ESP_ERR_TIMEOUT);
            state_.store(4);
            continue;
        }
        const std::uint64_t duration_us = static_cast<std::uint64_t>(requested_ms_.load()) * 1000U;
        if (requested_mode_.load() == 1U) {
            // This call returns only if IDF rejects the sleep request. A timer
            // wake starts a fresh boot and exposes boot_wake_cause afterward.
            state_.store(2);
            const esp_err_t rejected = esp_deep_sleep_try(duration_us);
            static_cast<void>(esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER));
            last_error_.store(rejected);
            state_.store(4);
            continue;
        }
        const esp_err_t armed = esp_sleep_enable_timer_wakeup(duration_us);
        if (armed != ESP_OK) {
            last_error_.store(armed);
            state_.store(4);
            continue;
        }
        state_.store(2);
        const std::int64_t before = esp_timer_get_time();
        const esp_err_t result = esp_light_sleep_start();
        const std::int64_t after = esp_timer_get_time();
        wake_cause_.store(esp_sleep_get_wakeup_causes());
        last_sleep_us_.store(static_cast<std::uint64_t>(after - before));
        static_cast<void>(esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER));
        last_error_.store(result);
        if (result == ESP_OK) {
            completed_.fetch_add(1);
            state_.store(3);
        } else {
            state_.store(4);
        }
    }
    quiesced_.store(true);
    vTaskDelete(nullptr);
}

void EspSleepComponent::task_entry(void* context) noexcept {
    static_cast<EspSleepComponent*>(context)->run();
}

} // namespace blip::power
