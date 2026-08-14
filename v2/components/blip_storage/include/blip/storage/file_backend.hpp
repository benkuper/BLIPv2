#pragma once

#include "blip/core/error.hpp"

#include <cstddef>
#include <span>
#include <string_view>

namespace blip::storage {

class FileBackend {
  public:
    FileBackend() = default;
    virtual ~FileBackend() = default;
    FileBackend(const FileBackend&) = delete;
    FileBackend& operator=(const FileBackend&) = delete;
    FileBackend(FileBackend&&) = delete;
    FileBackend& operator=(FileBackend&&) = delete;

    [[nodiscard]] virtual core::Result<std::size_t> read(std::string_view path,
                                                         std::span<std::byte> output) noexcept = 0;
    [[nodiscard]] virtual core::Status write_durable(std::string_view path,
                                                     std::span<const std::byte> value) noexcept = 0;
    [[nodiscard]] virtual core::Status replace(std::string_view source,
                                               std::string_view destination) noexcept = 0;
    [[nodiscard]] virtual core::Status remove(std::string_view path) noexcept = 0;
};

} // namespace blip::storage
