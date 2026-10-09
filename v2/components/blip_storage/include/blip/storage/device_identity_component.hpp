#pragma once

#include "blip/core/component.hpp"
#include "blip/storage/settings_store.hpp"
#include <array>
#include <mutex>

namespace blip::storage {

inline constexpr std::size_t kMaxDeviceNameBytes = 63;
struct DeviceNameSnapshot {
    std::array<char, kMaxDeviceNameBytes + 1> name{};
    std::uint32_t revision{};
};

// DNS label, including a stable 12-digit MAC suffix; output includes a NUL.
[[nodiscard]] core::Status device_hostname(std::string_view name,
    std::span<const std::uint8_t, 6> mac, std::span<char> output) noexcept;

class DeviceIdentityComponent final : public core::Component {
  public:
    DeviceIdentityComponent(SettingsStore& settings, std::string_view type) noexcept;
    [[nodiscard]] const core::ComponentDescriptor& descriptor() const noexcept override;
    [[nodiscard]] core::Status start(const core::StartContext&) noexcept override;
    [[nodiscard]] core::Status stop() noexcept override;
    [[nodiscard]] core::Status read_parameter(std::string_view id, core::ScalarValue& value) noexcept override;
    [[nodiscard]] core::Status read_parameter_owned(std::string_view id, core::ScalarValue& value,
        std::span<char> output) noexcept override;
    [[nodiscard]] core::Status write_parameter(std::string_view id, const core::ScalarValue& value) noexcept override;
    [[nodiscard]] DeviceNameSnapshot snapshot() const noexcept;
    [[nodiscard]] std::string_view type() const noexcept { return type_; }
  private:
    SettingsStore* settings_{};
    const std::string_view type_;
    core::ComponentDescriptor descriptor_{};
    std::array<core::ParameterDescriptor, 2> parameters_{};
    mutable std::mutex mutex_{};
    DeviceNameSnapshot state_{};
    bool started_{};
};

} // namespace blip::storage
