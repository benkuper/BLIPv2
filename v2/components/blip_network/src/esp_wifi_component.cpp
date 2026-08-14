#include "blip/network/esp_wifi_component.hpp"

#include "blip/network/provisioning_form.hpp"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "soc/soc_caps.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>

namespace blip::network {
namespace {

constexpr char kTag[] = "blip_wifi";
constexpr std::array<std::string_view, 2> kProvidedServices{"transport.wifi", "network.http"};
constexpr std::array<std::string_view, 2> kRequiredServices{"storage.settings",
                                                            "storage.legacy_import"};
constexpr std::array<std::string_view, 1> kRadioAlternatives{"radio0"};
constexpr std::array<core::ResourceRequest, 1> kResources{{
    {core::ResourceClass::radio, "wifi", core::OwnershipMode::multiplexed, kRadioAlternatives, 0, 1,
     0, 0x01, 0, true},
}};
constexpr std::array<core::LegacyEnumValue, 3> kLegacyModeValues{{
    {core::ScalarValue::from_integer(0), "Wifi"},
    {core::ScalarValue::from_integer(1), "AP"},
    {core::ScalarValue::from_integer(2), "Wifi+AP"},
}};
constexpr std::array<core::LegacyEnumValue, 4> kLegacyTxPowerValues{{
    {core::ScalarValue::from_integer(0), "15dBm"},
    {core::ScalarValue::from_integer(1), "17dBm"},
    {core::ScalarValue::from_integer(2), "19.5dBm"},
    {core::ScalarValue::from_integer(3), "20.5dBm"},
}};
constexpr std::array<core::LegacyEnumValue, 4> kLegacyProtocolValues{{
    {core::ScalarValue::from_integer(0), "11B"},
    {core::ScalarValue::from_integer(1), "11BG"},
    {core::ScalarValue::from_integer(2), "11BGN"},
    {core::ScalarValue::from_integer(3), "AX"},
}};
constexpr std::array<core::LegacyParameterAlias, 7> kLegacyParameters{{
    {"mode", "mode", "Mode", core::ValueType::string, kLegacyModeValues},
    {"password", "pass", "Pass", core::ValueType::string, {}},
    {"manual_ip", "manualIP", "Manual IP", core::ValueType::string, {}},
    {"manual_gateway", "manualGateway", "Manual Gateway", core::ValueType::string, {}},
    {"channel_scan", "channelScanMode", "Channel Scan Mode", core::ValueType::boolean, {}},
    {"tx_power", "txPower", "Tx Power", core::ValueType::string, kLegacyTxPowerValues},
    {"protocol", "wifiProtocol", "Wifi Protocol", core::ValueType::string, kLegacyProtocolValues},
}};
constexpr std::array<core::MetadataEntry, 6> kMetadata{{
    {"legacy_path", "/wifi"},
    {"radio", "2.4GHz Wi-Fi"},
    {"provisioning", "serial-or-softap"},
    {"softap_address", "192.168.4.1"},
    {"secret_policy", "password-write-only"},
    {"antenna_modes", "0=board-default,1=onboard,2=external"},
}};
constexpr std::array<core::ParameterDescriptor, 14> kParameters{{
    {"enabled",
     "Wi-Fi enabled",
     core::ValueType::boolean,
     core::Access::read_write,
     true,
     core::ScalarValue::from_bool(true),
     {},
     ""},
    {"mode",
     "Wi-Fi mode",
     core::ValueType::integer,
     core::Access::read_write,
     true,
     core::ScalarValue::from_integer(0),
     {true, 0, 2, 1},
     ""},
    {"ssid",
     "Station SSID",
     core::ValueType::string,
     core::Access::read_write,
     true,
     core::ScalarValue::from_string(""),
     {},
     ""},
    {"password",
     "Station password",
     core::ValueType::string,
     core::Access::write_only,
     true,
     core::ScalarValue::from_string(""),
     {},
     ""},
    {"manual_ip",
     "Manual IPv4 address",
     core::ValueType::string,
     core::Access::read_write,
     true,
     core::ScalarValue::from_string(""),
     {},
     ""},
    {"manual_gateway",
     "Manual IPv4 gateway",
     core::ValueType::string,
     core::Access::read_write,
     true,
     core::ScalarValue::from_string(""),
     {},
     ""},
    {"channel_scan",
     "Scan all channels",
     core::ValueType::boolean,
     core::Access::read_write,
     true,
     core::ScalarValue::from_bool(true),
     {},
     ""},
    {"tx_power",
     "Transmit power profile",
     core::ValueType::integer,
     core::Access::read_write,
     true,
     core::ScalarValue::from_integer(2),
     {true, 0, 3, 1},
     ""},
    {"protocol",
     "802.11 protocol profile",
     core::ValueType::integer,
     core::Access::read_write,
     true,
     core::ScalarValue::from_integer(2),
     {true, 0, 3, 1},
     ""},
    {"channel",
     "Preferred channel",
     core::ValueType::integer,
     core::Access::read_write,
     true,
     core::ScalarValue::from_integer(0),
     {true, 0, 14, 1},
     ""},
    {"antenna",
     "RF antenna selection",
     core::ValueType::integer,
     core::Access::read_write,
     true,
     core::ScalarValue::from_integer(0),
     {true, 0, 2, 1},
     ""},
    {"signal",
     "Station signal",
     core::ValueType::number,
     core::Access::read_only,
     false,
     core::ScalarValue::from_number(0),
     {true, 0, 1, 0.01},
     "ratio"},
    {"state",
     "Connection state",
     core::ValueType::integer,
     core::Access::read_only,
     false,
     core::ScalarValue::from_integer(0),
     {true, 0, 5, 1},
     ""},
    {"worker_stack_headroom",
     "Wi-Fi worker stack headroom",
     core::ValueType::integer,
     core::Access::read_only,
     false,
     core::ScalarValue::from_integer(0),
     {true, 0, EspWifiComponent::kWorkerTaskStackBytes, 1},
     "bytes"},
}};
constexpr std::array<core::FieldDescriptor, 2> kProvisionArguments{{
    {"ssid", core::ValueType::string, true},
    {"password", core::ValueType::string, true},
}};
constexpr std::array<core::ActionDescriptor, 1> kActions{{
    {"provision", "Save station credentials", kProvisionArguments},
}};
constexpr std::array<core::FieldDescriptor, 1> kStateFields{{
    {"state", core::ValueType::integer, true},
}};
constexpr std::array<core::EventDescriptor, 1> kEvents{{
    {"connection_state_changed", kStateFields},
}};
constexpr std::array<core::DiagnosticDescriptor, 4> kDiagnostics{{
    {"retry_count", core::ValueType::integer, "attempts"},
    {"configuration_updates", core::ValueType::integer, "updates"},
    {"configuration_failures", core::ValueType::integer, "failures"},
    {"active_callbacks", core::ValueType::integer, "callbacks"},
}};

constexpr char kPortalHtml[] =
    "<!doctype html><html><head><meta charset=utf-8><meta name=viewport "
    "content=\"width=device-width,initial-scale=1\"><title>BLIP Wi-Fi setup</title></head>"
    "<body><main><h1>BLIP Wi-Fi setup</h1><form method=post action=/provision>"
    "<label>Network <input name=ssid maxlength=32 required></label><br>"
    "<label>Password <input name=password type=password maxlength=63></label><br>"
    "<button type=submit>Connect</button></form></main></body></html>";
constexpr char kProvisionAccepted[] =
    "<!doctype html><meta charset=utf-8><title>BLIP Wi-Fi setup</title>"
    "<p>Credentials saved. BLIP is connecting now.</p>";

[[nodiscard]] constexpr core::ComponentDescriptor wifi_descriptor() noexcept {
    core::ComponentDescriptor descriptor{};
    descriptor.schema_version = 1;
    descriptor.id = "blip.transport.wifi";
    descriptor.display_name = "Wi-Fi network manager";
    descriptor.description =
        "Versioned station/AP management with bounded serial and SoftAP provisioning";
    descriptor.metadata = kMetadata;
    descriptor.provided_services = kProvidedServices;
    descriptor.required_services = kRequiredServices;
    descriptor.parameters = kParameters;
    descriptor.legacy_parameters = kLegacyParameters;
    descriptor.actions = kActions;
    descriptor.events = kEvents;
    descriptor.diagnostics = kDiagnostics;
    descriptor.resources = kResources;
    descriptor.settings = {1, 1};
    descriptor.disable_policy = core::DisablePolicy::live;
    descriptor.supports_restart = true;
    descriptor.cost = {196608, 10240,
                       EspWifiComponent::kPortalTaskStackBytes +
                           EspWifiComponent::kWorkerTaskStackBytes};
    return descriptor;
}

[[nodiscard]] core::Error wifi_error(core::ErrorCode code, std::string_view operation,
                                     std::string_view detail) noexcept {
    return {core::ErrorDomain::transport, code, "blip.transport.wifi", operation, detail};
}

[[nodiscard]] core::Status platform_status(esp_err_t result, std::string_view operation,
                                           std::string_view detail) noexcept {
    return result == ESP_OK
               ? core::Status::success()
               : core::Status::failure(wifi_error(core::ErrorCode::io_failed, operation, detail));
}

void saturating_increment(std::atomic<std::uint32_t>& value) noexcept {
    std::uint32_t current = value.load();
    while (current != std::numeric_limits<std::uint32_t>::max() &&
           !value.compare_exchange_weak(current, current + 1U)) {
    }
}

[[nodiscard]] constexpr std::uint8_t protocol_bitmap(WifiProtocol protocol) noexcept {
    switch (protocol) {
    case WifiProtocol::b:
        return WIFI_PROTOCOL_11B;
    case WifiProtocol::bg:
        return WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G;
    case WifiProtocol::bgn:
        return WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N;
    case WifiProtocol::ax:
        return WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AX;
    }
    return 0;
}

[[nodiscard]] constexpr std::int8_t tx_power_quarter_dbm(std::uint8_t profile) noexcept {
    constexpr std::array<std::int8_t, 4> values{60, 68, 78, 82};
    return values[profile];
}

} // namespace

const core::ComponentDescriptor EspWifiComponent::descriptor_{wifi_descriptor()};

EspWifiComponent::EspWifiComponent(storage::SettingsStore& settings) noexcept
    : settings_(&settings) {}

const core::ComponentDescriptor& EspWifiComponent::descriptor() const noexcept {
    return descriptor_;
}

bool EspWifiComponent::lock() noexcept {
    return mutex_ != nullptr && xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE;
}

void EspWifiComponent::unlock() noexcept {
    if (mutex_ != nullptr) {
        static_cast<void>(xSemaphoreGive(mutex_));
    }
}

core::Status EspWifiComponent::load_config() noexcept {
    const auto loaded = settings_->load(descriptor_, settings_buffer_);
    if (!loaded) {
        if (loaded.error().code == core::ErrorCode::not_found) {
            config_ = {};
            return core::Status::success();
        }
        return core::Status::failure(loaded.error());
    }
    const auto decoded = decode_wifi_config(
        std::span<const std::byte>{settings_buffer_.data(), loaded.value().payload_size},
        decode_workspace_);
    if (!decoded) {
        return core::Status::failure(decoded.error());
    }
    config_ = decoded.value().config;
    if (decoded.value().source == WifiSettingsSource::legacy_import) {
        return save_config_locked(config_);
    }
    return core::Status::success();
}

core::Status EspWifiComponent::save_config_locked(const WifiConfig& config) noexcept {
    const auto encoded = encode_wifi_config(config, settings_buffer_);
    if (!encoded) {
        return core::Status::failure(encoded.error());
    }
    return settings_->save(descriptor_,
                           std::span<const std::byte>{settings_buffer_.data(), encoded.value()});
}

core::Status EspWifiComponent::queue_config_locked(const WifiConfig& config, bool defer_radio,
                                                   std::uint32_t& update_id) noexcept {
    update_id = 0;
    const auto valid = validate_wifi_config(config);
    if (!valid) {
        return valid;
    }
#if !SOC_WIFI_HE_SUPPORT
    if (config.protocol == WifiProtocol::ax) {
        return core::Status::failure(
            wifi_error(core::ErrorCode::validation_failed, "configure", "wifi-6-unsupported"));
    }
#endif
    if (config == config_) {
        return core::Status::success();
    }
    pending_config_ = config;
    pending_defer_radio_ = defer_radio;
    reconfigure_pending_ = true;
    ++next_update_id_;
    if (next_update_id_ == 0U) {
        ++next_update_id_;
    }
    pending_update_id_ = next_update_id_;
    update_id = pending_update_id_;
    if (worker_task_ != nullptr) {
        xTaskNotifyGive(worker_task_);
    }
    return core::Status::success();
}

core::Status EspWifiComponent::commit_config(const WifiConfig& config, bool defer_radio) noexcept {
    while (completion_ != nullptr && xSemaphoreTake(completion_, 0) == pdTRUE) {
    }
    if (!started_.load() || !lock()) {
        return core::Status::failure(
            wifi_error(core::ErrorCode::invalid_state, "configure", "not-started"));
    }
    std::uint32_t update_id{};
    auto status = queue_config_locked(config, defer_radio, update_id);
    unlock();
    if (status && update_id != 0U) {
        const TickType_t started_at = xTaskGetTickCount();
        const TickType_t timeout = pdMS_TO_TICKS(10000);
        bool complete{};
        while (!complete) {
            const TickType_t now = xTaskGetTickCount();
            const TickType_t elapsed = now - started_at;
            if (elapsed >= timeout || completion_ == nullptr ||
                xSemaphoreTake(completion_, timeout - elapsed) != pdTRUE) {
                status = core::Status::failure(
                    wifi_error(core::ErrorCode::io_failed, "configure", "commit-timeout"));
                break;
            }
            if (lock()) {
                complete = completed_update_id_ == update_id;
                if (complete) {
                    status = completed_update_status_;
                }
                unlock();
            }
        }
    }
    return status;
}

core::Status EspWifiComponent::initialize_platform() noexcept {
    esp_err_t result = esp_netif_init();
    if (result == ESP_OK) {
        netif_owned_ = true;
    } else if (result != ESP_ERR_INVALID_STATE) {
        return platform_status(result, "start", "netif-init-failed");
    }
    result = esp_event_loop_create_default();
    if (result == ESP_OK) {
        event_loop_owned_ = true;
    } else if (result != ESP_ERR_INVALID_STATE) {
        return platform_status(result, "start", "event-loop-failed");
    }
    station_netif_ = esp_netif_create_default_wifi_sta();
    access_point_netif_ = esp_netif_create_default_wifi_ap();
    if (station_netif_ == nullptr || access_point_netif_ == nullptr) {
        return core::Status::failure(
            wifi_error(core::ErrorCode::start_failed, "start", "netif-create-failed"));
    }

    wifi_init_config_t initialization = WIFI_INIT_CONFIG_DEFAULT();
    result = esp_wifi_init(&initialization);
    if (result != ESP_OK) {
        return platform_status(result, "start", "wifi-init-failed");
    }
    wifi_initialized_ = true;
    if ((result = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK) {
        return platform_status(result, "start", "wifi-storage-failed");
    }
    if ((result = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_callback,
                                                      this, &wifi_event_instance_)) != ESP_OK) {
        return platform_status(result, "start", "wifi-event-handler-failed");
    }
    if ((result = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event_callback,
                                                      this, &ip_event_instance_)) != ESP_OK) {
        return platform_status(result, "start", "ip-event-handler-failed");
    }
    return core::Status::success();
}

