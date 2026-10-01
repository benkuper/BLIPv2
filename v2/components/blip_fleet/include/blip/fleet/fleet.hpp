#pragma once

#include "blip/core/error.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace blip::fleet {

inline constexpr std::size_t kMaximumMembers = 16U;
inline constexpr std::size_t kMaximumCues = 8U;
inline constexpr std::size_t kMaximumCueBytes = 512U;
inline constexpr std::size_t kHeaderBytes = 76U;
inline constexpr std::size_t kMaximumPacketBytes = kHeaderBytes + kMaximumCueBytes;
inline constexpr std::uint64_t kHeartbeatUs = 250'000U;
inline constexpr std::uint64_t kMemberTimeoutUs = 2'000'000U;
inline constexpr std::uint64_t kMaximumRoundTripUs = 10'000U;
inline constexpr std::uint64_t kMaximumCueDelayUs = 60'000'000U;
inline constexpr std::uint64_t kMaximumCueLatenessUs = 20'000U;

enum class Kind : std::uint8_t { heartbeat = 1, probe, reply, cue, ack };

struct Message {
    Kind kind{Kind::heartbeat};
    std::uint32_t fleet_id{};
    std::uint64_t sender{};
    std::uint32_t session{};
    std::uint64_t leader{};
    std::uint32_t leader_session{};
    std::uint32_t sequence{};
    std::uint64_t sent_us{};
    std::uint64_t request_us{};
    std::uint64_t received_us{};
    std::uint64_t deadline_us{};
    std::uint16_t payload_size{};
    std::array<std::byte, kMaximumCueBytes> payload{};
};

[[nodiscard]] core::Result<std::size_t> encode(const Message& message,
                                               std::span<std::byte> output) noexcept;
[[nodiscard]] core::Status decode(std::span<const std::byte> packet,
                                   Message& output) noexcept;

struct Cue {
    std::uint32_t sequence{};
    std::uint64_t deadline_us{};
    std::uint16_t size{};
    std::array<std::byte, kMaximumCueBytes> bytes{};
};

struct Metrics {
    std::uint32_t leader_changes{};
    std::uint32_t clock_samples{};
    std::uint32_t rejected{};
    std::uint32_t duplicates{};
    std::uint32_t scheduled{};
    std::uint32_t due{};
    std::uint32_t late{};
    std::uint32_t cancelled{};
};

// One caller owns the engine. No allocation, blocking, radio or control calls.
// The adapter transmits next_message() results and hands take_due() to a
// separate control executor so a slow control action cannot hold up discovery.
class Engine {
  public:
    [[nodiscard]] core::Status start(std::uint32_t fleet_id, std::uint64_t node,
                                      std::uint32_t session, std::uint64_t now_us) noexcept;
    void service(std::uint64_t now_us) noexcept;
    [[nodiscard]] core::Status receive(const Message& message, std::uint64_t now_us,
                                        Message& reply, bool& has_reply) noexcept;
    [[nodiscard]] bool next_message(std::uint64_t now_us, Message& output) noexcept;
    [[nodiscard]] core::Result<std::uint32_t> schedule(std::span<const std::byte> command,
                                                       std::uint64_t delay_us,
                                                       std::uint64_t now_us) noexcept;
    [[nodiscard]] bool take_due(std::uint64_t now_us, Cue& output) noexcept;
    void cancel_all() noexcept;

    [[nodiscard]] std::uint64_t time_us(std::uint64_t local_us) noexcept;
    [[nodiscard]] std::uint64_t leader() const noexcept { return leader_; }
    [[nodiscard]] std::uint32_t leader_session() const noexcept { return leader_session_; }
    [[nodiscard]] bool is_leader() const noexcept { return leader_ == node_; }
    [[nodiscard]] bool synchronized() const noexcept { return synchronized_; }
    [[nodiscard]] std::int64_t offset_us() const noexcept { return offset_us_; }
    [[nodiscard]] std::uint64_t uncertainty_us() const noexcept { return uncertainty_us_; }
    [[nodiscard]] std::uint64_t sample_age_us(std::uint64_t local_us) const noexcept {
        return is_leader() || local_us < last_sample_us_ ? 0U : local_us - last_sample_us_;
    }
    [[nodiscard]] std::size_t members() const noexcept;
    [[nodiscard]] std::size_t pending() const noexcept;
    [[nodiscard]] const Metrics& metrics() const noexcept { return metrics_; }

  private:
    struct Member {
        std::uint64_t node{};
        std::uint32_t session{};
        std::uint64_t seen_us{};
        std::array<std::uint32_t, 4> retired_sessions{};
        std::uint8_t retired_index{};
    };
    struct Pending {
        Cue cue{};
        std::uint64_t next_send_us{};
        std::uint16_t awaiting{};
        bool occupied{};
        bool transmit{};
    };
    void elect(std::uint64_t now_us) noexcept;
    void base(Kind kind, Message& message, std::uint64_t now_us) noexcept;
    [[nodiscard]] std::uint32_t next_sequence() noexcept;
    [[nodiscard]] core::Status reject(const char* reason) noexcept;

    std::array<Member, kMaximumMembers - 1U> members_{};
    std::array<Pending, kMaximumCues> cues_{};
    Metrics metrics_{};
    std::uint32_t fleet_id_{};
    std::uint64_t node_{};
    std::uint32_t session_{};
    std::uint64_t leader_{};
    std::uint32_t leader_session_{};
    std::uint32_t sequence_{};
    std::uint32_t cue_sequence_{};
    std::uint32_t probe_sequence_{};
    std::uint64_t probe_sent_us_{};
    std::uint64_t heartbeat_due_us_{};
    std::uint64_t ready_us_{};
    std::uint64_t probe_due_us_{};
    std::uint64_t last_sample_us_{};
    std::uint64_t last_time_us_{};
    std::uint64_t uncertainty_us_{};
    std::int64_t offset_us_{};
    std::uint32_t cue_highest_{};
    std::uint32_t cue_seen_{};
    std::uint8_t good_samples_{};
    bool synchronized_{};
    bool clock_initialized_{};
};

static_assert(sizeof(Engine) <= 8192U, "fleet engine exceeds its fixed RAM budget");

} // namespace blip::fleet
