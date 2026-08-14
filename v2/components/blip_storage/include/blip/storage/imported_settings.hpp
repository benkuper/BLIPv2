#pragma once

#include "blip/storage/legacy_settings_importer.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace blip::storage {

inline constexpr std::uint16_t kImportedSettingsFormatVersion = 1;
inline constexpr std::size_t kImportedSettingsHeaderBytes = 16;
inline constexpr std::size_t kMaxImportedFieldsPerComponent = 32;

class ImportedSettingsBuilder {
  public:
    explicit ImportedSettingsBuilder(std::span<std::byte> output) noexcept : output_(output) {
        reset();
    }

    void reset() noexcept;
    [[nodiscard]] core::Status append(const ImportedSetting& setting) noexcept;
    [[nodiscard]] core::Result<std::size_t> finish() noexcept;
    [[nodiscard]] std::size_t field_count() const noexcept { return field_count_; }

  private:
    std::span<std::byte> output_{};
    std::size_t size_{};
    std::size_t field_count_{};
    bool finished_{};
};

struct ImportedSettingsDecodeWorkspace {
    std::array<std::uint64_t, kMaxImportedFieldsPerComponent> field_hashes{};
    std::array<double, kMaxLegacyNumericArrayValues> numeric_array{};
};

class ImportedSettingsView {
  public:
    [[nodiscard]] core::Result<std::size_t> visit(std::span<const std::byte> payload,
                                                  ImportedSettingsDecodeWorkspace& workspace,
                                                  ImportedSettingVisitor visitor,
                                                  void* context) const noexcept;
};

} // namespace blip::storage
