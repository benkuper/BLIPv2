#include "blip/storage/littlefs_storage_component.hpp"

#include "esp_littlefs.h"
#include "esp_partition.h"

#include <algorithm>
#include <array>
#include <string_view>

namespace blip::storage {
namespace {

constexpr char kPartitionLabel[] = "storage";
constexpr std::array<std::string_view, 1> kProvidedServices{"storage.files.internal"};

[[nodiscard]] constexpr core::ComponentDescriptor storage_descriptor() noexcept {
    core::ComponentDescriptor descriptor{};
    descriptor.schema_version = 1;
    descriptor.id = "blip.storage.files.internal";
    descriptor.display_name = "Internal file storage";
    descriptor.description = "Atomic LittleFS file storage service";
    descriptor.provided_services = kProvidedServices;
    descriptor.settings = {1, 1};
    descriptor.disable_policy = core::DisablePolicy::reboot_required;
    descriptor.cost = {65536, 5120, 0};
    return descriptor;
}

[[nodiscard]] core::Error littlefs_error(core::ErrorCode code, std::string_view operation,
                                         std::string_view detail) noexcept {
    return {core::ErrorDomain::storage, code, "blip.storage.files.internal", operation, detail};
}

[[nodiscard]] bool partition_is_blank(const esp_partition_t& partition,
                                      std::span<std::byte> buffer) noexcept {
    for (std::size_t offset = 0; offset < partition.size; offset += buffer.size()) {
        const std::size_t remaining = static_cast<std::size_t>(partition.size - offset);
        const std::size_t count = std::min(buffer.size(), remaining);
        if (esp_partition_read(&partition, offset, buffer.data(), count) != ESP_OK) {
            return false;
        }
        if (!std::all_of(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(count),
                         [](std::byte value) { return value == std::byte{0xff}; })) {
            return false;
        }
    }
    return true;
}

} // namespace

const core::ComponentDescriptor LittleFsStorageComponent::descriptor_{storage_descriptor()};

LittleFsStorageComponent::LittleFsStorageComponent() noexcept
    : backend_("/littlefs"), store_(backend_, scratch_, FileCommitPolicy::atomic_rename) {}

const core::ComponentDescriptor& LittleFsStorageComponent::descriptor() const noexcept {
    return descriptor_;
}

core::Status LittleFsStorageComponent::start(const core::StartContext&) noexcept {
    if (mounted_) {
        return core::Status::success();
    }
    if (!backend_.valid()) {
        return core::Status::failure(
            littlefs_error(core::ErrorCode::invalid_argument, "start", "invalid-mount-path"));
    }
    const esp_partition_t* partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_LITTLEFS, kPartitionLabel);
    if (partition == nullptr) {
        return core::Status::failure(
            littlefs_error(core::ErrorCode::not_found, "start", "partition-not-found"));
    }
    const esp_vfs_littlefs_conf_t configuration{
        .base_path = "/littlefs",
        .partition_label = nullptr,
        .partition = partition,
        .blockdev = nullptr,
        .format_if_mount_failed = false,
        .read_only = false,
        .dont_mount = false,
        .grow_on_mount = false,
    };
    esp_err_t result = esp_vfs_littlefs_register(&configuration);
    if (result != ESP_OK && partition_is_blank(*partition, scratch_)) {
        static_cast<void>(esp_vfs_littlefs_unregister_partition(partition));
        result = esp_littlefs_format_partition(partition);
        if (result == ESP_OK) {
            result = esp_vfs_littlefs_register(&configuration);
        }
    }
    if (result != ESP_OK) {
        return core::Status::failure(
            littlefs_error(core::ErrorCode::corrupt_data, "start", "mount-failed"));
    }
    mounted_ = true;
    return core::Status::success();
}

core::Status LittleFsStorageComponent::stop() noexcept {
    if (!mounted_) {
        return core::Status::success();
    }
    const esp_partition_t* partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_LITTLEFS, kPartitionLabel);
    if (partition == nullptr || esp_vfs_littlefs_unregister_partition(partition) != ESP_OK) {
        return core::Status::failure(
            littlefs_error(core::ErrorCode::io_failed, "stop", "unmount-failed"));
    }
    mounted_ = false;
    return core::Status::success();
}

} // namespace blip::storage
