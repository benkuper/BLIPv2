#pragma once

#include "blip/storage/legacy_import_coordinator.hpp"
#include "nvs.h"

namespace blip::storage {

class EspNvsLegacySettingsSource final : public LegacySettingsSource {
  public:
    EspNvsLegacySettingsSource() = default;
    ~EspNvsLegacySettingsSource() override;

    EspNvsLegacySettingsSource(const EspNvsLegacySettingsSource&) = delete;
    EspNvsLegacySettingsSource& operator=(const EspNvsLegacySettingsSource&) = delete;
    EspNvsLegacySettingsSource(EspNvsLegacySettingsSource&&) = delete;
    EspNvsLegacySettingsSource& operator=(EspNvsLegacySettingsSource&&) = delete;

    [[nodiscard]] core::Status start() noexcept;
    [[nodiscard]] core::Status stop() noexcept;
    [[nodiscard]] core::Result<std::size_t> read(std::span<std::byte> output) noexcept override;

  private:
    nvs_handle_t handle_{};
    bool started_{};
    bool source_available_{};
};

} // namespace blip::storage
