#include "blip/network/wifi_state_machine.hpp"

#include <algorithm>

namespace blip::network {

void WifiStateMachine::set_state(WifiConnectionState state, WifiTransition& transition) noexcept {
    if (state_ != state) {
        state_ = state;
        transition.state_changed = true;
    }
}

WifiTransition WifiStateMachine::apply(const WifiConfig& config, std::uint64_t now_ms) noexcept {
    WifiTransition transition{};
    config_ = config;
    retry_count_ = 0;
    connection_started_ms_ = now_ms;
    next_retry_ms_ = now_ms + kInitialRetryMs;
    if (!config.enabled) {
        transition.stop_radio = station_active_ || ap_active_;
        station_active_ = false;
        ap_active_ = false;
        set_state(WifiConnectionState::disabled, transition);
        return transition;
    }

    const bool credentials = !config.ssid.empty();
    const bool needs_station = config.mode != WifiMode::access_point && credentials;
    // Keep an already-running provisioning AP reachable until a station actually connects.
    // This lets the HTTP request that supplied credentials finish and preserves a recovery path
    // throughout association. Station-only mode removes the AP in station_connected().
    const bool needs_ap =
        config.mode != WifiMode::station || !credentials || (needs_station && ap_active_);
    if (needs_station) {
        station_active_ = true;
        transition.start_station = true;
        transition.connect_station = true;
    } else {
        station_active_ = false;
    }
    if (needs_ap && !ap_active_) {
        transition.start_ap = true;
    } else if (!needs_ap && ap_active_) {
        transition.stop_ap = true;
    }
    ap_active_ = needs_ap;
    set_state(needs_station ? WifiConnectionState::connecting : WifiConnectionState::hotspot,
              transition);
    return transition;
}

WifiTransition WifiStateMachine::station_connected(std::uint64_t) noexcept {
    WifiTransition transition{};
    if (!station_active_) {
        return transition;
    }
    retry_count_ = 0;
    set_state(WifiConnectionState::connected, transition);
    if (config_.mode == WifiMode::station && ap_active_) {
        ap_active_ = false;
        transition.stop_ap = true;
    }
    return transition;
}

WifiTransition WifiStateMachine::station_disconnected(std::uint64_t now_ms) noexcept {
    WifiTransition transition{};
    if (!station_active_) {
        return transition;
    }
    if (state_ == WifiConnectionState::connected) {
        connection_started_ms_ = now_ms;
    }
    next_retry_ms_ = now_ms;
    set_state(ap_active_ ? WifiConnectionState::connection_error : WifiConnectionState::connecting,
              transition);
    return transition;
}

WifiTransition WifiStateMachine::tick(std::uint64_t now_ms) noexcept {
    WifiTransition transition{};
    if (!station_active_ || (state_ != WifiConnectionState::connecting &&
                             state_ != WifiConnectionState::connection_error)) {
        return transition;
    }
    if (now_ms - connection_started_ms_ >= kConnectionTimeoutMs) {
        if (!ap_active_) {
            ap_active_ = true;
            transition.start_ap = true;
        }
        set_state(WifiConnectionState::connection_error, transition);
    }
    if (now_ms >= next_retry_ms_) {
        transition.connect_station = true;
        if (retry_count_ != UINT32_MAX) {
            ++retry_count_;
        }
        const std::uint32_t shift = std::min<std::uint32_t>(retry_count_, 4U);
        const std::uint64_t delay = std::min(kInitialRetryMs << shift, kMaximumRetryMs);
        next_retry_ms_ = now_ms + delay;
    }
    return transition;
}

} // namespace blip::network
