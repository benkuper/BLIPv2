#include "blip/e131/e131.hpp"

#include <algorithm>
#include <array>
namespace blip::e131 {
namespace {
[[nodiscard]] std::uint16_t be16(std::span<const std::byte> d, std::size_t at) noexcept {
    return static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(d[at])) << 8U |
           std::to_integer<std::uint8_t>(d[at + 1U]);
}
[[nodiscard]] std::uint32_t be32(std::span<const std::byte> d, std::size_t at) noexcept {
    return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(d[at])) << 24U |
           static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(d[at + 1U])) << 16U |
           static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(d[at + 2U])) << 8U |
           std::to_integer<std::uint8_t>(d[at + 3U]);
}
[[nodiscard]] bool pdu_length(std::span<const std::byte> data, std::size_t at) noexcept {
    return (be16(data, at) & 0xf000U) == 0x7000U && (be16(data, at) & 0x0fffU) == data.size() - at;
}
} // namespace
core::Result<Packet> parse(std::span<const std::byte> data) noexcept {
    constexpr std::array<std::byte, 12> acn{std::byte{'A'}, std::byte{'S'}, std::byte{'C'},
                                            std::byte{'-'}, std::byte{'E'}, std::byte{'1'},
                                            std::byte{'.'}, std::byte{'1'}, std::byte{'7'},
                                            std::byte{0},   std::byte{0},   std::byte{0}};
    const auto fail = [](std::string_view why) {
        return core::Result<Packet>::failure({core::ErrorDomain::transport,
                                              core::ErrorCode::corrupt_data, "blip.e131", "parse",
                                              why});
    };
    if (data.size() < 127U || data.size() > 638U || be16(data, 0U) != 0x0010U ||
        be16(data, 2U) != 0U || !std::equal(acn.begin(), acn.end(), data.begin() + 4) ||
        !pdu_length(data, 16U) || be32(data, 18U) != 0x00000004U || !pdu_length(data, 38U) ||
        be32(data, 40U) != 0x00000002U || !pdu_length(data, 115U) ||
        data[117U] != std::byte{0x02} || data[118U] != std::byte{0xa1} || be16(data, 119U) != 0U ||
        be16(data, 121U) != 1U || std::to_integer<std::uint8_t>(data[108U]) > 200U ||
        be16(data, 109U) > 63999U)
        return fail("invalid-root");
    const auto count = be16(data, 123U);
    if (count < 2U || count > 513U || data.size() != 125U + count || data[125U] != std::byte{0})
        return fail("invalid-properties");
    const auto universe = be16(data, 113U);
    if (universe == 0U || universe > 63999U)
        return fail("invalid-universe");
    Packet packet{};
    packet.universe = universe;
    packet.priority = std::to_integer<std::uint8_t>(data[108U]);
    packet.sequence = std::to_integer<std::uint8_t>(data[111U]);
    packet.options = std::to_integer<std::uint8_t>(data[112U]);
    packet.synchronization_address = be16(data, 109U);
    std::copy_n(data.begin() + 22U, packet.source_id.size(), packet.source_id.begin());
    packet.dmx = data.subspan(126U, count - 1U);
    return core::Result<Packet>::success(packet);
}
core::Result<PixelUpdate> map_dmx(std::span<const std::byte> dmx, const Mapping& mapping,
                                  std::size_t maximum_pixels) noexcept {
    if (mapping.start_channel == 0U || mapping.start_channel > 512U ||
        (mapping.channels_per_pixel != 3U && mapping.channels_per_pixel != 4U) ||
        maximum_pixels == 0U) {
        return core::Result<PixelUpdate>::failure({core::ErrorDomain::control,
                                                    core::ErrorCode::validation_failed,
                                                    "blip.e131", "map-dmx", "invalid-mapping"});
    }
    const auto offset = static_cast<std::size_t>(mapping.start_channel - 1U);
    if (offset >= dmx.size()) {
        return core::Result<PixelUpdate>::failure({core::ErrorDomain::control,
                                                    core::ErrorCode::validation_failed,
                                                    "blip.e131", "map-dmx", "start-out-of-range"});
    }
    const auto width = static_cast<std::size_t>(mapping.channels_per_pixel) *
                       (mapping.sixteen_bit ? 2U : 1U);
    const auto complete = std::min((dmx.size() - offset) / width, maximum_pixels) * width;
    if (complete == 0U) {
        return core::Result<PixelUpdate>::failure({core::ErrorDomain::control,
                                                    core::ErrorCode::validation_failed,
                                                    "blip.e131", "map-dmx", "partial-pixel"});
    }
    return core::Result<PixelUpdate>::success(
        {mapping.start_pixel, dmx.subspan(offset, complete), mapping.channels_per_pixel,
         mapping.sixteen_bit});
}

