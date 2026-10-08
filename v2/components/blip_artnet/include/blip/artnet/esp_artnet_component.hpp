#pragma once

#include "blip/artnet/artnet.hpp"
#include "blip/core/component.hpp"
#include "blip/led/esp_rmt_strip_component.hpp"
#include "blip/network/esp_wifi_component.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace blip::artnet {

class EspArtNetComponent final : public core::Component {
  public:
    static constexpr std::size_t kTaskStackBytes = 4096U;
    static constexpr std::size_t kTaskStackWords = kTaskStackBytes / sizeof(StackType_t);

    EspArtNetComponent(network::EspWifiComponent& wifi, led::EspRmtStripComponent& output) noexcept
        : wifi_(&wifi), output_(&output) {}

    [[nodiscard]] const core::ComponentDescriptor& descriptor() const noexcept override;
    [[nodiscard]] core::Status start(const core::StartContext&) noexcept override;
    [[nodiscard]] core::Status stop() noexcept override;
    [[nodiscard]] bool callbacks_quiesced() const noexcept override;
    [[nodiscard]] core::Status read_parameter(std::string_view id,
                                              core::ScalarValue& output) noexcept override;

  private:
    static void task_entry(void* context) noexcept;
    void run() noexcept;
    void service_discovery(std::uint64_t now_us) noexcept;
    static const core::ComponentDescriptor descriptor_;

    network::EspWifiComponent* wifi_{};
    led::EspRmtStripComponent* output_{};
    NodeIdentity identity_{};
    DmxMapping mapping_{};
    std::array<std::byte, 530> packet_{};
    std::array<std::byte, kPollReplyBytes> response_{};
    std::array<char, 18> node_name_{};
    struct Controller {
        std::uint32_t ip{};
        std::uint16_t port{};
        std::uint64_t expires_us{};
        std::uint64_t reply_due_us{};
        bool notify{};
    };
    std::array<Controller, 4> controllers_{};
    std::uint64_t last_dmx_us_{};
    alignas(16) std::array<StackType_t, kTaskStackWords> task_stack_{};
    StaticTask_t task_storage_{};
    TaskHandle_t task_{};
    int socket_{-1};
    std::atomic<bool> started_{};
    std::atomic<bool> task_quiesced_{true};
    std::atomic<std::uint32_t> accepted_{};
    std::atomic<std::uint32_t> rejected_{};
    std::atomic<std::uint32_t> discovery_replies_{};
};

} // namespace blip::artnet