core::Status EspWifiComponent::configure_ip_locked() noexcept {
    if (config_.manual_ip.empty()) {
        const esp_err_t result = esp_netif_dhcpc_start(station_netif_);
        return result == ESP_OK || result == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED
                   ? core::Status::success()
                   : platform_status(result, "configure-ip", "dhcp-start-failed");
    }
    esp_err_t result = esp_netif_dhcpc_stop(station_netif_);
    if (result != ESP_OK && result != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        return platform_status(result, "configure-ip", "dhcp-stop-failed");
    }
    esp_netif_ip_info_t information{};
    if (esp_netif_str_to_ip4(config_.manual_ip.bytes.data(), &information.ip) != ESP_OK ||
        esp_netif_str_to_ip4(config_.manual_gateway.bytes.data(), &information.gw) != ESP_OK ||
        esp_netif_str_to_ip4("255.255.255.0", &information.netmask) != ESP_OK) {
        return core::Status::failure(
            wifi_error(core::ErrorCode::validation_failed, "configure-ip", "invalid-address"));
    }
    return platform_status(esp_netif_set_ip_info(station_netif_, &information), "configure-ip",
                           "set-address-failed");
}

core::Status EspWifiComponent::configure_station_locked() noexcept {
    wifi_config_t station{};
    std::memcpy(station.sta.ssid, config_.ssid.bytes.data(), config_.ssid.size);
    std::memcpy(station.sta.password, config_.password.bytes.data(), config_.password.size);
    station.sta.scan_method = config_.channel_scan ? WIFI_ALL_CHANNEL_SCAN : WIFI_FAST_SCAN;
    station.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    station.sta.threshold.authmode = WIFI_AUTH_OPEN;
    station.sta.pmf_cfg.capable = true;
    station.sta.pmf_cfg.required = false;
    station.sta.channel = config_.channel;
    auto status = configure_ip_locked();
    if (!status) {
        return status;
    }
    return platform_status(esp_wifi_set_config(WIFI_IF_STA, &station), "configure-station",
                           "set-config-failed");
}

