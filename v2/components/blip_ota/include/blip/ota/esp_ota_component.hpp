#pragma once

#include "blip/core/component.hpp"
#include "blip/ota/update_service.hpp"
#include "esp_ota_ops.h"

#include <cstddef>
#include <string_view>

namespace blip::ota {

class EspOtaBackend final : public UpdateBackend {
  public:
    [[nodiscard]] std::size_t maximum_image_size() const noexcept override;
    [[nodiscard]] core::Status begin(std::size_t image_size) noexcept override;
    [[nodiscard]] core::Status write(std::span<const std::byte> data) noexcept override;
    [[nodiscard]] core::Status finish() noexcept override;
    void abort() noexcept override;
    [[nodiscard]] core::Status activate() noexcept override;
    [[nodiscard]] core::Status confirm_running() noexcept override;
    [[nodiscard]] core::Status rollback_running() noexcept override;
    [[nodiscard]] bool running_image_pending_confirmation() const noexcept override;
    [[nodiscard]] bool signature_enforced() const noexcept override;

  private:
    const esp_partition_t* update_partition_{};
    esp_ota_handle_t handle_{};
};

class EspOtaComponent final : public core::Component {
  public:
    EspOtaComponent(std::string_view project, std::string_view target,
                    std::string_view profile) noexcept;

    [[nodiscard]] const core::ComponentDescriptor& descriptor() const noexcept override;
    [[nodiscard]] core::Status start(const core::StartContext&) noexcept override;
    [[nodiscard]] core::Status stop() noexcept override;
    [[nodiscard]] core::Status read_parameter(std::string_view id,
                                              core::ScalarValue& output) noexcept override;
    [[nodiscard]] core::Status invoke_action(std::string_view id,
                                             std::span<const core::ScalarValue> arguments,
                                             std::span<core::ScalarValue> output,
                                             std::size_t& output_count) noexcept override;

    [[nodiscard]] UpdateService& updates() noexcept { return service_; }
    void prepare_boot() noexcept { service_.refresh_boot_state(); }
    [[nodiscard]] bool pending_confirmation() const noexcept {
        return service_.status().state == UpdateState::pending_confirmation;
    }
    [[nodiscard]] core::Status confirm_boot() noexcept { return service_.confirm_boot(); }
    [[nodiscard]] core::Status reject_boot() noexcept { return service_.reject_boot(); }

  private:
    static const core::ComponentDescriptor descriptor_;
    EspOtaBackend backend_{};
    UpdateService service_;
    bool started_{};
};

} // namespace blip::ota
