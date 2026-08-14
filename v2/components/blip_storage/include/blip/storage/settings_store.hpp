#pragma once

#include "blip/core/descriptor.hpp"
#include "blip/core/error.hpp"
#include "blip/storage/blob_store.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace blip::storage {

inline constexpr std::uint16_t kSettingsRecordFormatVersion = 1;
inline constexpr std::size_t kSettingsRecordHeaderBytes = 32;
inline constexpr std::size_t kSettingsCommitRecordBytes = 32;
inline constexpr std::size_t kMaxSettingsComponentIdBytes = 96;
inline constexpr std::size_t kSettingsNvsKeyBytes = 13;

enum class SettingsSlot : std::uint8_t { a = 0, b = 1 };

struct LoadedSettings {
    std::uint32_t schema_version{};
    std::uint64_t generation{};
    std::size_t payload_size{};
    SettingsSlot slot{SettingsSlot::a};
};

class SettingsStore {
  public:
    SettingsStore(BlobStore& backend, std::span<std::byte> scratch) noexcept
        : backend_(&backend), scratch_(scratch) {}

    SettingsStore(const SettingsStore&) = delete;
    SettingsStore& operator=(const SettingsStore&) = delete;
    SettingsStore(SettingsStore&&) = delete;
    SettingsStore& operator=(SettingsStore&&) = delete;

    [[nodiscard]] core::Result<LoadedSettings> load(const core::ComponentDescriptor& descriptor,
                                                    std::span<std::byte> output) noexcept;
    [[nodiscard]] core::Status save(const core::ComponentDescriptor& descriptor,
                                    std::span<const std::byte> payload) noexcept;

    [[nodiscard]] std::size_t maximum_payload_size(std::string_view component_id) const noexcept;

  private:
    static constexpr std::size_t kKeyBufferBytes = kSettingsNvsKeyBytes + 1;

    struct KeySet {
        std::array<char, kKeyBufferBytes> slot_a{};
        std::array<char, kKeyBufferBytes> slot_b{};
        std::array<char, kKeyBufferBytes> commit{};
    };

    struct SlotState {
        bool present{};
        bool valid{};
        SettingsSlot slot{SettingsSlot::a};
        std::uint32_t schema_version{};
        std::uint64_t generation{};
        std::size_t payload_offset{};
        std::size_t payload_size{};
    };

    struct CommitState {
        bool present{};
        bool valid{};
        SettingsSlot slot{SettingsSlot::a};
        std::uint64_t generation{};
    };

    struct StoreState {
        std::array<SlotState, 2> slots{};
        CommitState commit{};
        bool any_present{};
    };

    [[nodiscard]] core::Status validate_descriptor(const core::ComponentDescriptor& descriptor,
                                                   std::string_view operation) const noexcept;
    [[nodiscard]] static std::uint64_t component_hash(std::string_view component_id) noexcept;
    [[nodiscard]] static KeySet make_keys(std::string_view component_id) noexcept;
    [[nodiscard]] core::Result<SlotState>
    read_slot(std::string_view key, std::string_view component_id, SettingsSlot slot) noexcept;
    [[nodiscard]] core::Result<CommitState> read_commit(std::string_view key,
                                                        std::string_view component_id) noexcept;
    [[nodiscard]] core::Result<StoreState> inspect(std::string_view component_id) noexcept;
    [[nodiscard]] core::Status build_slot_record(std::string_view component_id,
                                                 std::uint32_t schema_version,
                                                 std::uint64_t generation,
                                                 std::span<const std::byte> payload,
                                                 std::size_t& record_size) noexcept;
    [[nodiscard]] core::Status build_commit_record(std::string_view component_id, SettingsSlot slot,
                                                   std::uint64_t generation) noexcept;
    [[nodiscard]] core::Status parse_slot_record(std::string_view component_id, SettingsSlot slot,
                                                 std::span<const std::byte> record,
                                                 SlotState& state) const noexcept;
    [[nodiscard]] core::Status parse_commit_record(std::string_view component_id,
                                                   std::span<const std::byte> record,
                                                   CommitState& state) const noexcept;
    [[nodiscard]] static const SlotState* select(const StoreState& state) noexcept;
    [[nodiscard]] static bool overlaps(std::span<const std::byte> first,
                                       std::span<const std::byte> second) noexcept;

    BlobStore* backend_{};
    std::span<std::byte> scratch_{};
};

} // namespace blip::storage