core::Status EspWifiComponent::configure_access_point_locked() noexcept {
    wifi_config_t access_point{};
    std::memcpy(access_point.ap.ssid, ap_ssid_.data(), ap_ssid_size_);
    access_point.ap.ssid_len = static_cast<std::uint8_t>(ap_ssid_size_);
    access_point.ap.channel = config_.channel == 0U ? 1U : config_.channel;
    access_point.ap.authmode = WIFI_AUTH_OPEN;
    access_point.ap.max_connection = 4;
    access_point.ap.pmf_cfg.required = false;
    access_point.ap.beacon_interval = 100;
    return platform_status(esp_wifi_set_config(WIFI_IF_AP, &access_point), "configure-access-point",
                           "set-config-failed");
}

core::Status EspWifiComponent::configure_protocol_locked() noexcept {
    const std::uint8_t bitmap = protocol_bitmap(config_.protocol);
    if (state_machine_.station_active() && esp_wifi_set_protocol(WIFI_IF_STA, bitmap) != ESP_OK) {
        return core::Status::failure(wifi_error(core::ErrorCode::io_failed, "configure-protocol",
                                                "station-protocol-failed"));
    }
    if (state_machine_.ap_active() && esp_wifi_set_protocol(WIFI_IF_AP, bitmap) != ESP_OK) {
        return core::Status::failure(wifi_error(core::ErrorCode::io_failed, "configure-protocol",
                                                "access-point-protocol-failed"));
    }
    return platform_status(esp_wifi_set_max_tx_power(tx_power_quarter_dbm(config_.tx_power_index)),
                           "configure-protocol", "tx-power-failed");
}

