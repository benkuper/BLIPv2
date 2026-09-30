#pragma once

#include "blip/led/engine.hpp"

#include <cstdint>
#include <span>

namespace blip::led {

struct StreamMetrics {
    std::uint64_t accepted{};
    std::uint64_t stale{};
    std::uint64_t malformed{};
    std::uint64_t timeouts{};
};

class StreamLayer {
  public:
    explicit StreamLayer(std::span<LinearPixel> pixels) noexcept : pixels_(pixels) {}

    // Channels are RGB, RGBA, RGBW, or 16-bit big-endian variants as declared.
    [[nodiscard]] core::Status ingest(std::uint8_t sequence, std::uint16_t start_pixel,
                                      std::span<const std::byte> channels,
                                      std::uint8_t channels_per_pixel, bool sixteen_bit,
                                      std::uint64_t received_at_us) noexcept;
    void expire(std::uint64_t now_us, std::uint64_t timeout_us, bool clear_on_timeout) noexcept;
    void clear() noexcept;
    [[nodiscard]] std::span<const LinearPixel> pixels() const noexcept { return pixels_; }
    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] const StreamMetrics& metrics() const noexcept { return metrics_; }

  private:
    std::span<LinearPixel> pixels_{};
    StreamMetrics metrics_{};
    std::uint64_t last_reception_us_{};
    std::uint8_t last_sequence_{};
    bool have_sequence_{};
    bool active_{};
};

} // namespace blip::led
