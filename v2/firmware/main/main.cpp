#include "blip/core/component.hpp"
#include "blip/core/registry.hpp"
#include "blip/core/scheduler.hpp"
#include "blip/resources/broker.hpp"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include <cstdint>

namespace {

constexpr char kTag[] = "blip_bootstrap";
constexpr std::uint32_t kBootstrapSchemaVersion = 1U;

static_assert(ESP_IDF_VERSION == ESP_IDF_VERSION_VAL(6, 0, 2),
              "BLIP V2 work package 1.1 requires ESP-IDF 6.0.2");

[[nodiscard]] constexpr blip::core::ComponentDescriptor bootstrap_descriptor() noexcept {
    blip::core::ComponentDescriptor descriptor{};
    descriptor.schema_version = kBootstrapSchemaVersion;
    descriptor.id = "blip.bootstrap";
    descriptor.display_name = "BLIP bootstrap";
    descriptor.description = "Milestone 1 registry self-test component";
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
blip::core::Registry<1> registry{};

class EspMonotonicClock final : public blip::core::Clock {
  public:
    [[nodiscard]] std::uint64_t now_us() noexcept override {
        return static_cast<std::uint64_t>(esp_timer_get_time());
    }
};

EspMonotonicClock monotonic_clock{};
blip::core::Scheduler<4> scheduler{monotonic_clock};
blip::resources::Broker<1, 1> resource_broker{};

[[nodiscard]] bool start_registry() noexcept {
    const auto add_status = registry.add(bootstrap_component);
    if (!add_status) {
        return false;
    }
    const auto validation_status = registry.validate();
    if (!validation_status) {
        return false;
    }
    return registry.start_all().ok();
}

} // namespace

extern "C" void app_main() {
    if (!start_registry()) {
        ESP_LOGE(kTag, "BLIP_V2_REGISTRY_FAILED");
        return;
    }
    ESP_LOGI(kTag,
             "BLIP_V2_BOOTSTRAP_READY schema=%lu registry=1 scheduler=ready resources=ready "
             "target=%s idf=%s",
             static_cast<unsigned long>(kBootstrapSchemaVersion), CONFIG_IDF_TARGET,
             esp_get_idf_version());
}
