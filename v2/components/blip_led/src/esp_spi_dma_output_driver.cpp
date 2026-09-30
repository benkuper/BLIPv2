#include "blip/led/esp_spi_dma_output_driver.hpp"

#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include <array>

namespace blip::led {
namespace {
constexpr std::array kProtocols{PixelProtocol::ws2812, PixelProtocol::sk6812, PixelProtocol::apa102,
                                PixelProtocol::sk9822, PixelProtocol::hd108};
constexpr std::array kFormats{PixelFormat::rgb8, PixelFormat::rgbw8, PixelFormat::rgb16};
constexpr std::array<std::string_view, 4> kResources{"spi.host", "dma.channel", "gpio.data",
                                                     "gpio.clock"};
constexpr DriverCapabilities kCapabilities{"esp-spi-dma",
                                           kProtocols,
                                           kFormats,
                                           1U,
                                           1U,
                                           1024U,
                                           1'000'000U,
                                           40'000'000U,
                                           80U,
                                           9U,
                                           24U,
                                           4U,
                                           BufferingMode::queued,
                                           DmaMode::required,
                                           SynchronizationMode::per_lane,
                                           false,
                                           "pending-Gate-C-capture",
                                           kResources};
[[nodiscard]] core::Status fail(core::ErrorCode code, std::string_view op,
                                std::string_view why) noexcept {
    return core::Status::failure({core::ErrorDomain::transport, code, "blip.led.spi-dma", op, why});
}
} // namespace

const DriverCapabilities& EspSpiDmaOutputDriver::capabilities() const noexcept {
    return kCapabilities;
}

core::Status EspSpiDmaOutputDriver::start(const OutputConfig& config) noexcept {
    if (device_ != nullptr || maximum_transfer_bytes_ == 0U ||
        !validate_output_config(capabilities(), config))
        return fail(core::ErrorCode::validation_failed, "start", "invalid-config-or-state");
    spi_bus_config_t bus{};
    bus.mosi_io_num = data_gpio_;
    bus.miso_io_num = -1;
    bus.sclk_io_num = clock_gpio_;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = static_cast<int>(maximum_transfer_bytes_);
    if (spi_bus_initialize(host_, &bus, SPI_DMA_CH_AUTO) != ESP_OK)
        return fail(core::ErrorCode::resource_conflict, "start", "spi-bus-or-dma-unavailable");
    owns_bus_ = true;
    spi_device_interface_config_t device{};
    device.clock_speed_hz = static_cast<int>(config.clock_hz == 0U ? 2'400'000U : config.clock_hz);
    device.mode = 0;
    device.spics_io_num = -1;
    device.queue_size = 1;
    if (spi_bus_add_device(host_, &device, &device_) != ESP_OK) {
        static_cast<void>(stop());
        return fail(core::ErrorCode::resource_unavailable, "start", "spi-device");
    }
    return core::Status::success();
}

core::Status EspSpiDmaOutputDriver::submit(const EncodedFrame& frame) noexcept {
    if (device_ == nullptr || frame.bytes.empty() || frame.bytes.size() > maximum_transfer_bytes_)
        return fail(core::ErrorCode::invalid_argument, "submit", "invalid-frame");
    if (in_flight_)
        return fail(core::ErrorCode::queue_full, "submit", "in-flight");
    if (!esp_ptr_dma_capable(frame.bytes.data()))
        return fail(core::ErrorCode::invalid_argument, "submit", "buffer-not-dma-capable");
    transaction_ = {};
    transaction_.length = frame.bytes.size() * 8U;
    transaction_.tx_buffer = frame.bytes.data();
    if (spi_device_queue_trans(device_, &transaction_, 0) != ESP_OK)
        return fail(core::ErrorCode::queue_full, "submit", "spi-queue");
    sequence_ = frame.sequence;
    in_flight_ = true;
    return core::Status::success();
}

Completion EspSpiDmaOutputDriver::poll() noexcept {
    if (!in_flight_)
        return {};
    spi_transaction_t* result{};
    const auto status = spi_device_get_trans_result(device_, &result, 0);
    if (status == ESP_ERR_TIMEOUT)
        return {};
    in_flight_ = false;
    return {status == ESP_OK && result == &transaction_ ? CompletionState::complete
                                                        : CompletionState::failed,
            sequence_, static_cast<std::uint64_t>(esp_timer_get_time())};
}

core::Status EspSpiDmaOutputDriver::stop() noexcept {
    if (device_ != nullptr) {
        if (in_flight_) {
            spi_transaction_t* result{};
            static_cast<void>(spi_device_get_trans_result(device_, &result, portMAX_DELAY));
        }
        static_cast<void>(spi_bus_remove_device(device_));
        device_ = nullptr;
    }
    if (owns_bus_) {
        static_cast<void>(spi_bus_free(host_));
        owns_bus_ = false;
    }
    in_flight_ = false;
    sequence_ = 0U;
    return core::Status::success();
}

} // namespace blip::led
