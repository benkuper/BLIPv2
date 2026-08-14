#pragma once

#include "blip/core/error.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace blip::storage {

using MigrationFunction = core::Result<std::size_t> (*)(std::span<const std::byte> input,
                                                        std::span<std::byte> output) noexcept;

struct MigrationStep {
    std::string_view component_id{};
    std::uint32_t from_schema{};
    std::uint32_t to_schema{};
    MigrationFunction migrate{};
};

struct MigrationResult {
    std::uint32_t schema_version{};
    std::size_t payload_size{};
    std::size_t steps_applied{};
};

class MigrationRegistry {
  public:
    explicit MigrationRegistry(std::span<MigrationStep> storage) noexcept : steps_(storage) {}

    [[nodiscard]] core::Status add(const MigrationStep& step) noexcept;
    [[nodiscard]] core::Result<MigrationResult>
    migrate(std::string_view component_id, std::uint32_t from_schema, std::uint32_t to_schema,
            std::span<const std::byte> input, std::span<std::byte> output,
            std::span<std::byte> scratch) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return size_; }

  private:
    [[nodiscard]] const MigrationStep* find(std::string_view component_id,
                                            std::uint32_t from_schema) const noexcept;
    [[nodiscard]] static bool overlaps(std::span<const std::byte> first,
                                       std::span<const std::byte> second) noexcept;

    std::span<MigrationStep> steps_{};
    std::size_t size_{};
};

} // namespace blip::storage