core::Status EspWifiComponent::configure_antenna_locked() noexcept {
#if defined(BLIP_WIFI_RF_SWITCH_POWER_GPIO) && defined(BLIP_WIFI_RF_SWITCH_SELECT_GPIO)
    const gpio_num_t power = static_cast<gpio_num_t>(BLIP_WIFI_RF_SWITCH_POWER_GPIO);
    const gpio_num_t select = static_cast<gpio_num_t>(BLIP_WIFI_RF_SWITCH_SELECT_GPIO);
    if (gpio_set_direction(power, GPIO_MODE_OUTPUT) != ESP_OK ||
        gpio_set_level(power, BLIP_WIFI_RF_SWITCH_POWER_ACTIVE_LEVEL) != ESP_OK) {
        return core::Status::failure(
            wifi_error(core::ErrorCode::io_failed, "configure-antenna", "switch-power-failed"));
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    const bool external = config_.antenna == WifiAntenna::external;
    if (gpio_set_direction(select, GPIO_MODE_OUTPUT) != ESP_OK ||
        gpio_set_level(select, external ? BLIP_WIFI_RF_SWITCH_EXTERNAL_LEVEL
                                        : !BLIP_WIFI_RF_SWITCH_EXTERNAL_LEVEL) != ESP_OK) {
        return core::Status::failure(
            wifi_error(core::ErrorCode::io_failed, "configure-antenna", "switch-select-failed"));
    }
    ESP_LOGI(kTag, "antenna=%s", external ? "external" : "onboard");
    return core::Status::success();
#else
    return config_.antenna == WifiAntenna::board_default
               ? core::Status::success()
               : core::Status::failure(wifi_error(core::ErrorCode::resource_unavailable,
                                                  "configure-antenna", "no-board-rf-switch"));
#endif
}

core::Status EspWifiComponent::start_portal_locked() noexcept {
    if (portal_ != nullptr) {
        return core::Status::success();
    }
    httpd_config_t configuration = HTTPD_DEFAULT_CONFIG();
    configuration.stack_size = kPortalTaskStackBytes;
    configuration.max_open_sockets = 4;
    configuration.lru_purge_enable = true;
    configuration.uri_match_fn = httpd_uri_match_wildcard;
    if (httpd_start(&portal_, &configuration) != ESP_OK) {
        portal_ = nullptr;
        return core::Status::failure(
            wifi_error(core::ErrorCode::start_failed, "start-portal", "http-server-failed"));
    }
    const httpd_uri_t root{
        .uri = "/*",
        .method = HTTP_GET,
        .handler = root_handler,
        .user_ctx = this,
        .is_websocket = true,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr,
    };
    const httpd_uri_t provision_uri{
        .uri = "/provision",
        .method = HTTP_POST,
        .handler = provision_handler,
        .user_ctx = this,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr,
    };
    const httpd_uri_t web_asset_update_uri{
        .uri = "/api/web-assets",
        .method = HTTP_PUT,
        .handler = root_handler,
        .user_ctx = this,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr,
    };
    const httpd_uri_t firmware_update_uri{
        .uri = "/api/firmware",
        .method = HTTP_PUT,
        .handler = root_handler,
        .user_ctx = this,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr,
    };
    if (httpd_register_uri_handler(portal_, &root) != ESP_OK ||
        httpd_register_uri_handler(portal_, &provision_uri) != ESP_OK ||
        httpd_register_uri_handler(portal_, &web_asset_update_uri) != ESP_OK ||
        httpd_register_uri_handler(portal_, &firmware_update_uri) != ESP_OK) {
        stop_portal_locked();
        return core::Status::failure(
            wifi_error(core::ErrorCode::start_failed, "start-portal", "handler-register-failed"));
    }
    return core::Status::success();
}

void EspWifiComponent::stop_portal_locked() noexcept {
    if (portal_ != nullptr) {
        static_cast<void>(httpd_stop(portal_));
        portal_ = nullptr;
    }
}

void EspWifiComponent::update_public_state_locked() noexcept {
    public_state_.store(state_machine_.state());
    public_ap_active_.store(state_machine_.ap_active());
}

void EspWifiComponent::update_signal_locked() noexcept {
    wifi_ap_record_t record{};
    if (state_machine_.state() == WifiConnectionState::connected &&
        esp_wifi_sta_get_ap_info(&record) == ESP_OK) {
        rssi_ = record.rssi;
    } else {
        rssi_ = -127;
    }
}

core::Status EspWifiComponent::reconfigure_radio_locked() noexcept {
    if (radio_started_) {
        static_cast<void>(esp_wifi_stop());
        radio_started_ = false;
    }
    connect_in_progress_ = false;
    const WifiTransition transition =
        state_machine_.apply(config_, static_cast<std::uint64_t>(esp_timer_get_time()) / 1000U);
    update_public_state_locked();
    rssi_ = -127;
    if (!config_.enabled) {
        return core::Status::success();
    }
    auto status = configure_antenna_locked();
    if (!status) {
        return status;
    }
    const wifi_mode_t mode = state_machine_.station_active()
                                 ? (state_machine_.ap_active() ? WIFI_MODE_APSTA : WIFI_MODE_STA)
                                 : WIFI_MODE_AP;
    status = platform_status(esp_wifi_set_mode(mode), "configure", "set-mode-failed");
    if (!status) {
        return status;
    }
    if (state_machine_.station_active()) {
        status = configure_station_locked();
        if (!status) {
            return status;
        }
    }
    if (state_machine_.ap_active()) {
        status = configure_access_point_locked();
        if (!status) {
            return status;
        }
    }
    status = platform_status(esp_wifi_start(), "configure", "wifi-start-failed");
    if (!status) {
        return status;
    }
    radio_started_ = true;
    status = configure_protocol_locked();
    if (!status) {
        return status;
    }
    status = start_portal_locked();
    if (!status) {
        return status;
    }
    if (transition.connect_station) {
        const esp_err_t connected = esp_wifi_connect();
        if (connected != ESP_OK) {
            return platform_status(connected, "configure", "station-connect-failed");
        }
        connect_in_progress_ = true;
    }
    return core::Status::success();
}

void EspWifiComponent::process_transition_locked(const WifiTransition& transition) noexcept {
    if (transition.stop_radio) {
        stop_portal_locked();
        if (radio_started_) {
            static_cast<void>(esp_wifi_stop());
            radio_started_ = false;
        }
        connect_in_progress_ = false;
        update_public_state_locked();
        return;
    }
    if (transition.start_ap && radio_started_) {
        if (esp_wifi_set_mode(state_machine_.station_active() ? WIFI_MODE_APSTA : WIFI_MODE_AP) !=
                ESP_OK ||
            !configure_access_point_locked() ||
            esp_wifi_set_protocol(WIFI_IF_AP, protocol_bitmap(config_.protocol)) != ESP_OK ||
            !start_portal_locked()) {
            saturating_increment(configuration_failures_);
        }
    }
    if (transition.stop_ap && radio_started_) {
        // Keep the bounded HTTP server task alive after its first start. Only the AP radio is
        // removed here; stopping the server from a Wi-Fi event callback could wait on an active
        // provisioning handler and deadlock its synchronous settings commit.
        if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) {
            saturating_increment(configuration_failures_);
        }
    }
    if (transition.connect_station && radio_started_ && !connect_in_progress_) {
        const esp_err_t result = esp_wifi_connect();
        if (result == ESP_OK || result == ESP_ERR_WIFI_CONN) {
            connect_in_progress_ = true;
        } else {
            saturating_increment(configuration_failures_);
        }
    }
    update_public_state_locked();
}