bool SourceMixer::expire(std::uint64_t now_us) noexcept {
    bool changed = false;
    for (auto& source : sources_) {
        if (source.active && now_us >= source.last_seen_us &&
            now_us - source.last_seen_us >= kSourceTimeoutUs) {
            source.active = false;
            changed = true;
        }
    }
    return changed;
}

SourceResult SourceMixer::ingest(const Packet& packet, std::uint64_t now_us) noexcept {
    if (packet.dmx.size() > 512U)
        return SourceResult::ignored;
    static_cast<void>(expire(now_us));
    Source* matching = nullptr;
    Source* vacant = nullptr;
    for (auto& source : sources_) {
        if (source.active && source.id == packet.source_id)
            matching = &source;
        if (!source.active && vacant == nullptr)
            vacant = &source;
    }
    if ((packet.options & 0x40U) != 0U) {
        if (matching == nullptr)
            return SourceResult::ignored;
        matching->active = false;
        return SourceResult::terminated;
    }
    if ((packet.options & 0x80U) != 0U || (packet.options & 0x20U) != 0U ||
        packet.synchronization_address != 0U)
        return SourceResult::ignored;
    if (matching != nullptr) {
        const auto delta = static_cast<std::uint8_t>(packet.sequence - matching->sequence);
        if (delta == 0U || delta >= 128U)
            return SourceResult::stale;
    } else if (vacant == nullptr) {
        return SourceResult::capacity;
    }
    auto& source = matching != nullptr ? *matching : *vacant;
    source.id = packet.source_id;
    source.size = packet.dmx.size();
    std::copy(packet.dmx.begin(), packet.dmx.end(), source.dmx.begin());
    source.priority = packet.priority;
    source.sequence = packet.sequence;
    source.last_seen_us = now_us;
    source.active = true;
    return SourceResult::accepted;
}

bool SourceMixer::compose(std::span<std::byte> output, std::size_t& size,
                          std::size_t& active_sources) const noexcept {
    size = 0U;
    active_sources = 0U;
    if (output.size() < 512U)
        return false;
    std::uint8_t highest_priority = 0U;
    bool have_source = false;
    for (const auto& source : sources_) {
        if (source.active && (!have_source || source.priority > highest_priority)) {
            highest_priority = source.priority;
            have_source = true;
        }
    }
    if (!have_source)
        return false;
    std::fill(output.begin(), output.begin() + 512U, std::byte{0});
    for (const auto& source : sources_) {
        if (!source.active || source.priority != highest_priority)
            continue;
        ++active_sources;
        size = std::max(size, source.size);
        for (std::size_t index = 0U; index < source.size; ++index)
            output[index] = std::max(output[index], source.dmx[index]);
    }
    return true;
}

core::Status ingest(std::span<const std::byte> datagram, const Mapping& mapping,
                    led::StreamLayer& stream, std::uint64_t received_at_us) noexcept {
    const auto packet = parse(datagram);
    if (!packet)
        return core::Status::failure(packet.error());
    if (packet.value().universe != mapping.universe)
        return core::Status::failure({core::ErrorDomain::control,
                                      core::ErrorCode::validation_failed, "blip.e131", "map-dmx",
                                      "mapping-or-universe"});
    const auto maximum_pixels = mapping.start_pixel < stream.pixels().size()
                                    ? stream.pixels().size() - mapping.start_pixel
                                    : 0U;
    const auto update = map_dmx(packet.value().dmx, mapping, maximum_pixels);
    if (!update)
        return core::Status::failure(update.error());
    return stream.ingest(0U, update.value().start_pixel, update.value().channels,
                         update.value().channels_per_pixel, update.value().sixteen_bit,
                         received_at_us);
}
} // namespace blip::e131
