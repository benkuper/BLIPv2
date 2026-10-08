#pragma once

#include "blip/core/error.hpp"
#include "blip/led/stream.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace blip::artnet {

inline constexpr std::uint16_t kPort = 6454U;
enum class PacketKind : std::uint8_t { poll, dmx, sync };
struct Packet {
    PacketKind kind{PacketKind::poll};
    std::uint16_t universe{};
    std::uint8_t sequence{};
    std::span<const std::byte> dmx{};
    bool notify_changes{};
    bool targeted{};
    std::uint16_t target_top{};
    std::uint16_t target_bottom{};
};

[[nodiscard]] bool poll_matches(const Packet& packet, std::uint16_t universe) noexcept;

[[nodiscard]] core::Result<Packet> parse(std::span<const std::byte> datagram) noexcept;

struct NodeIdentity {
    std::array<std::uint8_t, 4> ipv4{};
    std::array<std::uint8_t, 6> mac{};
    std::string_view short_name{"BLIP V2"};
    std::string_view long_name{"BLIP V2 Art-Net node"};
    std::uint16_t oem{0x7ff0U};
    std::uint16_t universe{};
    bool dhcp{};
    bool output_active{};
    std::uint16_t report_counter{};
};

inline constexpr std::size_t kPollReplyBytes = 239U;
[[nodiscard]] core::Result<std::size_t> encode_poll_reply(const NodeIdentity& identity,
                                                          std::span<std::byte> output) noexcept;

struct DmxMapping {
    std::uint16_t universe{};
    std::uint16_t start_channel{1U};
    std::uint16_t start_pixel{};
    std::uint8_t channels_per_pixel{3U};
    bool sixteen_bit{};
};
struct PixelUpdate {
    std::uint8_t sequence{};
    std::uint16_t start_pixel{};
    std::span<const std::byte> channels{};
    std::uint8_t channels_per_pixel{3U};
    bool sixteen_bit{};
};
[[nodiscard]] core::Result<PixelUpdate> map_dmx(const Packet& packet, const DmxMapping& mapping,
                                                std::size_t maximum_pixels) noexcept;
enum class ReceiverAction : std::uint8_t { ignored, stream_updated, poll_reply_ready, sync };

class Receiver {
  public:
    Receiver(NodeIdentity identity, DmxMapping mapping, led::StreamLayer& stream) noexcept
        : identity_(identity), mapping_(mapping), stream_(&stream) {}
    [[nodiscard]] core::Result<ReceiverAction> receive(std::span<const std::byte> datagram,
                                                       std::uint64_t received_at_us,
                                                       std::span<std::byte> response,
                                                       std::size_t& response_size) noexcept;

  private:
    NodeIdentity identity_{};
    DmxMapping mapping_{};
    led::StreamLayer* stream_{};
};

} // namespace blip::artnet
