#pragma once

#include "blip/core/component.hpp"
#include "blip/core/control.hpp"
#include "blip/storage/settings_store.hpp"
#include "blip/transport/ble_fragments.hpp"
#include "blip/transport/serial_protocol.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace blip::transport {

class EspBleTransportComponent final : public core::Component {
  public:
    static constexpr std::size_t kWorkerStackBytes = 4096;
    static constexpr std::size_t kWorkerStackWords = kWorkerStackBytes / sizeof(StackType_t);

    EspBleTransportComponent(core::ControlService& controls,
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
    [[nodiscard]] bool initialize_nimble() noexcept;
    void deinitialize_nimble() noexcept;
    void advertise() noexcept;
    void dispatch_request() noexcept;
    void send_next_fragment() noexcept;
    void run() noexcept;
    void on_gap_event(const ble_gap_event& event) noexcept;
    [[nodiscard]] int on_gatt_access(std::uint16_t connection,
                                     ble_gatt_access_ctxt& context) noexcept;

    static void worker_entry(void* context) noexcept;
    static void host_entry(void* context) noexcept;
    static void sync_callback() noexcept;
    static void reset_callback(int reason) noexcept;
    static int gap_callback(ble_gap_event* event, void* context) noexcept;
    static int gatt_callback(std::uint16_t connection, std::uint16_t attribute,
                             ble_gatt_access_ctxt* context, void* argument) noexcept;

    static const core::ComponentDescriptor descriptor_;
    static EspBleTransportComponent* instance_;

    core::ControlService* controls_{};
    storage::SettingsStore* settings_{};
    std::array<std::byte, kMaxEnvelopeBytes> request_buffer_{};
    std::array<std::byte, kMaxEnvelopeBytes> response_buffer_{};
    std::array<std::byte, kMaxControlPayloadBytes> payload_buffer_{};
    BleRequestAssembler request_assembler_{};
    BleResponseFragments response_fragments_{};
    ControlEnvelopeEndpoint endpoint_;
    alignas(16) std::array<StackType_t, kWorkerStackWords> worker_stack_{};
    StaticTask_t worker_task_storage_{};
    TaskHandle_t worker_task_{};
    std::size_t request_size_{};
    std::uint32_t request_epoch_{};
    std::uint32_t response_epoch_{};
    std::uint8_t request_frame_id_{};
    std::uint8_t own_address_type_{};
    std::uint16_t tx_value_handle_{};
    std::atomic<bool> waiting_for_ack_{};
    std::atomic<bool> started_{};
    std::atomic<bool> quiesced_{true};
    std::atomic<bool> desired_enabled_{};
    std::atomic<bool> active_{};
    std::atomic<bool> host_running_{};
    std::atomic<bool> synced_{};
    std::atomic<bool> control_ready_{};
    std::atomic<bool> advertising_{};
    std::atomic<bool> subscribed_{};
    std::atomic<bool> busy_{};
    std::atomic<bool> request_pending_{};
    std::atomic<bool> ack_pending_{};
    std::atomic<int> ack_status_{};
    std::atomic<std::uint16_t> connection_handle_{0xffffU};
    std::atomic<std::uint32_t> connection_epoch_{};
    std::atomic<std::uint32_t> received_requests_{};
    std::atomic<std::uint32_t> sent_responses_{};
    std::atomic<std::uint32_t> rejected_chunks_{};
};

} // namespace blip::transport

static_assert(sizeof(blip::transport::EspBleTransportComponent) <= 8192,
              "BLE transport exceeds its declared static RAM budget");
