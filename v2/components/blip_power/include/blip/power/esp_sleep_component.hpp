#pragma once

#include "blip/core/component.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <array>
#include <atomic>
#include <cstdint>

namespace blip::power {

// Manual, timer-bounded sleep for the USB-powered HUZZAH32 qualification rig.
// Other boards need their power-latch and wake circuits qualified first.
class EspSleepComponent final : public core::Component {
  public:
    EspSleepComponent(core::Component& wifi, core::Component& strip) noexcept
        : wifi_(&wifi), strip_(&strip) {}

    [[nodiscard]] const core::ComponentDescriptor& descriptor() const noexcept override;
    [[nodiscard]] core::Status start(const core::StartContext&) noexcept override;
    [[nodiscard]] core::Status stop() noexcept override;
    [[nodiscard]] bool callbacks_quiesced() const noexcept override { return quiesced_.load(); }
    [[nodiscard]] core::Status read_parameter(std::string_view id,
                                               core::ScalarValue& output) noexcept override;
    [[nodiscard]] core::Status invoke_action(std::string_view id,
                                              std::span<const core::ScalarValue> arguments,
                                              std::span<core::ScalarValue> output,
                                              std::size_t& output_count) noexcept override;

  private:
    [[nodiscard]] bool peripherals_idle() noexcept;
    static void task_entry(void* context) noexcept;
    void run() noexcept;

    static const core::ComponentDescriptor descriptor_;
    static constexpr std::size_t kTaskStackWords = 3072 / sizeof(StackType_t);
    core::Component* wifi_{};
    core::Component* strip_{};
    alignas(16) std::array<StackType_t, kTaskStackWords> task_stack_{};
    StaticTask_t task_storage_{};
    TaskHandle_t task_handle_{};
    std::atomic<bool> running_{};
    std::atomic<bool> quiesced_{true};
    std::atomic<std::uint32_t> state_{}; // 0 idle, 1 pending, 2 sleeping, 3 woke, 4 failed
    std::atomic<std::uint32_t> requested_ms_{};
    std::atomic<std::uint32_t> completed_{};
    std::atomic<std::uint32_t> wake_cause_{};
    std::atomic<std::uint32_t> last_error_{};
    std::atomic<std::uint64_t> last_sleep_us_{};
};

} // namespace blip::power
