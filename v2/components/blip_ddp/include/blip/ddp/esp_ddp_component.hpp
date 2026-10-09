#pragma once

#include "blip/core/component.hpp"
#include "blip/ddp/ddp.hpp"
#include "blip/led/esp_rmt_strip_component.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace blip::ddp {

class EspDdpComponent final : public core::Component {
  public:
    static constexpr std::size_t kTaskStackBytes = 3072U;
    static constexpr std::size_t kTaskStackWords = kTaskStackBytes / sizeof(StackType_t);

    explicit EspDdpComponent(led::EspRmtStripComponent& output) noexcept : output_(&output) {}

    [[nodiscard]] const core::ComponentDescriptor& descriptor() const noexcept override;
    [[nodiscard]] core::Status start(const core::StartContext&) noexcept override;
    [[nodiscard]] core::Status stop() noexcept override;
    [[nodiscard]] bool callbacks_quiesced() const noexcept override;
    [[nodiscard]] core::Status read_parameter(std::string_view id,
                                              core::ScalarValue& output) noexcept override;

  private:
    static void task_entry(void* context) noexcept;
    void run() noexcept;
    static const core::ComponentDescriptor descriptor_;

    led::EspRmtStripComponent* output_{};
    Mapping mapping_{};
    SequenceTracker sequences_{};
    std::array<std::byte, 1536U> packet_{};
    std::array<std::byte, 384U> response_{};
    alignas(16) std::array<StackType_t, kTaskStackWords> task_stack_{};
    StaticTask_t task_storage_{};
    TaskHandle_t task_{};
    int socket_{-1};
    std::atomic<bool> started_{};
    std::atomic<bool> task_quiesced_{true};
    std::atomic<std::uint32_t> accepted_{};
    std::atomic<std::uint32_t> rejected_{};
    std::atomic<std::uint32_t> invalid_{};
    std::atomic<std::uint32_t> stale_{};
    std::atomic<std::uint32_t> output_rejections_{};
    std::atomic<std::uint32_t> discovery_replies_{};
};

} // namespace blip::ddp
