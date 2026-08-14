#pragma once

#include "blip/storage/blob_store.hpp"
#include "nvs.h"

#include <array>
#include <cstddef>
#include <span>
#include <string_view>

namespace blip::storage {

class EspNvsBlobStore final : public BlobStore {
  public:
    explicit EspNvsBlobStore(std::string_view namespace_name) noexcept;
    ~EspNvsBlobStore() override;

    EspNvsBlobStore(const EspNvsBlobStore&) = delete;
    EspNvsBlobStore& operator=(const EspNvsBlobStore&) = delete;
    EspNvsBlobStore(EspNvsBlobStore&&) = delete;
    EspNvsBlobStore& operator=(EspNvsBlobStore&&) = delete;

    [[nodiscard]] core::Status start() noexcept;
    [[nodiscard]] core::Status stop() noexcept;
    [[nodiscard]] bool started() const noexcept { return started_; }

    [[nodiscard]] core::Result<std::size_t> read(std::string_view key,
                                                 std::span<std::byte> output) noexcept override;
    [[nodiscard]] core::Status write(std::string_view key,
                                     std::span<const std::byte> value) noexcept override;

  private:
    [[nodiscard]] core::Status reopen_after_failure() noexcept;
    [[nodiscard]] static core::Error map_error(esp_err_t error,
                                               std::string_view operation) noexcept;
    [[nodiscard]] static bool copy_name(std::string_view source,
                                        std::span<char> destination) noexcept;

    std::array<char, NVS_NS_NAME_MAX_SIZE> namespace_name_{};
    nvs_handle_t handle_{};
    bool namespace_valid_{};
    bool started_{};
};

} // namespace blip::storage
