#include "blip/storage/legacy_settings_import_component.hpp"

#include <array>
#include <string_view>

namespace blip::storage {
namespace {

constexpr std::array<std::string_view, 1> kProvidedServices{"storage.legacy_import"};
constexpr std::array<std::string_view, 1> kRequiredServices{"storage.settings"};

[[nodiscard]] constexpr core::ComponentDescriptor import_descriptor() noexcept {
    core::ComponentDescriptor descriptor{};
    descriptor.schema_version = 1;
    descriptor.id = "blip.storage.legacy-import";
    descriptor.display_name = "V1 settings importer";
    descriptor.description = "Bounded, journaled import of the read-only V1 MessagePack blob";
    descriptor.provided_services = kProvidedServices;
    descriptor.required_services = kRequiredServices;
    descriptor.settings = {1, 1};
    descriptor.disable_policy = core::DisablePolicy::reboot_required;
    descriptor.cost = {32768, 12288, 0};
    return descriptor;
}

} // namespace

const core::ComponentDescriptor LegacySettingsImportComponent::descriptor_{import_descriptor()};

LegacySettingsImportComponent::LegacySettingsImportComponent(SettingsStore& settings) noexcept
    : coordinator_(settings, source_, source_buffer_, payload_buffer_, readback_buffer_,
                   workspace_) {}

const core::ComponentDescriptor& LegacySettingsImportComponent::descriptor() const noexcept {
    return descriptor_;
}

core::Status LegacySettingsImportComponent::start(const core::StartContext&) noexcept {
    if (started_) {
        return core::Status::success();
    }
    auto status = source_.start();
    if (!status) {
        return status;
    }
    const auto imported = coordinator_.run();
    const auto stopped = source_.stop();
    if (!imported) {
        return core::Status::failure(imported.error());
    }
    if (!stopped) {
        return stopped;
    }
    disposition_ = imported.value().disposition;
    started_ = true;
    return core::Status::success();
}

core::Status LegacySettingsImportComponent::confirm_boot() noexcept {
    if (!started_) {
        return core::Status::failure({core::ErrorDomain::storage, core::ErrorCode::invalid_state,
                                      descriptor_.id, "confirm-import", "not-started"});
    }
    const auto status = coordinator_.confirm_boot();
    if (status && disposition_ != LegacyImportDisposition::no_source) {
        disposition_ = LegacyImportDisposition::already_confirmed;
    }
    return status;
}

core::Status LegacySettingsImportComponent::stop() noexcept {
    const auto status = source_.stop();
    if (status) {
        started_ = false;
    }
    return status;
}

} // namespace blip::storage
