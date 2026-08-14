#pragma once

#include "blip/core/error.hpp"

#include <cstddef>
#include <span>
#include <string_view>

namespace blip::storage {

class BlobStore {
  public:
    BlobStore() = default;
    virtual ~BlobStore() = default;
    BlobStore(const BlobStore&) = delete;
    BlobStore& operator=(const BlobStore&) = delete;
    BlobStore(BlobStore&&) = delete;
    BlobStore& operator=(BlobStore&&) = delete;

    [[nodiscard]] virtual core::Result<std::size_t> read(std::string_view key,
                                                         std::span<std::byte> output) noexcept = 0;
    [[nodiscard]] virtual core::Status write(std::string_view key,
                                             std::span<const std::byte> value) noexcept = 0;
};

} // namespace blip::storage
