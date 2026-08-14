#pragma once

#include "blip/storage/file_backend.hpp"
#include "blip/storage/web_asset_store.hpp"

#include <array>
#include <cstddef>
#include <cstdio>
#include <span>
#include <string_view>

namespace blip::storage {

class PosixFileBackend final : public FileBackend, public WebAssetBackend {
  public:
    static constexpr std::size_t kMaxRootBytes = 31;
    static constexpr std::size_t kMaxFullPathBytes = 160;

    explicit PosixFileBackend(std::string_view root) noexcept;
    ~PosixFileBackend() override;

    [[nodiscard]] core::Result<std::size_t> read(std::string_view path,
                                                 std::span<std::byte> output) noexcept override;
    [[nodiscard]] core::Status write_durable(std::string_view path,
                                             std::span<const std::byte> value) noexcept override;
    [[nodiscard]] core::Status replace(std::string_view source,
                                       std::string_view destination) noexcept override;
    [[nodiscard]] core::Status remove(std::string_view path) noexcept override;
    [[nodiscard]] core::Result<std::size_t> file_size(std::string_view path) noexcept override;
    [[nodiscard]] core::Result<std::size_t> read_at(std::string_view path, std::size_t offset,
                                                    std::span<std::byte> output) noexcept override;
    [[nodiscard]] core::Status begin_write(std::string_view path) noexcept override;
    [[nodiscard]] core::Status append_write(std::span<const std::byte> value) noexcept override;
    [[nodiscard]] core::Status finish_write() noexcept override;
    void abort_write() noexcept override;
    [[nodiscard]] bool valid() const noexcept { return valid_; }

  private:
    [[nodiscard]] bool full_path(std::string_view path, std::span<char> output) const noexcept;
    [[nodiscard]] core::Status create_parent_directories(std::span<char> full_path) const noexcept;
    [[nodiscard]] static core::Error errno_error(std::string_view operation, int value) noexcept;

    std::array<char, kMaxRootBytes + 1> root_{};
    std::size_t root_size_{};
    std::FILE* write_file_{};
    bool valid_{};
};

} // namespace blip::storage
