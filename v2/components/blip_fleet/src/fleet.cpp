#include "blip/fleet/fleet.hpp"

#include <algorithm>
#include <limits>

namespace blip::fleet {
namespace {
constexpr auto kMaximumTime = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
constexpr std::uint64_t kMaximumNode = 0xffffffffffffULL;
core::Error error(core::ErrorCode code, const char* operation, const char* detail) {
    return {core::ErrorDomain::transport, code, "blip.fleet", operation, detail};
}
void increment(std::uint32_t& value) { if (value != UINT32_MAX) ++value; }
void write(std::span<std::byte> bytes, std::size_t at, std::uint64_t value, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) bytes[at + i] = static_cast<std::byte>(value >> (i * 8U));
}
std::uint64_t read(std::span<const std::byte> bytes, std::size_t at, std::size_t count) {
    std::uint64_t value{};
    for (std::size_t i = 0; i < count; ++i) value |= std::to_integer<std::uint64_t>(bytes[at + i]) << (i * 8U);
    return value;
}
std::uint32_t checksum(std::span<const std::byte> bytes) {
    std::uint32_t crc = UINT32_MAX;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i >= 44U && i < kHeaderBytes) continue;
        crc ^= std::to_integer<std::uint8_t>(bytes[i]);
        for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1U) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}
bool newer(std::uint32_t value, std::uint32_t previous) { return value != previous && value - previous < 0x80000000U; }
bool valid(const Message& m) {
    return (m.kind == Kind::beacon || m.kind == Kind::cue) && m.fleet_id != 0U &&
        m.sender > 0U && m.sender <= kMaximumNode && m.session != 0U && m.sequence != 0U &&
        m.sent_us <= kMaximumTime && m.deadline_us <= kMaximumTime &&
        (m.kind == Kind::cue ? m.payload_size > 0U && m.payload_size <= kMaximumCueBytes : m.payload_size == 0U);
}
}

core::Result<std::size_t> encode(const Message& m, std::span<std::byte> output) noexcept {
    if (!valid(m)) return core::Result<std::size_t>::failure(error(core::ErrorCode::invalid_argument, "encode", "message"));
    const auto size = kHeaderBytes + m.payload_size;
    if (output.size() < size) return core::Result<std::size_t>::failure(error(core::ErrorCode::serialization_overflow, "encode", "buffer"));
    output = output.first(size);
    std::fill(output.begin(), output.end(), std::byte{});
    output[0] = std::byte{'B'}; output[1] = std::byte{'F'}; output[2] = std::byte{2}; output[3] = static_cast<std::byte>(m.kind);
    write(output, 4, m.fleet_id, 4); write(output, 8, m.sender, 8); write(output, 16, m.session, 4);
    write(output, 20, m.sequence, 4); write(output, 24, m.sent_us, 8); write(output, 32, m.deadline_us, 8);
    write(output, 40, m.payload_size, 2);
    std::copy_n(m.payload.begin(), m.payload_size, output.begin() + kHeaderBytes);
    write(output, 44, checksum(output), 4);
    return core::Result<std::size_t>::success(size);
}
core::Status decode(std::span<const std::byte> bytes, Message& output) noexcept {
    if (bytes.size() < kHeaderBytes || bytes.size() > kMaximumPacketBytes || bytes[0] != std::byte{'B'} ||
        bytes[1] != std::byte{'F'} || bytes[2] != std::byte{2} || read(bytes, 42, 2) != 0U || read(bytes, 44, 4) != checksum(bytes))
        return core::Status::failure(error(core::ErrorCode::corrupt_data, "decode", "header-or-checksum"));
    Message m{};
    m.kind = static_cast<Kind>(std::to_integer<std::uint8_t>(bytes[3]));
    m.fleet_id = static_cast<std::uint32_t>(read(bytes, 4, 4)); m.sender = read(bytes, 8, 8);
    m.session = static_cast<std::uint32_t>(read(bytes, 16, 4)); m.sequence = static_cast<std::uint32_t>(read(bytes, 20, 4));
    m.sent_us = read(bytes, 24, 8); m.deadline_us = read(bytes, 32, 8); m.payload_size = static_cast<std::uint16_t>(read(bytes, 40, 2));
    if (!valid(m) || bytes.size() != kHeaderBytes + m.payload_size)
        return core::Status::failure(error(core::ErrorCode::corrupt_data, "decode", "message-or-length"));
    std::copy(bytes.begin() + kHeaderBytes, bytes.end(), m.payload.begin());
    output = m;
    return core::Status::success();
}

