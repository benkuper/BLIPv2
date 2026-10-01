#include "blip/transport/espnow_frames.hpp"

#include <algorithm>
#include <cstring>

namespace blip::transport {
namespace {

constexpr std::byte kMagic0{0x42};
constexpr std::byte kMagic1{0x4e};
constexpr std::byte kVersion{2};

void write_u32(std::span<std::byte> out, std::size_t offset, std::uint32_t value) noexcept {
    for (std::size_t i = 0; i < 4U; ++i) {
        out[offset + i] = static_cast<std::byte>((value >> (8U * i)) & 0xffU);
    }
}

[[nodiscard]] std::uint32_t read_u32(std::span<const std::byte> in,
                                     std::size_t offset) noexcept {
    std::uint32_t result{};
    for (std::size_t i = 0; i < 4U; ++i) {
        result |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(in[offset + i]))
                  << (8U * i);
    }
    return result;
}

void write_header(std::span<std::byte> output, EspNowPacketKind kind,
                  std::uint32_t session, std::uint32_t sequence,
                  std::uint16_t total_size, std::uint8_t fragment_index,
                  std::uint8_t fragment_count) noexcept {
    output[0] = kMagic0;
    output[1] = kMagic1;
    output[2] = kVersion;
    output[3] = static_cast<std::byte>(kind);
    write_u32(output, 4, session);
    write_u32(output, 8, sequence);
    output[12] = static_cast<std::byte>(total_size & 0xffU);
    output[13] = static_cast<std::byte>(total_size >> 8U);
    output[14] = static_cast<std::byte>(fragment_index);
    output[15] = static_cast<std::byte>(fragment_count);
}

} // namespace

bool decode_espnow_packet(std::span<const std::byte> packet,
                          EspNowPacketView& view) noexcept {
    if (packet.size() < kEspNowHeaderBytes || packet.size() > kEspNowPacketBytes ||
        packet[0] != kMagic0 || packet[1] != kMagic1 || packet[2] != kVersion) {
        return false;
    }
    const auto kind = static_cast<EspNowPacketKind>(packet[3]);
    const auto session = read_u32(packet, 4);
    const auto sequence = read_u32(packet, 8);
    const auto total_size = static_cast<std::uint16_t>(
        std::to_integer<std::uint8_t>(packet[12]) |
        (static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(packet[13])) << 8U));
    const auto index = std::to_integer<std::uint8_t>(packet[14]);
    const auto count = std::to_integer<std::uint8_t>(packet[15]);
    const auto expected_count =
        (static_cast<std::size_t>(total_size) + kEspNowFragmentBytes - 1U) /
        kEspNowFragmentBytes;
    if (session == 0U || sequence == 0U) {
        return false;
    }
    if (kind == EspNowPacketKind::acknowledgement) {
        if (packet.size() != kEspNowHeaderBytes || total_size != 0U || index != 0U ||
            count != 0U) {
            return false;
        }
    } else if (kind == EspNowPacketKind::data) {
        if (total_size == 0U || total_size > kMaxEnvelopeBytes ||
            count != expected_count || count > kEspNowMaxFragments || index >= count ||
            packet.size() - kEspNowHeaderBytes !=
                std::min(kEspNowFragmentBytes,
                         static_cast<std::size_t>(total_size) -
                             static_cast<std::size_t>(index) * kEspNowFragmentBytes)) {
            return false;
        }
    } else {
        return false;
    }
    view = {kind, session, sequence, total_size, index, count,
            packet.subspan(kEspNowHeaderBytes)};
    return true;
}

std::size_t encode_espnow_ack(std::uint32_t session, std::uint32_t sequence,
                              std::span<std::byte> output) noexcept {
    if (output.size() < kEspNowHeaderBytes || session == 0U || sequence == 0U) {
        return 0;
    }
    write_header(output, EspNowPacketKind::acknowledgement, session, sequence, 0, 0, 0);
    return kEspNowHeaderBytes;
}

