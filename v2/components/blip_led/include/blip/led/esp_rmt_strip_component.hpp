#pragma once

#include "blip/core/component.hpp"
#include "blip/led/strip_config.hpp"
#include "blip/led/current_limiter.hpp"
#include "blip/led/stream.hpp"
#include "blip/pm/esp_power_manager_component.hpp"
#if defined(BLIP_BOARD_CREATORS_BALL_V2)
#include "blip/led/esp_spi_dma_output_driver.hpp"
#endif
#include "blip/resources/device_broker.hpp"
#include "blip/storage/settings_store.hpp"
#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace blip::led {

class EspRmtStripComponent final : public core::Component {
  public:
    static constexpr std::size_t kTaskStackBytes = 6144U;
    static constexpr std::size_t kTaskStackWords = kTaskStackBytes / sizeof(StackType_t);

    EspRmtStripComponent(storage::SettingsStore& settings,
                         resources::DeviceBroker& resources,
                         pm::EspPowerManagerComponent& power_manager) noexcept;

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
    [[nodiscard]] core::Status ingest_stream(std::uint8_t sequence, std::uint16_t start_pixel,
                                             std::span<const std::byte> channels,
                                             std::uint8_t channels_per_pixel, bool sixteen_bit,
                                             std::uint64_t received_at_us) noexcept;

  private:
    [[nodiscard]] core::Status load_config() noexcept;
    [[nodiscard]] core::Status save_config(const StripConfig& config) noexcept;
    [[nodiscard]] core::Status apply_config_locked(const StripConfig& candidate) noexcept;
    [[nodiscard]] core::Result<resources::DeviceBroker::Lease>
    reserve_pin(std::uint8_t gpio) noexcept;
    void process_pending_config() noexcept;
    [[nodiscard]] core::Status initialize_output(const StripConfig& config) noexcept;
    void deinitialize_output() noexcept;
    [[nodiscard]] core::Status transmit(const StripConfig& config,
                                         std::size_t payload_size) noexcept;
    [[nodiscard]] core::Status limit_encoded(const StripConfig& config,
                                              std::span<std::byte> frame) noexcept;
#if !defined(BLIP_BOARD_CREATORS_BALL_V2)
    [[nodiscard]] core::Result<std::size_t> render_one_wire(const StripConfig& config) noexcept;
#endif
    [[nodiscard]] bool lock_config() noexcept;
    void unlock_config() noexcept;
    void run() noexcept;

    static void task_entry(void* context) noexcept;
    static const core::ComponentDescriptor descriptor_;

    storage::SettingsStore* settings_{};
    resources::DeviceBroker* resources_{};
    pm::EspPowerManagerComponent* power_manager_{};
    resources::DeviceBroker::Lease pin_lease_{};
    StripConfig config_{};
    StripConfig pending_config_{};
    std::array<std::byte, kStripSettingsBytes> settings_buffer_{};
#if defined(BLIP_BOARD_CREATORS_BALL_V2)
    static constexpr std::size_t kBoardPixelCount = 36U;
    static constexpr std::size_t kBoardFrameBytes = 16U + kBoardPixelCount * 8U + 8U;
    alignas(4) std::array<std::uint8_t, kBoardFrameBytes> pixel_buffer_{};
    std::array<LinearPixel, kBoardPixelCount> stream_pixels_{};
    EspSpiDmaOutputDriver spi_driver_{SPI2_HOST, 3, 2, kBoardFrameBytes};
    resources::DeviceBroker::Lease clock_lease_{};
    resources::DeviceBroker::Lease spi_lease_{};
#else
    std::array<std::uint8_t, kMaximumStripPixels * 4U> pixel_buffer_{};
    std::array<LinearPixel, kMaximumStripPixels> stream_pixels_{};
#endif
    StreamLayer stream_layer_{stream_pixels_};
    CurrentLimiter current_limiter_{};
    StaticSemaphore_t stream_mutex_storage_{};
    SemaphoreHandle_t stream_mutex_{};
    alignas(16) std::array<StackType_t, kTaskStackWords> task_stack_{};
    StaticTask_t task_storage_{};
    TaskHandle_t task_{};
    StaticSemaphore_t config_mutex_storage_{};
    SemaphoreHandle_t config_mutex_{};
    StaticSemaphore_t request_mutex_storage_{};
    SemaphoreHandle_t request_mutex_{};
    StaticSemaphore_t completion_storage_{};
    SemaphoreHandle_t completion_{};
    rmt_channel_handle_t channel_{};
    rmt_encoder_handle_t encoder_{};
    std::atomic<bool> started_{};
    std::atomic<bool> task_quiesced_{true};
    std::atomic<std::uint32_t> applied_frames_{};
    std::atomic<std::uint32_t> failed_frames_{};
    std::atomic<std::uint32_t> last_frame_us_{};
    std::atomic<std::uint32_t> estimated_current_ma_{};
    std::atomic<std::uint32_t> power_scale_q16_{65535U};
    std::uint32_t next_update_id_{};
    std::uint32_t pending_update_id_{};
    std::uint32_t completed_update_id_{};
    core::Status completed_update_status_{core::Status::success()};
    bool reconfigure_pending_{};
};

static_assert(sizeof(EspRmtStripComponent) <= 24576U,
              "RMT strip component exceeds its declared static RAM budget");

} // namespace blip::led
