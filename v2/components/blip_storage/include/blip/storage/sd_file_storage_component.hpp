#pragma once

#include "blip/core/component.hpp"
#include "blip/storage/atomic_file_store.hpp"
#include "blip/storage/posix_file_backend.hpp"

#include <span>
#include <string_view>

namespace blip::storage {

using StorageMountCallback = core::Status (*)(void* context) noexcept;

class SdFileStorageComponent final : public core::Component {
  public:
    SdFileStorageComponent(std::string_view mount_path, std::span<std::byte> scratch,
                           StorageMountCallback mount, StorageMountCallback unmount,
                           void* context) noexcept;

    [[nodiscard]] const core::ComponentDescriptor& descriptor() const noexcept override;
    [[nodiscard]] core::Status start(const core::StartContext&) noexcept override;
    [[nodiscard]] core::Status stop() noexcept override;
    [[nodiscard]] AtomicFileStore& files() noexcept { return store_; }

  private:
    static const core::ComponentDescriptor descriptor_;

    PosixFileBackend backend_;
    AtomicFileStore store_;
    StorageMountCallback mount_{};
    StorageMountCallback unmount_{};
    void* context_{};
    bool mounted_{};
};

} // namespace blip::storage
