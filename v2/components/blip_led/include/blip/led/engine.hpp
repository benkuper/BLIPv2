#pragma once

#include "blip/core/error.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace blip::led {

inline constexpr std::size_t kMaximumPixels = 1024U;
inline constexpr std::size_t kMaximumLanes = 16U;

// Surfaces are always linear-light. Alpha is straight (not premultiplied).
struct LinearPixel {
    std::uint16_t red{};
    std::uint16_t green{};
    std::uint16_t blue{};
    std::uint16_t white{};
    std::uint16_t alpha{65535U};
    [[nodiscard]] bool operator==(const LinearPixel&) const noexcept = default;
};

enum class PixelFormat : std::uint8_t { rgb8, rgbw8, rgb16, rgbw16, rgba16 };
enum class PixelProtocol : std::uint8_t { ws2812, sk6812, apa102, sk9822, hd108 };
enum class BufferingMode : std::uint8_t { synchronous, single, double_buffered, queued };
enum class DmaMode : std::uint8_t { none, optional, required };
enum class SynchronizationMode : std::uint8_t { none, per_lane, simultaneous_lanes };

struct PixelSurface {
    std::span<LinearPixel> pixels{};
    std::uint16_t lane_count{1U};
    std::uint16_t pixels_per_lane{};

    [[nodiscard]] constexpr bool valid() const noexcept {
        return lane_count > 0U && lane_count <= kMaximumLanes && pixels_per_lane > 0U &&
               pixels_per_lane <= kMaximumPixels &&
               pixels.size() == static_cast<std::size_t>(lane_count) * pixels_per_lane;
    }
};

struct ConstPixelSurface {
    std::span<const LinearPixel> pixels{};
    std::uint16_t lane_count{1U};
    std::uint16_t pixels_per_lane{};

    [[nodiscard]] constexpr bool valid() const noexcept {
        return lane_count > 0U && lane_count <= kMaximumLanes && pixels_per_lane > 0U &&
               pixels_per_lane <= kMaximumPixels &&
               pixels.size() == static_cast<std::size_t>(lane_count) * pixels_per_lane;
    }
};

struct DriverCapabilities {
    std::string_view id{};
    std::span<const PixelProtocol> protocols{};
    std::span<const PixelFormat> formats{};
    std::uint16_t minimum_lanes{1U};
    std::uint16_t maximum_lanes{1U};
    std::uint32_t maximum_pixels_per_lane{};
    std::uint32_t minimum_clock_hz{};
    std::uint32_t maximum_clock_hz{};
    std::uint32_t reset_time_us{};
    std::uint32_t staging_bytes_per_pixel{};
    std::uint32_t fixed_staging_bytes{};
    std::uint32_t dma_alignment{1U};
    BufferingMode buffering{BufferingMode::synchronous};
    DmaMode dma{DmaMode::none};
    SynchronizationMode synchronization{SynchronizationMode::none};
    bool qualified{};
    std::string_view qualification{};
    std::span<const std::string_view> claimed_resources{};

    [[nodiscard]] bool supports(PixelProtocol protocol) const noexcept;
    [[nodiscard]] std::uint64_t physical_frame_period_us(PixelProtocol protocol,
                                                         std::uint32_t pixels,
                                                         std::uint32_t clock_hz) const noexcept;
    [[nodiscard]] std::size_t required_staging_bytes(std::size_t pixels) const noexcept;
};

struct OutputConfig {
    PixelProtocol protocol{PixelProtocol::ws2812};
    PixelFormat format{PixelFormat::rgb8};
    std::uint16_t lanes{1U};
    std::uint16_t pixels_per_lane{1U};
    std::uint32_t clock_hz{};
};

struct EncodedFrame {
    std::span<const std::byte> bytes{};
    std::uint64_t sequence{};
    std::uint64_t deadline_us{};
};

enum class CompletionState : std::uint8_t { pending, complete, failed };
struct Completion {
    CompletionState state{CompletionState::pending};
    std::uint64_t sequence{};
    std::uint64_t completed_at_us{};
};

class OutputDriver {
  public:
    virtual ~OutputDriver() = default;
    [[nodiscard]] virtual const DriverCapabilities& capabilities() const noexcept = 0;
    [[nodiscard]] virtual core::Status start(const OutputConfig& config) noexcept = 0;
    [[nodiscard]] virtual core::Status submit(const EncodedFrame& frame) noexcept = 0;
    [[nodiscard]] virtual Completion poll() noexcept = 0;
    [[nodiscard]] virtual core::Status stop() noexcept = 0;
};

[[nodiscard]] core::Status validate_output_config(const DriverCapabilities& capabilities,
                                                  const OutputConfig& config) noexcept;

} // namespace blip::led
