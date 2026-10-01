#include "blip/fleet/fleet.hpp"

#include <array>
#include <iostream>

namespace {
using namespace blip::fleet;
#define CHECK(x) do { if (!(x)) { std::cerr << "FAIL " << __func__ << ':' << __LINE__ << " " #x "\n"; return false; } } while (false)

Message heartbeat(std::uint64_t node, std::uint32_t session = 1U) {
    Message m{};
    m.fleet_id = 77; m.sender = node; m.session = session;
    m.leader = node; m.leader_session = session;
    return m;
}
bool feed(Engine& engine, const Message& message, std::uint64_t now) {
    Message reply{}; bool has_reply{};
    return static_cast<bool>(engine.receive(message, now, reply, has_reply));
}

struct Pair {
    static constexpr std::uint64_t a_base = 2'000'000;
    static constexpr std::uint64_t b_base = 5'000'000;
    Engine a{}, b{};
    bool start() {
        return static_cast<bool>(a.start(77, 1, 11, a_base)) &&
               static_cast<bool>(b.start(77, 2, 22, b_base));
    }
    bool step(std::uint64_t real, bool deliver_cues = true, bool deliver_acks = true) {
        const auto pump = [&](Engine& from, Engine& to, std::uint64_t source,
                               std::uint64_t target) {
            Message m{}, reply{}; bool has_reply{};
            for (unsigned i = 0; i < 16U && from.next_message(source + real, m); ++i) {
                if (m.kind == Kind::cue && !deliver_cues) continue;
                if (!to.receive(m, target + real + 1000U, reply, has_reply)) return false;
                if (has_reply && (reply.kind != Kind::ack || deliver_acks)) {
                    Message unused{}; bool ignored{};
                    if (!from.receive(reply, source + real + 2000U, unused, ignored)) return false;
                }
            }
            return true;
        };
        return pump(a, b, a_base, b_base) && pump(b, a, b_base, a_base);
    }
    bool settle() {
        for (std::uint64_t real = 0; real <= 1'000'000U; real += 10'000U)
            if (!step(real)) return false;
        return true;
    }
};

bool packet_codec_is_bounded_and_checked() {
    auto m = heartbeat(1);
    m.kind = Kind::cue; m.sequence = 0xffffffffU; m.payload_size = kMaximumCueBytes;
    m.sent_us = 1'000'000; m.deadline_us = 1'500'000;
    for (std::size_t i = 0; i < m.payload.size(); ++i) m.payload[i] = static_cast<std::byte>(i);
    std::array<std::byte, kMaximumPacketBytes> packet{};
    const auto result = encode(m, packet);
    CHECK(result && result.value() == kMaximumPacketBytes);
    CHECK(packet[0] == std::byte{'B'} && packet[2] == std::byte{2});
    CHECK(packet[4] == std::byte{77} && packet[5] == std::byte{0});
    Message decoded{};
    CHECK(decode(packet, decoded));
    CHECK(decoded.sender == m.sender && decoded.sequence == m.sequence && decoded.payload == m.payload);
    CHECK(!decode(std::span{packet}.first(kHeaderBytes - 1U), decoded));
    CHECK(!encode(m, std::span{packet}.first(packet.size() - 1U)));
    packet.back() ^= std::byte{1}; CHECK(!decode(packet, decoded));
    m.payload_size = 0; CHECK(!encode(m, packet));
    m = heartbeat(1); m.session = 0; CHECK(!encode(m, packet));
    return true;
}

bool election_is_deterministic_and_recovers() {
    Engine engine{};
    CHECK(!engine.start(0, 3, 1, 0));
    CHECK(engine.start(77, 3, 1, 0));
    CHECK(feed(engine, heartbeat(2), 0));
    CHECK(feed(engine, heartbeat(1), 0));
    CHECK(engine.leader() == 1 && engine.members() == 3);
    CHECK(feed(engine, heartbeat(2), 1'000'000));
    engine.service(kMemberTimeoutUs);
    CHECK(engine.leader() == 2 && engine.members() == 2 && !engine.synchronized());
    engine.service(3'000'000);
    CHECK(engine.is_leader() && engine.synchronized() && engine.members() == 1);
    CHECK(feed(engine, heartbeat(1, 55), 3'010'000));
    CHECK(engine.leader() == 1 && engine.leader_session() == 55);
    CHECK(engine.metrics().leader_changes == 5);
    return true;
}

bool clocks_align_different_uptimes() {
    Pair pair{}; CHECK(pair.start()); CHECK(pair.settle());
    CHECK(pair.a.is_leader() && !pair.b.is_leader() && pair.b.synchronized());
    CHECK(pair.b.offset_us() == -3'000'000);
    CHECK(pair.b.uncertainty_us() == 1000);
    CHECK(pair.a.time_us(Pair::a_base + 1'010'000) == pair.b.time_us(Pair::b_base + 1'010'000));
    auto foreign = heartbeat(3); foreign.fleet_id = 78;
    CHECK(!feed(pair.b, foreign, Pair::b_base + 1'020'000));
    CHECK(pair.b.leader() == 1);
    return true;
}

bool lost_cue_and_ack_retry_without_duplicate_execution() {
    Pair pair{}; CHECK(pair.start()); CHECK(pair.settle());
    const std::array command{std::byte{0x42}, std::byte{0x17}};
    CHECK(!pair.b.schedule(command, 500'000, Pair::b_base + 1'010'000));
    const auto scheduled = pair.a.schedule(command, 500'000, Pair::a_base + 1'010'000);
    CHECK(scheduled);
    CHECK(pair.step(1'010'000, false)); CHECK(pair.b.pending() == 0);
    CHECK(pair.step(1'110'000, true, false)); CHECK(pair.b.pending() == 1);
    CHECK(pair.step(1'210'000)); CHECK(pair.b.metrics().duplicates == 1);
    Cue a{}, b{};
    CHECK(!pair.a.take_due(Pair::a_base + 1'500'000, a));
    CHECK(!pair.b.take_due(Pair::b_base + 1'500'000, b));
    CHECK(pair.a.take_due(Pair::a_base + 1'510'000, a));
    CHECK(pair.b.take_due(Pair::b_base + 1'510'000, b));
    CHECK(a.sequence == scheduled.value() && b.sequence == a.sequence);
    CHECK(a.bytes[0] == command[0] && b.bytes[1] == command[1]);
    CHECK(!pair.a.take_due(Pair::a_base + 1'510'000, a));
    CHECK(!pair.b.take_due(Pair::b_base + 1'510'000, b));
    CHECK(pair.b.metrics().scheduled == 1 && pair.b.metrics().due == 1);
    return true;
}

bool leader_reboot_cancels_old_cues_and_clock() {
    Pair pair{}; CHECK(pair.start()); CHECK(pair.settle());
    const std::array command{std::byte{1}};
    CHECK(pair.a.schedule(command, 500'000, Pair::a_base + 1'010'000));
    CHECK(pair.step(1'010'000)); CHECK(pair.b.pending() == 1);
    CHECK(feed(pair.b, heartbeat(1, 12), Pair::b_base + 1'020'000));
    CHECK(pair.b.pending() == 0 && !pair.b.synchronized());
    CHECK(pair.b.metrics().cancelled == 1 && pair.b.leader_session() == 12);
    CHECK(!feed(pair.b, heartbeat(1, 11), Pair::b_base + 1'025'000));
    CHECK(pair.b.leader_session() == 12);
    auto stale = heartbeat(1, 11); stale.kind = Kind::cue; stale.sequence = 1;
    stale.payload_size = 1; stale.deadline_us = Pair::a_base + 1'510'000;
    CHECK(!feed(pair.b, stale, Pair::b_base + 1'030'000));
    Cue output{};
    CHECK(!pair.b.take_due(Pair::b_base + 1'510'000, output));
    return true;
}

bool membership_and_cue_queues_are_bounded() {
    Engine engine{}; CHECK(engine.start(77, 1, 1, 0));
    for (std::uint64_t node = 2; node <= kMaximumMembers; ++node)
        CHECK(feed(engine, heartbeat(node), 800'000));
    CHECK(engine.members() == kMaximumMembers);
    CHECK(!feed(engine, heartbeat(17), 800'000));
    const std::array command{std::byte{1}};
    for (unsigned i = 0; i < kMaximumCues; ++i) CHECK(engine.schedule(command, 100'000, 800'000));
    CHECK(!engine.schedule(command, 100'000, 800'000));
    engine.cancel_all(); CHECK(engine.pending() == 0 && engine.metrics().cancelled == kMaximumCues);
    return true;
}

bool late_cues_do_not_execute_and_deadlines_are_ordered() {
    Engine engine{}; CHECK(engine.start(77, 1, 1, 0));
    const std::array command{std::byte{1}};
    const auto later = engine.schedule(command, 500'000, 800'000);
    const auto sooner = engine.schedule(command, 100'000, 800'000);
    CHECK(later && sooner);
    Cue output{}; CHECK(engine.take_due(900'000, output)); CHECK(output.sequence == sooner.value());
    CHECK(!engine.take_due(1'321'000, output)); CHECK(engine.pending() == 0 && engine.metrics().late == 1);
    CHECK(!engine.schedule(command, 99'999, 1'400'000));
    CHECK(!engine.schedule(command, kMaximumCueDelayUs + 1U, 1'400'000));
    CHECK(engine.schedule(command, 100'000, 1'500'000));
    const auto fresh = engine.schedule(command, 200'000, 1'500'000);
    CHECK(fresh && engine.take_due(1'700'000, output));
    CHECK(output.sequence == fresh.value() && engine.metrics().late == 2);
    return true;
}

bool reordered_cues_and_conflicting_duplicates_are_checked() {
    Pair pair{}; CHECK(pair.start()); CHECK(pair.settle());
    const std::array command{std::byte{1}};
    CHECK(pair.a.schedule(command, 500'000, Pair::a_base + 1'010'000));
    CHECK(pair.a.schedule(command, 600'000, Pair::a_base + 1'010'000));
    Message first{}, second{};
    CHECK(pair.a.next_message(Pair::a_base + 1'010'000, first));
    CHECK(pair.a.next_message(Pair::a_base + 1'010'000, second));
    CHECK(first.kind == Kind::cue && second.kind == Kind::cue);
    CHECK(feed(pair.b, second, Pair::b_base + 1'011'000));
    CHECK(feed(pair.b, first, Pair::b_base + 1'012'000));
    CHECK(pair.b.pending() == 2);
    first.payload[0] = std::byte{2}; CHECK(!feed(pair.b, first, Pair::b_base + 1'013'000));
    Cue cue{}; CHECK(pair.b.take_due(Pair::b_base + 1'510'000, cue));
    CHECK(cue.sequence == first.sequence && cue.bytes[0] == std::byte{1});
    first.payload[0] = std::byte{1}; CHECK(feed(pair.b, first, Pair::b_base + 1'511'000));
    CHECK(pair.b.pending() == 1 && pair.b.metrics().duplicates == 1);
    return true;
}

bool lost_clock_samples_cancel_pending_without_changing_live_leader() {
    Pair pair{}; CHECK(pair.start()); CHECK(pair.settle());
    const std::array command{std::byte{1}};
    CHECK(pair.a.schedule(command, 5'000'000, Pair::a_base + 1'010'000));
    CHECK(pair.step(1'010'000)); CHECK(pair.b.pending() == 1);
    CHECK(feed(pair.b, heartbeat(1, 11), Pair::b_base + 2'500'000));
    CHECK(pair.b.sample_age_us(Pair::b_base + 3'020'000) >= kMemberTimeoutUs);
    pair.b.service(Pair::b_base + 3'020'000);
    CHECK(pair.b.leader() == 1 && !pair.b.synchronized());
    CHECK(pair.b.pending() == 0 && pair.b.metrics().cancelled == 1);
    return true;
}

bool leader_loss_cancels_cues_and_new_leader_can_schedule() {
    Pair pair{}; CHECK(pair.start()); CHECK(pair.settle());
    const std::array command{std::byte{1}};
    CHECK(pair.a.schedule(command, 5'000'000, Pair::a_base + 1'010'000));
    CHECK(pair.step(1'010'000)); CHECK(pair.b.pending() == 1);
    pair.b.service(Pair::b_base + 3'100'000);
    CHECK(pair.b.is_leader() && pair.b.synchronized() && pair.b.pending() == 0);
    CHECK(pair.b.metrics().cancelled == 1);
    CHECK(pair.b.schedule(command, 100'000, Pair::b_base + 3'900'000));
    Cue output{}; CHECK(pair.b.take_due(Pair::b_base + 4'000'000, output));
    return true;
}

bool slow_and_replayed_clock_replies_are_rejected() {
    Pair pair{}; CHECK(pair.start()); CHECK(pair.step(0));
    Message probe{}, reply{}, unused{}; bool has_reply{};
    CHECK(pair.b.next_message(Pair::b_base + 250'000, probe));
    CHECK(probe.kind == Kind::heartbeat);
    CHECK(pair.b.next_message(Pair::b_base + 250'000, probe));
    CHECK(probe.kind == Kind::probe);
    CHECK(pair.a.receive(probe, Pair::a_base + 251'000, reply, has_reply) && has_reply);
    CHECK(!feed(pair.b, reply, Pair::b_base + 270'000));
    CHECK(pair.b.metrics().clock_samples == 0);
    CHECK(pair.b.next_message(Pair::b_base + 500'000, probe));
    CHECK(probe.kind == Kind::heartbeat);
    CHECK(pair.b.next_message(Pair::b_base + 500'000, probe));
    CHECK(pair.a.receive(probe, Pair::a_base + 501'000, reply, has_reply) && has_reply);
    CHECK(pair.b.receive(reply, Pair::b_base + 502'000, unused, has_reply));
    CHECK(!feed(pair.b, reply, Pair::b_base + 503'000));
    CHECK(pair.b.metrics().clock_samples == 1);
    return true;
}
} // namespace

int main() {
    const std::array tests{packet_codec_is_bounded_and_checked, election_is_deterministic_and_recovers,
        clocks_align_different_uptimes, lost_cue_and_ack_retry_without_duplicate_execution,
        leader_reboot_cancels_old_cues_and_clock, membership_and_cue_queues_are_bounded,
        late_cues_do_not_execute_and_deadlines_are_ordered,
        reordered_cues_and_conflicting_duplicates_are_checked,
        lost_clock_samples_cancel_pending_without_changing_live_leader,
        leader_loss_cancels_cues_and_new_leader_can_schedule,
        slow_and_replayed_clock_replies_are_rejected};
    for (auto test : tests) if (!test()) return 1;
    std::cout << "PASS blip_fleet_tests " << tests.size() << " cases\n";
}
