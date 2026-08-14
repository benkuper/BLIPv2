#pragma once

#include "blip/core/error.hpp"
#include "blip/storage/file_backend.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace blip::storage {

inline constexpr std::uint16_t kAtomicFileFormatVersion = 1;
inline constexpr std::size_t kAtomicFileHeaderBytes = 32;
inline constexpr std::size_t kAtomicFilePointerBytes = 32;
inline constexpr std::size_t kMaxLogicalPathBytes = 96;

enum class FileCommitPolicy : std::uint8_t { atomic_rename, two_slot };
enum class FileSlot : std::uint8_t { a = 0, b = 1 };

struct LoadedFile {
    std::uint64_t generation{};
    std::size_t payload_size{};
    FileSlot slot{FileSlot::a};
};

class AtomicFileStore {
  public:
    AtomicFileStore(FileBackend& backend, std::span<std::byte> scratch,
                    FileCommitPolicy policy) noexcept
        : backend_(&backend), scratch_(scratch), policy_(policy) {}

    AtomicFileStore(const AtomicFileStore&) = delete;
    AtomicFileStore& operator=(const AtomicFileStore&) = delete;
    AtomicFileStore(AtomicFileStore&&) = delete;
    AtomicFileStore& operator=(AtomicFileStore&&) = delete;

    [[nodiscard]] core::Result<LoadedFile> load(std::string_view logical_path,
                                                std::span<std::byte> output) noexcept;
    [[nodiscard]] core::Status save(std::string_view logical_path,
                                    std::span<const std::byte> payload) noexcept;
    [[nodiscard]] std::size_t maximum_payload_size(std::string_view logical_path) const noexcept;

  private:
    static constexpr std::size_t kPathBufferBytes = kMaxLogicalPathBytes + 5;

    struct FileState {
        bool present{};
        bool valid{};
        FileSlot slot{FileSlot::a};
        std::uint64_t generation{};
        std::size_t payload_offset{};
        std::size_t payload_size{};
    };

    struct PointerState {
        bool present{};
        bool valid{};
        FileSlot slot{FileSlot::a};
        std::uint64_t generation{};
    };

    struct SlotSet {
        std::array<FileState, 2> slots{};
        PointerState pointer{};
        bool any_present{};
    };

    [[nodiscard]] core::Status validate_path(std::string_view logical_path,
                                             std::string_view operation) const noexcept;
    [[nodiscard]] static std::uint64_t path_hash(std::string_view logical_path) noexcept;
    [[nodiscard]] static bool make_path(std::string_view logical_path, std::string_view suffix,
                                        std::span<char> output, std::string_view& result) noexcept;
    [[nodiscard]] core::Result<FileState> read_record(std::string_view storage_path,
                                                      std::string_view logical_path,
                                                      FileSlot slot) noexcept;
    [[nodiscard]] core::Result<PointerState> read_pointer(std::string_view storage_path,
                                                          std::string_view logical_path) noexcept;
    [[nodiscard]] core::Result<SlotSet> inspect_slots(std::string_view logical_path) noexcept;
    [[nodiscard]] core::Status build_record(std::string_view logical_path, std::uint64_t generation,
                                            std::span<const std::byte> payload,
                                            std::size_t& record_size) noexcept;
    [[nodiscard]] core::Status build_pointer(std::string_view logical_path, FileSlot slot,
                                             std::uint64_t generation) noexcept;
    [[nodiscard]] core::Status parse_record(std::string_view logical_path, FileSlot slot,
                                            std::span<const std::byte> record,
                                            FileState& state) const noexcept;
    [[nodiscard]] core::Status parse_pointer(std::string_view logical_path,
                                             std::span<const std::byte> record,
                                             PointerState& state) const noexcept;
    [[nodiscard]] static const FileState* select(const SlotSet& state) noexcept;
    [[nodiscard]] core::Result<LoadedFile> load_rename(std::string_view logical_path,
                                                       std::span<std::byte> output) noexcept;
    [[nodiscard]] core::Result<LoadedFile> load_slots(std::string_view logical_path,
                                                      std::span<std::byte> output) noexcept;
    [[nodiscard]] core::Status save_rename(std::string_view logical_path,
                                           std::span<const std::byte> payload) noexcept;
    [[nodiscard]] core::Status save_slots(std::string_view logical_path,
                                          std::span<const std::byte> payload) noexcept;
    [[nodiscard]] static bool overlaps(std::span<const std::byte> first,
                                       std::span<const std::byte> second) noexcept;

    FileBackend* backend_{};
    std::span<std::byte> scratch_{};
    FileCommitPolicy policy_{FileCommitPolicy::atomic_rename};
};

} // namespace blip::storage
