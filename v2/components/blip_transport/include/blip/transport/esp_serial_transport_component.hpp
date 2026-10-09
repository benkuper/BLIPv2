#pragma once

#include "blip/core/component.hpp"
#include "blip/core/control.hpp"
#include "blip/transport/serial_protocol.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace blip::transport {

class EspSerialTransportComponent final : public core::Component {
  public:
    static constexpr std::size_t kTaskStackBytes = 8192;
    static constexpr std::size_t kTaskStackWords = kTaskStackBytes / sizeof(StackType_t);

    explicit EspSerialTransportComponent(core::ControlService& controls) noexcept;

    [[nodiscard]] const core::ComponentDescriptor& descriptor() const noexcept override;
    [[nodiscard]] core::Status start(const core::StartContext&) noexcept override;
    [[nodiscard]] core::Status stop() noexcept override;
    [[nodiscard]] bool callbacks_quiesced() const noexcept override { return quiesced_.load(); }
    [[nodiscard]] core::Status read_parameter(std::string_view, core::ScalarValue&) noexcept override;

    void enable_control() noexcept { control_enabled_.store(true); }
    [[nodiscard]] std::uint32_t received_frames() const noexcept { return received_frames_.load(); }
    [[nodiscard]] std::uint32_t transmitted_frames() const noexcept {
        return transmitted_frames_.load();
    }
    [[nodiscard]] std::uint32_t rejected_frames() const noexcept { return rejected_frames_.load(); }
    [[nodiscard]] std::uint32_t overflow_frames() const noexcept { return overflow_frames_.load(); }

  private:
    [[nodiscard]] core::Status install_driver() noexcept;
    [[nodiscard]] core::Status uninstall_driver() noexcept;
    [[nodiscard]] int read_bytes(std::span<std::byte> output) noexcept;
    [[nodiscard]] bool write_bytes(std::span<const std::byte> input) noexcept;
    void run() noexcept;
    void process_bytes(std::span<const std::byte> input) noexcept;
    static void task_entry(void* context) noexcept;

    static const core::ComponentDescriptor descriptor_;

    core::ControlService* controls_{};
    std::array<std::byte, kMaxSerialFrameBytes> incoming_frame_{};
    std::array<std::byte, kMaxSerialFrameBytes> decode_buffer_{};
    std::array<std::byte, kMaxEnvelopeBytes> envelope_buffer_{};
    std::array<std::byte, kMaxControlPayloadBytes> payload_buffer_{};
    std::array<std::byte, kMaxSerialFrameBytes + 1U> response_frame_{};
    SerialControlEndpoint endpoint_;
    alignas(16) std::array<StackType_t, kTaskStackWords> task_stack_{};
    StaticTask_t task_storage_{};
    TaskHandle_t task_handle_{};
    std::size_t incoming_size_{};
    bool discard_until_delimiter_{};
    bool driver_owned_{};
    std::atomic<bool> running_{};
    std::atomic<bool> control_enabled_{};
    std::atomic<bool> quiesced_{true};
    std::atomic<std::uint32_t> received_frames_{};
    std::atomic<std::uint32_t> transmitted_frames_{};
    std::atomic<std::uint32_t> rejected_frames_{};
    std::atomic<std::uint32_t> overflow_frames_{};
};

static_assert(sizeof(EspSerialTransportComponent) <= 12288,
              "serial transport exceeds its declared static RAM budget");

} // namespace blip::transport
