#pragma once

#include "blip/network/wifi_config.hpp"

#include <cstdint>

namespace blip::network {

enum class WifiConnectionState : std::uint8_t {
    off,
    connecting,
    connected,
    connection_error,
    disabled,
    hotspot,
};

struct WifiTransition {
    bool start_station{};
    bool connect_station{};
    bool start_ap{};
    bool stop_ap{};
    bool stop_radio{};
    bool state_changed{};
};

class WifiStateMachine {
  public:
    static constexpr std::uint64_t kConnectionTimeoutMs = 20000;
    static constexpr std::uint64_t kInitialRetryMs = 500;
    static constexpr std::uint64_t kMaximumRetryMs = 5000;

    [[nodiscard]] WifiTransition apply(const WifiConfig& config, std::uint64_t now_ms) noexcept;
    [[nodiscard]] WifiTransition station_connected(std::uint64_t now_ms) noexcept;
    [[nodiscard]] WifiTransition station_disconnected(std::uint64_t now_ms) noexcept;
    [[nodiscard]] WifiTransition tick(std::uint64_t now_ms) noexcept;

    [[nodiscard]] WifiConnectionState state() const noexcept { return state_; }
    [[nodiscard]] std::uint32_t retry_count() const noexcept { return retry_count_; }
    [[nodiscard]] bool ap_active() const noexcept { return ap_active_; }
    [[nodiscard]] bool station_active() const noexcept { return station_active_; }

  private:
    void set_state(WifiConnectionState state, WifiTransition& transition) noexcept;

    WifiConfig config_{};
    WifiConnectionState state_{WifiConnectionState::off};
    std::uint64_t connection_started_ms_{};
    std::uint64_t next_retry_ms_{};
    std::uint32_t retry_count_{};
    bool ap_active_{};
    bool station_active_{};
};

} // namespace blip::network
