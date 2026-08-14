#pragma once

#include "blip/core/component.hpp"
#include "blip/storage/esp_nvs_blob_store.hpp"
#include "blip/storage/settings_store.hpp"

#include <array>
#include <cstddef>

namespace blip::storage {

class NvsSettingsComponent final : public core::Component {
  public:
    static constexpr std::size_t kScratchBytes = 4096;

    NvsSettingsComponent() noexcept;

    [[nodiscard]] const core::ComponentDescriptor& descriptor() const noexcept override;
    [[nodiscard]] core::Status start(const core::StartContext&) noexcept override;
    [[nodiscard]] core::Status stop() noexcept override;
    [[nodiscard]] SettingsStore& settings() noexcept { return store_; }
    [[nodiscard]] const SettingsStore& settings() const noexcept { return store_; }

  private:
    static const core::ComponentDescriptor descriptor_;

    EspNvsBlobStore backend_;
    std::array<std::byte, kScratchBytes> scratch_{};
    SettingsStore store_;
};

static_assert(sizeof(NvsSettingsComponent) <= 4864,
              "settings service exceeds its declared static RAM budget");

} // namespace blip::storage
