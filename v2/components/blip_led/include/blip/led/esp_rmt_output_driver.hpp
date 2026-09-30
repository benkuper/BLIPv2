#pragma once

#include "blip/led/engine.hpp"
#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"

#include <atomic>

namespace blip::led {

// Production RMT adapter. EncodedFrame bytes are protocol bytes (GRB/GRBW);
// ESP-IDF's bytes encoder generates the waveform without an expansion buffer.
class EspRmtOutputDriver final : public OutputDriver {
  public:
    explicit EspRmtOutputDriver(int gpio, bool use_dma = false) noexcept
        : gpio_(gpio), use_dma_(use_dma) {}
    [[nodiscard]] const DriverCapabilities& capabilities() const noexcept override;
    [[nodiscard]] core::Status start(const OutputConfig& config) noexcept override;
    [[nodiscard]] core::Status submit(const EncodedFrame& frame) noexcept override;
    [[nodiscard]] Completion poll() noexcept override;
    [[nodiscard]] core::Status stop() noexcept override;

  private:
    static bool on_done(rmt_channel_handle_t, const rmt_tx_done_event_data_t*, void*) noexcept;
    int gpio_{};
    bool use_dma_{};
    rmt_channel_handle_t channel_{};
    rmt_encoder_handle_t encoder_{};
    std::atomic<std::uint64_t> active_sequence_{};
    std::atomic<bool> in_flight_{};
    std::atomic<bool> completion_pending_{};
};

} // namespace blip::led
