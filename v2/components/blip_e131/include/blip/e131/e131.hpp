#pragma once
#include "blip/core/error.hpp"
#include "blip/led/stream.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <array>
namespace blip::e131 {
inline constexpr std::uint16_t kPort = 5568U;
struct Packet {
    std::uint16_t universe{};
    std::uint8_t priority{};
    std::uint8_t sequence{};
    std::uint8_t options{};
    std::uint16_t synchronization_address{};
    std::array<std::byte, 16> source_id{};
    std::span<const std::byte> dmx{};
};
[[nodiscard]] core::Result<Packet> parse(std::span<const std::byte> datagram) noexcept;
struct Mapping {
    std::uint16_t universe{1U};
    std::uint16_t start_channel{1U};
    std::uint16_t start_pixel{};
    std::uint8_t channels_per_pixel{3U};
    bool sixteen_bit{};
};
struct PixelUpdate {
    std::uint16_t start_pixel{};
    std::span<const std::byte> channels{};
    std::uint8_t channels_per_pixel{};
    bool sixteen_bit{};
};
[[nodiscard]] core::Result<PixelUpdate> map_dmx(std::span<const std::byte> dmx,
                                                 const Mapping& mapping,
                                                 std::size_t maximum_pixels) noexcept;

enum class SourceResult : std::uint8_t { accepted, terminated, ignored, stale, capacity };
class SourceMixer {
  public:
    static constexpr std::uint64_t kSourceTimeoutUs = 2'500'000U;
    static constexpr std::size_t kMaximumSources = 2U;

    [[nodiscard]] SourceResult ingest(const Packet& packet, std::uint64_t now_us) noexcept;
    [[nodiscard]] bool expire(std::uint64_t now_us) noexcept;
    [[nodiscard]] bool compose(std::span<std::byte> output, std::size_t& size,
                               std::size_t& active_sources) const noexcept;

  private:
    struct Source {
        std::array<std::byte, 16> id{};
        std::array<std::byte, 512> dmx{};
        std::size_t size{};
        std::uint64_t last_seen_us{};
        std::uint8_t priority{};
        std::uint8_t sequence{};
        bool active{};
    };
    std::array<Source, kMaximumSources> sources_{};
};

[[nodiscard]] core::Status ingest(std::span<const std::byte> datagram, const Mapping& mapping,
                                  led::StreamLayer& stream, std::uint64_t received_at_us) noexcept;
} // namespace blip::e131
