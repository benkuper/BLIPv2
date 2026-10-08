#include "blip/core/component.hpp"
#include "blip/core/control.hpp"
#include "blip/core/esp_diagnostics_component.hpp"
#include "blip/core/registry.hpp"
#include "blip/core/scheduler.hpp"
#if defined(BLIP_ENABLE_WASM)
#include "blip/wasm/esp_wasm_component.hpp"
#include "blip/wasm/wamr_runtime.hpp"
#endif
#if defined(BLIP_ENABLE_FLEET)
#include "blip/fleet/esp_fleet_component.hpp"
#endif
#include "blip/led/esp_rmt_strip_component.hpp"
#include "blip/pm/esp_power_manager_component.hpp"
#if defined(BLIP_BOARD_ADAFRUIT_HUZZAH32)
#include "blip/power/esp_battery_component.hpp"
#include "blip/power/esp_sleep_component.hpp"
#endif
#include "blip/network/esp_wifi_component.hpp"
#include "blip/oscquery/esp_oscquery_component.hpp"
#include "blip/ota/esp_ota_component.hpp"
#include "blip/resources/broker.hpp"
#include "blip/resources/board_manifest.hpp"
#include "blip/storage/littlefs_storage_component.hpp"
#include "blip/storage/nvs_settings_component.hpp"
#include "blip/transport/esp_serial_transport_component.hpp"
#if defined(BLIP_ENABLE_ESPNOW)
#include "blip/transport/esp_espnow_transport_component.hpp"
#endif
#if defined(BLIP_ENABLE_BLE)
#include "blip/transport/esp_ble_transport_component.hpp"
#endif
#if defined(BLIP_ENABLE_CLASSIC_BT)
#include "blip/transport/esp_classic_transport_component.hpp"
#endif
#include "network_lighting_features.hpp"
#include "driver/gpio.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#if defined(BLIP_QUALIFY_WASM_PROVIDERS)
#include "../../qualification/wasm-providers/lifecycle.hpp"
#endif

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <string_view>

namespace {

constexpr char kTag[] = "blip_bootstrap";

[[nodiscard]] bool initialize_board_power() noexcept {
#if defined(BLIP_BOARD_CREATORS_TAB) || defined(BLIP_BOARD_CREATORS_CLUB)
    constexpr gpio_num_t kPowerHold = GPIO_NUM_12;
    constexpr gpio_num_t kLedPower = GPIO_NUM_27;
#elif defined(BLIP_BOARD_CREATORS_BALL_V2)
    constexpr gpio_num_t kPowerHold = GPIO_NUM_22;
    constexpr gpio_num_t kLedPower = GPIO_NUM_21;
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
    constexpr gpio_num_t kPowerHold = GPIO_NUM_46;
    constexpr gpio_num_t kLedPower = GPIO_NUM_38;
#else
    return true;
#endif
#if defined(BLIP_BOARD_CREATORS_TAB) || defined(BLIP_BOARD_CREATORS_CLUB) || \
    defined(BLIP_BOARD_CREATORS_BALL_V2) || \
    defined(CONFIG_IDF_TARGET_ESP32S3)
    // Raise the board power latch before storage, network, or registry startup.
    return gpio_set_level(kPowerHold, 1) == ESP_OK &&
           gpio_set_direction(kPowerHold, GPIO_MODE_OUTPUT) == ESP_OK &&
           gpio_set_level(kLedPower, 0) == ESP_OK &&
           gpio_set_direction(kLedPower, GPIO_MODE_OUTPUT) == ESP_OK;
#endif
}
#if defined(BLIP_BOARD_CREATORS_BALL_V2)
constexpr char kLedBootStatus[] = "hd108-spi-dma";
#elif defined(BLIP_BOARD_CREATORS_CLUB)
constexpr char kLedBootStatus[] = "sk9822-spi-dma";
#elif defined(BLIP_BOARD_CREATORS_TAB)
constexpr char kLedBootStatus[] = "ws2812b-board-power-pending";
#elif defined(BLIP_BOARD_M5STICKC)
constexpr char kLedBootStatus[] = "external-rmt-gpio26";
#else
constexpr char kLedBootStatus[] = "native-rmt-v1";
#endif
constexpr std::uint32_t kBootstrapSchemaVersion = 1U;
constexpr std::array<std::string_view, 8> kBootstrapDependencies{
    "diagnostics.runtime",   "storage.settings",   "storage.files.internal",
    "transport.serial",      "transport.wifi",     "firmware.ota",
    "discovery.oscquery",    "output.pixel-strip"};
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
        output = blip::core::ScalarValue::from_integer(probe_value_.load());
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
    std::atomic<std::int64_t> probe_value_{};
};

