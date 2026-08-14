#pragma once

#include "blip/core/component.hpp"
#include "blip/storage/atomic_file_store.hpp"
#include "blip/storage/posix_file_backend.hpp"

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
    [[nodiscard]] AtomicFileStore& files() noexcept { return store_; }

  private:
    static const core::ComponentDescriptor descriptor_;

    PosixFileBackend backend_;
    std::array<std::byte, kScratchBytes> scratch_{};
    AtomicFileStore store_;
    bool mounted_{};
};

static_assert(sizeof(LittleFsStorageComponent) <= 5120,
              "LittleFS service exceeds its declared static RAM budget");

} // namespace blip::storage
