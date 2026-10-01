#pragma once

#include "blip/core/component.hpp"

#include "esp_pm.h"

#include <atomic>
#include <cstdint>

namespace blip::pm {

class EspPowerManagerComponent final : public core::Component {
  public:
    [[nodiscard]] const core::ComponentDescriptor& descriptor() const noexcept override;
    [[nodiscard]] core::Status start(const core::StartContext&) noexcept override;
    [[nodiscard]] core::Status stop() noexcept override;
    [[nodiscard]] core::Status read_parameter(std::string_view id,
                                               core::ScalarValue& output) noexcept override;

    [[nodiscard]] bool acquire_frame_lock() noexcept;
    void release_frame_lock() noexcept;

  private:
    static const core::ComponentDescriptor descriptor_;
    esp_pm_lock_handle_t frame_lock_{};
    std::atomic<std::uint32_t> active_frame_locks_{};
    std::atomic<std::uint32_t> acquired_frames_{};
    std::atomic<std::uint32_t> release_errors_{};
    bool started_{};
};

class FrameCpuLock final {
  public:
    explicit FrameCpuLock(EspPowerManagerComponent& manager) noexcept
        : manager_(&manager), acquired_(manager.acquire_frame_lock()) {}
    ~FrameCpuLock() {
        if (acquired_) {
            manager_->release_frame_lock();
        }
    }
    FrameCpuLock(const FrameCpuLock&) = delete;
    FrameCpuLock& operator=(const FrameCpuLock&) = delete;
    [[nodiscard]] bool acquired() const noexcept { return acquired_; }

  private:
    EspPowerManagerComponent* manager_{};
    bool acquired_{};
};

} // namespace blip::pm
