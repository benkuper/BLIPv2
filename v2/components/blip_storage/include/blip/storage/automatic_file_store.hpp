#pragma once

#include "blip/storage/atomic_file_store.hpp"
#include "blip/storage/web_asset_store.hpp"

#include <array>
#include <mutex>

namespace blip::storage {

// Handles deliberately contain no filesystem paths or medium selection. A file
// stays on its original medium until close/cancel, including on an I/O failure.
struct FileReadHandle {
    std::uint32_t token{};
    std::uint64_t generation{};
    std::size_t size{};
};

// Bulk files use two independently checksummed generations on every medium.
// This also works on FAT: publishing does not depend on overwriting by rename.
class AutomaticFileStore {
  public:
    static constexpr std::size_t kMaximumFileBytes = 16U * 1024U * 1024U;
    static constexpr std::size_t kMaximumReaders = 4;

    AutomaticFileStore(WebAssetBackend& internal, std::span<std::byte> scratch) noexcept
        : internal_(&internal), scratch_(scratch) {}

    // Mount/unmount belongs to the owner. Changing media is allowed only when
    // no readers or writer are admitted; missing media never formats a device.
    [[nodiscard]] core::Status set_external(WebAssetBackend* external) noexcept;
    [[nodiscard]] bool external() const noexcept;
    [[nodiscard]] core::Result<FileReadHandle> open(std::string_view path) noexcept;
    [[nodiscard]] core::Result<std::size_t> read(FileReadHandle handle, std::size_t offset,
                                                std::span<std::byte> output) noexcept;
    void close(FileReadHandle handle) noexcept;
    [[nodiscard]] core::Result<std::uint32_t> begin(std::string_view path,
                                                    std::size_t size) noexcept;
    [[nodiscard]] core::Status append(std::uint32_t token,
                                      std::span<const std::byte> bytes) noexcept;
    [[nodiscard]] core::Status finish(std::uint32_t token) noexcept;
    // Close and flush without publishing; validators can inspect the payload.
    [[nodiscard]] core::Status prepare(std::uint32_t token) noexcept;
    [[nodiscard]] core::Result<std::size_t> read_pending(std::uint32_t token,
        std::size_t offset, std::span<std::byte> output) noexcept;
    [[nodiscard]] core::Status commit(std::uint32_t token) noexcept;
    void cancel(std::uint32_t token) noexcept;
    [[nodiscard]] core::Result<LoadedFile> load(std::string_view path,
                                               std::span<std::byte> output) noexcept;
    [[nodiscard]] core::Status save(std::string_view path,
                                    std::span<const std::byte> input) noexcept;
    [[nodiscard]] core::Status erase(std::string_view path) noexcept;

  private:
    struct Record {
        std::uint64_t generation{};
        std::size_t size{};
        unsigned slot{};
        bool deleted{};
    };
    struct Reader {
        WebAssetBackend* backend{};
        std::array<char, kMaxLogicalPathBytes + 4> path{};
        FileReadHandle handle{};
    };
    struct Writer {
        WebAssetBackend* backend{};
        std::array<char, kMaxLogicalPathBytes + 4> path{};
        std::uint32_t token{};
        std::uint32_t crc{0xffffffffU};
        std::size_t expected{};
        std::size_t received{};
        bool prepared{};
    };
    [[nodiscard]] core::Result<Record> inspect(WebAssetBackend& backend,
                                                std::string_view path) noexcept;
    [[nodiscard]] core::Result<std::uint32_t> begin_record(std::string_view path,
        std::size_t size, bool deleted) noexcept;
    [[nodiscard]] core::Result<Record> inspect_slot(WebAssetBackend& backend,
                                                     std::string_view path, unsigned slot) noexcept;
    [[nodiscard]] static bool valid_path(std::string_view path) noexcept;
    [[nodiscard]] static std::string_view physical(std::string_view path, unsigned slot,
                                                   std::span<char> buffer) noexcept;
    [[nodiscard]] std::uint32_t next_token() noexcept;
    void cancel_writer() noexcept;

    WebAssetBackend* internal_{};
    WebAssetBackend* external_{};
    std::span<std::byte> scratch_{};
    std::array<Reader, kMaximumReaders> readers_{};
    Writer writer_{};
    std::uint32_t token_{};
    mutable std::recursive_mutex mutex_{};
};

} // namespace blip::storage