BootstrapComponent bootstrap_component{false};
BootstrapComponent recovery_component{true};
blip::core::EspDiagnosticsComponent diagnostics_component{};
blip::pm::EspPowerManagerComponent power_manager_component{};
blip::storage::NvsSettingsComponent settings_component{};
blip::storage::LittleFsStorageComponent file_storage_component{};
blip::network::EspWifiComponent wifi_component{settings_component.settings()};
constexpr auto board_manifest = blip::resources::selected_board_manifest();
blip::resources::DeviceBroker resource_broker{};
#if defined(CONFIG_IDF_TARGET_ESP32) && defined(BLIP_ENABLE_CLASSIC_BT)
// Bluedroid reserves more fixed DRAM than NimBLE. Construct these large
// components on the heap before starting any services.
blip::led::EspRmtStripComponent* led_component{};
#else
blip::led::EspRmtStripComponent led_storage{settings_component.settings(), resource_broker,
                                              power_manager_component};
blip::led::EspRmtStripComponent* led_component{&led_storage};
#endif
#if defined(BLIP_BOARD_ADAFRUIT_HUZZAH32)
blip::power::EspBatteryComponent battery_component{};
#if defined(BLIP_ENABLE_CLASSIC_BT)
blip::power::EspSleepComponent* sleep_component{};
#else
blip::power::EspSleepComponent sleep_storage{wifi_component, *led_component};
blip::power::EspSleepComponent* sleep_component{&sleep_storage};
#endif
#endif
blip::ota::EspOtaComponent ota_component{"blip-v2", CONFIG_IDF_TARGET, "minimal"};
blip::core::Registry<20> registry{};
blip::core::RegistryControlService<20> control_component{registry};
#if defined(BLIP_ENABLE_WASM)
blip::wasm::WamrRuntime wasm_runtime{};
blip::wasm::EspWasmComponent wasm_component{wasm_runtime};
#endif
#if defined(BLIP_ENABLE_FLEET)
blip::fleet::EspFleetComponent fleet_component{
    control_component, settings_component.settings(), wifi_component};
#endif
blip::transport::EspSerialTransportComponent serial_transport_component{control_component};
#if defined(BLIP_ENABLE_BLE)
blip::transport::EspBleTransportComponent ble_transport_component{
    control_component, settings_component.settings()};
#endif
#if defined(BLIP_ENABLE_CLASSIC_BT)
blip::transport::EspClassicTransportComponent classic_transport_component{
    control_component, settings_component.settings()};
#endif
#if defined(BLIP_ENABLE_ESPNOW)
blip::transport::EspEspNowTransportComponent espnow_transport_component{
    control_component, settings_component.settings(), wifi_component};
#endif
#if defined(CONFIG_IDF_TARGET_ESP32) && \
    (defined(BLIP_ENABLE_BLE) || defined(BLIP_ENABLE_CLASSIC_BT))
// The original ESP32 reserves a fixed DRAM region for its BT controller. Keep
// OSCQuery available by placing its large fixed packet buffers in the heap.
blip::oscquery::EspOscQueryComponent* oscquery_component{};
#else
blip::oscquery::EspOscQueryComponent oscquery_storage{
    registry, control_component, wifi_component, file_storage_component.web_assets(),
    ota_component.updates(), resource_broker, board_manifest};
