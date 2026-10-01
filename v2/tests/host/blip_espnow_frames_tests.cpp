#include "blip/transport/espnow_frames.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>

namespace {
using namespace blip::transport;

#define CHECK(expression)                                                                          \
    do {                                                                                           \
        if (!(expression)) {                                                                       \
            std::cerr << __func__ << ':' << __LINE__ << ": " #expression << '\n';                \
            return false;                                                                          \
        }                                                                                          \
    } while (false)

bool reordered_fragments_and_duplicate_delivery() {
    std::array<std::byte, kMaxEnvelopeBytes> envelope{};
    for (std::size_t i = 0; i < envelope.size(); ++i) {
        envelope[i] = static_cast<std::byte>(i & 0xffU);
    }
    EspNowTransmit tx{};
    EspNowReassemble rx{};
    CHECK(tx.begin(0x12345678U, 99U, envelope));
    CHECK(tx.fragment_count() == 3U);
    std::array<std::array<std::byte, kEspNowPacketBytes>, 3> packets{};
    std::array<std::size_t, 3> sizes{};
    for (std::size_t i = 0; i < packets.size(); ++i) {
        sizes[i] = tx.fragment(i, packets[i]);
        CHECK(sizes[i] > kEspNowHeaderBytes && sizes[i] <= kEspNowPacketBytes);
    }
    const auto feed = [&](std::size_t index, EspNowReceiveResult expected) {
        EspNowPacketView view{};
        return decode_espnow_packet({packets[index].data(), sizes[index]}, view) &&
               rx.accept(view) == expected;
    };
    CHECK(feed(2, EspNowReceiveResult::partial));
    CHECK(feed(0, EspNowReceiveResult::partial));
    CHECK(feed(0, EspNowReceiveResult::partial));
    CHECK(feed(1, EspNowReceiveResult::complete));
    CHECK(std::equal(rx.envelope().begin(), rx.envelope().end(), envelope.begin()));
    CHECK(feed(2, EspNowReceiveResult::duplicate));
    std::array<std::byte, kEspNowHeaderBytes> ack{};
    CHECK(encode_espnow_ack(0x12345678U, 99U, ack) == ack.size());
    EspNowPacketView ack_view{};
    CHECK(decode_espnow_packet(ack, ack_view));
    CHECK(tx.pending() && tx.acknowledge(ack_view) && !tx.pending());
    CHECK(!tx.fragment(0, packets[0]));
    return true;
}

bool loss_ack_loss_and_replay() {
    std::array<std::byte, 400> envelope{};
    envelope.fill(std::byte{0x51});
    EspNowTransmit tx{};
    EspNowReassemble rx{};
    CHECK(tx.begin(5, 1, envelope));
    std::array<std::byte, kEspNowPacketBytes> packet{};
    auto send = [&](std::size_t index) {
        const auto size = tx.fragment(index, packet);
        EspNowPacketView view{};
        return size != 0U && decode_espnow_packet({packet.data(), size}, view)
                   ? rx.accept(view)
                   : EspNowReceiveResult::rejected;
    };
    CHECK(send(0) == EspNowReceiveResult::partial);
    // Fragment 1 is lost. Whole-message retry resends 0 and then 1.
    CHECK(send(0) == EspNowReceiveResult::partial);
    CHECK(send(1) == EspNowReceiveResult::complete);
    // ACK is lost; a second retry must not dispatch this request again.
    CHECK(send(0) == EspNowReceiveResult::duplicate);
    CHECK(send(1) == EspNowReceiveResult::duplicate);
    std::array<std::byte, kEspNowHeaderBytes> ack{};
    EspNowPacketView decoded{};
    CHECK(encode_espnow_ack(5, 2, ack) == ack.size());
    CHECK(decode_espnow_packet(ack, decoded) && !tx.acknowledge(decoded));
    CHECK(encode_espnow_ack(5, 1, ack) == ack.size());
    CHECK(decode_espnow_packet(ack, decoded) && tx.acknowledge(decoded));
    CHECK(send(0) == EspNowReceiveResult::rejected);
    return true;
}

bool malformed_frames_and_sequence_window() {
    std::array<std::byte, 1> envelope{std::byte{0x61}};
    EspNowTransmit tx{};
    EspNowReassemble rx{};
    CHECK(!tx.begin(0, 1, envelope));
    CHECK(!tx.begin(1, 0, envelope));
    CHECK(tx.begin(8, 0xfffffffeU, envelope));
    std::array<std::byte, kEspNowPacketBytes> packet{};
    EspNowPacketView view{};
    auto size = tx.fragment(0, packet);
    CHECK(decode_espnow_packet({packet.data(), size}, view));
    CHECK(rx.accept(view) == EspNowReceiveResult::complete);
    packet[2] = std::byte{1};
    CHECK(!decode_espnow_packet({packet.data(), size}, view)); // No V1 fallback.
    packet[2] = std::byte{2};
    packet[15] = std::byte{2};
    CHECK(!decode_espnow_packet({packet.data(), size}, view));
    packet[15] = std::byte{1};
    packet[12] = std::byte{0};
    CHECK(!decode_espnow_packet({packet.data(), size}, view));
    CHECK(tx.begin(8, 1, envelope)); // Sequence wraps in the same session.
    size = tx.fragment(0, packet);
    CHECK(decode_espnow_packet({packet.data(), size}, view));
    CHECK(rx.accept(view) == EspNowReceiveResult::complete);
    CHECK(tx.begin(8, 0xfffffffdU, envelope));
    size = tx.fragment(0, packet);
    CHECK(decode_espnow_packet({packet.data(), size}, view));
    CHECK(rx.accept(view) == EspNowReceiveResult::rejected);
    return true;
}
} // namespace

int main() {
    if (!reordered_fragments_and_duplicate_delivery() ||
        !loss_ack_loss_and_replay() || !malformed_frames_and_sequence_window()) {
        return 1;
    }
    std::cout << "blip ESP-NOW frame tests passed: 3\n";
    return 0;
}
