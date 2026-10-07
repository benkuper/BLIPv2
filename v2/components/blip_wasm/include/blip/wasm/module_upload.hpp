#pragma once
#include "blip/core/error.hpp"
#include <cstddef>
#include <cstdint>
#include <span>

namespace blip::wasm {
// Worker-owned, contiguous upload into fixed scratch. The caller must unload
// any instance using this scratch before begin. finish returns a borrowed view
// for immediate Service::load; no producer/transport pointer is retained.
class ModuleUpload final {
  public:
    explicit ModuleUpload(std::span<std::byte> storage) noexcept : storage_(storage) {}
    [[nodiscard]] core::Status begin(std::size_t bytes, std::uint32_t crc) noexcept;
    [[nodiscard]] core::Status append(std::size_t offset, std::span<const std::byte> bytes) noexcept;
    [[nodiscard]] core::Result<std::span<const std::byte>> finish() noexcept;
    void cancel() noexcept { expected_ = received_ = 0; }
    [[nodiscard]] std::size_t received() const noexcept { return received_; }
  private:
    std::span<std::byte> storage_;
    std::size_t expected_{}, received_{};
    std::uint32_t expected_crc_{}, crc_{0xffffffffU};
};
} // namespace blip::wasm
