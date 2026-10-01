#include "blip/fleet/fleet.hpp"

#include <algorithm>
#include <limits>

namespace blip::fleet {
namespace {
constexpr auto kMaximumTime = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
constexpr std::uint64_t kMaximumNode = 0xffffffffffffULL;
core::Error error(core::ErrorCode code, const char* operation, const char* detail) noexcept {
    return {core::ErrorDomain::transport, code, "blip.fleet", operation, detail};
}
void increment(std::uint32_t& value) noexcept {
    if (value != std::numeric_limits<std::uint32_t>::max()) ++value;
}
void write(std::span<std::byte> bytes, std::size_t at, std::uint64_t value,
           std::size_t count) noexcept {
    for (std::size_t i = 0; i < count; ++i) bytes[at + i] = static_cast<std::byte>(value >> (i * 8U));
}
std::uint64_t read(std::span<const std::byte> bytes, std::size_t at, std::size_t count) noexcept {
    std::uint64_t value{};
    for (std::size_t i = 0; i < count; ++i)
        value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[at + i])) << (i * 8U);
    return value;
}
std::uint32_t checksum(std::span<const std::byte> bytes) noexcept {
    std::uint32_t crc = 0xffffffffU;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i >= 72U && i < kHeaderBytes) continue;
        crc ^= std::to_integer<std::uint8_t>(bytes[i]);
        for (unsigned bit = 0; bit < 8U; ++bit)
            crc = (crc >> 1U) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}
bool valid(const Message& m) noexcept {
    const auto kind = static_cast<std::uint8_t>(m.kind);
    return kind >= 1U && kind <= 5U && m.fleet_id != 0U &&
           m.sender != 0U && m.sender <= kMaximumNode && m.session != 0U &&
           m.leader != 0U && m.leader <= kMaximumNode && m.leader_session != 0U &&
           (m.kind == Kind::heartbeat || m.sequence != 0U) &&
           m.sent_us <= kMaximumTime && m.request_us <= kMaximumTime &&
           m.received_us <= kMaximumTime && m.deadline_us <= kMaximumTime &&
           (m.kind == Kind::cue ? m.payload_size > 0U && m.payload_size <= kMaximumCueBytes
                                : m.payload_size == 0U);
}
} // namespace

core::Result<std::size_t> encode(const Message& m, std::span<std::byte> output) noexcept {
    if (!valid(m)) return core::Result<std::size_t>::failure(error(core::ErrorCode::invalid_argument, "encode", "message"));
    const auto size = kHeaderBytes + m.payload_size;
    if (output.size() < size) return core::Result<std::size_t>::failure(error(core::ErrorCode::serialization_overflow, "encode", "buffer"));
    output = output.first(size);
    std::fill(output.begin(), output.end(), std::byte{0});
    output[0] = std::byte{'B'}; output[1] = std::byte{'F'};
    output[2] = std::byte{2}; output[3] = static_cast<std::byte>(m.kind);
    write(output, 4, m.fleet_id, 4); write(output, 8, m.sender, 8);
    write(output, 16, m.session, 4); write(output, 20, m.leader_session, 4);
    write(output, 24, m.leader, 8); write(output, 32, m.sequence, 4);
    write(output, 36, m.payload_size, 2);
    write(output, 40, m.sent_us, 8); write(output, 48, m.request_us, 8);
    write(output, 56, m.received_us, 8); write(output, 64, m.deadline_us, 8);
    std::copy_n(m.payload.begin(), m.payload_size, output.begin() + kHeaderBytes);
    write(output, 72, checksum(output), 4);
    return core::Result<std::size_t>::success(size);
}

core::Status decode(std::span<const std::byte> bytes, Message& output) noexcept {
    if (bytes.size() < kHeaderBytes || bytes.size() > kMaximumPacketBytes ||
        bytes[0] != std::byte{'B'} || bytes[1] != std::byte{'F'} || bytes[2] != std::byte{2} ||
        read(bytes, 38, 2) != 0U || read(bytes, 72, 4) != checksum(bytes))
        return core::Status::failure(error(core::ErrorCode::corrupt_data, "decode", "header-or-checksum"));
    Message m{};
    m.kind = static_cast<Kind>(std::to_integer<std::uint8_t>(bytes[3]));
    m.fleet_id = static_cast<std::uint32_t>(read(bytes, 4, 4)); m.sender = read(bytes, 8, 8);
    m.session = static_cast<std::uint32_t>(read(bytes, 16, 4));
    m.leader_session = static_cast<std::uint32_t>(read(bytes, 20, 4)); m.leader = read(bytes, 24, 8);
    m.sequence = static_cast<std::uint32_t>(read(bytes, 32, 4));
    m.payload_size = static_cast<std::uint16_t>(read(bytes, 36, 2));
    m.sent_us = read(bytes, 40, 8); m.request_us = read(bytes, 48, 8);
    m.received_us = read(bytes, 56, 8); m.deadline_us = read(bytes, 64, 8);
    if (!valid(m) || bytes.size() != kHeaderBytes + m.payload_size)
        return core::Status::failure(error(core::ErrorCode::corrupt_data, "decode", "message-or-length"));
    std::copy(bytes.begin() + kHeaderBytes, bytes.end(), m.payload.begin());
    output = m;
    return core::Status::success();
}

