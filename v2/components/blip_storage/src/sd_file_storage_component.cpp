#include "blip/storage/sd_file_storage_component.hpp"

#include <array>
#include <string_view>

namespace blip::storage {
namespace {

constexpr std::array<std::string_view, 1> kProvidedServices{"storage.files.sd"};
constexpr std::array<std::string_view, 1> kRequiredServices{"storage.sd.mount"};

[[nodiscard]] constexpr core::ComponentDescriptor storage_descriptor() noexcept {
    core::ComponentDescriptor descriptor{};
    descriptor.schema_version = 1;
    descriptor.id = "blip.storage.files.sd";
    descriptor.display_name = "SD file storage";
    descriptor.description = "Optional two-generation SD file storage service";
    descriptor.provided_services = kProvidedServices;
    descriptor.required_services = kRequiredServices;
    descriptor.settings = {1, 1};
    descriptor.disable_policy = core::DisablePolicy::reboot_required;
    descriptor.cost = {8192, 256, 0};
    return descriptor;
}

} // namespace

const core::ComponentDescriptor SdFileStorageComponent::descriptor_{storage_descriptor()};

SdFileStorageComponent::SdFileStorageComponent(std::string_view mount_path,
                                               std::span<std::byte> scratch,
                                               StorageMountCallback mount,
                                               StorageMountCallback unmount, void* context) noexcept
    : backend_(mount_path), store_(backend_, scratch, FileCommitPolicy::two_slot), mount_(mount),
      unmount_(unmount), context_(context) {}

const core::ComponentDescriptor& SdFileStorageComponent::descriptor() const noexcept {
    return descriptor_;
}

core::Status SdFileStorageComponent::start(const core::StartContext&) noexcept {
    if (mounted_) {
        return core::Status::success();
    }
    if (!backend_.valid() || mount_ == nullptr || unmount_ == nullptr) {
        return core::Status::failure({core::ErrorDomain::storage, core::ErrorCode::invalid_argument,
                                      descriptor_.id, "start", "invalid-mount-provider"});
    }
    const auto status = mount_(context_);
    if (!status) {
        return core::Status::failure({status.error().domain, status.error().code, descriptor_.id,
                                      "start", status.error().detail});
    }
    mounted_ = true;
    return core::Status::success();
}

core::Status SdFileStorageComponent::stop() noexcept {
    if (!mounted_) {
        return core::Status::success();
    }
    const auto status = unmount_(context_);
    if (!status) {
        return core::Status::failure({status.error().domain, status.error().code, descriptor_.id,
                                      "stop", status.error().detail});
    }
    mounted_ = false;
    return core::Status::success();
}

} // namespace blip::storage
