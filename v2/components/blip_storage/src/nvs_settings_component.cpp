#include "blip/storage/nvs_settings_component.hpp"

#include <array>
#include <string_view>

namespace blip::storage {
namespace {

constexpr std::array<std::string_view, 1> kProvidedServices{"storage.settings"};

[[nodiscard]] constexpr core::ComponentDescriptor settings_descriptor() noexcept {
    core::ComponentDescriptor descriptor{};
    descriptor.schema_version = 1;
    descriptor.id = "blip.storage.settings";
    descriptor.display_name = "Settings storage";
    descriptor.description = "Versioned, two-slot per-component NVS settings service";
    descriptor.provided_services = kProvidedServices;
    descriptor.settings = {1, 1};
    descriptor.disable_policy = core::DisablePolicy::reboot_required;
    descriptor.cost = {24576, 4864, 0};
    return descriptor;
}

[[nodiscard]] core::Status component_status(const core::Status& status,
                                            std::string_view operation) noexcept {
    if (status) {
        return status;
    }
    return core::Status::failure({status.error().domain, status.error().code,
                                  "blip.storage.settings", operation, status.error().detail});
}

} // namespace

const core::ComponentDescriptor NvsSettingsComponent::descriptor_{settings_descriptor()};

NvsSettingsComponent::NvsSettingsComponent() noexcept
    : backend_("blip_v2"), store_(backend_, scratch_) {}

const core::ComponentDescriptor& NvsSettingsComponent::descriptor() const noexcept {
    return descriptor_;
}

core::Status NvsSettingsComponent::start(const core::StartContext&) noexcept {
    return component_status(backend_.start(), "start");
}

core::Status NvsSettingsComponent::stop() noexcept {
    return component_status(backend_.stop(), "stop");
}

} // namespace blip::storage