core::Status Engine::start(std::uint32_t fleet, std::uint64_t node,
                           std::uint32_t session, std::uint64_t now) noexcept {
    if (fleet == 0U || node == 0U || node > kMaximumNode || session == 0U ||
        now > kMaximumTime - kMaximumCueDelayUs)
        return core::Status::failure(error(core::ErrorCode::invalid_argument, "start", "identity-or-time"));
    *this = Engine{};
    fleet_id_ = fleet; node_ = node; session_ = session;
    leader_ = node; leader_session_ = session;
    synchronized_ = true; clock_initialized_ = true; last_time_us_ = now;
    heartbeat_due_us_ = now; ready_us_ = now + 750'000U;
    return core::Status::success();
}

std::size_t Engine::members() const noexcept {
    return node_ == 0U ? 0U : 1U + static_cast<std::size_t>(std::count_if(members_.begin(), members_.end(),
                                                     [](const Member& m) { return m.node != 0U; }));
}
std::size_t Engine::pending() const noexcept {
    return static_cast<std::size_t>(std::count_if(cues_.begin(), cues_.end(),
                                                [](const Pending& p) { return p.occupied; }));
}
void Engine::cancel_all() noexcept {
    for (auto& pending : cues_) if (pending.occupied) {
        increment(metrics_.cancelled); pending = {};
    }
}
void Engine::elect(std::uint64_t now) noexcept {
    auto leader = node_;
    auto session = session_;
    for (auto& member : members_) {
        if (member.node != 0U && now >= member.seen_us && now - member.seen_us >= kMemberTimeoutUs)
            member = {};
        if (member.node != 0U && member.node < leader) {
            leader = member.node; session = member.session;
        }
    }
    if (leader == leader_ && session == leader_session_) return;
    cancel_all(); increment(metrics_.leader_changes);
    leader_ = leader; leader_session_ = session;
    synchronized_ = is_leader(); clock_initialized_ = is_leader(); offset_us_ = 0; good_samples_ = 0;
    uncertainty_us_ = 0; last_time_us_ = now; last_sample_us_ = now;
    probe_sequence_ = 0; probe_due_us_ = now;
    ready_us_ = now + 750'000U; cue_seen_ = 0; cue_highest_ = 0;
}
void Engine::service(std::uint64_t now) noexcept {
    if (node_ == 0U) return;
    elect(now);
    if (!is_leader() && synchronized_ && now >= last_sample_us_ &&
        now - last_sample_us_ >= kMemberTimeoutUs) {
        synchronized_ = false; good_samples_ = 0; cancel_all();
    }
}
std::uint64_t Engine::time_us(std::uint64_t local) noexcept {
    std::uint64_t adjusted{};
    if (offset_us_ >= 0) {
        const auto offset = static_cast<std::uint64_t>(offset_us_);
        adjusted = local > kMaximumTime - offset ? kMaximumTime : local + offset;
    } else {
        const auto offset = static_cast<std::uint64_t>(-offset_us_);
        adjusted = local < offset ? 0U : local - offset;
    }
    last_time_us_ = std::max(last_time_us_, adjusted);
    return last_time_us_;
}
void Engine::base(Kind kind, Message& m, std::uint64_t now) noexcept {
    m = {}; m.kind = kind; m.fleet_id = fleet_id_; m.sender = node_; m.session = session_;
    m.leader = leader_; m.leader_session = leader_session_; m.sent_us = time_us(now);
}
std::uint32_t Engine::next_sequence() noexcept {
    if (++sequence_ == 0U) ++sequence_;
    return sequence_;
}
core::Status Engine::reject(const char* reason) noexcept {
    increment(metrics_.rejected);
    return core::Status::failure(error(core::ErrorCode::validation_failed, "receive", reason));
}