blip::oscquery::EspOscQueryComponent* oscquery_component{&oscquery_storage};
#endif

class EspMonotonicClock final : public blip::core::Clock {
  public:
    [[nodiscard]] std::uint64_t now_us() noexcept override {
        return static_cast<std::uint64_t>(esp_timer_get_time());
    }
};

EspMonotonicClock monotonic_clock{};
blip::core::Scheduler<4> scheduler{monotonic_clock};

[[nodiscard]] bool initialize_resources() noexcept {
    for (const auto& pin : board_manifest.pins) {
        if (!resource_broker.add_resource(pin)) {
            return false;
        }
    }
#if defined(BLIP_BOARD_CREATORS_BALL_V2) || defined(BLIP_BOARD_CREATORS_CLUB)
    if (!resource_broker.add_resource(
            {blip::core::ResourceClass::spi, "spi2", 0U, 1U,
             "blip.output.strip0:spi", "SPI2 host", -1, "", "clocked-led-output", "", false})) {
        return false;
    }
#endif
    return true;
}

void reject_pending_update() noexcept {
    if (ota_component.pending_confirmation() && !ota_component.reject_boot()) {
        ESP_LOGE(kTag, "BLIP_V2_OTA_ROLLBACK_FAILED");
    }
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
        const auto power_status = registry.add(power_manager_component);
        if (!power_status) {
            return false;
        }
        const auto storage_status = registry.add(settings_component);
        if (!storage_status) {
            return false;
        }
        const auto file_storage_status = registry.add(file_storage_component);
        if (!file_storage_status) {
            return false;
        }
        const auto wifi_status = registry.add(wifi_component);
        if (!wifi_status) {
            return false;
        }
#if defined(BLIP_ENABLE_FLEET)
        if (!registry.add(fleet_component)) {
            return false;
        }
#endif
#if defined(BLIP_ENABLE_WASM)
        if (!registry.add(wasm_component)) return false;
#endif
#if defined(BLIP_ENABLE_BLE)
        const auto ble_status = registry.add(ble_transport_component);
        if (!ble_status) {
            return false;
        }
#endif
#if defined(BLIP_ENABLE_CLASSIC_BT)
        const auto classic_status = registry.add(classic_transport_component);
        if (!classic_status) {
            return false;
        }
#endif
#if defined(BLIP_ENABLE_ESPNOW)
        const auto espnow_status = registry.add(espnow_transport_component);
        if (!espnow_status) {
            return false;
        }
#endif
        const auto ota_status = registry.add(ota_component);
        if (!ota_status) {
            return false;
        }
        const auto oscquery_status = registry.add(*oscquery_component);
        if (!oscquery_status) {
            return false;
        }
        const auto led_status = registry.add(*led_component);
        if (!led_status) {
            return false;
        }
#if defined(BLIP_BOARD_ADAFRUIT_HUZZAH32)
        const auto battery_status = registry.add(battery_component);
        if (!battery_status) {
            return false;
        }
        const auto sleep_status = registry.add(*sleep_component);
        if (!sleep_status) {
            return false;
        }
#endif
#if defined(BLIP_ENABLE_DDP)
        const auto ddp_status = registry.add(blip::firmware::ddp_feature(*led_component));
        if (!ddp_status) {
            const auto& error = ddp_status.error();
            ESP_LOGE(kTag, "DDP registry add failed code=%u detail=%.*s",
                     static_cast<unsigned>(error.code), static_cast<int>(error.detail.size()),
                     error.detail.data());
            return false;
        }
#endif
#if defined(BLIP_ENABLE_ARTNET)
        const auto artnet_status = registry.add(
            blip::firmware::artnet_feature(wifi_component, *led_component));
        if (!artnet_status) {
            const auto& error = artnet_status.error();
            ESP_LOGE(kTag, "Art-Net registry add failed code=%u detail=%.*s",
                     static_cast<unsigned>(error.code), static_cast<int>(error.detail.size()),
                     error.detail.data());
            return false;
        }
#endif
#if defined(BLIP_ENABLE_E131)
        const auto e131_status = registry.add(
            blip::firmware::e131_feature(wifi_component, *led_component));
        if (!e131_status) {
            const auto& error = e131_status.error();
            ESP_LOGE(kTag, "E1.31 registry add failed code=%u detail=%.*s",
                     static_cast<unsigned>(error.code), static_cast<int>(error.detail.size()),
                     error.detail.data());
            return false;
        }
#endif
        const auto add_status = registry.add(bootstrap_component);
        if (!add_status) {
            return false;
        }
    }
