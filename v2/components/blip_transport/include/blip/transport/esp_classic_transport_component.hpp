#pragma once

#include "blip/core/component.hpp"
#include "blip/core/control.hpp"
#include "blip/storage/settings_store.hpp"
#include "blip/transport/serial_protocol.hpp"
#include "esp_gap_bt_api.h"
#include "esp_spp_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace blip::transport {

class EspClassicTransportComponent final : public core::Component {
  public:
    static constexpr std::size_t kWorkerStackBytes = 8192;

    EspClassicTransportComponent(core::ControlService& controls,
                                 storage::SettingsStore& settings) noexcept;

    [[nodiscard]] const core::ComponentDescriptor& descriptor() const noexcept override;
    [[nodiscard]] core::Status start(const core::StartContext&) noexcept override;
    [[nodiscard]] core::Status stop() noexcept override;
    [[nodiscard]] bool callbacks_quiesced() const noexcept override;
    [[nodiscard]] core::Status read_parameter(std::string_view id,
                                              core::ScalarValue& output) noexcept override;
    [[nodiscard]] core::Status write_parameter(std::string_view id,
                                               const core::ScalarValue& value) noexcept override;
    void enable_control() noexcept;

  private:
    [[nodiscard]] core::Status load_enabled() noexcept;
    [[nodiscard]] core::Status save_enabled(bool enabled) noexcept;
    [[nodiscard]] bool initialize_stack() noexcept;
    void deinitialize_stack() noexcept;
    void process_bytes(std::span<const std::byte> input) noexcept;
    [[nodiscard]] bool write_response(std::size_t size) noexcept;
    void run() noexcept;
    void on_spp_event(esp_spp_cb_event_t event, esp_spp_cb_param_t* param) noexcept;
    void on_gap_event(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param) noexcept;

    static void worker_entry(void* context) noexcept;
    static void spp_callback(esp_spp_cb_event_t event, esp_spp_cb_param_t* param) noexcept;
    static void gap_callback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param) noexcept;

    static const core::ComponentDescriptor descriptor_;
    static EspClassicTransportComponent* instance_;

    storage::SettingsStore* settings_{};
    std::array<std::byte, kMaxSerialFrameBytes> incoming_frame_{};
    std::array<std::byte, kMaxSerialFrameBytes> decode_buffer_{};
    std::array<std::byte, kMaxEnvelopeBytes> envelope_buffer_{};
    std::array<std::byte, kMaxControlPayloadBytes> payload_buffer_{};
    std::array<std::byte, kMaxSerialFrameBytes + 1U> response_frame_{};
    std::array<char, 18> address_text_{};
    SerialControlEndpoint endpoint_;
    StreamBufferHandle_t receive_stream_{};
    TaskHandle_t worker_task_{};
    std::size_t incoming_size_{};
    bool discard_until_delimiter_{};
    bool ble_memory_released_{};
    bool controller_initialized_{};
    bool controller_enabled_{};
    bool bluedroid_initialized_{};
    bool bluedroid_enabled_{};
    bool spp_initialized_{};
    std::atomic<bool> started_{};
    std::atomic<bool> quiesced_{true};
    std::atomic<bool> desired_enabled_{};
    std::atomic<bool> control_ready_{};
    std::atomic<bool> active_{};
    std::atomic<bool> discoverable_{};
    std::atomic<bool> stack_started_{};
    std::atomic<bool> spp_uninitialized_{};
    std::atomic<bool> input_overflow_{};
    std::atomic<bool> write_complete_{};
    std::atomic<bool> write_ok_{};
    std::atomic<bool> write_congested_{};
    std::atomic<std::uint32_t> connection_handle_{};
    std::atomic<std::uint32_t> received_requests_{};
    std::atomic<std::uint32_t> sent_responses_{};
    std::atomic<std::uint32_t> rejected_frames_{};
    std::atomic<std::uint32_t> overflow_frames_{};
};

static_assert(sizeof(EspClassicTransportComponent) <= 4096,
              "Classic transport exceeds its static RAM budget");

} // namespace blip::transport
