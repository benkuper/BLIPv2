#pragma once

#include "blip/storage/automatic_file_store.hpp"

namespace blip::storage {

// Adapts a web installation to the common automatic bulk-file API. The plain
// factory bundle remains readable in internal flash until a checked update is
// committed. A candidate is invisible after reboot until WebAssetStore has
// validated it and replace() publishes its durable commit trailer.
class AutomaticWebBackend final : public WebAssetBackend {
  public:
    AutomaticWebBackend(AutomaticFileStore& files, WebAssetBackend& factory) noexcept
        : files_(&files), factory_(&factory) {}
    ~AutomaticWebBackend() override;
    [[nodiscard]] core::Result<std::size_t> file_size(std::string_view path) noexcept override;
    [[nodiscard]] core::Result<std::size_t> read_at(std::string_view path, std::size_t offset,
                                                  std::span<std::byte> output) noexcept override;
    [[nodiscard]] core::Status begin_write(std::string_view path) noexcept override;
    [[nodiscard]] core::Status append_write(std::span<const std::byte> value) noexcept override;
    [[nodiscard]] core::Status finish_write() noexcept override;
    void abort_write() noexcept override;
    [[nodiscard]] core::Status replace(std::string_view source,
                                       std::string_view destination) noexcept override;
    [[nodiscard]] core::Status remove(std::string_view path) noexcept override;
    void reset() noexcept;
    // Startup/recovery only, while the WebAssetStore mutex is held.
    void use_factory() noexcept;
    [[nodiscard]] bool using_factory() const noexcept { return using_factory_; }

  private:
    AutomaticFileStore* files_{};
    WebAssetBackend* factory_{};
    FileReadHandle active_{};
    std::array<std::byte, kWebAssetBundleHeaderBytes> header_{};
    std::size_t header_size_{};
    std::size_t expected_{};
    std::uint32_t writer_{};
    bool writing_{};
    bool prepared_{};
    bool using_factory_{};
};
} // namespace blip::storage
