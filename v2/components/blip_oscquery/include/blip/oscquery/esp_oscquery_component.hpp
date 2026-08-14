#pragma once

#include "blip/core/component.hpp"
#include "blip/core/control.hpp"
#include "blip/core/registry.hpp"
#include "blip/network/esp_wifi_component.hpp"
#include "blip/oscquery/legacy_osc.hpp"
#include "blip/oscquery/osc.hpp"
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
    static constexpr std::size_t kTaskStackBytes = 6144;
    static constexpr std::size_t kTaskStackWords = kTaskStackBytes / sizeof(StackType_t);

    EspOscQueryComponent(const core::RegistryView& registry, core::ControlService& controls,
                         network::EspWifiComponent& wifi) noexcept;

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
    [[nodiscard]] esp_err_t handle_websocket(httpd_req_t* request) noexcept;
    [[nodiscard]] std::string_view local_ip() noexcept;

    static void task_entry(void* context) noexcept;
    static const core::ComponentDescriptor descriptor_;

    const core::RegistryView* registry_{};
    core::ControlService* controls_{};
    network::EspWifiComponent* wifi_{};
    std::array<char, 18> device_id_{};
    std::array<char, 16> local_ip_buffer_{};
    DeviceIdentity identity_{};
    LegacyOscEndpoint udp_endpoint_;
    LegacyOscEndpoint websocket_endpoint_;
    std::array<std::byte, kMaxOscPacketBytes> udp_packet_{};
    std::array<std::byte, kMaxOscPacketBytes> udp_response_{};
    std::array<std::byte, kMaxOscPacketBytes> websocket_packet_{};
    std::array<std::byte, kMaxOscPacketBytes> websocket_response_{};
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