core::Status EspWifiComponent::start(const core::StartContext&) noexcept {
    if (started_.load()) {
        return core::Status::success();
    }
    mutex_ = xSemaphoreCreateMutexStatic(&mutex_storage_);
    request_mutex_ = xSemaphoreCreateMutexStatic(&request_mutex_storage_);
    completion_ = xSemaphoreCreateBinaryStatic(&completion_storage_);
    if (mutex_ == nullptr || request_mutex_ == nullptr || completion_ == nullptr) {
        mutex_ = nullptr;
        request_mutex_ = nullptr;
        completion_ = nullptr;
        return core::Status::failure(
            wifi_error(core::ErrorCode::start_failed, "start", "mutex-create-failed"));
    }
    auto status = load_config();
    if (!status) {
        mutex_ = nullptr;
        return status;
    }
#if !SOC_WIFI_HE_SUPPORT
    if (config_.protocol == WifiProtocol::ax) {
        mutex_ = nullptr;
        return core::Status::failure(
            wifi_error(core::ErrorCode::validation_failed, "start", "wifi-6-unsupported"));
    }
#endif
    status = initialize_platform();
    if (!status) {
        cleanup_platform();
        mutex_ = nullptr;
        return status;
    }
    std::array<std::uint8_t, 6> mac{};
    if (esp_read_mac(mac.data(), ESP_MAC_WIFI_STA) != ESP_OK) {
        cleanup_platform();
        mutex_ = nullptr;
        return core::Status::failure(
            wifi_error(core::ErrorCode::start_failed, "start", "read-mac-failed"));
    }
    const int written = std::snprintf(ap_ssid_.data(), ap_ssid_.size(), "BLIP-%02X%02X%02X", mac[3],
                                      mac[4], mac[5]);
    if (written <= 0 || static_cast<std::size_t>(written) >= ap_ssid_.size()) {
        cleanup_platform();
        mutex_ = nullptr;
        return core::Status::failure(
            wifi_error(core::ErrorCode::start_failed, "start", "format-ap-name-failed"));
    }
    ap_ssid_size_ = static_cast<std::size_t>(written);
    quiesced_.store(false);
    started_.store(true);
    if (!lock()) {
        started_.store(false);
        cleanup_platform();
        mutex_ = nullptr;
        quiesced_.store(true);
        return core::Status::failure(
            wifi_error(core::ErrorCode::start_failed, "start", "mutex-take-failed"));
    }
    status = reconfigure_radio_locked();
    unlock();
    if (!status) {
        started_.store(false);
        cleanup_platform();
        mutex_ = nullptr;
        quiesced_.store(true);
        return status;
    }
    worker_quiesced_.store(false);
    worker_task_ = xTaskCreateStatic(task_entry, "blip_wifi", worker_stack_.size(), this, 4,
                                     worker_stack_.data(), &worker_task_storage_);
    if (worker_task_ == nullptr) {
        worker_quiesced_.store(true);
        started_.store(false);
        cleanup_platform();
        mutex_ = nullptr;
        quiesced_.store(true);
        return core::Status::failure(
            wifi_error(core::ErrorCode::start_failed, "start", "worker-create-failed"));
    }
    ESP_LOGI(kTag, "network manager ready state=%u ap=%.*s",
             static_cast<unsigned>(public_state_.load()), static_cast<int>(ap_ssid_size_),
             ap_ssid_.data());
    return core::Status::success();
}

