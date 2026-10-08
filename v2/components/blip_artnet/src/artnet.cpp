#include "blip/artnet/artnet.hpp"

#include <algorithm>
#include <cstdio>

namespace blip::artnet {
namespace {
constexpr std::array<std::byte, 8> kId{std::byte{'A'}, std::byte{'r'}, std::byte{'t'},
                                       std::byte{'-'}, std::byte{'N'}, std::byte{'e'},
                                       std::byte{'t'}, std::byte{0}};
[[nodiscard]] std::uint16_t le16(std::span<const std::byte> data, std::size_t at) noexcept {
    return std::to_integer<std::uint8_t>(data[at]) |
           static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(data[at + 1U])) << 8U;
}
[[nodiscard]] std::uint16_t be16(std::span<const std::byte> data, std::size_t at) noexcept {
    return static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(data[at])) << 8U |
           std::to_integer<std::uint8_t>(data[at + 1U]);
}
void put_le16(std::span<std::byte> data, std::size_t at, std::uint16_t value) noexcept {
    data[at] = static_cast<std::byte>(value);
    data[at + 1U] = static_cast<std::byte>(value >> 8U);
}
void put_be16(std::span<std::byte> data, std::size_t at, std::uint16_t value) noexcept {
    data[at] = static_cast<std::byte>(value >> 8U);
    data[at + 1U] = static_cast<std::byte>(value);
}
[[nodiscard]] core::Result<Packet> fail(std::string_view detail) noexcept {
    return core::Result<Packet>::failure({core::ErrorDomain::transport,
                                          core::ErrorCode::corrupt_data, "blip.artnet", "parse",
                                          detail});
}
void text_field(std::span<std::byte> output, std::size_t at, std::size_t size,
                std::string_view value) noexcept {
    const auto count = std::min(size - 1U, value.size());
    for (std::size_t i = 0; i < count; ++i)
        output[at + i] = static_cast<std::byte>(value[i]);
}
} // namespace

core::Result<Packet> parse(std::span<const std::byte> datagram) noexcept {
    if (datagram.size() < 10U || !std::equal(kId.begin(), kId.end(), datagram.begin()))
        return fail("invalid-id");
    const auto opcode = le16(datagram, 8U);
    if (opcode == 0x2000U) {
        if (datagram.size() < 14U)
            return fail("short-poll");
        if (be16(datagram, 10U) < 14U)
            return fail("old-protocol-version");
        Packet packet{PacketKind::poll};
        const auto flags = std::to_integer<std::uint8_t>(datagram[12U]);
        packet.notify_changes = (flags & 0x02U) != 0U;
        packet.targeted = (flags & 0x20U) != 0U;
        // Missing fields in older, 14-byte polls are defined as zero.
        const auto field = [datagram](std::size_t at) -> std::uint16_t {
            const auto high =
                at < datagram.size() ? std::to_integer<std::uint8_t>(datagram[at]) : 0U;
            const auto low =
                at + 1U < datagram.size() ? std::to_integer<std::uint8_t>(datagram[at + 1U]) : 0U;
            return static_cast<std::uint16_t>((high << 8U) | low);
        };
        packet.target_top = field(14U);
        packet.target_bottom = field(16U);
        return core::Result<Packet>::success(packet);
    }
    if (opcode == 0x5200U) {
        if (datagram.size() < 14U)
            return fail("short-sync");
        if (be16(datagram, 10U) < 14U)
            return fail("old-protocol-version");
        return core::Result<Packet>::success({PacketKind::sync});
    }
    if (opcode != 0x5000U || datagram.size() < 18U)
        return fail("unsupported-opcode");
    if (be16(datagram, 10U) < 14U)
        return fail("old-protocol-version");
    const auto length = be16(datagram, 16U);
    if (length < 2U || length > 512U || (length & 1U) != 0U || datagram.size() != 18U + length)
        return fail("invalid-dmx-length");
    return core::Result<Packet>::success(
        {PacketKind::dmx, static_cast<std::uint16_t>(le16(datagram, 14U) & 0x7fffU),
         std::to_integer<std::uint8_t>(datagram[12U]), datagram.subspan(18U, length)});
}

bool poll_matches(const Packet& packet, std::uint16_t universe) noexcept {
    return packet.kind == PacketKind::poll &&
           (!packet.targeted ||
            (universe >= packet.target_bottom && universe <= packet.target_top));
}