bool EspNowTransmit::begin(std::uint32_t session, std::uint32_t sequence,
                           std::span<const std::byte> envelope) noexcept {
    pending_ = false;
    if (session == 0U || sequence == 0U || envelope.empty() ||
        envelope.size() > kMaxEnvelopeBytes) {
        return false;
    }
    session_ = session;
    sequence_ = sequence;
    envelope_ = envelope;
    fragment_count_ = (envelope.size() + kEspNowFragmentBytes - 1U) /
                      kEspNowFragmentBytes;
    pending_ = true;
    return true;
}

std::size_t EspNowTransmit::fragment(std::size_t index,
                                     std::span<std::byte> output) const noexcept {
    if (!pending_ || index >= fragment_count_) {
        return 0;
    }
    const auto offset = index * kEspNowFragmentBytes;
    const auto size = std::min(kEspNowFragmentBytes, envelope_.size() - offset);
    if (output.size() < kEspNowHeaderBytes + size) {
        return 0;
    }
    write_header(output, EspNowPacketKind::data, session_, sequence_,
                 static_cast<std::uint16_t>(envelope_.size()),
                 static_cast<std::uint8_t>(index),
                 static_cast<std::uint8_t>(fragment_count_));
    std::memcpy(output.data() + kEspNowHeaderBytes, envelope_.data() + offset, size);
    return kEspNowHeaderBytes + size;
}

bool EspNowTransmit::acknowledge(const EspNowPacketView& packet) noexcept {
    if (!pending_ || packet.kind != EspNowPacketKind::acknowledgement ||
        packet.session != session_ || packet.sequence != sequence_) {
        return false;
    }
    pending_ = false;
    return true;
}

void EspNowReassemble::reset() noexcept {
    active_ = false;
    completed_ = false;
    received_mask_ = 0;
}

EspNowReceiveResult EspNowReassemble::accept(const EspNowPacketView& packet) noexcept {
    if (packet.kind != EspNowPacketKind::data || packet.total_size == 0U ||
        packet.total_size > kMaxEnvelopeBytes || packet.fragment_count == 0U ||
        packet.fragment_count > kEspNowMaxFragments ||
        packet.fragment_count !=
            (static_cast<std::size_t>(packet.total_size) + kEspNowFragmentBytes - 1U) /
                kEspNowFragmentBytes ||
        packet.fragment_index >= packet.fragment_count ||
        packet.payload.size() !=
            std::min(kEspNowFragmentBytes,
                     static_cast<std::size_t>(packet.total_size) -
                         static_cast<std::size_t>(packet.fragment_index) *
                             kEspNowFragmentBytes)) {
        return EspNowReceiveResult::rejected;
    }
    if (active_ && packet.session == session_ && packet.sequence == sequence_) {
        if (packet.total_size != total_size_ || packet.fragment_count != fragment_count_) {
            return EspNowReceiveResult::rejected;
        }
        if (completed_) {
            return EspNowReceiveResult::duplicate;
        }
    } else {
        if (active_ && packet.session == session_ &&
            static_cast<std::int32_t>(packet.sequence - sequence_) <= 0) {
            return EspNowReceiveResult::rejected;
        }
        reset();
        session_ = packet.session;
        sequence_ = packet.sequence;
        total_size_ = packet.total_size;
        fragment_count_ = packet.fragment_count;
        active_ = true;
    }
    const auto offset = static_cast<std::size_t>(packet.fragment_index) *
                        kEspNowFragmentBytes;
    const auto bit = static_cast<std::uint8_t>(1U << packet.fragment_index);
    if ((received_mask_ & bit) != 0U) {
        return std::memcmp(buffer_.data() + offset, packet.payload.data(),
                           packet.payload.size()) == 0
                   ? EspNowReceiveResult::partial
                   : EspNowReceiveResult::rejected;
    }
    std::memcpy(buffer_.data() + offset, packet.payload.data(), packet.payload.size());
    received_mask_ |= bit;
    const auto complete_mask = static_cast<std::uint8_t>((1U << fragment_count_) - 1U);
    if (received_mask_ != complete_mask) {
        return EspNowReceiveResult::partial;
    }
    completed_ = true;
    return EspNowReceiveResult::complete;
}

} // namespace blip::transport
