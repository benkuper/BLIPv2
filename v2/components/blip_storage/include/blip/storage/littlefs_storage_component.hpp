#pragma once

#include "blip/core/component.hpp"
#include "blip/storage/atomic_file_store.hpp"
#include "blip/storage/posix_file_backend.hpp"
#include "blip/storage/web_asset_store.hpp"

#include <array>
#include <cstddef>

namespace blip::storage {

class LittleFsStorageComponent final : public core::Component {
  public:
    static constexpr std::size_t kScratchBytes = 4096;

    LittleFsStorageComponent() noexcept;

    [[nodiscard]] const core::ComponentDescriptor& descriptor() const noexcept override;
    [[nodiscard]] core::Status start(const core::StartContext&) noexcept override;
    [[nodiscard]] core::Status stop() noexcept override;
    [[nodiscard]] core::Status read_parameter(std::string_view id,
                                              core::ScalarValue& output) noexcept override;
    [[nodiscard]] AtomicFileStore& files() noexcept { return store_; }
    [[nodiscard]] WebAssetStore& web_assets() noexcept { return web_assets_; }

  private:
    static const core::ComponentDescriptor descriptor_;

    PosixFileBackend backend_;
    std::array<std::byte, kScratchBytes> scratch_{};
    AtomicFileStore store_;
    std::array<std::byte, 512> web_scratch_{};
    WebAssetStore web_assets_;
    bool mounted_{};
};

static_assert(sizeof(LittleFsStorageComponent) <= 8192,
              "LittleFS service exceeds its declared static RAM budget");

} // namespace blip::storage
