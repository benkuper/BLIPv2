#include "blip/core/component.hpp"
#include "blip/core/control.hpp"
#include "blip/core/esp_diagnostics_component.hpp"
#include "blip/core/registry.hpp"
#include "blip/core/scheduler.hpp"
#include "blip/network/esp_wifi_component.hpp"
#include "blip/oscquery/esp_oscquery_component.hpp"
#include "blip/resources/broker.hpp"
#include "blip/storage/legacy_settings_import_component.hpp"
#include "blip/storage/littlefs_storage_component.hpp"
#include "blip/storage/nvs_settings_component.hpp"
#include "blip/transport/esp_serial_transport_component.hpp"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <string_view>

namespace {

constexpr char kTag[] = "blip_bootstrap";
constexpr std::uint32_t kBootstrapSchemaVersion = 1U;
constexpr std::array<std::string_view, 7> kBootstrapDependencies{
    "diagnostics.runtime", "storage.settings", "storage.files.internal", "storage.legacy_import",
    "transport.serial",    "transport.wifi",   "discovery.oscquery"};
constexpr std::array<std::string_view, 2> kRecoveryDependencies{"diagnostics.runtime",
                                                                "transport.serial"};
constexpr std::array<blip::core::ParameterDescriptor, 1> kBootstrapParameters{{
    {"probe_value",
     "Control transport probe",
     blip::core::ValueType::integer,
     blip::core::Access::read_write,
     false,
     blip::core::ScalarValue::from_integer(0),
     {true, -100, 100, 1},
     ""},
}};
constexpr std::array<blip::core::ActionDescriptor, 1> kBootstrapActions{{
    {"reset_probe", "Reset control transport probe", {}},
}};

static_assert(ESP_IDF_VERSION == ESP_IDF_VERSION_VAL(6, 0, 2),
              "BLIP V2 work package 1.1 requires ESP-IDF 6.0.2");

[[nodiscard]] constexpr blip::core::ComponentDescriptor
bootstrap_descriptor(bool recovery) noexcept {
    blip::core::ComponentDescriptor descriptor{};
    descriptor.schema_version = kBootstrapSchemaVersion;
    descriptor.id = recovery ? "blip.recovery" : "blip.bootstrap";
    descriptor.display_name = recovery ? "BLIP recovery" : "BLIP bootstrap";
    descriptor.description =
        recovery ? "Safe-mode recovery composition" : "Milestone 2 registry self-test component";
    descriptor.required_services = recovery
                                       ? std::span<const std::string_view>{kRecoveryDependencies}
                                       : std::span<const std::string_view>{kBootstrapDependencies};
    if (!recovery) {
        descriptor.parameters = kBootstrapParameters;
        descriptor.actions = kBootstrapActions;
    }
    descriptor.settings = {1, 1};
    descriptor.disable_policy = blip::core::DisablePolicy::reboot_required;
    return descriptor;
}

class BootstrapComponent final : public blip::core::Component {
  public:
    explicit constexpr BootstrapComponent(bool recovery) noexcept : recovery_(recovery) {}

    [[nodiscard]] const blip::core::ComponentDescriptor& descriptor() const noexcept override {
        return recovery_ ? recovery_descriptor_ : descriptor_;
    }

    [[nodiscard]] blip::core::Status start(const blip::core::StartContext&) noexcept override {
        started_ = true;
        return blip::core::Status::success();
    }

    [[nodiscard]] blip::core::Status stop() noexcept override {
        started_ = false;
        return blip::core::Status::success();
    }

    [[nodiscard]] blip::core::Status
    read_parameter(std::string_view id, blip::core::ScalarValue& output) noexcept override {
        if (!started_ || recovery_ || id != "probe_value") {
            return control_error(blip::core::ErrorCode::not_found, "read-parameter",
                                 "parameter-not-found");
        }
        output = blip::core::ScalarValue::from_integer(probe_value_);
        return blip::core::Status::success();
    }

    [[nodiscard]] blip::core::Status
    write_parameter(std::string_view id, const blip::core::ScalarValue& value) noexcept override {
        if (!started_ || recovery_ || id != "probe_value") {
            return control_error(blip::core::ErrorCode::not_found, "write-parameter",
                                 "parameter-not-found");
        }
        if (value.type != blip::core::ValueType::integer || value.integer < -100 ||
            value.integer > 100) {
            return control_error(blip::core::ErrorCode::validation_failed, "write-parameter",
                                 "invalid-value");
        }
        probe_value_ = value.integer;
        return blip::core::Status::success();
    }

