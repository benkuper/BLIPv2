#pragma once

#include "blip/core/component.hpp"
#include "blip/e131/e131.hpp"
#include "blip/led/esp_rmt_strip_component.hpp"
#include "blip/network/esp_wifi_component.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace blip::e131 {

class EspE131Component final : public core::Component {
  public:
    static constexpr std::size_t kTaskStackBytes = 4096U;
    static constexpr std::size_t kTaskStackWords = kTaskStackBytes / sizeof(StackType_t);

    EspE131Component(network::EspWifiComponent& wifi,
                     led::EspRmtStripComponent& output) noexcept
        : wifi_(&wifi), output_(&output) {}

    [[nodiscard]] const core::ComponentDescriptor& descriptor() const noexcept override;
    [[nodiscard]] core::Status start(const core::StartContext&) noexcept override;
    [[nodiscard]] core::Status stop() noexcept override;
    [[nodiscard]] bool callbacks_quiesced() const noexcept override;
    [[nodiscard]] core::Status read_parameter(std::string_view id,
                                              core::ScalarValue& output) noexcept override;

  private:
    static void task_entry(void* context) noexcept;
    void maintain_membership(std::uint64_t now_us) noexcept;
    void submit_merged(std::uint64_t now_us) noexcept;
    void run() noexcept;
    static const core::ComponentDescriptor descriptor_;

    network::EspWifiComponent* wifi_{};
    led::EspRmtStripComponent* output_{};
    Mapping mapping_{};
    SourceMixer mixer_{};
    std::array<std::byte, 768U> packet_{};
    std::array<std::byte, 512U> merged_{};
    alignas(16) std::array<StackType_t, kTaskStackWords> task_stack_{};
    StaticTask_t task_storage_{};
    TaskHandle_t task_{};
    int socket_{-1};
    ip_mreq membership_{};
    std::uint64_t last_join_attempt_us_{};
    std::atomic<bool> started_{};
    std::atomic<bool> task_quiesced_{true};
    std::atomic<bool> multicast_joined_{};
    std::atomic<std::uint32_t> accepted_{};
    std::atomic<std::uint32_t> rejected_{};
    std::atomic<std::uint32_t> invalid_{};
    std::atomic<std::uint32_t> ignored_{};
    std::atomic<std::uint32_t> stale_{};
    std::atomic<std::uint32_t> source_capacity_{};
    std::atomic<std::uint32_t> output_rejections_{};
    std::atomic<std::uint32_t> join_failures_{};
    std::atomic<std::uint32_t> active_sources_{};
};

} // namespace blip::e131
