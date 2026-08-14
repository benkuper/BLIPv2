#pragma once

#include "blip/core/component.hpp"
#include "blip/storage/esp_nvs_legacy_source.hpp"

#include <array>
#include <cstddef>

namespace blip::storage {

class LegacySettingsImportComponent final : public core::Component {
  public:
    static constexpr std::size_t kPayloadBytes = 1024;

    explicit LegacySettingsImportComponent(SettingsStore& settings) noexcept;

    [[nodiscard]] const core::ComponentDescriptor& descriptor() const noexcept override;
    [[nodiscard]] core::Status start(const core::StartContext&) noexcept override;
    [[nodiscard]] core::Status stop() noexcept override;
    [[nodiscard]] core::Status confirm_boot() noexcept;
    [[nodiscard]] LegacyImportDisposition disposition() const noexcept { return disposition_; }

  private:
    static const core::ComponentDescriptor descriptor_;

    EspNvsLegacySettingsSource source_{};
    std::array<std::byte, kMaxLegacySettingsBytes> source_buffer_{};
    std::array<std::byte, kPayloadBytes> payload_buffer_{};
    std::array<std::byte, kPayloadBytes> readback_buffer_{};
    LegacyImportWorkspace workspace_{};
    LegacyImportCoordinator coordinator_;
    LegacyImportDisposition disposition_{LegacyImportDisposition::no_source};
    bool started_{};
};

static_assert(sizeof(LegacySettingsImportComponent) <= 12288,
              "legacy settings importer exceeds its declared static RAM budget");

} // namespace blip::storage
