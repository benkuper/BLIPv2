#include "blip/storage/littlefs_storage_component.hpp"

#include "esp_littlefs.h"
#include "esp_partition.h"

#include <algorithm>
#include <array>
#include <string_view>

namespace blip::storage {
namespace {

constexpr char kPartitionLabel[] = "storage";
constexpr std::array<std::string_view, 3> kProvidedServices{"storage.files", "storage.files.internal",
                                                            "storage.web_assets"};
constexpr std::array<core::ParameterDescriptor, 5> kParameters{{
    {"preferred_medium", "File storage", core::ValueType::string, core::Access::read_only,
     false, core::ScalarValue::from_string("internal"), {}, {}},
    {"external_state", "External storage", core::ValueType::string, core::Access::read_only,
     false, core::ScalarValue::from_string("not-probed"), {}, {}},
    {"web_bundle_version",
     "Web bundle version",
     core::ValueType::integer,
     core::Access::read_only,
     false,
     core::ScalarValue::from_integer(0),
     {},
     "version"},
    {"web_asset_count",
     "Web asset count",
     core::ValueType::integer,
     core::Access::read_only,
     false,
     core::ScalarValue::from_integer(0),
     {true, 0, kMaxWebAssets, 1},
     "files"},
    {"web_bundle_bytes",
     "Web bundle size",
     core::ValueType::integer,
     core::Access::read_only,
     false,
     core::ScalarValue::from_integer(0),
     {true, 0, kMaxWebAssetBundleBytes, 1},
     "bytes"},
}};


[[nodiscard]] constexpr core::ComponentDescriptor storage_descriptor() noexcept {
    core::ComponentDescriptor descriptor{};
    descriptor.schema_version = 2;
    descriptor.id = "blip.storage.files.internal";
    descriptor.display_name = "File storage";
    descriptor.description = "Automatic external/internal storage for web, scripts and playback";
    descriptor.provided_services = kProvidedServices;
    descriptor.parameters = kParameters;
    descriptor.settings = {1, 1};
    descriptor.disable_policy = core::DisablePolicy::reboot_required;
    descriptor.cost = {131072, 12288, 0};
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

LittleFsStorageComponent::LittleFsStorageComponent(resources::BoardStorage external) noexcept
    : backend_("/littlefs"), external_backend_("/blipmedia/blip-v2"), external_(external),
      store_(backend_, scratch_), web_backend_(store_, backend_),
      web_assets_(web_backend_, web_scratch_) {}

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
    if (external_.mount()) {
        const auto selected = store_.set_external(&external_backend_);
        if (!selected) { static_cast<void>(stop()); return selected; }
    }
    web_assets_.cancel_install(); // Discard an interrupted staging file at boot.
    auto loaded = web_assets_.load_active();
    bool factory_recovery = false;
    if (!loaded && (loaded.error().code == core::ErrorCode::corrupt_data ||
        loaded.error().code == core::ErrorCode::incompatible_version ||
        loaded.error().code == core::ErrorCode::verification_failed)) {
        // Rollback can encounter an external UI format from a newer firmware.
        // Keep that copy intact and serve the checked internal factory fallback.
        web_backend_.use_factory();
        loaded = web_assets_.load_active();
        factory_recovery = true;
    }
    if (!loaded && loaded.error().code != core::ErrorCode::not_found &&
        loaded.error().code != core::ErrorCode::corrupt_data &&
        loaded.error().code != core::ErrorCode::incompatible_version &&
        loaded.error().code != core::ErrorCode::verification_failed) {
        static_cast<void>(stop()); return core::Status::failure(loaded.error());
    }
    const auto factory_size = backend_.file_size(WebAssetStore::kActivePath);
    std::uint32_t factory_code{};
    if (factory_size && factory_size.value() >= kWebAssetBundleHeaderBytes &&
        factory_size.value() <= kMaxWebAssetBundleBytes) {
        const auto header = backend_.read_at(WebAssetStore::kActivePath, 0,
            std::span(web_scratch_).first(kWebAssetBundleHeaderBytes));
        if (header && header.value() == kWebAssetBundleHeaderBytes &&
            web_scratch_[0] == std::byte{0x42} && web_scratch_[1] == std::byte{0x4c} &&
            web_scratch_[2] == std::byte{0x57} && web_scratch_[3] == std::byte{0x42}) {
            for (std::size_t i = 0; i < 4; ++i)
                factory_code |= std::uint32_t(std::to_integer<unsigned char>(web_scratch_[8 + i])) << (i * 8U);
        }
    }
    if (loaded && factory_size && factory_code &&
        (factory_code > loaded.value().bundle_version ||
         (external_.mounted() && web_backend_.using_factory() && !factory_recovery))) {
        // Factory flashing populates internal LittleFS. Move a checked copy to
        // external media without buffering the full UI or changing app slots.
        auto copied = web_assets_.begin_install(factory_size.value());
        for (std::size_t offset = 0; copied && offset < factory_size.value();) {
            const auto count = std::min(web_scratch_.size(), factory_size.value() - offset);
            const auto read = backend_.read_at(WebAssetStore::kActivePath, offset,
                                               std::span(web_scratch_).first(count));
            if (!read || read.value() != count) { copied = core::Status::failure(
                littlefs_error(core::ErrorCode::io_failed, "factory-copy", "read-failed")); break; }
            copied = web_assets_.append_install(std::span(web_scratch_).first(count));
            offset += count;
        }
        if (copied) static_cast<void>(web_assets_.finish_install());
        else web_assets_.cancel_install(); // Factory copy remains available.
    }
    return core::Status::success();
}

core::Status LittleFsStorageComponent::stop() noexcept {
    if (!mounted_) {
        return core::Status::success();
    }
    web_assets_.cancel_install();
    web_backend_.reset();
    const auto detached = store_.set_external(nullptr);
    if (!detached) return detached;
    if (!external_.unmount()) return core::Status::failure(
        littlefs_error(core::ErrorCode::io_failed, "stop", "external-unmount-failed"));
    const esp_partition_t* partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_LITTLEFS, kPartitionLabel);
    if (partition == nullptr || esp_vfs_littlefs_unregister_partition(partition) != ESP_OK) {
        return core::Status::failure(
            littlefs_error(core::ErrorCode::io_failed, "stop", "unmount-failed"));
    }
    mounted_ = false;
    return core::Status::success();
}

core::Status LittleFsStorageComponent::read_parameter(std::string_view id,
                                                      core::ScalarValue& output) noexcept {
    if (!mounted_) {
        return core::Status::failure(
            littlefs_error(core::ErrorCode::invalid_state, "read-parameter", "not-ready"));
    }
    if (id == "preferred_medium") {
        output = core::ScalarValue::from_string(store_.external() ? external_.media() : "internal");
    } else if (id == "external_state") {
        output = core::ScalarValue::from_string(external_.state());
    } else if (id == "web_bundle_version") {
        output = core::ScalarValue::from_integer(web_assets_.info().bundle_version);
    } else if (id == "web_asset_count") {
        output = core::ScalarValue::from_integer(web_assets_.info().asset_count);
    } else if (id == "web_bundle_bytes") {
        output = core::ScalarValue::from_integer(web_assets_.info().total_size);
    } else {
        return core::Status::failure(
            littlefs_error(core::ErrorCode::not_found, "read-parameter", "parameter"));
    }
    return core::Status::success();
}

} // namespace blip::storage