void EspWifiComponent::cleanup_platform() noexcept {
    if (mutex_ != nullptr && lock()) {
        stop_portal_locked();
        if (radio_started_) {
            static_cast<void>(esp_wifi_stop());
            radio_started_ = false;
        }
        unlock();
    } else if (portal_ != nullptr) {
        static_cast<void>(httpd_stop(portal_));
        portal_ = nullptr;
    }
    if (wifi_event_instance_ != nullptr) {
        static_cast<void>(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                                wifi_event_instance_));
        wifi_event_instance_ = nullptr;
    }
    if (ip_event_instance_ != nullptr) {
        static_cast<void>(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                                ip_event_instance_));
        ip_event_instance_ = nullptr;
    }
    if (wifi_initialized_) {
        static_cast<void>(esp_wifi_deinit());
        wifi_initialized_ = false;
    }
    if (station_netif_ != nullptr) {
        esp_netif_destroy_default_wifi(station_netif_);
        station_netif_ = nullptr;
    }
    if (access_point_netif_ != nullptr) {
        esp_netif_destroy_default_wifi(access_point_netif_);
        access_point_netif_ = nullptr;
    }
    if (event_loop_owned_) {
        static_cast<void>(esp_event_loop_delete_default());
        event_loop_owned_ = false;
    }
    if (netif_owned_) {
        static_cast<void>(esp_netif_deinit());
        netif_owned_ = false;
    }
}

core::Status EspWifiComponent::stop() noexcept {
    if (!started_.exchange(false) && quiesced_.load()) {
        return core::Status::success();
    }
    if (worker_task_ != nullptr) {
        xTaskNotifyGive(worker_task_);
    }
    for (std::size_t attempt = 0; attempt < 200U && !worker_quiesced_.load(); ++attempt) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    worker_task_ = nullptr;
    cleanup_platform();
    for (std::size_t attempt = 0; attempt < 100U && active_callbacks_.load() != 0U; ++attempt) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    public_state_.store(WifiConnectionState::off);
    public_ap_active_.store(false);
    reconfigure_pending_ = false;
    pending_defer_radio_ = false;
    deferred_apply_pending_ = false;
    connect_in_progress_ = false;
    deferred_apply_after_ms_ = 0;
    mutex_ = nullptr;
    request_mutex_ = nullptr;
    completion_ = nullptr;
    const bool stopped = active_callbacks_.load() == 0U && worker_quiesced_.load();
    quiesced_.store(stopped);
    return stopped ? core::Status::success()
                   : core::Status::failure(
                         wifi_error(core::ErrorCode::stop_failed, "stop", "callbacks-active"));
}

bool EspWifiComponent::callbacks_quiesced() const noexcept {
    return quiesced_.load() && worker_quiesced_.load() && active_callbacks_.load() == 0U;
}

core::Status EspWifiComponent::read_parameter(std::string_view id,
                                              core::ScalarValue& output) noexcept {
    if (!started_.load() || !lock()) {
        return core::Status::failure(
            wifi_error(core::ErrorCode::invalid_state, "read-parameter", "not-started"));
    }
    if (id == "enabled") {
        output = core::ScalarValue::from_bool(config_.enabled);
    } else if (id == "mode") {
        output = core::ScalarValue::from_integer(static_cast<std::int64_t>(config_.mode));
    } else if (id == "channel_scan") {
        output = core::ScalarValue::from_bool(config_.channel_scan);
    } else if (id == "tx_power") {
        output = core::ScalarValue::from_integer(config_.tx_power_index);
    } else if (id == "protocol") {
        output = core::ScalarValue::from_integer(static_cast<std::int64_t>(config_.protocol));
    } else if (id == "channel") {
        output = core::ScalarValue::from_integer(config_.channel);
    } else if (id == "antenna") {
        output = core::ScalarValue::from_integer(static_cast<std::int64_t>(config_.antenna));
    } else if (id == "signal") {
        const double signal = rssi_ <= -100 ? 0.0 : rssi_ >= -50 ? 1.0 : (rssi_ + 100) / 50.0;
        output = core::ScalarValue::from_number(signal);
    } else if (id == "state") {
        output = core::ScalarValue::from_integer(static_cast<std::int64_t>(state_machine_.state()));
    } else if (id == "worker_stack_headroom") {
        output = core::ScalarValue::from_integer(worker_stack_headroom_bytes());
    } else {
        std::string_view value{};
        if (id == "ssid") {
            value = config_.ssid.view();
        } else if (id == "manual_ip") {
            value = config_.manual_ip.view();
        } else if (id == "manual_gateway") {
            value = config_.manual_gateway.view();
        } else {
            unlock();
            return core::Status::failure(
                wifi_error(core::ErrorCode::not_found, "read-parameter", "parameter-not-found"));
        }
        std::copy(value.begin(), value.end(), readback_text_.begin());
        readback_text_size_ = value.size();
        output = core::ScalarValue::from_string({readback_text_.data(), readback_text_size_});
    }
    unlock();
    return core::Status::success();
}

core::Status EspWifiComponent::write_parameter(std::string_view id,
                                               const core::ScalarValue& value) noexcept {
    if (request_mutex_ == nullptr ||
        xSemaphoreTake(request_mutex_, pdMS_TO_TICKS(10000)) != pdTRUE) {
        return core::Status::failure(
            wifi_error(core::ErrorCode::invalid_state, "write-parameter", "request-busy"));
    }
    if (!started_.load() || !lock()) {
        static_cast<void>(xSemaphoreGive(request_mutex_));
        return core::Status::failure(
            wifi_error(core::ErrorCode::invalid_state, "write-parameter", "not-started"));
    }
    WifiConfig candidate = config_;
    bool assigned = true;
    if (id == "enabled" && value.type == core::ValueType::boolean) {
        candidate.enabled = value.boolean;
    } else if (id == "mode" && value.type == core::ValueType::integer) {
        candidate.mode = static_cast<WifiMode>(value.integer);
    } else if (id == "ssid" && value.type == core::ValueType::string) {
        assigned = candidate.ssid.assign(value.string);
    } else if (id == "password" && value.type == core::ValueType::string) {
        assigned = candidate.password.assign(value.string);
    } else if (id == "manual_ip" && value.type == core::ValueType::string) {
        assigned = candidate.manual_ip.assign(value.string);
    } else if (id == "manual_gateway" && value.type == core::ValueType::string) {
        assigned = candidate.manual_gateway.assign(value.string);
    } else if (id == "channel_scan" && value.type == core::ValueType::boolean) {
        candidate.channel_scan = value.boolean;
    } else if (id == "tx_power" && value.type == core::ValueType::integer) {
        candidate.tx_power_index = static_cast<std::uint8_t>(value.integer);
    } else if (id == "protocol" && value.type == core::ValueType::integer) {
        candidate.protocol = static_cast<WifiProtocol>(value.integer);
    } else if (id == "channel" && value.type == core::ValueType::integer) {
        candidate.channel = static_cast<std::uint8_t>(value.integer);
    } else if (id == "antenna" && value.type == core::ValueType::integer) {
        candidate.antenna = static_cast<WifiAntenna>(value.integer);
    } else {
        unlock();
        static_cast<void>(xSemaphoreGive(request_mutex_));
        return core::Status::failure(
            wifi_error(core::ErrorCode::not_found, "write-parameter", "parameter-not-found"));
    }
    unlock();
    const auto status =
        assigned ? commit_config(candidate, false)
                 : core::Status::failure(wifi_error(core::ErrorCode::validation_failed,
                                                    "write-parameter", "value-too-long"));
    static_cast<void>(xSemaphoreGive(request_mutex_));
    return status;
}