core::Result<std::size_t> encode_poll_reply(const NodeIdentity& identity,
                                            std::span<std::byte> output) noexcept {
    if (output.size() < kPollReplyBytes || identity.short_name.empty())
        return core::Result<std::size_t>::failure(
            {core::ErrorDomain::transport, core::ErrorCode::serialization_overflow, "blip.artnet",
             "poll-reply", "output-too-small-or-name-empty"});
    std::fill(output.begin(), output.begin() + kPollReplyBytes, std::byte{0});
    std::copy(kId.begin(), kId.end(), output.begin());
    put_le16(output, 8U, 0x2100U);
    for (std::size_t i = 0; i < 4U; ++i)
        output[10U + i] = static_cast<std::byte>(identity.ipv4[i]);
    put_le16(output, 14U, kPort);
    output[16U] = std::byte{0};
    output[17U] = std::byte{1};
    put_be16(output, 20U, identity.oem);
    text_field(output, 26U, 18U, identity.short_name);
    text_field(output, 44U, 64U, identity.long_name);
    std::array<char, 64> report{};
    std::snprintf(report.data(), report.size(), "#0001 [%04u] BLIP V2 ready",
                  static_cast<unsigned>(identity.report_counter % 10000U));
    text_field(output, 108U, 64U, report.data());
    put_be16(output, 172U, 1U);
    output[174U] = std::byte{0x80};
    output[182U] = identity.output_active ? std::byte{0x80} : std::byte{0};
    output[190U] = static_cast<std::byte>(identity.universe & 0x0fU);
    output[18U] = static_cast<std::byte>((identity.universe >> 8U) & 0x7fU);
    output[19U] = static_cast<std::byte>((identity.universe >> 4U) & 0x0fU);
    output[23U] = std::byte{0xc0}; // Indicators normal; no RDM/remote address programming.
    for (std::size_t i = 0; i < identity.mac.size(); ++i)
        output[201U + i] = static_cast<std::byte>(identity.mac[i]);
    for (std::size_t i = 0; i < 4U; ++i)
        output[207U + i] = static_cast<std::byte>(identity.ipv4[i]);
    output[211U] = static_cast<std::byte>(0x0dU | (identity.dhcp ? 0x02U : 0U));
    // Web configuration, DHCP capable, 15-bit Port-Address; no sACN switching/RDM.
    return core::Result<std::size_t>::success(kPollReplyBytes);
}

core::Result<PixelUpdate> map_dmx(const Packet& packet, const DmxMapping& mapping,
                                  std::size_t maximum_pixels) noexcept {
    const auto bytes_per_pixel =
        static_cast<std::size_t>(mapping.channels_per_pixel) * (mapping.sixteen_bit ? 2U : 1U);
    if (packet.kind != PacketKind::dmx || packet.universe != mapping.universe ||
        mapping.start_channel == 0U || mapping.start_channel > 512U ||
        (mapping.channels_per_pixel != 3U && mapping.channels_per_pixel != 4U) ||
        maximum_pixels == 0U) {
        return core::Result<PixelUpdate>::failure({core::ErrorDomain::control,
                                                   core::ErrorCode::validation_failed,
                                                   "blip.artnet", "map-dmx", "invalid-mapping"});
    }
    const auto channel_offset = static_cast<std::size_t>(mapping.start_channel - 1U);
    if (channel_offset >= packet.dmx.size()) {
        return core::Result<PixelUpdate>::failure({core::ErrorDomain::control,
                                                   core::ErrorCode::validation_failed,
                                                   "blip.artnet", "map-dmx", "invalid-mapping"});
    }
    const auto available = packet.dmx.size() - channel_offset;
    const auto complete = std::min(available / bytes_per_pixel, maximum_pixels) * bytes_per_pixel;
    return core::Result<PixelUpdate>::success({packet.sequence, mapping.start_pixel,
                                               packet.dmx.subspan(channel_offset, complete),
                                               mapping.channels_per_pixel, mapping.sixteen_bit});
}

core::Result<ReceiverAction> Receiver::receive(std::span<const std::byte> datagram,
                                               std::uint64_t received_at_us,
                                               std::span<std::byte> response,
                                               std::size_t& response_size) noexcept {
    response_size = 0U;
    const auto packet = parse(datagram);
    if (!packet) {
        return core::Result<ReceiverAction>::failure(packet.error());
    }
    if (packet.value().kind == PacketKind::poll) {
        if (!poll_matches(packet.value(), mapping_.universe))
            return core::Result<ReceiverAction>::success(ReceiverAction::ignored);
        const auto encoded = encode_poll_reply(identity_, response);
        if (!encoded) {
            return core::Result<ReceiverAction>::failure(encoded.error());
        }
        response_size = encoded.value();
        return core::Result<ReceiverAction>::success(ReceiverAction::poll_reply_ready);
    }
    if (packet.value().kind == PacketKind::sync) {
        return core::Result<ReceiverAction>::success(ReceiverAction::sync);
    }
    if (packet.value().universe != mapping_.universe) {
        return core::Result<ReceiverAction>::success(ReceiverAction::ignored);
    }
    const auto maximum_pixels = mapping_.start_pixel < stream_->pixels().size()
                                    ? stream_->pixels().size() - mapping_.start_pixel
                                    : 0U;
    const auto update = map_dmx(packet.value(), mapping_, maximum_pixels);
    if (!update) {
        return core::Result<ReceiverAction>::failure(update.error());
    }
    if (update.value().channels.empty()) {
        return core::Result<ReceiverAction>::success(ReceiverAction::ignored);
    }
    const auto status = stream_->ingest(update.value().sequence, update.value().start_pixel,
                                        update.value().channels, update.value().channels_per_pixel,
                                        update.value().sixteen_bit, received_at_us);
    return status ? core::Result<ReceiverAction>::success(ReceiverAction::stream_updated)
                  : core::Result<ReceiverAction>::failure(status.error());
}

} // namespace blip::artnet
