#pragma once

#include "blip/core/component.hpp"
#include "blip/core/control.hpp"
#include "blip/core/registry.hpp"
#include "blip/network/esp_wifi_component.hpp"
#include "blip/oscquery/legacy_osc.hpp"
#include "blip/oscquery/osc.hpp"
#include "blip/ota/update_service.hpp"
#include "blip/ota/esp_release_component.hpp"
#include "blip/resources/board_manifest.hpp"
#include "blip/resources/snapshot.hpp"
#include "blip/storage/web_asset_store.hpp"
#include "blip/storage/automatic_file_store.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace blip::oscquery {

class EspOscQueryComponent final : public core::Component, public network::HttpRootDelegate {
  public:
    static constexpr std::uint16_t kOscPort = 9000;
    // Persisted writes can trigger NVS page collection beneath OSC dispatch.
    static constexpr std::size_t kTaskStackBytes = 8192;
    static constexpr std::size_t kTaskStackWords = kTaskStackBytes / sizeof(StackType_t);

    EspOscQueryComponent(const core::RegistryView& registry, core::ControlService& controls,
                         network::EspWifiComponent& wifi, storage::WebAssetStore& web_assets,
                         storage::AutomaticFileStore& files,
                         ota::UpdateService& updates, ota::EspReleaseComponent& releases, resources::DeviceBroker& resources,
                         resources::BoardManifest board) noexcept;

    [[nodiscard]] const core::ComponentDescriptor& descriptor() const noexcept override;
    [[nodiscard]] core::Status start(const core::StartContext&) noexcept override;
    [[nodiscard]] core::Status stop() noexcept override;
    [[nodiscard]] bool callbacks_quiesced() const noexcept override;
    [[nodiscard]] core::Status read_parameter(std::string_view id,
                                              core::ScalarValue& output) noexcept override;
    [[nodiscard]] esp_err_t handle_http_root(httpd_req_t* request) noexcept override;

    [[nodiscard]] std::uint32_t task_stack_headroom_bytes() const noexcept;

  private:
    void run() noexcept;
    void handle_udp_packet(std::span<const std::byte> packet, const void* source,
                           std::size_t source_size) noexcept;
    [[nodiscard]] esp_err_t handle_http_get(httpd_req_t* request) noexcept;
    [[nodiscard]] esp_err_t handle_asset_get(httpd_req_t* request, std::string_view path) noexcept;
    [[nodiscard]] esp_err_t handle_asset_status(httpd_req_t* request) noexcept;
    [[nodiscard]] esp_err_t handle_asset_upload(httpd_req_t* request) noexcept;
    [[nodiscard]] esp_err_t handle_update_status(httpd_req_t* request) noexcept;
    [[nodiscard]] esp_err_t handle_release_status(httpd_req_t* request) noexcept;
    [[nodiscard]] esp_err_t handle_release_action(httpd_req_t* request) noexcept;
    [[nodiscard]] esp_err_t handle_resource_status(httpd_req_t* request) noexcept;
    [[nodiscard]] esp_err_t handle_resource_reassignment(httpd_req_t* request) noexcept;
    [[nodiscard]] esp_err_t handle_firmware_upload(httpd_req_t* request) noexcept;
    [[nodiscard]] esp_err_t handle_file(httpd_req_t* request) noexcept;
    [[nodiscard]] esp_err_t handle_websocket(httpd_req_t* request) noexcept;
    [[nodiscard]] std::string_view local_ip() noexcept;

    static void task_entry(void* context) noexcept;
    static const core::ComponentDescriptor descriptor_;

    const core::RegistryView* registry_{};
    core::ControlService* controls_{};
    network::EspWifiComponent* wifi_{};
    storage::WebAssetStore* web_assets_{};
    storage::AutomaticFileStore* files_{};
    ota::UpdateService* updates_{};
    ota::EspReleaseComponent* releases_{};
    resources::DeviceBroker* resources_{};
    resources::BoardManifest board_{};
    std::array<char, 18> device_id_{};
    std::array<char, 16> local_ip_buffer_{};
    DeviceIdentity identity_{};
    LegacyOscEndpoint udp_endpoint_;
    LegacyOscEndpoint websocket_endpoint_;
    std::array<std::byte, kMaxOscPacketBytes> udp_packet_{};
    std::array<std::byte, kMaxOscPacketBytes> udp_response_{};
    std::array<std::byte, kMaxOscPacketBytes> websocket_packet_{};
    std::array<std::byte, kMaxOscPacketBytes> websocket_response_{};
    std::array<std::byte, 1024> http_asset_buffer_{};
    alignas(16) std::array<StackType_t, kTaskStackWords> task_stack_{};
    StaticTask_t task_storage_{};
    TaskHandle_t task_{};
    int socket_{-1};
    std::atomic<bool> started_{};
    std::atomic<bool> task_quiesced_{true};
    std::atomic<std::uint32_t> active_http_callbacks_{};
    std::atomic<std::uint32_t> received_packets_{};
    std::atomic<std::uint32_t> rejected_packets_{};
    std::atomic<std::uint32_t> http_stack_headroom_bytes_{
        network::EspWifiComponent::kPortalTaskStackBytes};
};

static_assert(sizeof(EspOscQueryComponent) <= 16384,
              "OSCQuery component exceeds its declared static RAM budget");

} // namespace blip::oscquery