core::Status Engine::start(std::uint32_t fleet, std::uint64_t node, std::uint32_t session, std::uint64_t now) noexcept {
    if (fleet == 0U || node == 0U || node > kMaximumNode || session == 0U || now > kMaximumTime - kMaximumCueDelayUs)
        return core::Status::failure(error(core::ErrorCode::invalid_argument, "start", "identity-or-time"));
    *this = Engine{};
    fleet_id_ = fleet; node_ = node; session_ = session;
    election_due_us_ = now + election_delay();
    return core::Status::success();
}
std::uint64_t Engine::election_delay() const noexcept {
    // Deterministic jitter per incarnation reduces simultaneous startup/failover
    // broadcasts. Lower identities can challenge a higher leader after hearing it.
    const auto hash = node_ ^ (node_ >> 23U) ^ (static_cast<std::uint64_t>(session_) * 0x9e3779b9U);
    return 20'000U + hash % 480'000U;
}
void Engine::cancel_all() noexcept {
    for (auto& cue : cues_) if (cue.occupied) { increment(metrics_.cancelled); cue = {}; }
}
std::size_t Engine::pending() const noexcept {
    return static_cast<std::size_t>(std::count_if(cues_.begin(), cues_.end(), [](const auto& c) { return c.occupied; }));
}
void Engine::adopt(std::uint64_t node, std::uint32_t session, std::uint64_t now) noexcept {
    if (leader_ == node && leader_session_ == session) return;
    if (leader_ == node && leader_session_ != 0U) {
        retired_[retired_index_++ % retired_.size()] = {node, leader_session_};
    }
    cancel_all(); increment(metrics_.leader_changes);
    leader_ = node; leader_session_ = session; leader_seen_us_ = now;
    received_beacon_ = 0;
    sample_count_ = 0; sample_index_ = 0; offset_us_ = 0;
    synchronized_ = is_leader(); clock_initialized_ = is_leader();
    last_time_us_ = now; last_sample_us_ = now; uncertainty_us_ = 0;
    heartbeat_due_us_ = now; ready_us_ = now + 750'000U;
}
void Engine::service(std::uint64_t now) noexcept {
    if (node_ == 0U) return;
    if (leader_ != 0U && !is_leader() && now >= leader_seen_us_ && now - leader_seen_us_ >= kLeaderTimeoutUs) {
        cancel_all(); leader_ = 0; leader_session_ = 0; synchronized_ = false;
        election_due_us_ = now + election_delay();
    }
    if (leader_ == 0U && now >= election_due_us_) adopt(node_, session_, now);
    if (!is_leader() && synchronized_ && now >= last_sample_us_ && now - last_sample_us_ >= kLeaderTimeoutUs) {
        synchronized_ = false; sample_count_ = 0; sample_index_ = 0; cancel_all();
    }
}
std::uint64_t Engine::time_us(std::uint64_t local) noexcept {
    const auto adjusted = offset_us_ >= 0
        ? local > kMaximumTime - static_cast<std::uint64_t>(offset_us_) ? kMaximumTime : local + static_cast<std::uint64_t>(offset_us_)
        : local < static_cast<std::uint64_t>(-offset_us_) ? 0U : local - static_cast<std::uint64_t>(-offset_us_);
    last_time_us_ = std::max(last_time_us_, adjusted);
    return last_time_us_;
}
void Engine::base(Kind kind, Message& m, std::uint64_t now) noexcept {
    m = {}; m.kind = kind; m.fleet_id = fleet_id_; m.sender = node_; m.session = session_; m.sent_us = time_us(now);
}
core::Status Engine::reject(const char* detail) noexcept {
    increment(metrics_.rejected);
    return core::Status::failure(error(core::ErrorCode::validation_failed, "receive", detail));
}
core::Status Engine::receive(const Message& m, std::uint64_t now) noexcept {
    service(now);
    if (!valid(m) || m.fleet_id != fleet_id_ || node_ == 0U || now > kMaximumTime) return reject("fleet-or-message");
    if (m.sender == node_) return reject("self-or-identity-collision");
    if (m.kind == Kind::beacon) {
        if (std::any_of(retired_.begin(), retired_.end(), [&](const auto& r) { return r.node == m.sender && r.session == m.session; }))
            return reject("retired-session");
        if (m.sender > node_ || (leader_ != 0U && leader_ < m.sender)) return reject("higher-leader");
        adopt(m.sender, m.session, now);
        if (received_beacon_ != 0U && !newer(m.sequence, received_beacon_)) return reject("stale-beacon");
        received_beacon_ = m.sequence; leader_seen_us_ = now;
        const auto sample = static_cast<std::int64_t>(m.sent_us) - static_cast<std::int64_t>(now);
        if (sample_count_ > 0U) {
            const auto best = *std::max_element(samples_.begin(), samples_.begin() + sample_count_);
            if (sample < best - static_cast<std::int64_t>(2U * kClockTransportBudgetUs)) return reject("delayed-beacon");
        } else if (clock_initialized_ && sample < offset_us_ - static_cast<std::int64_t>(2U * kClockTransportBudgetUs))
            return reject("delayed-beacon");
        samples_[sample_index_++ % samples_.size()] = sample;
        if (sample_count_ < samples_.size()) ++sample_count_;
        const auto best = *std::max_element(samples_.begin(), samples_.begin() + sample_count_);
        if (!clock_initialized_) { offset_us_ = best; last_time_us_ = 0; clock_initialized_ = true; }
        else offset_us_ += best / 8 - offset_us_ / 8;
        uncertainty_us_ = kClockTransportBudgetUs; // Assumed link budget, not a measured RTT bound.
        last_sample_us_ = now; synchronized_ = sample_count_ >= 3U;
        increment(metrics_.clock_samples);
        return core::Status::success();
    }
    if (m.sender != leader_ || m.session != leader_session_ || !synchronized_) return reject("cue-source-or-clock");
    auto replay = std::find_if(replay_.begin(), replay_.end(), [&](const auto& r) { return r.node == m.sender && r.session == m.session; });
    if (replay == replay_.end()) {
        replay = replay_.begin() + replay_index_++ % replay_.size();
        *replay = {m.sender, m.session, 0, 0};
    }
    const auto forward = m.sequence - replay->highest;
    const auto age = replay->highest - m.sequence;
    const bool fresh = replay->seen == 0U || newer(m.sequence, replay->highest);
    if (!fresh && (age >= 32U || (replay->seen & (1U << age)) != 0U)) {
        if (age >= 32U) return reject("stale-cue");
        for (const auto& c : cues_) if (c.occupied && c.cue.sequence == m.sequence &&
            (c.cue.deadline_us != m.deadline_us || c.cue.size != m.payload_size ||
             !std::equal(m.payload.begin(), m.payload.begin() + m.payload_size, c.cue.bytes.begin()))) return reject("contradictory-cue");
        increment(metrics_.duplicates);
        return core::Status::success();
    }
    const auto time = time_us(now);
    if ((m.deadline_us < time && time - m.deadline_us > kMaximumCueLatenessUs) ||
        (m.deadline_us >= time && m.deadline_us - time > kMaximumCueDelayUs)) return reject("cue-deadline");
    auto cue = std::find_if(cues_.begin(), cues_.end(), [](const auto& c) { return !c.occupied; });
    if (cue == cues_.end()) return reject("cue-capacity");
    *cue = {}; cue->occupied = true; cue->cue = {m.sequence, m.deadline_us, m.payload_size, m.payload};
    if (fresh) { replay->seen = replay->seen == 0U || forward >= 32U ? 1U : (replay->seen << forward) | 1U; replay->highest = m.sequence; }
    else replay->seen |= 1U << age;
    increment(metrics_.scheduled);
    return core::Status::success();
}
bool Engine::next_message(std::uint64_t now, Message& output) noexcept {
    service(now);
    if (!is_leader()) return false;
    if (now >= heartbeat_due_us_) {
        base(Kind::beacon, output, now);
        if (++beacon_sequence_ == 0U) ++beacon_sequence_;
        output.sequence = beacon_sequence_; heartbeat_due_us_ = now + kHeartbeatUs;
        return true;
    }
    for (auto& c : cues_) if (c.occupied && c.transmit && now >= c.next_send_us && time_us(now) < c.cue.deadline_us) {
        base(Kind::cue, output, now); output.sequence = c.cue.sequence; output.deadline_us = c.cue.deadline_us;
        output.payload_size = c.cue.size; output.payload = c.cue.bytes;
        c.next_send_us = now + 100'000U;
        return true;
    }
    return false;
}
core::Result<std::uint32_t> Engine::schedule(std::span<const std::byte> bytes, std::uint64_t delay, std::uint64_t now) noexcept {
    service(now);
    if (!is_leader() || now < ready_us_) return core::Result<std::uint32_t>::failure(error(core::ErrorCode::invalid_state, "schedule", "leader-not-ready"));
    if (bytes.empty() || bytes.size() > kMaximumCueBytes || delay < 100'000U || delay > kMaximumCueDelayUs || time_us(now) > kMaximumTime - delay)
        return core::Result<std::uint32_t>::failure(error(core::ErrorCode::invalid_argument, "schedule", "command-or-delay"));
    auto cue = std::find_if(cues_.begin(), cues_.end(), [](const auto& c) { return !c.occupied; });
    if (cue == cues_.end()) return core::Result<std::uint32_t>::failure(error(core::ErrorCode::queue_full, "schedule", "cue-capacity"));
    *cue = {}; cue->occupied = true; cue->transmit = true; cue->next_send_us = now;
    if (++cue_sequence_ == 0U) ++cue_sequence_;
    cue->cue.sequence = cue_sequence_; cue->cue.deadline_us = time_us(now) + delay;
    cue->cue.size = static_cast<std::uint16_t>(bytes.size());
    std::copy(bytes.begin(), bytes.end(), cue->cue.bytes.begin()); increment(metrics_.scheduled);
    return core::Result<std::uint32_t>::success(cue_sequence_);
}
bool Engine::take_due(std::uint64_t now, Cue& output) noexcept {
    service(now);
    if (!synchronized_) return false;
    const auto time = time_us(now);
    while (true) {
        Pending* selected{};
        for (auto& c : cues_) if (c.occupied && c.cue.deadline_us <= time &&
            (selected == nullptr || c.cue.deadline_us < selected->cue.deadline_us)) selected = &c;
        if (selected == nullptr) return false;
        const bool late = time - selected->cue.deadline_us > kMaximumCueLatenessUs;
        if (!late) output = selected->cue;
        *selected = {};
        if (late) { increment(metrics_.late); continue; }
        increment(metrics_.due);
        return true;
    }
}
} // namespace blip::fleet
