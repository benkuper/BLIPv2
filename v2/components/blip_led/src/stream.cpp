#include "blip/led/stream.hpp"

#include <algorithm>

namespace blip::led {
namespace {
[[nodiscard]] bool newer(std::uint8_t incoming, std::uint8_t previous) noexcept {
    if (incoming == 0U || previous == 0U)
        return true;
    const auto delta = static_cast<std::uint8_t>(incoming - previous);
    return delta != 0U && delta < 128U;
}
[[nodiscard]] std::uint16_t read_channel(std::span<const std::byte> data, std::size_t offset,
                                         bool sixteen_bit) noexcept {
    if (!sixteen_bit)
        return static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(data[offset])) * 257U;
    return static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(data[offset])) << 8U |
           std::to_integer<std::uint8_t>(data[offset + 1U]);
}
} // namespace

core::Status StreamLayer::ingest(std::uint8_t sequence, std::uint16_t start_pixel,
                                 std::span<const std::byte> channels,
                                 std::uint8_t channels_per_pixel, bool sixteen_bit,
                                 std::uint64_t received_at_us) noexcept {
    const auto width = static_cast<std::size_t>(channels_per_pixel) * (sixteen_bit ? 2U : 1U);
    if ((channels_per_pixel != 3U && channels_per_pixel != 4U) || width == 0U || channels.empty() ||
        channels.size() % width != 0U || start_pixel >= pixels_.size() ||
        channels.size() / width > pixels_.size() - start_pixel) {
        ++metrics_.malformed;
        return core::Status::failure({core::ErrorDomain::transport,
                                      core::ErrorCode::validation_failed, "blip.led.stream",
                                      "ingest", "invalid-channel-range"});
    }
    if (have_sequence_ && !newer(sequence, last_sequence_)) {
        ++metrics_.stale;
        return core::Status::failure({core::ErrorDomain::transport, core::ErrorCode::cancelled,
                                      "blip.led.stream", "ingest", "stale-sequence"});
    }
    for (std::size_t pixel = 0; pixel < channels.size() / width; ++pixel) {
        const auto base = pixel * width;
        auto& output = pixels_[start_pixel + pixel];
        const auto stride = sixteen_bit ? 2U : 1U;
        output.red = read_channel(channels, base, sixteen_bit);
        output.green = read_channel(channels, base + stride, sixteen_bit);
        output.blue = read_channel(channels, base + stride * 2U, sixteen_bit);
        if (channels_per_pixel == 4U)
            output.alpha = read_channel(channels, base + stride * 3U, sixteen_bit);
        else
            output.alpha = 65535U;
    }
    if (sequence != 0U) {
        last_sequence_ = sequence;
        have_sequence_ = true;
    }
    last_reception_us_ = received_at_us;
    active_ = true;
    ++metrics_.accepted;
    return core::Status::success();
}

void StreamLayer::expire(std::uint64_t now_us, std::uint64_t timeout_us,
                         bool clear_on_timeout) noexcept {
    if (!active_ || timeout_us == 0U || now_us - last_reception_us_ < timeout_us)
        return;
    active_ = false;
    have_sequence_ = false;
    ++metrics_.timeouts;
    if (clear_on_timeout) {
        std::fill(pixels_.begin(), pixels_.end(), LinearPixel{0U, 0U, 0U, 0U, 0U});
    }
}

void StreamLayer::clear() noexcept {
    active_ = false;
    have_sequence_ = false;
    std::fill(pixels_.begin(), pixels_.end(), LinearPixel{0U, 0U, 0U, 0U, 0U});
}

} // namespace blip::led
