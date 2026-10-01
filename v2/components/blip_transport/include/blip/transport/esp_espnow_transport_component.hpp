#pragma once

#include "blip/core/component.hpp"
#include "blip/network/esp_wifi_component.hpp"
#include "blip/storage/settings_store.hpp"
#include "blip/transport/espnow_frames.hpp"
#include "blip/transport/serial_protocol.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_now.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace blip::transport {

class EspEspNowTransportComponent final : public core::Component {
  public:
    static constexpr std::size_t kWorkerStackBytes = 6144;

    EspEspNowTransportComponent(core::ControlService& controls,
                                storage::SettingsStore& settings,
                                network::EspWifiComponent& wifi) noexcept;

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
    void enable_control() noexcept;

  private:
    struct ReceivedPacket {
        std::array<std::uint8_t, 6> source{};
        std::uint16_t size{};
        std::array<std::byte, kEspNowPacketBytes> bytes{};
    };

    [[nodiscard]] core::Status load_settings() noexcept;
    [[nodiscard]] core::Status save_settings(bool enabled, std::uint64_t peer) noexcept;
    [[nodiscard]] bool activate() noexcept;
    void deactivate() noexcept;
    void run() noexcept;
    void handle_packet(const ReceivedPacket& packet) noexcept;
    void handle_message() noexcept;
    void begin_transmit(std::span<const std::byte> envelope, bool response) noexcept;
    void send_fragments() noexcept;
    void complete_request(bool success, const core::ScalarValue& value = {}) noexcept;

    static void worker_entry(void* context) noexcept;
    static void receive_callback(void* context, const esp_now_recv_info_t* info,
                                 const std::uint8_t* data, int length) noexcept;

    static const core::ComponentDescriptor descriptor_;
    static EspEspNowTransportComponent* instance_;

    storage::SettingsStore* settings_{};
    network::EspWifiComponent* wifi_{};
    std::array<std::byte, kMaxEnvelopeBytes> request_frame_{};
    std::array<std::byte, kMaxEnvelopeBytes> response_frame_{};
    std::array<std::byte, kMaxControlPayloadBytes> payload_buffer_{};
    std::array<std::byte, kMaxControlPayloadBytes> send_payload_{};
    std::array<std::byte, kEspNowPacketBytes> send_packet_{};
    std::array<char, kMaxControlStringBytes + 1U> response_text_{};
    std::array<char, 18> address_text_{};
    std::array<char, 18> peer_text_{};
    EspNowReassemble receive_{};
    EspNowTransmit transmit_{};
    ControlEnvelopeEndpoint endpoint_;
    QueueHandle_t receive_queue_{};
    SemaphoreHandle_t request_done_{};
    TaskHandle_t worker_task_{};
    core::ScalarValue remote_value_{};
    std::uint32_t session_{};
    std::uint32_t next_sequence_{};
    std::uint32_t request_id_{};
    std::uint32_t transmit_sequence_{};
    std::uint32_t transmit_started_ms_{};
    std::uint64_t active_peer_{};
    std::size_t request_size_{};
    std::size_t response_size_{};
    std::uint8_t attempts_{};
    bool transmit_is_response_{};
    bool response_waiting_{};
    bool request_expects_value_{};
    std::atomic<std::uint64_t> peer_packed_{};
    std::atomic<bool> started_{};
    std::atomic<bool> quiesced_{true};
    std::atomic<bool> active_{};
    std::atomic<bool> enabled_{};
    std::atomic<bool> control_ready_{};
    std::atomic<bool> reconfigure_{};
    std::atomic<bool> request_busy_{};
    std::atomic<bool> request_pending_{};
    std::atomic<bool> request_cancel_{};
    std::atomic<bool> request_success_{};
    std::atomic<std::uint32_t> received_requests_{};
    std::atomic<std::uint32_t> sent_responses_{};
    std::atomic<std::uint32_t> rejected_packets_{};
    std::atomic<std::uint32_t> retries_{};
};

} // namespace blip::transport

static_assert(sizeof(blip::transport::EspEspNowTransportComponent) <= 8192,
              "ESP-NOW component exceeds its static RAM budget");