core::Status EspWifiComponent::provision(std::string_view ssid, std::string_view password,
                                         bool defer_radio) noexcept {
    if (request_mutex_ == nullptr ||
        xSemaphoreTake(request_mutex_, pdMS_TO_TICKS(10000)) != pdTRUE) {
        return core::Status::failure(
            wifi_error(core::ErrorCode::invalid_state, "provision", "request-busy"));
    }
    if (!started_.load() || !lock()) {
        static_cast<void>(xSemaphoreGive(request_mutex_));
        return core::Status::failure(
            wifi_error(core::ErrorCode::invalid_state, "provision", "not-started"));
    }
    WifiConfig candidate = config_;
    candidate.enabled = true;
    if (candidate.mode == WifiMode::access_point) {
        candidate.mode = WifiMode::station;
    }
    const bool assigned = candidate.ssid.assign(ssid) && candidate.password.assign(password);
    const auto valid = assigned
                           ? validate_wifi_config(candidate, true)
                           : core::Status::failure(wifi_error(core::ErrorCode::validation_failed,
                                                              "provision", "invalid-credentials"));
    unlock();
    const auto status = valid ? commit_config(candidate, defer_radio) : valid;
    static_cast<void>(xSemaphoreGive(request_mutex_));
    return status;
}

core::Status EspWifiComponent::invoke_action(std::string_view id,
                                             std::span<const core::ScalarValue> arguments,
                                             std::span<core::ScalarValue>,
                                             std::size_t& output_count) noexcept {
    output_count = 0;
    if (id != "provision" || arguments.size() != 2U ||
        arguments[0].type != core::ValueType::string ||
        arguments[1].type != core::ValueType::string) {
        return core::Status::failure(
            wifi_error(core::ErrorCode::invalid_argument, "invoke-action", "invalid-action"));
    }
    return provision(arguments[0].string, arguments[1].string, false);
}

void EspWifiComponent::tick() noexcept {
    if (started_.load() && lock()) {
        const std::uint64_t now_ms = static_cast<std::uint64_t>(esp_timer_get_time()) / 1000U;
        process_transition_locked(state_machine_.tick(now_ms));
        update_signal_locked();
        unlock();
    }
}

void EspWifiComponent::run() noexcept {
    while (started_.load()) {
        WifiConfig candidate{};
        std::uint32_t update_id{};
        bool defer_radio{};
        bool has_candidate{};
        if (lock()) {
            if (reconfigure_pending_) {
                candidate = pending_config_;
                update_id = pending_update_id_;
                defer_radio = pending_defer_radio_;
                reconfigure_pending_ = false;
                has_candidate = true;
            }
            unlock();
        }
        if (has_candidate) {
            core::Status outcome = core::Status::failure(
                wifi_error(core::ErrorCode::invalid_state, "configure", "worker-stopping"));
            if (defer_radio) {
                outcome = save_config_locked(candidate);
                if (outcome && started_.load() && lock()) {
                    if (!deferred_apply_pending_) {
                        deferred_previous_config_ = config_;
                    }
                    config_ = candidate;
                    deferred_apply_pending_ = true;
                    deferred_apply_after_ms_ =
                        static_cast<std::uint64_t>(esp_timer_get_time()) / 1000U + 1000U;
                    unlock();
                } else if (outcome) {
                    outcome = core::Status::failure(
                        wifi_error(core::ErrorCode::invalid_state, "configure", "worker-stopping"));
                }
            } else {
                WifiConfig previous{};
                core::Status applied = core::Status::failure(
                    wifi_error(core::ErrorCode::invalid_state, "configure", "worker-stopping"));
                if (started_.load() && lock()) {
                    previous = config_;
                    deferred_apply_pending_ = false;
                    config_ = candidate;
                    applied = reconfigure_radio_locked();
                    if (!applied) {
                        config_ = previous;
                        static_cast<void>(reconfigure_radio_locked());
                    }
                    unlock();
                }
                outcome = applied ? save_config_locked(candidate) : applied;
                if (!outcome && applied && started_.load() && lock()) {
                    config_ = previous;
                    static_cast<void>(reconfigure_radio_locked());
                    unlock();
                }
                if (outcome) {
                    saturating_increment(configuration_updates_);
                }
            }
            if (!outcome) {
                saturating_increment(configuration_failures_);
                ESP_LOGE(kTag, "configuration failed operation=%.*s detail=%.*s",
                         static_cast<int>(outcome.error().operation.size()),
                         outcome.error().operation.data(),
                         static_cast<int>(outcome.error().detail.size()),
                         outcome.error().detail.data());
            }
            if (lock()) {
                completed_update_id_ = update_id;
                completed_update_status_ = outcome;
                unlock();
            }
            if (completion_ != nullptr) {
                static_cast<void>(xSemaphoreGive(completion_));
            }
        } else {
            WifiConfig previous{};
            bool apply_deferred{};
            core::Status applied = core::Status::success();
            if (lock()) {
                const std::uint64_t now_ms =
                    static_cast<std::uint64_t>(esp_timer_get_time()) / 1000U;
                if (deferred_apply_pending_ && now_ms >= deferred_apply_after_ms_ &&
                    active_callbacks_.load() == 0U) {
                    previous = deferred_previous_config_;
                    deferred_apply_pending_ = false;
                    apply_deferred = true;
                    applied = reconfigure_radio_locked();
                    if (!applied) {
                        config_ = previous;
                        static_cast<void>(reconfigure_radio_locked());
                    }
                }
                unlock();
            }
            if (apply_deferred) {
                if (!applied) {
                    const auto rollback = save_config_locked(previous);
                    saturating_increment(configuration_failures_);
                    const auto& error = rollback ? applied.error() : rollback.error();
                    ESP_LOGE(kTag, "deferred configuration failed operation=%.*s detail=%.*s",
                             static_cast<int>(error.operation.size()), error.operation.data(),
                             static_cast<int>(error.detail.size()), error.detail.data());
                } else {
                    saturating_increment(configuration_updates_);
                }
            } else {
                tick();
            }
        }
        static_cast<void>(ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250)));
    }
    worker_quiesced_.store(true);
    vTaskDelete(nullptr);
}