core::Status Engine::receive(const Message& m, std::uint64_t now, Message& reply,
                              bool& has_reply) noexcept {
    has_reply = false;
    service(now);
    if (node_ == 0U || now > kMaximumTime || !valid(m) || m.fleet_id != fleet_id_)
        return reject("fleet-or-message");
    if (m.sender == node_) return m.session == session_ ? core::Status::success() : reject("identity-collision");
    if (m.kind == Kind::heartbeat) {
        auto member = std::find_if(members_.begin(), members_.end(), [&](const Member& v) { return v.node == m.sender; });
        if (member == members_.end())
            member = std::find_if(members_.begin(), members_.end(), [](const Member& v) { return v.node == 0U; });
        if (member == members_.end()) return reject("member-capacity");
        if (member->session != 0U && member->session != m.session) {
            if (std::find(member->retired_sessions.begin(), member->retired_sessions.end(),
                          m.session) != member->retired_sessions.end())
                return reject("retired-member-session");
            member->retired_sessions[member->retired_index] = member->session;
            member->retired_index = static_cast<std::uint8_t>((member->retired_index + 1U) % 4U);
        }
        member->node = m.sender; member->session = m.session; member->seen_us = now;
        elect(now);
        return core::Status::success();
    }
    if (m.leader != leader_ || m.leader_session != leader_session_) return reject("leader-epoch");
    if (m.kind == Kind::probe) {
        if (!is_leader()) return reject("not-leader");
        base(Kind::reply, reply, now); reply.sequence = m.sequence;
        reply.request_us = m.request_us; reply.received_us = time_us(now);
        has_reply = true;
        return core::Status::success();
    }
    if (m.kind == Kind::reply) {
        if (is_leader() || m.sender != leader_ || m.session != leader_session_ ||
            m.sequence != probe_sequence_ || m.request_us != probe_sent_us_ ||
            now < m.request_us || m.sent_us < m.received_us)
            return reject("clock-reply");
        const auto elapsed = now - m.request_us;
        const auto residence = m.sent_us - m.received_us;
        if (elapsed < residence || elapsed - residence > kMaximumRoundTripUs)
            return reject("clock-round-trip");
        const auto local_mid = m.request_us + elapsed / 2U;
        const auto remote_mid = m.received_us + residence / 2U;
        const auto sample = static_cast<std::int64_t>(remote_mid) - static_cast<std::int64_t>(local_mid);
        // The first sample establishes this epoch's time domain; later samples
        // slew by 1/8. time_us() prevents a backward tick within the epoch.
        if (!clock_initialized_) { offset_us_ = sample; last_time_us_ = 0; clock_initialized_ = true; }
        else offset_us_ += sample / 8 - offset_us_ / 8;
        uncertainty_us_ = (elapsed - residence + 1U) / 2U;
        last_sample_us_ = now; probe_sequence_ = 0;
        if (good_samples_ < 3U) ++good_samples_;
        synchronized_ = good_samples_ >= 3U;
        increment(metrics_.clock_samples);
        return core::Status::success();
    }
    if (m.kind == Kind::ack) {
        if (!is_leader()) return reject("not-leader");
        const auto member = std::find_if(members_.begin(), members_.end(), [&](const Member& v) {
            return v.node == m.sender && v.session == m.session;
        });
        if (member == members_.end()) return reject("unknown-ack-member");
        const auto index = static_cast<std::size_t>(member - members_.begin());
        for (auto& p : cues_) if (p.occupied && p.cue.sequence == m.sequence) {
            p.awaiting &= static_cast<std::uint16_t>(~(1U << index));
            return core::Status::success();
        }
        return reject("unknown-ack-cue");
    }
    if (m.sender != leader_ || m.session != leader_session_ || !synchronized_)
        return reject("cue-source-or-clock");
    const auto clock = time_us(now);
    const auto forward = m.sequence - cue_highest_;
    const auto age = cue_highest_ - m.sequence;
    const bool newer = cue_seen_ == 0U || (forward != 0U && forward < 0x80000000U);
    if (!newer && (age >= 32U || (cue_seen_ & (1U << age)) != 0U)) {
        if (age >= 32U) return reject("stale-cue");
        for (const auto& p : cues_) if (p.occupied && p.cue.sequence == m.sequence &&
            (p.cue.deadline_us != m.deadline_us || p.cue.size != m.payload_size ||
             !std::equal(m.payload.begin(), m.payload.begin() + m.payload_size, p.cue.bytes.begin())))
            return reject("contradictory-cue");
        increment(metrics_.duplicates);
    } else {
        if ((m.deadline_us < clock && clock - m.deadline_us > kMaximumCueLatenessUs) ||
            (m.deadline_us >= clock && m.deadline_us - clock > kMaximumCueDelayUs))
            return reject("cue-deadline");
        auto pending = std::find_if(cues_.begin(), cues_.end(), [](const Pending& p) { return !p.occupied; });
        if (pending == cues_.end()) return reject("cue-capacity");
        *pending = {}; pending->occupied = true;
        pending->cue = {m.sequence, m.deadline_us, m.payload_size, m.payload};
        if (newer) {
            cue_seen_ = cue_seen_ == 0U || forward >= 32U ? 1U : (cue_seen_ << forward) | 1U;
            cue_highest_ = m.sequence;
        } else cue_seen_ |= 1U << age;
        increment(metrics_.scheduled);
    }
    base(Kind::ack, reply, now); reply.sequence = m.sequence; has_reply = true;
    return core::Status::success();
}

