#include "blip/storage/esp_nvs_legacy_source.hpp"

#include "nvs_flash.h"

namespace blip::storage {
namespace {

[[nodiscard]] core::Error source_error(core::ErrorCode code, std::string_view operation,
                                       std::string_view detail) noexcept {
    return {core::ErrorDomain::storage, code, "blip.storage.legacy-import", operation, detail};
}

[[nodiscard]] core::Error map_error(esp_err_t error, std::string_view operation) noexcept {
    switch (error) {
    case ESP_ERR_NVS_NOT_FOUND:
        return source_error(core::ErrorCode::not_found, operation, "v1-settings-not-found");
    case ESP_ERR_NVS_INVALID_LENGTH:
        return source_error(core::ErrorCode::capacity_exceeded, operation, "v1-settings-too-large");
    case ESP_ERR_NVS_NEW_VERSION_FOUND:
        return source_error(core::ErrorCode::incompatible_version, operation, "nvs-newer-version");
    case ESP_ERR_NO_MEM:
        return source_error(core::ErrorCode::storage_full, operation, "nvs-no-memory");
    default:
        return source_error(core::ErrorCode::io_failed, operation, "nvs-read-failed");
    }
}

} // namespace

EspNvsLegacySettingsSource::~EspNvsLegacySettingsSource() { static_cast<void>(stop()); }

core::Status EspNvsLegacySettingsSource::start() noexcept {
    if (started_) {
        return core::Status::success();
    }
    const esp_err_t initialized = nvs_flash_init();
    if (initialized != ESP_OK) {
        return core::Status::failure(map_error(initialized, "legacy-nvs-init"));
    }
    const esp_err_t opened = nvs_open("blip", NVS_READONLY, &handle_);
    if (opened == ESP_ERR_NVS_NOT_FOUND) {
        handle_ = 0;
        source_available_ = false;
        started_ = true;
        return core::Status::success();
    }
    if (opened != ESP_OK) {
        handle_ = 0;
        return core::Status::failure(map_error(opened, "legacy-nvs-open"));
    }
    source_available_ = true;
    started_ = true;
    return core::Status::success();
}

core::Status EspNvsLegacySettingsSource::stop() noexcept {
    if (!started_) {
        return core::Status::success();
    }
    if (handle_ != 0) {
        nvs_close(handle_);
    }
    handle_ = 0;
    source_available_ = false;
    started_ = false;
    return core::Status::success();
}

core::Result<std::size_t> EspNvsLegacySettingsSource::read(std::span<std::byte> output) noexcept {
    if (!started_) {
        return core::Result<std::size_t>::failure(
            source_error(core::ErrorCode::invalid_state, "legacy-nvs-read", "not-started"));
    }
    if (!source_available_) {
        return core::Result<std::size_t>::failure(
            source_error(core::ErrorCode::not_found, "legacy-nvs-read", "v1-settings-not-found"));
    }
    std::size_t required_size{};
    esp_err_t result = nvs_get_blob(handle_, "settings", nullptr, &required_size);
    if (result != ESP_OK) {
        return core::Result<std::size_t>::failure(map_error(result, "legacy-nvs-read-size"));
    }
    if (required_size == 0U || required_size > output.size()) {
        return core::Result<std::size_t>::failure(
            source_error(required_size > output.size() ? core::ErrorCode::capacity_exceeded
                                                       : core::ErrorCode::corrupt_data,
                         "legacy-nvs-read", "invalid-v1-settings-size"));
    }
    std::size_t actual_size = output.size();
    result = nvs_get_blob(handle_, "settings", output.data(), &actual_size);
    if (result != ESP_OK) {
        return core::Result<std::size_t>::failure(map_error(result, "legacy-nvs-read"));
    }
    return core::Result<std::size_t>::success(actual_size);
}

} // namespace blip::storage
