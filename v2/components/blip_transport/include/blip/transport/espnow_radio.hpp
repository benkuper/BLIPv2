#pragma once

#include "blip/core/error.hpp"
#include "esp_now.h"
#include "freertos/FreeRTOS.h"
#include <array>
#include <mutex>

namespace blip::transport {

// One owner for ESP-NOW initialization and its single SDK receive callback.
// Subscribers only copy packets into bounded queues from the Wi-Fi task.
class EspNowRadio final {
  public:
    using Callback = void (*)(void*, const esp_now_recv_info_t*, const std::uint8_t*, int) noexcept;
    [[nodiscard]] static EspNowRadio& shared() noexcept;
    [[nodiscard]] core::Status subscribe(void*, Callback, bool broadcast) noexcept;
    void unsubscribe(void*) noexcept;
  private:
    struct Subscriber { void* context{}; Callback callback{}; bool broadcast{}; };
    static void receive(const esp_now_recv_info_t*, const std::uint8_t*, int) noexcept;
    std::array<Subscriber, 2> subscribers_{};
    std::mutex lifecycle_mutex_{};
    portMUX_TYPE callback_mux_ = portMUX_INITIALIZER_UNLOCKED;
    bool initialized_{};
};

} // namespace blip::transport
