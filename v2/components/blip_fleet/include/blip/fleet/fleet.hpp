#pragma once

#include "blip/core/error.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace blip::fleet {

inline constexpr std::size_t kMaximumCues = 8U;
inline constexpr std::size_t kHeaderBytes = 48U;
inline constexpr std::size_t kMaximumCueBytes = 488U;
inline constexpr std::size_t kMaximumPacketBytes = kHeaderBytes + kMaximumCueBytes;
inline constexpr std::uint64_t kHeartbeatUs = 250'000U;
inline constexpr std::uint64_t kLeaderTimeoutUs = 2'000'000U;
inline constexpr std::uint64_t kClockTransportBudgetUs = 5'000U;
inline constexpr std::uint64_t kMaximumCueDelayUs = 60'000'000U;
inline constexpr std::uint64_t kMaximumCueLatenessUs = 20'000U;

enum class Kind : std::uint8_t { beacon = 1, cue = 2 };
struct Message {
    Kind kind{Kind::beacon};
    std::uint32_t fleet_id{};
    std::uint64_t sender{};
    std::uint32_t session{};
    std::uint32_t sequence{};
    std::uint64_t sent_us{};
    std::uint64_t deadline_us{};
    std::uint16_t payload_size{};
    std::array<std::byte, kMaximumCueBytes> payload{};
};
[[nodiscard]] core::Result<std::size_t> encode(const Message&, std::span<std::byte>) noexcept;
[[nodiscard]] core::Status decode(std::span<const std::byte>, Message&) noexcept;
struct Cue {
    std::uint32_t sequence{};
    std::uint64_t deadline_us{};
    std::uint16_t size{};
    std::array<std::byte, kMaximumCueBytes> bytes{};
};
struct Metrics {
    std::uint32_t leader_changes{}, clock_samples{}, rejected{}, duplicates{};
    std::uint32_t scheduled{}, due{}, late{}, cancelled{};
};

// Single caller, fixed memory, no participant roster. Followers are silent.
// Only the elected leader broadcasts clock beacons and repeated cues.
class Engine {
  public:
    [[nodiscard]] core::Status start(std::uint32_t fleet, std::uint64_t node,
                                     std::uint32_t session, std::uint64_t now) noexcept;
    void service(std::uint64_t now) noexcept;
    [[nodiscard]] core::Status receive(const Message&, std::uint64_t received_us) noexcept;
    [[nodiscard]] bool next_message(std::uint64_t now, Message&) noexcept;
    [[nodiscard]] core::Result<std::uint32_t> schedule(std::span<const std::byte>,
                                                      std::uint64_t delay, std::uint64_t now) noexcept;
    [[nodiscard]] bool take_due(std::uint64_t now, Cue&) noexcept;
    void cancel_all() noexcept;
    [[nodiscard]] std::uint64_t time_us(std::uint64_t local) noexcept;
    [[nodiscard]] std::uint64_t leader() const noexcept { return leader_; }
    [[nodiscard]] std::uint32_t leader_session() const noexcept { return leader_session_; }
    [[nodiscard]] bool is_leader() const noexcept { return leader_ != 0U && leader_ == node_; }
    [[nodiscard]] bool synchronized() const noexcept { return synchronized_; }
    [[nodiscard]] std::int64_t offset_us() const noexcept { return offset_us_; }
    [[nodiscard]] std::uint64_t uncertainty_us() const noexcept { return uncertainty_us_; }
    [[nodiscard]] std::uint64_t sample_age_us(std::uint64_t local) const noexcept {
        return is_leader() || local < last_sample_us_ ? 0U : local - last_sample_us_;
    }
    [[nodiscard]] std::size_t pending() const noexcept;
    [[nodiscard]] const Metrics& metrics() const noexcept { return metrics_; }
  private:
    struct Pending { Cue cue{}; std::uint64_t next_send_us{}; bool occupied{}, transmit{}; };
    struct Retired { std::uint64_t node{}; std::uint32_t session{}; };
    struct Replay { std::uint64_t node{}; std::uint32_t session{}, highest{}, seen{}; };
    void adopt(std::uint64_t node, std::uint32_t session, std::uint64_t now) noexcept;
    void base(Kind, Message&, std::uint64_t now) noexcept;
    [[nodiscard]] core::Status reject(const char*) noexcept;
    [[nodiscard]] std::uint64_t election_delay() const noexcept;
    std::array<Pending, kMaximumCues> cues_{};
    std::array<Retired, 4> retired_{};
    std::array<Replay, 4> replay_{};
    std::array<std::int64_t, 8> samples_{};
    Metrics metrics_{};
    std::uint32_t fleet_id_{}, session_{}, leader_session_{}, beacon_sequence_{}, cue_sequence_{};
    std::uint32_t received_beacon_{};
    std::uint64_t node_{}, leader_{}, leader_seen_us_{}, election_due_us_{}, ready_us_{};
    std::uint64_t heartbeat_due_us_{}, last_sample_us_{}, last_time_us_{}, uncertainty_us_{};
    std::int64_t offset_us_{};
    std::uint8_t sample_count_{}, sample_index_{}, retired_index_{}, replay_index_{};
    bool synchronized_{}, clock_initialized_{};
};
static_assert(sizeof(Engine) <= 6144U, "fleet engine exceeds its fixed memory budget");
} // namespace blip::fleet
