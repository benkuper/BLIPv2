#include "blip/core/component.hpp"
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
#include <string_view>

namespace {

constexpr char kTag[] = "blip_bootstrap";
constexpr std::uint32_t kBootstrapSchemaVersion = 1U;
constexpr std::array<std::string_view, 3> kBootstrapDependencies{
    "storage.settings", "storage.files.internal", "storage.legacy_import"};

static_assert(ESP_IDF_VERSION == ESP_IDF_VERSION_VAL(6, 0, 2),
              "BLIP V2 work package 1.1 requires ESP-IDF 6.0.2");

[[nodiscard]] constexpr blip::core::ComponentDescriptor bootstrap_descriptor() noexcept {
    blip::core::ComponentDescriptor descriptor{};
    descriptor.schema_version = kBootstrapSchemaVersion;
    descriptor.id = "blip.bootstrap";
    descriptor.display_name = "BLIP bootstrap";
    descriptor.description = "Milestone 1 registry self-test component";
    descriptor.required_services = kBootstrapDependencies;
    descriptor.settings = {1, 1};
    descriptor.disable_policy = blip::core::DisablePolicy::reboot_required;
    return descriptor;
}

class BootstrapComponent final : public blip::core::Component {
  public:
    [[nodiscard]] const blip::core::ComponentDescriptor& descriptor() const noexcept override {
        return descriptor_;
    }

    [[nodiscard]] blip::core::Status start(const blip::core::StartContext&) noexcept override {
        return blip::core::Status::success();
    }

    [[nodiscard]] blip::core::Status stop() noexcept override {
        return blip::core::Status::success();
    }

  private:
    static constexpr blip::core::ComponentDescriptor descriptor_{bootstrap_descriptor()};
};

BootstrapComponent bootstrap_component{};
blip::storage::NvsSettingsComponent settings_component{};
blip::storage::LittleFsStorageComponent file_storage_component{};
blip::storage::LegacySettingsImportComponent legacy_import_component{settings_component.settings()};
blip::core::Registry<4> registry{};

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

[[nodiscard]] bool start_registry() noexcept {
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
    if (!start_registry()) {
        ESP_LOGE(kTag, "BLIP_V2_REGISTRY_FAILED");
        return;
    }
    if (!legacy_import_component.confirm_boot()) {
        ESP_LOGE(kTag, "BLIP_V2_IMPORT_CONFIRM_FAILED");
        static_cast<void>(registry.stop_all());
        return;
    }
    ESP_LOGI(kTag,
             "BLIP_V2_BOOTSTRAP_READY schema=%lu registry=1 scheduler=ready resources=ready "
             "settings=nvs-v1 files=littlefs-v1 legacy_import=%s target=%s idf=%s",
             static_cast<unsigned long>(kBootstrapSchemaVersion), legacy_import_status(),
             CONFIG_IDF_TARGET, esp_get_idf_version());
}
