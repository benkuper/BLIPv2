#include "blip/core/component.hpp"
#include "blip/core/esp_diagnostics_component.hpp"
#include "blip/core/registry.hpp"
#include "blip/core/scheduler.hpp"
#include "blip/resources/broker.hpp"
#include "blip/storage/legacy_settings_import_component.hpp"
#include "blip/storage/littlefs_storage_component.hpp"
#include "blip/storage/nvs_settings_component.hpp"
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
constexpr std::array<std::string_view, 4> kBootstrapDependencies{
    "diagnostics.runtime", "storage.settings", "storage.files.internal", "storage.legacy_import"};
constexpr std::array<std::string_view, 1> kRecoveryDependencies{"diagnostics.runtime"};

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
        return blip::core::Status::success();
    }

    [[nodiscard]] blip::core::Status stop() noexcept override {
        return blip::core::Status::success();
    }

  private:
    static constexpr blip::core::ComponentDescriptor descriptor_{bootstrap_descriptor(false)};
    static constexpr blip::core::ComponentDescriptor recovery_descriptor_{
        bootstrap_descriptor(true)};
    bool recovery_{};
};

BootstrapComponent bootstrap_component{false};
BootstrapComponent recovery_component{true};
blip::core::EspDiagnosticsComponent diagnostics_component{};
blip::storage::NvsSettingsComponent settings_component{};
blip::storage::LittleFsStorageComponent file_storage_component{};
blip::storage::LegacySettingsImportComponent legacy_import_component{settings_component.settings()};
blip::core::Registry<5> registry{};

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
    const auto& diagnostics = diagnostics_component.snapshot();
    if (safe_mode) {
        ESP_LOGW(kTag,
                 "BLIP_V2_SAFE_MODE_READY schema=%lu registry=1 diagnostics=structured-v1 "
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
             "safe_mode=0 reset=%s coredump=%s coredump_id=%08lx heap_free=%lu "
             "heap_largest=%lu stack_hwm=%lu target=%s idf=%s",
             static_cast<unsigned long>(kBootstrapSchemaVersion), legacy_import_status(),
             blip::core::reset_cause_name(diagnostics.boot.reset_cause).data(),
             blip::core::coredump_status_name(diagnostics.coredump.status),
             static_cast<unsigned long>(diagnostics.coredump.identity_crc32),
             static_cast<unsigned long>(diagnostics.free_internal_heap),
             static_cast<unsigned long>(diagnostics.largest_free_internal_block),
             static_cast<unsigned long>(diagnostics.main_stack_high_water_bytes), CONFIG_IDF_TARGET,
             esp_get_idf_version());
}
