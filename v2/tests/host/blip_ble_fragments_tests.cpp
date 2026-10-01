#include "blip/transport/ble_fragments.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>

namespace {

using namespace blip::transport;

#define BLIP_CHECK(expression)                                                                     \
    do {                                                                                           \
        if (!(expression)) {                                                                       \
            std::cerr << __func__ << ':' << __LINE__ << ": check failed: " #expression << '\n';    \
            return false;                                                                          \
        }                                                                                          \
    } while (false)

bool minimum_mtu_round_trip_at_envelope_limit() {
    std::array<std::byte, kMaxEnvelopeBytes> frame{};
    for (std::size_t index = 0; index < frame.size(); ++index) {
        frame[index] = static_cast<std::byte>(index & 0xffU);
    }
    BleResponseFragments sender{};
    BleRequestAssembler receiver{};
    BLIP_CHECK(sender.begin(frame, 0x91));
    std::array<std::byte, 20> chunk{}; // Default ATT MTU 23 minus three ATT bytes.
    std::size_t chunks{};
    while (!sender.complete()) {
        const auto size = sender.next(chunk);
        BLIP_CHECK(size && size.value() <= chunk.size());
        BLIP_CHECK(!sender.next(chunk)); // The prior indication needs an ACK.
        const auto received = receiver.append({chunk.data(), size.value()});
        BLIP_CHECK(received);
        BLIP_CHECK(receiver.frame_id() == 0x91);
        BLIP_CHECK(received.value() == (chunks == (frame.size() - 1U) / 16U));
        BLIP_CHECK(sender.acknowledge());
        ++chunks;
    }
    BLIP_CHECK(chunks > 30U);
    BLIP_CHECK(receiver.frame().size() == frame.size());
    BLIP_CHECK(std::equal(receiver.frame().begin(), receiver.frame().end(), frame.begin()));
    BLIP_CHECK(!sender.next(chunk));
    BLIP_CHECK(!sender.acknowledge());
    return true;
}

bool rejects_reorder_duplicate_and_bad_bounds() {
    std::array<std::byte, 40> frame{};
    frame.fill(std::byte{0x55});
    BleResponseFragments sender{};
    BLIP_CHECK(sender.begin(frame, 7));
    std::array<std::byte, 20> first{};
    std::array<std::byte, 20> second{};
    const auto first_size = sender.next(first);
    BLIP_CHECK(first_size && sender.acknowledge());
    const auto second_size = sender.next(second);
    BLIP_CHECK(second_size);
    BleRequestAssembler receiver{};
    BLIP_CHECK(!receiver.append({second.data(), second_size.value()}));
    BLIP_CHECK(receiver.append({first.data(), first_size.value()}));
    BLIP_CHECK(receiver.frame().empty());
    BLIP_CHECK(receiver.append({second.data(), second_size.value()}));
    BLIP_CHECK(!receiver.append({second.data(), second_size.value()}));
    BLIP_CHECK(receiver.append({first.data(), first_size.value()})); // A new start recovers.

    first[0] = std::byte{4};
    BLIP_CHECK(!receiver.append({first.data(), first_size.value()}));
    first[0] = std::byte{1};
    first[2] = std::byte{1};
    BLIP_CHECK(!receiver.append({first.data(), first_size.value()}));
    std::array<std::byte, kMaxBleChunkBytes + 1U> oversized{};
    BLIP_CHECK(!receiver.append(oversized));
    BLIP_CHECK(!sender.begin({}, 0));
    return true;
}

} // namespace

int main() {
    if (!minimum_mtu_round_trip_at_envelope_limit() ||
        !rejects_reorder_duplicate_and_bad_bounds()) {
        return 1;
    }
    std::cout << "blip BLE fragment tests passed: 2\n";
    return 0;
}
