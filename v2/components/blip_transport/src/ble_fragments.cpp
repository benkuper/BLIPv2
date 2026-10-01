#include "blip/transport/ble_fragments.hpp"

#include <algorithm>
#include <cstring>
#include <string_view>

namespace blip::transport {
namespace {

constexpr std::uint8_t kStart = 1U;
constexpr std::uint8_t kEnd = 2U;

[[nodiscard]] core::Error chunk_error(core::ErrorCode code, std::string_view operation,
                                      std::string_view detail) noexcept {
    return {core::ErrorDomain::transport, code, "blip.transport.ble", operation, detail};
}

[[nodiscard]] std::uint16_t offset_of(std::span<const std::byte> chunk) noexcept {
    return static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(chunk[2])) |
           static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(chunk[3])) << 8U;
}

} // namespace

void BleRequestAssembler::reset() noexcept {
    size_ = 0;
    active_ = false;
    complete_ = false;
}

core::Result<bool> BleRequestAssembler::append(std::span<const std::byte> chunk) noexcept {
    if (chunk.size() <= kBleChunkHeaderBytes || chunk.size() > kMaxBleChunkBytes) {
        reset();
        return core::Result<bool>::failure(
            chunk_error(core::ErrorCode::invalid_argument, "append", "chunk-size"));
    }
    const auto flags = std::to_integer<std::uint8_t>(chunk[0]);
    const auto frame_id = std::to_integer<std::uint8_t>(chunk[1]);
    const auto offset = offset_of(chunk);
    if ((flags & ~(kStart | kEnd)) != 0U) {
        reset();
        return core::Result<bool>::failure(
            chunk_error(core::ErrorCode::invalid_argument, "append", "chunk-flags"));
    }
    if ((flags & kStart) != 0U) {
        reset();
        if (offset != 0U) {
            return core::Result<bool>::failure(
                chunk_error(core::ErrorCode::invalid_argument, "append", "start-offset"));
        }
        active_ = true;
        frame_id_ = frame_id;
    }
    const std::size_t payload_size = chunk.size() - kBleChunkHeaderBytes;
    if (!active_ || complete_ || frame_id != frame_id_ || offset != size_ ||
        payload_size > buffer_.size() - size_) {
        reset();
        return core::Result<bool>::failure(
            chunk_error(core::ErrorCode::corrupt_data, "append", "sequence-or-overflow"));
    }
    std::memcpy(buffer_.data() + size_, chunk.data() + kBleChunkHeaderBytes, payload_size);
    size_ += payload_size;
    complete_ = (flags & kEnd) != 0U;
    return core::Result<bool>::success(complete_);
}

bool BleResponseFragments::begin(std::span<const std::byte> frame,
                                 std::uint8_t frame_id) noexcept {
    reset();
    if (frame.empty() || frame.size() > kMaxEnvelopeBytes) {
        return false;
    }
    frame_ = frame;
    frame_id_ = frame_id;
    active_ = true;
    return true;
}

core::Result<std::size_t> BleResponseFragments::next(std::span<std::byte> output) noexcept {
    if (!active_ || offset_ == frame_.size() || pending_size_ != 0U ||
        output.size() <= kBleChunkHeaderBytes) {
        return core::Result<std::size_t>::failure(
            chunk_error(core::ErrorCode::invalid_state, "next", "not-ready"));
    }
    const std::size_t capacity = std::min(output.size(), kMaxBleChunkBytes) -
                                 kBleChunkHeaderBytes;
    pending_size_ = std::min(capacity, frame_.size() - offset_);
    const bool last = offset_ + pending_size_ == frame_.size();
    output[0] = static_cast<std::byte>((offset_ == 0U ? kStart : 0U) | (last ? kEnd : 0U));
    output[1] = static_cast<std::byte>(frame_id_);
    output[2] = static_cast<std::byte>(offset_ & 0xffU);
    output[3] = static_cast<std::byte>((offset_ >> 8U) & 0xffU);
    std::memcpy(output.data() + kBleChunkHeaderBytes, frame_.data() + offset_, pending_size_);
    return core::Result<std::size_t>::success(kBleChunkHeaderBytes + pending_size_);
}

bool BleResponseFragments::acknowledge() noexcept {
    if (!active_ || pending_size_ == 0U) {
        return false;
    }
    offset_ += pending_size_;
    pending_size_ = 0U;
    return true;
}

void BleResponseFragments::reset() noexcept {
    frame_ = {};
    offset_ = 0U;
    pending_size_ = 0U;
    active_ = false;
}

} // namespace blip::transport
