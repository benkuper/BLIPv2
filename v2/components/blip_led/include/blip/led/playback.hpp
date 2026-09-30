#pragma once

#include "blip/led/engine.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace blip::led {

inline constexpr std::size_t kPlaybackHeaderBytes = 32U;
inline constexpr std::uint16_t kPlaybackVersion = 1U;

struct PlaybackInfo {
    std::uint16_t pixel_count{};
    std::uint32_t frame_count{};
    std::uint32_t fps_milli{};
    PixelFormat format{PixelFormat::rgba16};
    std::uint32_t data_bytes{};
};

class PlaybackReader {
  public:
    [[nodiscard]] core::Status open(std::span<const std::byte> file) noexcept;
    [[nodiscard]] core::Status read_frame(std::uint32_t index,
                                          std::span<LinearPixel> output) const noexcept;
    [[nodiscard]] const PlaybackInfo& info() const noexcept { return info_; }
    [[nodiscard]] bool open() const noexcept { return !file_.empty(); }

  private:
    std::span<const std::byte> file_{};
    PlaybackInfo info_{};
};

[[nodiscard]] core::Result<std::size_t>
import_legacy_playback(std::string_view metadata_json, std::span<const std::byte> argb_frames,
                       std::uint16_t pixel_count, std::span<std::byte> output) noexcept;

enum class PlaybackState : std::uint8_t { stopped, playing, paused };
class PlaybackClock {
  public:
    void play(std::uint64_t now_us, std::uint32_t frame, bool loop) noexcept;
    void pause(std::uint64_t now_us, std::uint32_t fps_milli) noexcept;
    void resume(std::uint64_t now_us) noexcept;
    void stop() noexcept;
    [[nodiscard]] core::Status seek(std::uint32_t frame, std::uint32_t frame_count,
                                    std::uint64_t now_us) noexcept;
    [[nodiscard]] std::uint32_t frame(std::uint64_t now_us, std::uint32_t fps_milli,
                                      std::uint32_t frame_count) noexcept;
    [[nodiscard]] PlaybackState state() const noexcept { return state_; }

  private:
    PlaybackState state_{PlaybackState::stopped};
    std::uint64_t origin_us_{};
    std::uint64_t paused_elapsed_us_{};
    std::uint32_t origin_frame_{};
    bool loop_{};
};

} // namespace blip::led