#if defined(BLIP_ENABLE_WASM)
    if (!safe_mode) {
        const auto bound = wasm_component.bind_capabilities(registry);
        if (!bound) {
            ESP_LOGE(kTag, "WASM provider binding failed code=%u detail=%.*s", static_cast<unsigned>(bound.error().code),
                static_cast<int>(bound.error().detail.size()), bound.error().detail.data());
            return false;
        }
    }
#endif
    const auto validation_status = registry.validate();
    if (!validation_status) {
        const auto& error = validation_status.error();
        ESP_LOGE(kTag, "registry validation failed component=%.*s code=%u detail=%.*s",
                 static_cast<int>(error.component.size()), error.component.data(),
                 static_cast<unsigned>(error.code), static_cast<int>(error.detail.size()),
                 error.detail.data());
        return false;
    }
#if defined(BLIP_ENABLE_WASM)
    if (!safe_mode) {
        const auto reserved = wasm_component.reserve_buffers();
        if (!reserved) {
            ESP_LOGE(kTag, "WASM fixed pool reservation failed before service startup");
            return false;
        }
    }
#endif
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
    if (!initialize_board_power()) {
        ESP_LOGE(kTag, "BLIP_V2_BOARD_POWER_INIT_FAILED");
        return;
    }
    if (!initialize_resources()) {
        ESP_LOGE(kTag, "BLIP_V2_RESOURCE_INVENTORY_FAILED");
        return;
    }
    ota_component.prepare_boot();
    if (!diagnostics_component.prepare_boot()) {
        ESP_LOGE(kTag, "BLIP_V2_DIAGNOSTICS_PREPARE_FAILED");
        reject_pending_update();
        return;
    }
    const bool safe_mode = diagnostics_component.safe_mode();
    if (safe_mode && ota_component.pending_confirmation()) {
        reject_pending_update();
        return;
    }
#if defined(CONFIG_IDF_TARGET_ESP32) && defined(BLIP_ENABLE_CLASSIC_BT)
    if (!safe_mode) {
        led_component = new (std::nothrow) blip::led::EspRmtStripComponent{
            settings_component.settings(), resource_broker, power_manager_component};
        if (led_component == nullptr) {
            ESP_LOGE(kTag, "BLIP_V2_LED_ALLOCATION_FAILED");
            reject_pending_update();
            return;
        }
#if defined(BLIP_BOARD_ADAFRUIT_HUZZAH32)
        sleep_component = new (std::nothrow) blip::power::EspSleepComponent{
            wifi_component, *led_component};
        if (sleep_component == nullptr) {
            ESP_LOGE(kTag, "BLIP_V2_SLEEP_ALLOCATION_FAILED");
            reject_pending_update();
            return;
        }
#endif
    }
#endif
#if defined(CONFIG_IDF_TARGET_ESP32) && \
    (defined(BLIP_ENABLE_BLE) || defined(BLIP_ENABLE_CLASSIC_BT))
    if (!safe_mode) {
        oscquery_component = new (std::nothrow) blip::oscquery::EspOscQueryComponent{
            registry, control_component, wifi_component, file_storage_component.web_assets(),
            ota_component.updates(), resource_broker, board_manifest};
        if (oscquery_component == nullptr) {
            ESP_LOGE(kTag, "BLIP_V2_OSCQUERY_ALLOCATION_FAILED");
            reject_pending_update();
            return;
        }
    }
#endif
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
        reject_pending_update();
        return;
    }
