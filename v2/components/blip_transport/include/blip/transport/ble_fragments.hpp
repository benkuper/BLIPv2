#pragma once

#include "blip/transport/envelope.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace blip::transport {

inline constexpr std::size_t kBleChunkHeaderBytes = 4;
inline constexpr std::size_t kMaxBleChunkBytes = 244;

// Each GATT value carries flags, frame ID, little-endian offset, and raw
// envelope bytes. Writes and indications supply link-level acknowledgements.
class BleRequestAssembler {
  public:
    [[nodiscard]] core::Result<bool> append(std::span<const std::byte> chunk) noexcept;
    [[nodiscard]] std::span<const std::byte> frame() const noexcept {
        return {buffer_.data(), complete_ ? size_ : 0U};
    }
    [[nodiscard]] std::uint8_t frame_id() const noexcept { return frame_id_; }
    void reset() noexcept;

  private:
    std::array<std::byte, kMaxEnvelopeBytes> buffer_{};
    std::size_t size_{};
    std::uint8_t frame_id_{};
    bool active_{};
    bool complete_{};
};

class BleResponseFragments {
  public:
    [[nodiscard]] bool begin(std::span<const std::byte> frame, std::uint8_t frame_id) noexcept;
    [[nodiscard]] core::Result<std::size_t> next(std::span<std::byte> output) noexcept;
    [[nodiscard]] bool acknowledge() noexcept;
    [[nodiscard]] bool complete() const noexcept { return active_ && offset_ == frame_.size(); }
    void reset() noexcept;

  private:
    std::span<const std::byte> frame_{};
    std::size_t offset_{};
    std::size_t pending_size_{};
    std::uint8_t frame_id_{};
    bool active_{};
};

} // namespace blip::transport
