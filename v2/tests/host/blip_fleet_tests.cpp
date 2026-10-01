#include "blip/fleet/fleet.hpp"

#include <array>
#include <iostream>
#include <vector>

namespace {
using namespace blip::fleet;
#define CHECK(x) do { if (!(x)) { std::cerr << "FAIL " << __func__ << ':' << __LINE__ << " " #x "\n"; return false; } } while (false)
Message beacon(std::uint64_t node, std::uint32_t session, std::uint32_t sequence, std::uint64_t sent) {
    Message m{}; m.fleet_id = 77; m.sender = node; m.session = session; m.sequence = sequence; m.sent_us = sent;
    return m;
}
struct Pair {
    static constexpr std::uint64_t a_base = 1'000'000U, b_base = 4'000'000U;
    Engine a{}, b{};
    bool start() { return a.start(77, 100, 3, a_base) && b.start(77, 200, 4, b_base); }
    void step(std::uint64_t t, bool link = true, bool drop_cues = false) {
        a.service(a_base + t); b.service(b_base + t);
        Message m{};
        while (a.next_message(a_base + t, m))
            if (link && !(drop_cues && m.kind == Kind::cue)) static_cast<void>(b.receive(m, b_base + t + 1000));
        while (b.next_message(b_base + t, m))
            if (link) static_cast<void>(a.receive(m, a_base + t + 1000));
    }
    void advance(std::uint64_t first, std::uint64_t last, bool link = true, bool drop = false) {
        for (auto t = first; t <= last; t += 10'000) step(t, link, drop);
    }
};

bool packet_bounds_and_corruption() {
    auto m = beacon(100, 2, 1, 123456);
    std::array<std::byte, kMaximumPacketBytes> bytes{};
    auto encoded = encode(m, bytes); CHECK(encoded && encoded.value() == kHeaderBytes);
    Message output{}; CHECK(decode({bytes.data(), encoded.value()}, output)); CHECK(output.sent_us == m.sent_us);
    bytes[8] ^= std::byte{1}; CHECK(!decode({bytes.data(), encoded.value()}, output));
    m.kind = Kind::cue; m.payload_size = kMaximumCueBytes; m.deadline_us = 987654;
    encoded = encode(m, bytes); CHECK(encoded && encoded.value() == kMaximumPacketBytes);
    CHECK(decode(bytes, output)); CHECK(output.payload_size == kMaximumCueBytes);
    CHECK(!encode(m, std::span<std::byte>{bytes}.first(kHeaderBytes)));
    CHECK(!decode(std::span<const std::byte>{bytes}.first(bytes.size() - 1), output));
    ++m.payload_size; CHECK(!encode(m, bytes));
    return true;
}
bool election_and_different_uptimes() {
    Pair pair; CHECK(pair.start()); pair.advance(0, 2'000'000);
    CHECK(pair.a.is_leader()); CHECK(pair.b.leader() == 100); CHECK(pair.b.synchronized());
    CHECK(pair.b.offset_us() == -3'001'000);
    CHECK(pair.b.uncertainty_us() == kClockTransportBudgetUs);
    Message m{}; CHECK(!pair.b.next_message(Pair::b_base + 2'000'000, m));
    return true;
}
bool cue_loss_retries_and_exactly_once_handoff() {
    Pair pair; CHECK(pair.start()); pair.advance(0, 2'000'000);
    const std::array command{std::byte{42}};
    CHECK(pair.a.schedule(command, 1'000'000, Pair::a_base + 2'000'000));
    pair.advance(2'010'000, 2'400'000, true, true); CHECK(pair.b.pending() == 0);
    pair.advance(2'410'000, 2'990'000); CHECK(pair.b.pending() == 1); CHECK(pair.b.metrics().duplicates > 0);
    Cue cue{};
    CHECK(pair.a.take_due(Pair::a_base + 3'005'000, cue)); CHECK(cue.bytes[0] == command[0]);
    CHECK(pair.b.take_due(Pair::b_base + 3'005'000, cue));
    CHECK(!pair.a.take_due(Pair::a_base + 3'005'000, cue)); CHECK(!pair.b.take_due(Pair::b_base + 3'005'000, cue));
    return true;
}
bool leader_loss_partition_and_rejoin() {
    Pair pair; CHECK(pair.start()); pair.advance(0, 2'000'000);
    const std::array command{std::byte{77}};
    CHECK(pair.a.schedule(command, 10'000'000, Pair::a_base + 2'000'000));
    pair.advance(2'010'000, 2'300'000); CHECK(pair.b.pending() == 1);
    pair.advance(2'310'000, 5'800'000, false);
    CHECK(pair.b.is_leader()); CHECK(pair.b.pending() == 0); CHECK(pair.b.metrics().cancelled == 1);
    CHECK(pair.b.schedule(command, 1'000'000, Pair::b_base + 5'800'000));
    pair.advance(5'810'000, 7'000'000); CHECK(pair.b.leader() == 100); CHECK(pair.b.synchronized()); CHECK(pair.b.pending() == 0);
    return true;
}
bool reboot_rejects_old_sessions_and_cues() {
    Pair pair; CHECK(pair.start()); pair.advance(0, 2'000'000);
    auto old = beacon(100, 3, 500, Pair::a_base + 2'000'000);
    const std::array command{std::byte{5}};
    CHECK(pair.a.schedule(command, 5'000'000, Pair::a_base + 2'000'000));
    pair.advance(2'010'000, 2'200'000); CHECK(pair.b.pending() == 1);
    auto rebooted = beacon(100, 9, 1, 1000);
    CHECK(pair.b.receive(rebooted, Pair::b_base + 2'210'000)); CHECK(pair.b.pending() == 0);
    CHECK(!pair.b.receive(old, Pair::b_base + 2'220'000)); CHECK(pair.b.leader_session() == 9);
    old.kind = Kind::cue; old.payload_size = 1; old.deadline_us = 5'000'000;
    CHECK(!pair.b.receive(old, Pair::b_base + 2'230'000));
    return true;
}
bool delayed_and_replayed_beacons_do_not_keep_clock_locked() {
    Engine node; CHECK(node.start(77, 200, 1, 1'000'000));
    for (std::uint32_t i = 1; i <= 3; ++i) CHECK(node.receive(beacon(100, 2, i, i * 250'000), 1'000'000 + i * 250'000));
    CHECK(node.synchronized());
    const auto before = node.time_us(2'000'000);
    CHECK(!node.receive(beacon(100, 2, 3, 750'000), 2'001'000));
    for (std::uint32_t i = 4; i <= 13; ++i)
        CHECK(!node.receive(beacon(100, 2, i, i * 250'000 - 50'000), 1'000'000 + i * 250'000));
    node.service(4'251'000); CHECK(!node.synchronized()); CHECK(node.leader() == 100);
    // Relocking keeps the epoch clock monotonic while rebuilding the sample window.
    for (std::uint32_t i = 14; i <= 16; ++i) CHECK(node.receive(beacon(100, 2, i, i * 250'000), 1'000'000 + i * 250'000));
    CHECK(node.synchronized()); CHECK(node.time_us(5'000'000) >= before);
    return true;
}
bool reordered_contradictory_and_late_cues() {
    Pair pair; CHECK(pair.start()); pair.advance(0, 2'000'000);
    auto m = beacon(100, 3, 2, Pair::a_base + 2'000'000);
    m.kind = Kind::cue; m.payload_size = 1; m.deadline_us = Pair::a_base + 3'000'000;
    CHECK(pair.b.receive(m, Pair::b_base + 2'000'000));
    m.sequence = 1; CHECK(pair.b.receive(m, Pair::b_base + 2'000'000)); CHECK(pair.b.pending() == 2);
    m.payload[0] = std::byte{99}; CHECK(!pair.b.receive(m, Pair::b_base + 2'001'000));
    Cue cue{}; CHECK(!pair.b.take_due(Pair::b_base + 3'050'000, cue)); CHECK(pair.b.metrics().late == 2);
    m.sequence = 3; CHECK(!pair.b.receive(m, Pair::b_base + 3'100'000));
    return true;
}
bool queue_bounds_and_cancellation() {
    Engine node; CHECK(node.start(77, 100, 1, 1'000'000)); node.service(2'000'000);
    const std::array command{std::byte{1}};
    CHECK(!node.schedule(command, 99'999, 3'000'000));
    for (std::size_t i = 0; i < kMaximumCues; ++i) CHECK(node.schedule(command, 1'000'000, 3'000'000));
    CHECK(!node.schedule(command, 1'000'000, 3'000'000));
    node.cancel_all(); CHECK(node.pending() == 0); CHECK(node.metrics().cancelled == kMaximumCues);
    return true;
}
bool thousands_of_silent_followers_use_constant_broadcast_work() {
    constexpr std::size_t count = 4096;
    std::vector<Engine> followers(count);
    Engine leader; CHECK(leader.start(77, 1, 1, 0)); leader.service(500'000);
    for (std::size_t i = 0; i < count; ++i) CHECK(followers[i].start(77, i + 2, 2, 10'000'000));
    std::size_t broadcasts{};
    Message message{};
    for (std::uint64_t t = 500'000; t <= 2'500'000; t += 250'000) {
        CHECK(leader.next_message(t, message)); ++broadcasts;
        for (auto& follower : followers) CHECK(follower.receive(message, 10'000'000 + t + 1000));
    }
    CHECK(broadcasts == 9);
    for (auto& follower : followers) { CHECK(follower.synchronized()); CHECK(!follower.next_message(12'501'000, message)); }
    const std::array command{std::byte{123}};
    CHECK(leader.schedule(command, 500'000, 2'500'000));
    CHECK(leader.next_message(2'501'000, message)); CHECK(message.kind == Kind::cue);
    for (auto& follower : followers) CHECK(follower.receive(message, 12'502'000));
    Cue cue{};
    for (auto& follower : followers) CHECK(follower.take_due(13'005'000, cue));
    return true;
}
} // namespace

int main() {
    const std::array tests{packet_bounds_and_corruption, election_and_different_uptimes,
        cue_loss_retries_and_exactly_once_handoff, leader_loss_partition_and_rejoin,
        reboot_rejects_old_sessions_and_cues, delayed_and_replayed_beacons_do_not_keep_clock_locked,
        reordered_contradictory_and_late_cues, queue_bounds_and_cancellation,
        thousands_of_silent_followers_use_constant_broadcast_work};
    for (const auto test : tests) if (!test()) return 1;
    std::cout << "PASS " << tests.size() << " broadcast fleet cases\n";
    return 0;
}