#if defined(BLIP_QUALIFY_WASM_PROVIDERS)
    if (!safe_mode && !blip::qualification::qualify_provider_lifecycle(wasm_component, *led_component, fleet_component, registry)) {
        ESP_LOGE(kTag, "BLIP_V2_PROVIDER_QUALIFICATION_FAILED");
        return;
    }
#endif
#if defined(BLIP_OTA_HIL_ABORT_BEFORE_CONFIRM)
    if (!safe_mode && ota_component.pending_confirmation()) {
        ESP_LOGE(kTag, "BLIP_OTA_HIL_ABORT_BEFORE_CONFIRM");
        std::abort();
    }
#endif
#if defined(BLIP_DIAGNOSTICS_HIL_FORCE_BOOT_LOOP)
    if (!safe_mode) {
        ESP_LOGE(kTag, "BLIP_DIAGNOSTICS_HIL_FORCED_FAILURE");
        std::abort();
    }
#endif
    if (!diagnostics_component.confirm_boot()) {
        ESP_LOGE(kTag, "BLIP_V2_BOOT_CONFIRM_FAILED");
        static_cast<void>(registry.stop_all());
        reject_pending_update();
        return;
    }
    if (!safe_mode && !ota_component.confirm_boot()) {
        ESP_LOGE(kTag, "BLIP_V2_OTA_CONFIRM_FAILED");
        static_cast<void>(registry.stop_all());
        return;
    }
    serial_transport_component.enable_control();
#if defined(BLIP_ENABLE_WASM)
    if (!safe_mode) wasm_component.enable_control();
#endif
#if defined(BLIP_ENABLE_FLEET)
    if (!safe_mode) {
        fleet_component.enable_control();
    }
#endif
#if defined(BLIP_ENABLE_BLE)
    if (!safe_mode) {
        ble_transport_component.enable_control();
    }
#endif
#if defined(BLIP_ENABLE_CLASSIC_BT)
    if (!safe_mode) {
        classic_transport_component.enable_control();
    }
#endif
#if defined(BLIP_ENABLE_ESPNOW)
    if (!safe_mode) {
        espnow_transport_component.enable_control();
    }
#endif
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
             "settings=nvs-v1 files=littlefs-v1 web=bundle-v1 web_version=%lu ota=ab-v1 "
             "led=%s "
             "web_assets=%lu web_bytes=%lu diagnostics=structured-v1 "
             "serial=blip-envelope-v1 osc=udp9000-oscquery-v1 osc_stack_hwm=%lu "
             "wifi_state=%u wifi_ap=%.*s "
             "safe_mode=0 reset=%s coredump=%s coredump_id=%08lx heap_free=%lu "
             "heap_largest=%lu stack_hwm=%lu target=%s board=%.*s idf=%s",
             static_cast<unsigned long>(kBootstrapSchemaVersion),
             static_cast<unsigned long>(file_storage_component.web_assets().info().bundle_version),
             kLedBootStatus,
             static_cast<unsigned long>(file_storage_component.web_assets().info().asset_count),
             static_cast<unsigned long>(file_storage_component.web_assets().info().total_size),
             static_cast<unsigned long>(oscquery_component->task_stack_headroom_bytes()),
             static_cast<unsigned>(wifi_component.connection_state()),
             static_cast<int>(wifi_component.access_point_ssid().size()),
             wifi_component.access_point_ssid().data(),
             blip::core::reset_cause_name(diagnostics.boot.reset_cause).data(),
             blip::core::coredump_status_name(diagnostics.coredump.status),
             static_cast<unsigned long>(diagnostics.coredump.identity_crc32),
             static_cast<unsigned long>(diagnostics.free_internal_heap),
             static_cast<unsigned long>(diagnostics.largest_free_internal_block),
             static_cast<unsigned long>(diagnostics.main_stack_high_water_bytes), CONFIG_IDF_TARGET,
             static_cast<int>(board_manifest.id.size()), board_manifest.id.data(),
             esp_get_idf_version());
}