    [[nodiscard]] blip::core::Status
    invoke_action(std::string_view id, std::span<const blip::core::ScalarValue> arguments,
                  std::span<blip::core::ScalarValue>, std::size_t& output_count) noexcept override {
        output_count = 0;
        if (!started_ || recovery_ || id != "reset_probe") {
            return control_error(blip::core::ErrorCode::not_found, "invoke-action",
                                 "action-not-found");
        }
        if (!arguments.empty()) {
            return control_error(blip::core::ErrorCode::validation_failed, "invoke-action",
                                 "arguments-not-allowed");
        }
        probe_value_ = 0;
        return blip::core::Status::success();
    }

  private:
    [[nodiscard]] blip::core::Status control_error(blip::core::ErrorCode code,
                                                   std::string_view operation,
                                                   std::string_view detail) const noexcept {
        return blip::core::Status::failure(
            {blip::core::ErrorDomain::control, code, descriptor().id, operation, detail});
    }

    static constexpr blip::core::ComponentDescriptor descriptor_{bootstrap_descriptor(false)};
    static constexpr blip::core::ComponentDescriptor recovery_descriptor_{
        bootstrap_descriptor(true)};
    bool recovery_{};
    bool started_{};
    std::int64_t probe_value_{};
};

BootstrapComponent bootstrap_component{false};
BootstrapComponent recovery_component{true};
blip::core::EspDiagnosticsComponent diagnostics_component{};
blip::storage::NvsSettingsComponent settings_component{};
blip::storage::LittleFsStorageComponent file_storage_component{};
blip::storage::LegacySettingsImportComponent legacy_import_component{settings_component.settings()};
blip::network::EspWifiComponent wifi_component{settings_component.settings()};
blip::core::Registry<9> registry{};
blip::core::RegistryControlService<9> control_component{registry};
blip::transport::EspSerialTransportComponent serial_transport_component{control_component};
blip::oscquery::EspOscQueryComponent oscquery_component{registry, control_component,
                                                        wifi_component};

class EspMonotonicClock final : public blip::core::Clock {
  public:
    [[nodiscard]] std::uint64_t now_us() noexcept override {
        return static_cast<std::uint64_t>(esp_timer_get_time());
    }
};

EspMonotonicClock monotonic_clock{};
blip::core::Scheduler<4> scheduler{monotonic_clock};
blip::resources::Broker<1, 1> resource_broker{};

[[nodiscard]] const char* legacy_import_status() noexcept {
    return legacy_import_component.disposition() ==
                   blip::storage::LegacyImportDisposition::no_source
               ? "none"
               : "confirmed";
}

[[nodiscard]] bool start_registry(bool safe_mode) noexcept {
    const auto diagnostics_status = registry.add(diagnostics_component);
    if (!diagnostics_status) {
        return false;
    }
    const auto control_status = registry.add(control_component);
    if (!control_status) {
        return false;
    }
    const auto transport_status = registry.add(serial_transport_component);
    if (!transport_status) {
        return false;
    }
    if (safe_mode) {
        const auto recovery_status = registry.add(recovery_component);
        if (!recovery_status) {
            return false;
        }
    } else {
        const auto storage_status = registry.add(settings_component);
        if (!storage_status) {
            return false;
        }
        const auto file_storage_status = registry.add(file_storage_component);
        if (!file_storage_status) {
            return false;
        }
        const auto import_status = registry.add(legacy_import_component);
        if (!import_status) {
            return false;
        }
        const auto wifi_status = registry.add(wifi_component);
        if (!wifi_status) {
            return false;
        }
        const auto oscquery_status = registry.add(oscquery_component);
        if (!oscquery_status) {
            return false;
        }
        const auto add_status = registry.add(bootstrap_component);
        if (!add_status) {
            return false;
        }
    }
    const auto validation_status = registry.validate();
    if (!validation_status) {
        return false;
    }
    const auto started = registry.start_all();
    if (!started.ok()) {
        const auto& error = started.primary;
        ESP_LOGE(kTag, "registry start failed component=%.*s operation=%.*s detail=%.*s",
                 static_cast<int>(error.component.size()), error.component.data(),
                 static_cast<int>(error.operation.size()), error.operation.data(),
                 static_cast<int>(error.detail.size()), error.detail.data());
        return false;
    }
    return true;
}

} // namespace

