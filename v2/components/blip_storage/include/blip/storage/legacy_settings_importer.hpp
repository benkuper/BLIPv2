#pragma once

#include "blip/core/error.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace blip::storage {

inline constexpr std::uint32_t kLegacySettingsImporterVersion = 1;
inline constexpr std::size_t kMaxLegacySettingsBytes = 6144;
inline constexpr std::size_t kMaxLegacySettingCount = 192;
inline constexpr std::size_t kMaxLegacyComponentCount = 64;
inline constexpr std::size_t kMaxLegacyNumericArrayValues = 16;

enum class LegacyValueType : std::uint8_t {
    boolean = 1,
    signed_integer = 2,
    unsigned_integer = 3,
    floating = 4,
    string = 5,
    numeric_array = 6,
};

struct LegacyValue {
    LegacyValueType type{LegacyValueType::boolean};
    bool boolean{};
    std::int64_t signed_integer{};
    std::uint64_t unsigned_integer{};
    double floating{};
    std::string_view string{};
    std::span<const double> numeric_array{};
};

struct ImportedSetting {
    std::string_view legacy_component_path{};
    std::string_view component_id{};
    std::string_view field{};
    LegacyValue value{};
};

struct LegacyComponentMapping {
    std::string_view legacy_path{};
    std::string_view component_id{};
};

struct LegacyImportSummary {
    std::array<std::byte, 32> source_sha256{};
    std::size_t setting_count{};
    std::size_t component_count{};
};

struct LegacyImportWorkspace {
    std::array<std::uint64_t, kMaxLegacySettingCount> setting_hashes{};
    std::array<std::uint64_t, kMaxLegacyComponentCount> component_hashes{};
    std::array<char, 97> component_path{};
    std::array<double, kMaxLegacyNumericArrayValues> numeric_array{};
};

using ImportedSettingVisitor = core::Status (*)(void* context,
                                                const ImportedSetting& setting) noexcept;

[[nodiscard]] std::span<const LegacyComponentMapping> legacy_component_mappings() noexcept;
[[nodiscard]] std::string_view map_legacy_component(std::string_view legacy_path) noexcept;

class LegacySettingsImporter {
  public:
    [[nodiscard]] core::Result<LegacyImportSummary> import(std::span<const std::byte> source,
                                                           LegacyImportWorkspace& workspace,
                                                           ImportedSettingVisitor visitor = nullptr,
                                                           void* context = nullptr) const noexcept;
};

} // namespace blip::storage
