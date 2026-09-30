#pragma once

#include "blip/led/engine.hpp"
#include "driver/spi_master.h"

namespace blip::led {

// Native asynchronous SPI-DMA adapter used both for APA102/SK9822/HD108
// frames and for pre-expanded one-wire frames.
class EspSpiDmaOutputDriver final : public OutputDriver {
  public:
    EspSpiDmaOutputDriver(spi_host_device_t host, int data_gpio, int clock_gpio,
                          std::size_t maximum_transfer_bytes) noexcept
        : host_(host), data_gpio_(data_gpio), clock_gpio_(clock_gpio),
          maximum_transfer_bytes_(maximum_transfer_bytes) {}
    [[nodiscard]] const DriverCapabilities& capabilities() const noexcept override;
    [[nodiscard]] core::Status start(const OutputConfig& config) noexcept override;
    [[nodiscard]] core::Status submit(const EncodedFrame& frame) noexcept override;
    [[nodiscard]] Completion poll() noexcept override;
    [[nodiscard]] core::Status stop() noexcept override;

  private:
    spi_host_device_t host_{};
    int data_gpio_{};
    int clock_gpio_{};
    std::size_t maximum_transfer_bytes_{};
    spi_device_handle_t device_{};
    spi_transaction_t transaction_{};
    std::uint64_t sequence_{};
    bool owns_bus_{};
    bool in_flight_{};
};

} // namespace blip::led