std::uint32_t EspWifiComponent::worker_stack_headroom_bytes() const noexcept {
    return worker_task_ == nullptr
               ? 0U
               : static_cast<std::uint32_t>(uxTaskGetStackHighWaterMark(worker_task_)) *
                     sizeof(StackType_t);
}

void EspWifiComponent::handle_event(esp_event_base_t event_base, std::int32_t event_id,
                                    void* event_data) noexcept {
    static_cast<void>(event_data);
    active_callbacks_.fetch_add(1U);
    if (started_.load() && lock()) {
        const std::uint64_t now_ms = static_cast<std::uint64_t>(esp_timer_get_time()) / 1000U;
        if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
            connect_in_progress_ = false;
            rssi_ = -127;
            process_transition_locked(state_machine_.station_disconnected(now_ms));
        } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
            connect_in_progress_ = false;
            process_transition_locked(state_machine_.station_connected(now_ms));
            update_signal_locked();
        }
        unlock();
    }
    active_callbacks_.fetch_sub(1U);
}

esp_err_t EspWifiComponent::handle_root(httpd_req_t* request) noexcept {
    auto* delegate = http_root_delegate_.load();
    const int socket = httpd_req_to_sockfd(request);
    const bool websocket =
        socket >= 0 && httpd_ws_get_fd_info(request->handle, socket) == HTTPD_WS_CLIENT_WEBSOCKET;
    const bool query = httpd_req_get_url_query_len(request) != 0U;
    const bool root = std::string_view{request->uri} == "/";
    if (delegate != nullptr && (websocket || query || !root || !public_ap_active_.load())) {
        return delegate->handle_http_root(request);
    }
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, kPortalHtml, HTTPD_RESP_USE_STRLEN);
}

bool EspWifiComponent::set_http_root_delegate(HttpRootDelegate& delegate) noexcept {
    HttpRootDelegate* expected{};
    return http_root_delegate_.compare_exchange_strong(expected, &delegate) ||
           expected == &delegate;
}

void EspWifiComponent::clear_http_root_delegate(const HttpRootDelegate& delegate) noexcept {
    HttpRootDelegate* expected = const_cast<HttpRootDelegate*>(&delegate);
    static_cast<void>(http_root_delegate_.compare_exchange_strong(expected, nullptr));
}

bool EspWifiComponent::local_ipv4(std::span<char> output, std::size_t& size) const noexcept {
    size = 0;
    if (!started_.load() || output.size() < 16U) {
        return false;
    }
    esp_netif_t* netif = public_state_.load() == WifiConnectionState::connected
                             ? station_netif_
                             : access_point_netif_;
    esp_netif_ip_info_t information{};
    if (netif == nullptr || esp_netif_get_ip_info(netif, &information) != ESP_OK ||
        esp_ip4addr_ntoa(&information.ip, output.data(), static_cast<int>(output.size())) ==
            nullptr) {
        return false;
    }
    size = std::char_traits<char>::length(output.data());
    return size != 0U;
}

esp_err_t EspWifiComponent::handle_provision(httpd_req_t* request) noexcept {
    if (request->content_len == 0U || request->content_len > kMaxProvisioningFormBytes) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "invalid form size");
    }
    std::size_t offset{};
    while (offset < request->content_len) {
        const int received =
            httpd_req_recv(request, form_buffer_.data() + offset, request->content_len - offset);
        if (received <= 0) {
            return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "incomplete form");
        }
        offset += static_cast<std::size_t>(received);
    }
    const auto parsed = parse_provisioning_form({form_buffer_.data(), offset});
    if (!parsed) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "invalid credentials");
    }
    const auto status = provision(parsed.value().ssid.view(), parsed.value().password.view(), true);
    std::fill_n(form_buffer_.begin(), offset, '\0');
    if (!status) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "credentials were not saved");
    }
    httpd_resp_set_status(request, "202 Accepted");
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, kProvisionAccepted, HTTPD_RESP_USE_STRLEN);
}

void EspWifiComponent::task_entry(void* context) noexcept {
    static_cast<EspWifiComponent*>(context)->run();
}

void EspWifiComponent::event_callback(void* context, esp_event_base_t event_base,
                                      std::int32_t event_id, void* event_data) noexcept {
    static_cast<EspWifiComponent*>(context)->handle_event(event_base, event_id, event_data);
}

esp_err_t EspWifiComponent::root_handler(httpd_req_t* request) noexcept {
    auto* component = static_cast<EspWifiComponent*>(request->user_ctx);
    component->active_callbacks_.fetch_add(1U);
    const esp_err_t result = component->handle_root(request);
    component->active_callbacks_.fetch_sub(1U);
    return result;
}

esp_err_t EspWifiComponent::provision_handler(httpd_req_t* request) noexcept {
    auto* component = static_cast<EspWifiComponent*>(request->user_ctx);
    component->active_callbacks_.fetch_add(1U);
    const esp_err_t result = component->handle_provision(request);
    component->active_callbacks_.fetch_sub(1U);
    return result;
}

} // namespace blip::network
