#include "blip/transport/espnow_radio.hpp"

#include <algorithm>
#include <cstring>

namespace blip::transport {
namespace {
constexpr std::uint8_t kBroadcast[6]{255, 255, 255, 255, 255, 255};
core::Status failure(std::string_view detail) {
    return core::Status::failure({core::ErrorDomain::transport, core::ErrorCode::resource_unavailable,
        "transport.espnow", "subscribe", detail});
}
}

EspNowRadio& EspNowRadio::shared() noexcept { static EspNowRadio radio; return radio; }

core::Status EspNowRadio::subscribe(void* context, Callback callback, bool broadcast) noexcept {
    if (context == nullptr || callback == nullptr) return failure("invalid-subscriber");
    const std::lock_guard lifecycle{lifecycle_mutex_};
    // Lifecycle callers own slot mutations. The Wi-Fi callback reads them under
    // callback_mux_, so unsubscribe also fences its final queue access.
    const auto free = std::find_if(subscribers_.begin(), subscribers_.end(), [](const auto& s) { return s.context == nullptr; });
    if (free == subscribers_.end()) return failure("subscriber-capacity");
    if (std::any_of(subscribers_.begin(), subscribers_.end(), [&](const auto& s) { return s.context == context; }))
        return failure("duplicate-subscriber");
    if (!initialized_) {
        if (esp_now_init() != ESP_OK) return failure("radio-init");
        if (esp_now_register_recv_cb(receive) != ESP_OK) {
            static_cast<void>(esp_now_deinit());
            return failure("radio-callback");
        }
        initialized_ = true;
    }
    if (broadcast && !esp_now_is_peer_exist(kBroadcast)) {
        esp_now_peer_info_t peer{};
        std::memcpy(peer.peer_addr, kBroadcast, 6);
        peer.ifidx = WIFI_IF_STA;
        if (esp_now_add_peer(&peer) != ESP_OK) {
            if (std::none_of(subscribers_.begin(), subscribers_.end(), [](const auto& s) { return s.context != nullptr; })) {
                static_cast<void>(esp_now_unregister_recv_cb());
                static_cast<void>(esp_now_deinit());
                initialized_ = false;
            }
            return failure("broadcast-peer");
        }
    }
    portENTER_CRITICAL(&callback_mux_);
    *free = {context, callback, broadcast};
    portEXIT_CRITICAL(&callback_mux_);
    return core::Status::success();
}

void EspNowRadio::unsubscribe(void* context) noexcept {
    const std::lock_guard lifecycle{lifecycle_mutex_};
    portENTER_CRITICAL(&callback_mux_);
    for (auto& subscriber : subscribers_) if (subscriber.context == context) subscriber = {};
    portEXIT_CRITICAL(&callback_mux_);
    if (std::none_of(subscribers_.begin(), subscribers_.end(), [](const auto& s) { return s.context != nullptr && s.broadcast; }))
        static_cast<void>(esp_now_del_peer(kBroadcast));
    if (initialized_ && std::none_of(subscribers_.begin(), subscribers_.end(), [](const auto& s) { return s.context != nullptr; })) {
        static_cast<void>(esp_now_unregister_recv_cb());
        static_cast<void>(esp_now_deinit());
        initialized_ = false;
    }
}

void EspNowRadio::receive(const esp_now_recv_info_t* info, const std::uint8_t* data, int length) noexcept {
    if (info == nullptr || info->des_addr == nullptr || data == nullptr || length <= 0) return;
    auto& self = shared();
    const bool broadcast = std::memcmp(info->des_addr, kBroadcast, 6) == 0;
    portENTER_CRITICAL(&self.callback_mux_);
    for (const auto& subscriber : self.subscribers_)
        if (subscriber.context != nullptr && subscriber.broadcast == broadcast)
            subscriber.callback(subscriber.context, info, data, length);
    portEXIT_CRITICAL(&self.callback_mux_);
}

} // namespace blip::transport
