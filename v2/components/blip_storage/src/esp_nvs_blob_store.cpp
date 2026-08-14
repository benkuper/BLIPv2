#include "blip/storage/esp_nvs_blob_store.hpp"

#include "esp_err.h"
#include "nvs_flash.h"

#include <cstring>

namespace blip::storage {
namespace {

[[nodiscard]] std::string_view error_detail(esp_err_t error) noexcept {
    switch (error) {
    case ESP_ERR_NVS_NOT_FOUND:
        return "nvs-not-found";
    case ESP_ERR_NVS_NOT_ENOUGH_SPACE:
        return "nvs-full";
    case ESP_ERR_NVS_INVALID_NAME:
        return "nvs-invalid-name";
    case ESP_ERR_NVS_INVALID_HANDLE:
        return "nvs-invalid-handle";
    case ESP_ERR_NVS_REMOVE_FAILED:
        return "nvs-remove-failed";
    case ESP_ERR_NVS_KEY_TOO_LONG:
        return "nvs-key-too-long";
    case ESP_ERR_NVS_INVALID_STATE:
        return "nvs-invalid-state";
    case ESP_ERR_NVS_INVALID_LENGTH:
        return "nvs-invalid-length";
    case ESP_ERR_NVS_NO_FREE_PAGES:
        return "nvs-no-free-pages";
    case ESP_ERR_NVS_VALUE_TOO_LONG:
        return "nvs-value-too-long";
    case ESP_ERR_NVS_PART_NOT_FOUND:
        return "nvs-partition-not-found";
    case ESP_ERR_NVS_NEW_VERSION_FOUND:
        return "nvs-new-version";
    case ESP_ERR_NO_MEM:
        return "nvs-no-memory";
    case ESP_ERR_INVALID_ARG:
        return "nvs-invalid-argument";
    default:
        return "nvs-io-error";
    }
}

} // namespace

EspNvsBlobStore::EspNvsBlobStore(std::string_view namespace_name) noexcept
    : namespace_valid_(copy_name(namespace_name, namespace_name_)) {}

EspNvsBlobStore::~EspNvsBlobStore() { static_cast<void>(stop()); }

bool EspNvsBlobStore::copy_name(std::string_view source, std::span<char> destination) noexcept {
    if (source.empty() || source.size() >= destination.size()) {
        return false;
    }
    std::memcpy(destination.data(), source.data(), source.size());
    destination[source.size()] = '\0';
    return true;
}

core::Error EspNvsBlobStore::map_error(esp_err_t error, std::string_view operation) noexcept {
    core::ErrorCode code = core::ErrorCode::io_failed;
    switch (error) {
    case ESP_ERR_NVS_NOT_FOUND:
        code = core::ErrorCode::not_found;
        break;
    case ESP_ERR_NVS_NOT_ENOUGH_SPACE:
    case ESP_ERR_NVS_NO_FREE_PAGES:
    case ESP_ERR_NVS_VALUE_TOO_LONG:
    case ESP_ERR_NO_MEM:
        code = core::ErrorCode::storage_full;
        break;
    case ESP_ERR_NVS_INVALID_LENGTH:
        code = core::ErrorCode::capacity_exceeded;
        break;
    case ESP_ERR_NVS_INVALID_NAME:
    case ESP_ERR_NVS_KEY_TOO_LONG:
    case ESP_ERR_INVALID_ARG:
        code = core::ErrorCode::invalid_argument;
        break;
    case ESP_ERR_NVS_NEW_VERSION_FOUND:
        code = core::ErrorCode::incompatible_version;
        break;
    default:
        break;
    }
    return {core::ErrorDomain::storage, code, {}, operation, error_detail(error)};
}

core::Status EspNvsBlobStore::start() noexcept {
    if (started_) {
        return core::Status::success();
    }
    if (!namespace_valid_) {
        return core::Status::failure({core::ErrorDomain::storage,
                                      core::ErrorCode::invalid_argument,
                                      {},
                                      "nvs-start",
                                      "namespace-length"});
    }

    const esp_err_t initialization = nvs_flash_init();
    if (initialization != ESP_OK) {
        return core::Status::failure(map_error(initialization, "nvs-init"));
    }
    const esp_err_t opened = nvs_open(namespace_name_.data(), NVS_READWRITE, &handle_);
    if (opened != ESP_OK) {
        handle_ = 0;
        return core::Status::failure(map_error(opened, "nvs-open"));
    }
    started_ = true;
    return core::Status::success();
}

core::Status EspNvsBlobStore::stop() noexcept {
    if (!started_) {
        return core::Status::success();
    }
    nvs_close(handle_);
    handle_ = 0;
    started_ = false;
    return core::Status::success();
}

core::Status EspNvsBlobStore::reopen_after_failure() noexcept {
    if (started_) {
        nvs_close(handle_);
    }
    handle_ = 0;
    started_ = false;
    const esp_err_t initialization = nvs_flash_init();
    if (initialization != ESP_OK) {
        return core::Status::failure(map_error(initialization, "nvs-recover-init"));
    }
    const esp_err_t opened = nvs_open(namespace_name_.data(), NVS_READWRITE, &handle_);
    if (opened != ESP_OK) {
        handle_ = 0;
        return core::Status::failure(map_error(opened, "nvs-recover-open"));
    }
    started_ = true;
    return core::Status::success();
}

core::Result<std::size_t> EspNvsBlobStore::read(std::string_view key,
                                                std::span<std::byte> output) noexcept {
    if (!started_) {
        return core::Result<std::size_t>::failure({core::ErrorDomain::storage,
                                                   core::ErrorCode::invalid_state,
                                                   {},
                                                   "nvs-read",
                                                   "not-started"});
    }
    std::array<char, NVS_KEY_NAME_MAX_SIZE> terminated_key{};
    if (!copy_name(key, terminated_key)) {
        return core::Result<std::size_t>::failure({core::ErrorDomain::storage,
                                                   core::ErrorCode::invalid_argument,
                                                   {},
                                                   "nvs-read",
                                                   "key-length"});
    }

    std::size_t required_size{};
    esp_err_t result = nvs_get_blob(handle_, terminated_key.data(), nullptr, &required_size);
    if (result != ESP_OK) {
        return core::Result<std::size_t>::failure(map_error(result, "nvs-read-size"));
    }
    if (required_size > output.size()) {
        return core::Result<std::size_t>::failure({core::ErrorDomain::storage,
                                                   core::ErrorCode::capacity_exceeded,
                                                   {},
                                                   "nvs-read",
                                                   "output-too-small"});
    }
    std::size_t actual_size = output.size();
    result = nvs_get_blob(handle_, terminated_key.data(), output.data(), &actual_size);
    if (result != ESP_OK) {
        return core::Result<std::size_t>::failure(map_error(result, "nvs-read"));
    }
    return core::Result<std::size_t>::success(actual_size);
}

core::Status EspNvsBlobStore::write(std::string_view key,
                                    std::span<const std::byte> value) noexcept {
    if (!started_) {
        return core::Status::failure({core::ErrorDomain::storage,
                                      core::ErrorCode::invalid_state,
                                      {},
                                      "nvs-write",
                                      "not-started"});
    }
    std::array<char, NVS_KEY_NAME_MAX_SIZE> terminated_key{};
    if (!copy_name(key, terminated_key)) {
        return core::Status::failure({core::ErrorDomain::storage,
                                      core::ErrorCode::invalid_argument,
                                      {},
                                      "nvs-write",
                                      "key-length"});
    }

    esp_err_t result = nvs_set_blob(handle_, terminated_key.data(), value.data(), value.size());
    if (result != ESP_OK) {
        return core::Status::failure(map_error(result, "nvs-set-blob"));
    }
    result = nvs_commit(handle_);
    if (result != ESP_OK) {
        const core::Error original = map_error(result, "nvs-commit");
        static_cast<void>(reopen_after_failure());
        return core::Status::failure(original);
    }
    return core::Status::success();
}

} // namespace blip::storage
