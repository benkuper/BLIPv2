#pragma once

#include "blip/core/component.hpp"
#include "blip/network/provisioning_form.hpp"
#include "blip/network/wifi_config.hpp"
#include "blip/network/wifi_state_machine.hpp"
#include "blip/storage/settings_store.hpp"
#include "blip/storage/device_identity_component.hpp"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace blip::network {

class HttpRootDelegate {
  public:
    virtual ~HttpRootDelegate() = default;
    [[nodiscard]] virtual esp_err_t handle_http_root(httpd_req_t* request) noexcept = 0;
};

class EspWifiComponent final : public core::Component {
  public:
    static constexpr std::size_t kPortalTaskStackBytes = 8192;
    static constexpr std::size_t kWorkerTaskStackBytes = 4096;
    static constexpr std::size_t kWorkerTaskStackWords =
        kWorkerTaskStackBytes / sizeof(StackType_t);

    EspWifiComponent(storage::SettingsStore& settings, storage::DeviceIdentityComponent& identity) noexcept;
    [[nodiscard]] storage::DeviceNameSnapshot device_name() const noexcept { return identity_->snapshot(); }
    [[nodiscard]] std::string_view device_type() const noexcept { return identity_->type(); }

    [[nodiscard]] const core::ComponentDescriptor& descriptor() const noexcept override;
    [[nodiscard]] core::Status start(const core::StartContext&) noexcept override;
    [[nodiscard]] core::Status stop() noexcept override;
    [[nodiscard]] bool callbacks_quiesced() const noexcept override;
    [[nodiscard]] core::Status read_parameter(std::string_view id,
                                              core::ScalarValue& output) noexcept override;
    [[nodiscard]] core::Status write_parameter(std::string_view id,
                                               const core::ScalarValue& value) noexcept override;
    [[nodiscard]] core::Status invoke_action(std::string_view id,
                                             std::span<const core::ScalarValue> arguments,
                                             std::span<core::ScalarValue> outputs,
                                             std::size_t& output_count) noexcept override;

    [[nodiscard]] WifiConnectionState connection_state() const noexcept {
        return public_state_.load();
    }
    [[nodiscard]] std::string_view access_point_ssid() const noexcept {
        return {ap_ssid_.data(), ap_ssid_size_};
    }
    [[nodiscard]] std::uint32_t configuration_updates() const noexcept {
        return configuration_updates_.load();
    }
    [[nodiscard]] std::uint32_t configuration_failures() const noexcept {
        return configuration_failures_.load();
    }
    [[nodiscard]] std::uint32_t worker_stack_headroom_bytes() const noexcept;
    [[nodiscard]] bool set_http_root_delegate(HttpRootDelegate& delegate) noexcept;
    void clear_http_root_delegate(const HttpRootDelegate& delegate) noexcept;
    // Publish only while the OSC listener and shared HTTP server are available.
    [[nodiscard]] core::Status advertise_osc(std::uint16_t port) noexcept;
    void withdraw_osc() noexcept;
    [[nodiscard]] bool local_ipv4(std::span<char> output, std::size_t& size) const noexcept;
    // Shared timing clients temporarily suspend modem sleep, without changing
    // persisted Wi-Fi configuration or the radio memory profile.
    [[nodiscard]] core::Status acquire_low_latency() noexcept;
    void release_low_latency() noexcept;
    // Temporarily run STA for ESP-NOW on a fixed channel without association
    // or scanning/AP advertising. Restore saved networking on release.
    [[nodiscard]] core::Status acquire_autonomous_radio(std::uint8_t channel) noexcept;
    [[nodiscard]] core::Status release_autonomous_radio() noexcept;