bool Engine::next_message(std::uint64_t now, Message& output) noexcept {
    service(now);
    if (node_ == 0U) return false;
    if (now >= heartbeat_due_us_) {
        base(Kind::heartbeat, output, now); heartbeat_due_us_ = now + kHeartbeatUs;
        return true;
    }
    if (!is_leader() && now >= probe_due_us_) {
        base(Kind::probe, output, now); output.sequence = next_sequence();
        output.request_us = now; probe_sent_us_ = now; probe_sequence_ = output.sequence;
        probe_due_us_ = now + kHeartbeatUs;
        return true;
    }
    if (is_leader()) for (auto& p : cues_) {
        if (p.occupied && p.transmit && now >= p.next_send_us && time_us(now) < p.cue.deadline_us) {
            base(Kind::cue, output, now); output.sequence = p.cue.sequence;
            output.deadline_us = p.cue.deadline_us; output.payload_size = p.cue.size;
            output.payload = p.cue.bytes;
            p.next_send_us = now + 100'000U;
            if (p.awaiting == 0U) p.transmit = false;
            return true;
        }
    }
    return false;
}

core::Result<std::uint32_t> Engine::schedule(std::span<const std::byte> command,
                                            std::uint64_t delay, std::uint64_t now) noexcept {
    service(now);
    if (node_ == 0U || !is_leader() || now < ready_us_)
        return core::Result<std::uint32_t>::failure(error(core::ErrorCode::invalid_state, "schedule", "leader-not-ready"));
    if (command.empty() || command.size() > kMaximumCueBytes || delay < 100'000U ||
        delay > kMaximumCueDelayUs || time_us(now) > kMaximumTime - delay)
        return core::Result<std::uint32_t>::failure(error(core::ErrorCode::invalid_argument, "schedule", "command-or-delay"));
    auto p = std::find_if(cues_.begin(), cues_.end(), [](const Pending& v) { return !v.occupied; });
    if (p == cues_.end()) return core::Result<std::uint32_t>::failure(error(core::ErrorCode::queue_full, "schedule", "cue-capacity"));
    *p = {}; p->occupied = true; p->transmit = true; p->next_send_us = now;
    if (++cue_sequence_ == 0U) ++cue_sequence_;
    p->cue.sequence = cue_sequence_; p->cue.deadline_us = time_us(now) + delay;
    p->cue.size = static_cast<std::uint16_t>(command.size());
    std::copy(command.begin(), command.end(), p->cue.bytes.begin());
    for (std::size_t i = 0; i < members_.size(); ++i) if (members_[i].node != 0U)
        p->awaiting |= static_cast<std::uint16_t>(1U << i);
    increment(metrics_.scheduled);
    return core::Result<std::uint32_t>::success(cue_sequence_);
}

bool Engine::take_due(std::uint64_t now, Cue& output) noexcept {
    service(now);
    if (!synchronized_) return false;
    const auto clock = time_us(now);
    // Drain expired entries too, so they cannot hold up another due cue.
    for (;;) {
        Pending* chosen{};
        for (auto& p : cues_) if (p.occupied && p.cue.deadline_us <= clock &&
            (chosen == nullptr || p.cue.deadline_us < chosen->cue.deadline_us)) chosen = &p;
        if (chosen == nullptr) return false;
        if (clock - chosen->cue.deadline_us > kMaximumCueLatenessUs) {
            *chosen = {}; increment(metrics_.late);
            continue;
        }
        output = chosen->cue; *chosen = {}; increment(metrics_.due);
        return true;
    }
}

} // namespace blip::fleet
