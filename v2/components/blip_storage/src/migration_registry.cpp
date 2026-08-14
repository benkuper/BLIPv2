#include "blip/storage/migration_registry.hpp"

#include <cstring>

namespace blip::storage {
namespace {

[[nodiscard]] core::Error migration_error(core::ErrorCode code, std::string_view component,
                                          std::string_view operation,
                                          std::string_view detail) noexcept {
    return {core::ErrorDomain::storage, code, component, operation, detail};
}

} // namespace

bool MigrationRegistry::overlaps(std::span<const std::byte> first,
                                 std::span<const std::byte> second) noexcept {
    if (first.empty() || second.empty()) {
        return false;
    }
    const auto first_start = reinterpret_cast<std::uintptr_t>(first.data());
    const auto second_start = reinterpret_cast<std::uintptr_t>(second.data());
    return first_start < second_start + second.size() && second_start < first_start + first.size();
}

core::Status MigrationRegistry::add(const MigrationStep& step) noexcept {
    if (step.component_id.empty() || step.from_schema == 0 ||
        step.to_schema != step.from_schema + 1U || step.migrate == nullptr) {
        return core::Status::failure(migration_error(core::ErrorCode::invalid_argument,
                                                     step.component_id, "register-migration",
                                                     "invalid-step"));
    }
    if (find(step.component_id, step.from_schema) != nullptr) {
        return core::Status::failure(migration_error(core::ErrorCode::duplicate_id,
                                                     step.component_id, "register-migration",
                                                     "duplicate-step"));
    }
    if (size_ == steps_.size()) {
        return core::Status::failure(migration_error(core::ErrorCode::capacity_exceeded,
                                                     step.component_id, "register-migration",
                                                     "registry-full"));
    }
    steps_[size_++] = step;
    return core::Status::success();
}

const MigrationStep* MigrationRegistry::find(std::string_view component_id,
                                             std::uint32_t from_schema) const noexcept {
    for (std::size_t index = 0; index < size_; ++index) {
        if (steps_[index].component_id == component_id &&
            steps_[index].from_schema == from_schema) {
            return &steps_[index];
        }
    }
    return nullptr;
}

core::Result<MigrationResult>
MigrationRegistry::migrate(std::string_view component_id, std::uint32_t from_schema,
                           std::uint32_t to_schema, std::span<const std::byte> input,
                           std::span<std::byte> output,
                           std::span<std::byte> scratch) const noexcept {
    if (component_id.empty() || from_schema == 0 || to_schema < from_schema ||
        overlaps(output, scratch) || overlaps(input, output) || overlaps(input, scratch)) {
        return core::Result<MigrationResult>::failure(
            migration_error(core::ErrorCode::invalid_argument, component_id, "migrate",
                            "invalid-buffers-or-schema"));
    }
    if (input.size() > output.size() || input.size() > scratch.size()) {
        return core::Result<MigrationResult>::failure(migration_error(
            core::ErrorCode::capacity_exceeded, component_id, "migrate", "buffer-too-small"));
    }
    if (from_schema == to_schema) {
        if (!input.empty()) {
            std::memcpy(output.data(), input.data(), input.size());
        }
        return core::Result<MigrationResult>::success({to_schema, input.size(), 0});
    }

    std::span<const std::byte> current = input;
    bool write_output = true;
    std::size_t steps_applied{};
    for (std::uint32_t schema = from_schema; schema < to_schema; ++schema) {
        const MigrationStep* step = find(component_id, schema);
        if (step == nullptr) {
            return core::Result<MigrationResult>::failure(migration_error(
                core::ErrorCode::incompatible_version, component_id, "migrate", "missing-step"));
        }
        std::span<std::byte> destination = write_output ? output : scratch;
        const auto migrated = step->migrate(current, destination);
        if (!migrated) {
            return core::Result<MigrationResult>::failure(migrated.error());
        }
        if (migrated.value() > destination.size()) {
            return core::Result<MigrationResult>::failure(migration_error(
                core::ErrorCode::verification_failed, component_id, "migrate", "invalid-size"));
        }
        current = std::span<const std::byte>{destination.data(), migrated.value()};
        write_output = !write_output;
        ++steps_applied;
    }
    if (current.data() != output.data() && !current.empty()) {
        std::memcpy(output.data(), current.data(), current.size());
    }
    return core::Result<MigrationResult>::success({to_schema, current.size(), steps_applied});
}

} // namespace blip::storage
