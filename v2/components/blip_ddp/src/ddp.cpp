#include "blip/ddp/ddp.hpp"
namespace blip::ddp {
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
    const bool supported_rgb8 = !mapping.sixteen_bit && mapping.channels_per_pixel == 3U &&
                                (packet.value().data_type == 0x0bU ||
                                 packet.value().data_type == 0x01U);
    if (packet.value().destination != mapping.destination ||
        !supported_rgb8 ||
        packet.value().offset < mapping.base_offset || width == 0U ||
        (packet.value().offset - mapping.base_offset) % width != 0U ||
        packet.value().data.empty() || packet.value().data.size() % width != 0U)
        return core::Result<PixelUpdate>::failure({core::ErrorDomain::control,
                                                    core::ErrorCode::validation_failed,
                                                    "blip.ddp", "map-data",
                                                    "unaligned-or-wrong-destination"});
    const auto pixel_offset = (packet.value().offset - mapping.base_offset) / width;
    if (pixel_offset > 65535U - mapping.start_pixel)
        return core::Result<PixelUpdate>::failure({core::ErrorDomain::control,
                                                    core::ErrorCode::capacity_exceeded,
                                                    "blip.ddp", "map-data", "pixel-offset"});
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
