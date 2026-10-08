#include "blip/ddp/ddp.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
namespace blip::ddp {
core::Result<std::size_t> encode_query_reply(std::span<const std::byte> query,
                                             const DiscoveryIdentity& identity,
                                             std::span<std::byte> response) noexcept {
    const auto fail = [](std::string_view detail) {
        return core::Result<std::size_t>::failure({core::ErrorDomain::transport,
                                                   core::ErrorCode::corrupt_data, "blip.ddp",
                                                   "query", detail});
    };
    if (query.size() < 10U || (std::to_integer<unsigned>(query[0]) & 0xfeU) != 0x42U)
        return fail("invalid-query-header");
    const auto requested =
        (std::to_integer<unsigned>(query[8]) << 8U) | std::to_integer<unsigned>(query[9]);
    if (query.size() != 10U && query.size() != 10U + requested)
        return fail("invalid-query-length");
    if (response.size() < 384U)
        return fail("response-too-small");
    const auto id = query[3];
    auto* json = reinterpret_cast<char*>(response.data() + 10U);
    const auto capacity = response.size() - 10U;
    int json_size{};
    if (id == std::byte{251}) {
        json_size = std::snprintf(
            json, capacity,
            "{\"status\":{\"man\":\"BLIP\",\"mod\":\"BLIP V2\",\"ver\":\"0.1.0\","
            "\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"push\":false,\"ntp\":false}}",
            identity.mac[0], identity.mac[1], identity.mac[2], identity.mac[3], identity.mac[4],
            identity.mac[5]);
    } else if (id == std::byte{250}) {
        json_size = std::snprintf(
            json, capacity,
            "{\"config\":{\"ports\":[{\"port\":1,\"l\":%u,\"ss\":0}],"
            "\"num_chan\":%u,\"data_type\":11,\"destination\":1,\"read_only\":true}}",
            static_cast<unsigned>(identity.pixels), static_cast<unsigned>(identity.pixels) * 3U);
    }
    if (json_size < 0 || static_cast<std::size_t>(json_size) >= capacity)
        return fail("response-too-small");
    std::uint32_t offset{};
    for (std::size_t i = 4U; i < 8U; ++i)
        offset = (offset << 8U) | std::to_integer<std::uint8_t>(query[i]);
    if (json_size == 0)
        offset = 0U;
    const auto available =
        offset < static_cast<unsigned>(json_size) ? static_cast<unsigned>(json_size) - offset : 0U;
    // A zero length discovery query requests the whole document. Payload hints
    // are accepted without treating their byte count as a read limit.
    const auto count = requested == 0U || query.size() > 10U
                           ? available
                           : std::min<std::uint32_t>(available, requested);
    if (count != 0U)
        std::memmove(json, json + offset, count);
    std::fill_n(response.begin(), 10U, std::byte{0});
    response[0] = static_cast<std::byte>(0x44U | (count == available ? 1U : 0U));
    response[3] = id;
    for (std::size_t i = 0; i < 4U; ++i)
        response[4U + i] = static_cast<std::byte>(offset >> ((3U - i) * 8U));
    response[8] = static_cast<std::byte>(count >> 8U);
    response[9] = static_cast<std::byte>(count);
    return core::Result<std::size_t>::success(10U + count);
}

bool SequenceTracker::accept(std::uint8_t sequence, std::uint64_t received_at_us) noexcept {
    if (sequence == 0U) {
        return true;
    }
    if (sequence > 15U) {
        return false;
    }
    if (!have_sequence_ || received_at_us < last_reception_us_ ||
        received_at_us - last_reception_us_ >= 1'000'000U) {
        have_sequence_ = true;
        last_sequence_ = sequence;
        last_reception_us_ = received_at_us;
        return true;
    }
    const auto advance = static_cast<std::uint8_t>((sequence - last_sequence_) & 0x0fU);
    if (advance > 7U) {
        return false;
    }
    if (advance != 0U) {
        last_sequence_ = sequence;
    }
    last_reception_us_ = received_at_us;
    return true;
}

core::Result<Packet> parse(std::span<const std::byte> d) noexcept {
    const auto fail = [](std::string_view why) {
        return core::Result<Packet>::failure({core::ErrorDomain::transport,
                                              core::ErrorCode::corrupt_data, "blip.ddp", "parse",
                                              why});
    };
    if (d.size() < 10U)
        return fail("short-header");
    const auto flags = std::to_integer<std::uint8_t>(d[0U]);
    if ((flags & 0xfeU) != 0x40U)
        return fail("unsupported-flags-or-version");
    const auto length = static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(d[8U])) << 8U |
                        std::to_integer<std::uint8_t>(d[9U]);
    if (d.size() != 10U + length)
        return fail("invalid-length");
    const auto offset = static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(d[4U])) << 24U |
                        static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(d[5U])) << 16U |
                        static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(d[6U])) << 8U |
                        std::to_integer<std::uint8_t>(d[7U]);
    return core::Result<Packet>::success(
        {(flags & 0x01U) != 0U,
         static_cast<std::uint8_t>(std::to_integer<std::uint8_t>(d[1U]) & 0x0fU),
         std::to_integer<std::uint8_t>(d[2U]), std::to_integer<std::uint8_t>(d[3U]), offset,
         d.subspan(10U)});
}
core::Result<PixelUpdate> map(std::span<const std::byte> datagram,
                              const Mapping& mapping) noexcept {
    const auto packet = parse(datagram);
    if (!packet)
        return core::Result<PixelUpdate>::failure(packet.error());
    const auto width =
        static_cast<std::size_t>(mapping.channels_per_pixel) * (mapping.sixteen_bit ? 2U : 1U);
    const bool supported_rgb8 =
        !mapping.sixteen_bit && mapping.channels_per_pixel == 3U &&
        (packet.value().data_type == 0x0bU || packet.value().data_type == 0x01U);
    if (packet.value().destination != mapping.destination || !supported_rgb8 ||
        packet.value().offset < mapping.base_offset || width == 0U ||
        (packet.value().offset - mapping.base_offset) % width != 0U ||
        packet.value().data.empty() || packet.value().data.size() % width != 0U)
        return core::Result<PixelUpdate>::failure({core::ErrorDomain::control,
                                                   core::ErrorCode::validation_failed, "blip.ddp",
                                                   "map-data", "unaligned-or-wrong-destination"});
    const auto pixel_offset = (packet.value().offset - mapping.base_offset) / width;
    if (pixel_offset > 65535U - mapping.start_pixel)
        return core::Result<PixelUpdate>::failure({core::ErrorDomain::control,
                                                   core::ErrorCode::capacity_exceeded, "blip.ddp",
                                                   "map-data", "pixel-offset"});
    return core::Result<PixelUpdate>::success(
        {packet.value().sequence, static_cast<std::uint16_t>(mapping.start_pixel + pixel_offset),
         packet.value().data, mapping.channels_per_pixel, mapping.sixteen_bit});
}
core::Status ingest(std::span<const std::byte> datagram, const Mapping& mapping,
                    led::StreamLayer& stream, std::uint64_t received_at_us) noexcept {
    const auto update = map(datagram, mapping);
    if (!update)
        return core::Status::failure(update.error());
    return stream.ingest(update.value().sequence, update.value().start_pixel,
                         update.value().channels, update.value().channels_per_pixel,
                         update.value().sixteen_bit, received_at_us);
}
} // namespace blip::ddp