  private:
    [[nodiscard]] core::Status load_config() noexcept;
    [[nodiscard]] core::Status save_config_locked(const WifiConfig& config) noexcept;
    [[nodiscard]] core::Status commit_config(const WifiConfig& config, bool defer_radio) noexcept;
    [[nodiscard]] core::Status queue_config_locked(const WifiConfig& config, bool defer_radio,
                                                   std::uint32_t& update_id) noexcept;
    [[nodiscard]] core::Status initialize_platform() noexcept;
    void cleanup_platform() noexcept;
    [[nodiscard]] core::Status reconfigure_radio_locked() noexcept;
    void process_transition_locked(const WifiTransition& transition) noexcept;
    [[nodiscard]] core::Status configure_station_locked() noexcept;
    [[nodiscard]] core::Status configure_access_point_locked() noexcept;
    [[nodiscard]] core::Status configure_protocol_locked() noexcept;
    [[nodiscard]] core::Status configure_antenna_locked() noexcept;
    [[nodiscard]] core::Status configure_ip_locked() noexcept;
    [[nodiscard]] core::Status start_portal_locked() noexcept;
    void stop_portal_locked() noexcept;
    [[nodiscard]] core::Status start_discovery_locked() noexcept;
    [[nodiscard]] core::Status update_discovery_name_locked() noexcept;
    void stop_discovery_locked() noexcept;
    [[nodiscard]] core::Status provision(std::string_view ssid, std::string_view password,
                                         bool defer_radio) noexcept;
    [[nodiscard]] bool lock() noexcept;
    void unlock() noexcept;
    void update_public_state_locked() noexcept;
    void update_signal_locked() noexcept;
    void tick() noexcept;
    void run() noexcept;
    void handle_event(esp_event_base_t event_base, std::int32_t event_id,
                      void* event_data) noexcept;
    [[nodiscard]] esp_err_t handle_root(httpd_req_t* request) noexcept;
    [[nodiscard]] esp_err_t handle_provision(httpd_req_t* request) noexcept;

    static void task_entry(void* context) noexcept;
    static void event_callback(void* context, esp_event_base_t event_base, std::int32_t event_id,
                               void* event_data) noexcept;
    static esp_err_t root_handler(httpd_req_t* request) noexcept;
    static esp_err_t provision_handler(httpd_req_t* request) noexcept;

    static const core::ComponentDescriptor descriptor_;

    storage::SettingsStore* settings_{};
    storage::DeviceIdentityComponent* identity_{};
    std::uint32_t advertised_identity_revision_{};
    WifiConfig config_{};
    RadioBootProfile active_boot_profile_{RadioBootProfile::wifi_loaded};
    WifiConfig pending_config_{};
    WifiConfig deferred_previous_config_{};
    WifiStateMachine state_machine_{};
    storage::ImportedSettingsDecodeWorkspace decode_workspace_{};
    std::array<std::byte, kMaxWifiSettingsBytes> settings_buffer_{};
    std::array<char, kMaxProvisioningFormBytes + 1U> form_buffer_{};
    std::array<char, 64> readback_text_{};
    std::array<char, 33> ap_ssid_{};
    alignas(16) std::array<StackType_t, kWorkerTaskStackWords> worker_stack_{};
    StaticTask_t worker_task_storage_{};
    TaskHandle_t worker_task_{};
    std::size_t readback_text_size_{};
    std::size_t ap_ssid_size_{};
    StaticSemaphore_t mutex_storage_{};
    SemaphoreHandle_t mutex_{};
    StaticSemaphore_t request_mutex_storage_{};
    SemaphoreHandle_t request_mutex_{};
    StaticSemaphore_t completion_storage_{};
    SemaphoreHandle_t completion_{};
    esp_netif_t* station_netif_{};
    esp_netif_t* access_point_netif_{};
    httpd_handle_t portal_{};
    std::uint16_t advertised_osc_port_{};
    bool mdns_started_{};
    esp_event_handler_instance_t wifi_event_instance_{};
    esp_event_handler_instance_t ip_event_instance_{};
    bool netif_owned_{};
    bool event_loop_owned_{};
    bool wifi_initialized_{};
    bool radio_started_{};
    wifi_ps_type_t previous_power_save_{WIFI_PS_MIN_MODEM};
    std::atomic<std::uint32_t> low_latency_clients_{};
    std::atomic<std::uint32_t> latency_policy_failures_{};
    std::atomic<std::uint8_t> autonomous_channel_{};
    bool reconfigure_pending_{};
    bool pending_defer_radio_{};
    bool deferred_apply_pending_{};
    bool connect_in_progress_{};
    std::uint64_t deferred_apply_after_ms_{};
    std::uint32_t next_update_id_{};
    std::uint32_t pending_update_id_{};
    std::uint32_t completed_update_id_{};
    core::Status completed_update_status_{core::Status::success()};
    std::int8_t rssi_{-127};
    std::atomic<bool> started_{};
    std::atomic<bool> quiesced_{true};
    std::atomic<bool> worker_quiesced_{true};
    std::atomic<std::uint32_t> active_callbacks_{};
    std::atomic<WifiConnectionState> public_state_{WifiConnectionState::off};
    std::atomic<bool> public_ap_active_{};
    std::atomic<std::uint32_t> configuration_updates_{};
    std::atomic<std::uint32_t> configuration_failures_{};
    std::atomic<HttpRootDelegate*> http_root_delegate_{};
};

static_assert(sizeof(EspWifiComponent) <= 10240,
              "Wi-Fi component exceeds its declared static RAM budget");

} // namespace blip::network
