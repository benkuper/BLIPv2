#pragma once

#include "blip/storage/imported_settings.hpp"
#include "blip/storage/settings_store.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace blip::storage {

inline constexpr std::size_t kLegacyImportJournalBytes = 64;

class LegacySettingsSource {
  public:
    LegacySettingsSource() = default;
    virtual ~LegacySettingsSource() = default;
    LegacySettingsSource(const LegacySettingsSource&) = delete;
    LegacySettingsSource& operator=(const LegacySettingsSource&) = delete;
    LegacySettingsSource(LegacySettingsSource&&) = delete;
    LegacySettingsSource& operator=(LegacySettingsSource&&) = delete;

    [[nodiscard]] virtual core::Result<std::size_t> read(std::span<std::byte> output) noexcept = 0;
};

enum class LegacyImportDisposition : std::uint8_t {
    no_source,
    imported_pending,
    already_pending,
    already_confirmed,
};

struct LegacyImportResult {
    LegacyImportDisposition disposition{LegacyImportDisposition::no_source};
    std::size_t imported_components{};
    std::size_t preserved_components{};
    std::size_t setting_count{};
};

class LegacyImportCoordinator {
  public:
    LegacyImportCoordinator(SettingsStore& settings, LegacySettingsSource& source,
                            std::span<std::byte> source_buffer, std::span<std::byte> payload_buffer,
                            std::span<std::byte> readback_buffer,
                            LegacyImportWorkspace& workspace) noexcept
        : settings_(&settings), source_(&source), source_buffer_(source_buffer),
          payload_buffer_(payload_buffer), readback_buffer_(readback_buffer),
          workspace_(&workspace) {}

    [[nodiscard]] core::Result<LegacyImportResult> run() noexcept;
    [[nodiscard]] core::Status confirm_boot() noexcept;

  private:
    SettingsStore* settings_{};
    LegacySettingsSource* source_{};
    std::span<std::byte> source_buffer_{};
    std::span<std::byte> payload_buffer_{};
    std::span<std::byte> readback_buffer_{};
    LegacyImportWorkspace* workspace_{};
};

} // namespace blip::storage
