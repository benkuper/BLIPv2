#pragma once
#include "blip/core/error.hpp"
#include "blip/led/stream.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
namespace blip::ddp {
inline constexpr std::uint16_t kPort = 4048U;
struct DiscoveryIdentity {
    std::array<std::uint8_t, 6> mac{};
    std::uint16_t pixels{};
};
// STATUS (251) and read-only CONFIG (250); unsupported IDs get an empty reply.
[[nodiscard]] core::Result<std::size_t> encode_query_reply(std::span<const std::byte> query,
                                                           const DiscoveryIdentity& identity,
                                                           std::span<std::byte> response) noexcept;
struct Packet {
    bool push{};
    std::uint8_t sequence{};
    std::uint8_t data_type{};
    std::uint8_t destination{};
    std::uint32_t offset{};
    std::span<const std::byte> data{};
};
[[nodiscard]] core::Result<Packet> parse(std::span<const std::byte> datagram) noexcept;
struct Mapping {
    std::uint8_t destination{1U};
    std::uint32_t base_offset{};
    std::uint16_t start_pixel{};
    std::uint8_t channels_per_pixel{3U};
    bool sixteen_bit{};
};
struct PixelUpdate {
    std::uint8_t sequence{};
    std::uint16_t start_pixel{};
    std::span<const std::byte> channels{};
    std::uint8_t channels_per_pixel{};
    bool sixteen_bit{};
};
class SequenceTracker {
  public:
    [[nodiscard]] bool accept(std::uint8_t sequence, std::uint64_t received_at_us) noexcept;
    void reset() noexcept { have_sequence_ = false; }

  private:
    std::uint8_t last_sequence_{};
    std::uint64_t last_reception_us_{};
    bool have_sequence_{};
};
[[nodiscard]] core::Result<PixelUpdate> map(std::span<const std::byte> datagram,
                                            const Mapping& mapping) noexcept;
[[nodiscard]] core::Status ingest(std::span<const std::byte> datagram, const Mapping& mapping,
                                  led::StreamLayer& stream, std::uint64_t received_at_us) noexcept;
} // namespace blip::ddp
