#include "blip/ota/esp_ota_component.hpp"

#include "esp_app_desc.h"
#include "esp_log.h"

#include <array>

namespace blip::ota {
namespace {

constexpr char kTag[] = "blip_ota";
constexpr std::array<std::string_view, 1> kProvidedServices{"firmware.ota"};
constexpr std::array<std::string_view, 1> kRequiredServices{"diagnostics.runtime"};
constexpr std::array<core::MetadataEntry, 4> kMetadata{{
    {"ui_topic", "Updates"},
    {"protocol", "raw-esp-image-v1"},
    {"rollback", "bootloader-confirmation"},
    {"signature", "esp-idf-policy"},
}};
constexpr std::array<core::ParameterDescriptor, 4> kParameters{{
    {"state",
     "Update state",
     core::ValueType::string,
     core::Access::read_only,
     false,
     core::ScalarValue::from_string("confirmed"),
     {},
     ""},
    {"received_bytes",
     "Received image bytes",
     core::ValueType::integer,
     core::Access::read_only,
     false,
     core::ScalarValue::from_integer(0),
     {},
     "bytes"},
    {"expected_bytes",
     "Expected image bytes",
     core::ValueType::integer,
     core::Access::read_only,
     false,
     core::ScalarValue::from_integer(0),
     {},
     "bytes"},
    {"signature_enforced",
     "Signed image policy",
     core::ValueType::boolean,
     core::Access::read_only,
     false,
     core::ScalarValue::from_bool(false),
     {},
     ""},
}};
constexpr std::array<core::ActionDescriptor, 1> kActions{{
    {"confirm_boot", "Confirm the running OTA image", {}},
}};

[[nodiscard]] constexpr core::ComponentDescriptor ota_descriptor() noexcept {
    core::ComponentDescriptor descriptor{};
    descriptor.schema_version = 1;
    descriptor.id = "blip.ota";
    descriptor.display_name = "Firmware update";
    descriptor.description =
        "Bounded A/B application update, validation, and rollback confirmation";
    descriptor.metadata = kMetadata;
    descriptor.provided_services = kProvidedServices;
    descriptor.required_services = kRequiredServices;
    descriptor.parameters = kParameters;
    descriptor.actions = kActions;
    descriptor.settings = {1, 1};
    descriptor.disable_policy = core::DisablePolicy::reboot_required;
    descriptor.cost = {32768, 1024, 0};
    return descriptor;
}

[[nodiscard]] core::Status ota_failure(core::ErrorCode code, std::string_view operation,
                                       std::string_view detail) noexcept {
    return core::Status::failure({core::ErrorDomain::storage, code, "blip.ota", operation, detail});
}

} // namespace

const core::ComponentDescriptor EspOtaComponent::descriptor_{ota_descriptor()};

std::size_t EspOtaBackend::maximum_image_size() const noexcept {
    const esp_partition_t* partition = esp_ota_get_next_update_partition(nullptr);
    return partition == nullptr ? 0U : partition->size;
}

core::Status EspOtaBackend::begin(std::size_t image_size) noexcept {
    if (handle_ != 0U) {
        return ota_failure(core::ErrorCode::invalid_state, "begin", "already-open");
    }
    update_partition_ = esp_ota_get_next_update_partition(nullptr);
    if (update_partition_ == nullptr || image_size > update_partition_->size ||
        esp_ota_begin(update_partition_, image_size, &handle_) != ESP_OK) {
        update_partition_ = nullptr;
        handle_ = 0;
        return ota_failure(core::ErrorCode::io_failed, "begin", "partition-open");
    }
    return core::Status::success();
}

core::Status EspOtaBackend::write(std::span<const std::byte> data) noexcept {
    if (handle_ == 0U || esp_ota_write(handle_, data.data(), data.size()) != ESP_OK) {
        return ota_failure(core::ErrorCode::io_failed, "write", "flash-write");
    }
    return core::Status::success();
}

core::Status EspOtaBackend::finish() noexcept {
    if (handle_ == 0U) {
        return ota_failure(core::ErrorCode::invalid_state, "finish", "not-open");
    }
    const esp_ota_handle_t handle = handle_;
    handle_ = 0;
    if (esp_ota_end(handle) != ESP_OK) {
        update_partition_ = nullptr;
        return ota_failure(core::ErrorCode::verification_failed, "finish", "idf-image-validation");
    }
    return core::Status::success();
}

void EspOtaBackend::abort() noexcept {
    if (handle_ != 0U) {
        static_cast<void>(esp_ota_abort(handle_));
    }
    handle_ = 0;
    update_partition_ = nullptr;
}

core::Status EspOtaBackend::activate() noexcept {
    if (handle_ != 0U || update_partition_ == nullptr ||
        esp_ota_set_boot_partition(update_partition_) != ESP_OK) {
        return ota_failure(core::ErrorCode::io_failed, "activate", "boot-partition");
    }
    update_partition_ = nullptr;
    return core::Status::success();
}

core::Status EspOtaBackend::confirm_running() noexcept {
    return esp_ota_mark_app_valid_cancel_rollback() == ESP_OK
               ? core::Status::success()
               : ota_failure(core::ErrorCode::io_failed, "confirm", "ota-ledger");
}

core::Status EspOtaBackend::rollback_running() noexcept {
    return esp_ota_mark_app_invalid_rollback_and_reboot() == ESP_OK
               ? core::Status::success()
               : ota_failure(core::ErrorCode::io_failed, "rollback", "ota-ledger");
}

bool EspOtaBackend::running_image_pending_confirmation() const noexcept {
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t state{};
    return running != nullptr && esp_ota_get_state_partition(running, &state) == ESP_OK &&
           state == ESP_OTA_IMG_PENDING_VERIFY;
}

bool EspOtaBackend::signature_enforced() const noexcept {
#if defined(CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT) || defined(CONFIG_SECURE_BOOT)
    return true;
#else
    return false;
#endif
}

EspOtaComponent::EspOtaComponent(std::string_view project, std::string_view target,
                                 std::string_view profile) noexcept
    : service_(backend_, project, target, profile) {}

const core::ComponentDescriptor& EspOtaComponent::descriptor() const noexcept {
    return descriptor_;
}

core::Status EspOtaComponent::start(const core::StartContext&) noexcept {
    service_.refresh_boot_state();
    started_ = true;
    ESP_LOGI(kTag, "OTA state=%s signed=%u", update_state_name(service_.status().state),
             static_cast<unsigned>(service_.status().signature_enforced));
    return core::Status::success();
}

core::Status EspOtaComponent::stop() noexcept {
    service_.cancel();
    started_ = false;
    return core::Status::success();
}

core::Status EspOtaComponent::read_parameter(std::string_view id,
                                             core::ScalarValue& output) noexcept {
    if (!started_) {
        return ota_failure(core::ErrorCode::invalid_state, "read-parameter", "not-started");
    }
    const auto& status = service_.status();
    if (id == "state") {
        output = core::ScalarValue::from_string(update_state_name(status.state));
    } else if (id == "received_bytes") {
        output = core::ScalarValue::from_integer(static_cast<std::int64_t>(status.received_bytes));
    } else if (id == "expected_bytes") {
        output = core::ScalarValue::from_integer(static_cast<std::int64_t>(status.expected_bytes));
    } else if (id == "signature_enforced") {
        output = core::ScalarValue::from_bool(status.signature_enforced);
    } else {
        return ota_failure(core::ErrorCode::not_found, "read-parameter", id);
    }
    return core::Status::success();
}

core::Status EspOtaComponent::invoke_action(std::string_view id,
                                            std::span<const core::ScalarValue> arguments,
                                            std::span<core::ScalarValue>,
                                            std::size_t& output_count) noexcept {
    output_count = 0;
    if (id != "confirm_boot") {
        return ota_failure(core::ErrorCode::not_found, "invoke-action", id);
    }
    if (!arguments.empty()) {
        return ota_failure(core::ErrorCode::validation_failed, "invoke-action", "arguments");
    }
    return service_.confirm_boot();
}

} // namespace blip::ota
