#pragma once
#include "blip/core/error.hpp"
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace blip::wasm {
inline constexpr std::uint32_t kStringAbiVersion = 1;
inline constexpr std::size_t kMaximumStringBytes = 256;
struct StringRef { std::uint32_t offset{}, bytes{}; };

// Worker-confined checked copies. Implementations validate the whole range
// before writing and never return a native guest-memory pointer.
class GuestMemory {
  public:
    virtual ~GuestMemory() = default;
    virtual core::Status read_memory(std::uint32_t offset, std::span<std::byte> output) noexcept = 0;
    virtual core::Status write_memory(std::uint32_t offset, std::span<const std::byte> input) noexcept = 0;
};
[[nodiscard]] bool valid_utf8(std::span<const std::byte> bytes) noexcept;
// On failure, output is unchanged and count is zero. No terminator is written.
[[nodiscard]] core::Status read_utf8(GuestMemory&, StringRef, std::span<char> output, std::size_t& count) noexcept;
// Complete validation precedes any write; no terminator is written.
[[nodiscard]] core::Status write_utf8(GuestMemory&, std::uint32_t offset, std::string_view input) noexcept;
} // namespace blip::wasm