extern "C" void app_main() {
    if (!diagnostics_component.prepare_boot()) {
        ESP_LOGE(kTag, "BLIP_V2_DIAGNOSTICS_PREPARE_FAILED");
        return;
    }
    const bool safe_mode = diagnostics_component.safe_mode();
#if defined(BLIP_DIAGNOSTICS_HIL_CLEAR_SAFE_MODE)
    if (safe_mode) {
        if (!diagnostics_component.clear_safe_mode()) {
            ESP_LOGE(kTag, "BLIP_V2_SAFE_MODE_CLEAR_FAILED");
            return;
        }
        ESP_LOGW(kTag, "BLIP_V2_SAFE_MODE_CLEARED");
        esp_restart();
    }
#endif
    if (!start_registry(safe_mode)) {
        ESP_LOGE(kTag, "BLIP_V2_REGISTRY_FAILED");
        return;
    }
#if defined(BLIP_DIAGNOSTICS_HIL_FORCE_BOOT_LOOP)
    if (!safe_mode) {
        ESP_LOGE(kTag, "BLIP_DIAGNOSTICS_HIL_FORCED_FAILURE");
        std::abort();
    }
#endif
    if (!safe_mode && !legacy_import_component.confirm_boot()) {
        ESP_LOGE(kTag, "BLIP_V2_IMPORT_CONFIRM_FAILED");
        static_cast<void>(registry.stop_all());
        return;
    }
    if (!diagnostics_component.confirm_boot()) {
        ESP_LOGE(kTag, "BLIP_V2_BOOT_CONFIRM_FAILED");
        static_cast<void>(registry.stop_all());
        return;
    }
    serial_transport_component.enable_control();
    const auto& diagnostics = diagnostics_component.snapshot();
    if (safe_mode) {
        ESP_LOGW(kTag,
                 "BLIP_V2_SAFE_MODE_READY schema=%lu registry=1 diagnostics=structured-v1 "
                 "serial=blip-envelope-v1 "
                 "safe_reason=%s reset=%s coredump=%s coredump_id=%08lx heap_free=%lu "
                 "stack_hwm=%lu target=%s idf=%s",
                 static_cast<unsigned long>(kBootstrapSchemaVersion),
                 blip::core::safe_mode_reason_name(diagnostics.boot.safe_mode_reason).data(),
                 blip::core::reset_cause_name(diagnostics.boot.reset_cause).data(),
                 blip::core::coredump_status_name(diagnostics.coredump.status),
                 static_cast<unsigned long>(diagnostics.coredump.identity_crc32),
                 static_cast<unsigned long>(diagnostics.free_internal_heap),
                 static_cast<unsigned long>(diagnostics.main_stack_high_water_bytes),
                 CONFIG_IDF_TARGET, esp_get_idf_version());
        return;
    }
    ESP_LOGI(kTag,
             "BLIP_V2_BOOTSTRAP_READY schema=%lu registry=1 scheduler=ready resources=ready "
             "settings=nvs-v1 files=littlefs-v1 legacy_import=%s diagnostics=structured-v1 "
             "serial=blip-envelope-v1 osc=udp9000-oscquery-v1 osc_stack_hwm=%lu "
             "wifi_state=%u wifi_ap=%.*s "
             "safe_mode=0 reset=%s coredump=%s coredump_id=%08lx heap_free=%lu "
             "heap_largest=%lu stack_hwm=%lu target=%s idf=%s",
             static_cast<unsigned long>(kBootstrapSchemaVersion), legacy_import_status(),
             static_cast<unsigned long>(oscquery_component.task_stack_headroom_bytes()),
             static_cast<unsigned>(wifi_component.connection_state()),
             static_cast<int>(wifi_component.access_point_ssid().size()),
             wifi_component.access_point_ssid().data(),
             blip::core::reset_cause_name(diagnostics.boot.reset_cause).data(),
             blip::core::coredump_status_name(diagnostics.coredump.status),
             static_cast<unsigned long>(diagnostics.coredump.identity_crc32),
             static_cast<unsigned long>(diagnostics.free_internal_heap),
             static_cast<unsigned long>(diagnostics.largest_free_internal_block),
             static_cast<unsigned long>(diagnostics.main_stack_high_water_bytes), CONFIG_IDF_TARGET,
             esp_get_idf_version());
}
