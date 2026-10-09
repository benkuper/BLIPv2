#include "blip/storage/device_identity_component.hpp"
#include <algorithm>
#include <cstring>

namespace blip::storage {
namespace {
constexpr std::array<std::string_view, 1> kRequires{"storage.settings"};
constexpr std::array<std::string_view, 1> kProvides{"device.identity"};
constexpr std::array<core::MetadataEntry, 1> kMetadata{{{"ui_topic", "System"}}};
core::Status failure(core::ErrorCode code, std::string_view detail) noexcept {
    return core::Status::failure({core::ErrorDomain::control, code, "blip.device.identity", "identity", detail});
}
bool valid_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > kMaxDeviceNameBytes || name.front() == ' ' || name.back() == ' ') return false;
    // Reject malformed UTF-8, controls, surrogates and overlong encodings.
    for (std::size_t i = 0; i < name.size();) {
        const auto first = static_cast<unsigned char>(name[i++]);
        if (first < 0x80U) { if (first < 0x20U || first == 0x7fU) return false; continue; }
        unsigned count{}; std::uint32_t cp{}, minimum{};
        if (first >= 0xc2U && first <= 0xdfU) { count = 1; cp = first & 0x1fU; minimum = 0x80; }
        else if (first >= 0xe0U && first <= 0xefU) { count = 2; cp = first & 0x0fU; minimum = 0x800; }
        else if (first >= 0xf0U && first <= 0xf4U) { count = 3; cp = first & 0x07U; minimum = 0x10000; }
        else return false;
        if (count > name.size() - i) return false;
        while (count-- != 0U) {
            const auto byte = static_cast<unsigned char>(name[i++]);
            if ((byte & 0xc0U) != 0x80U) return false;
            cp = (cp << 6U) | (byte & 0x3fU);
        }
        if (cp < minimum || cp > 0x10ffffU || (cp >= 0xd800U && cp <= 0xdfffU) ||
            (cp >= 0x80U && cp <= 0x9fU) || cp == 0x2028U || cp == 0x2029U) return false;
    }
    return true;
}
}

core::Status device_hostname(std::string_view name, std::span<const std::uint8_t, 6> mac,
    std::span<char> output) noexcept {
    if (!valid_name(name)) return failure(core::ErrorCode::invalid_argument, "invalid-name");
    if (output.size() < 64U) return failure(core::ErrorCode::capacity_exceeded, "hostname-buffer");
    std::size_t size{}; bool separator{};
    for (const auto character : name) {
        auto c = static_cast<unsigned char>(character);
        if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c + ('a' - 'A'));
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            if (separator && size != 0 && size < 50U) output[size++] = '-';
            if (size >= 50U) break;
            output[size++] = static_cast<char>(c); separator = false;
        } else separator = true;
    }
    while (size != 0 && output[size - 1] == '-') --size;
    if (size == 0) { std::memcpy(output.data(), "blip", 4); size = 4; }
    output[size++] = '-';
    constexpr char hex[] = "0123456789abcdef";
    for (auto byte : mac) { output[size++] = hex[byte >> 4U]; output[size++] = hex[byte & 15U]; }
    output[size] = '\0';
    return core::Status::success();
}

DeviceIdentityComponent::DeviceIdentityComponent(SettingsStore& settings, std::string_view type) noexcept
    : settings_(&settings), type_(type) {
    parameters_ = {{{"name", "Device name", core::ValueType::string, core::Access::read_write, true,
        core::ScalarValue::from_string(type), {}, ""},
        {"type", "Device type", core::ValueType::string, core::Access::read_only, false,
        core::ScalarValue::from_string(type), {}, ""}}};
    descriptor_.schema_version = 1; descriptor_.id = "blip.device.identity";
    descriptor_.display_name = "Device identity"; descriptor_.description = "Saved device name and board type";
    descriptor_.metadata = kMetadata; descriptor_.parameters = parameters_;
    descriptor_.required_services = kRequires; descriptor_.provided_services = kProvides;
    descriptor_.settings = {1, 1}; descriptor_.cost = {4096, 640, 0};
    if (valid_name(type)) std::copy(type.begin(), type.end(), state_.name.begin());
}
const core::ComponentDescriptor& DeviceIdentityComponent::descriptor() const noexcept { return descriptor_; }
core::Status DeviceIdentityComponent::start(const core::StartContext&) noexcept {
    const std::lock_guard lock{mutex_};
    if (started_) return core::Status::success();
    if (!valid_name(type_)) return failure(core::ErrorCode::invalid_argument, "invalid-type");
    std::array<std::byte, kMaxDeviceNameBytes> data{};
    const auto loaded = settings_->load(descriptor_, data);
    if (loaded) {
        const std::string_view name{reinterpret_cast<const char*>(data.data()), loaded.value().payload_size};
        if (!valid_name(name)) return failure(core::ErrorCode::corrupt_data, "saved-name");
        state_.name.fill(0); std::copy(name.begin(), name.end(), state_.name.begin());
    } else if (loaded.error().code != core::ErrorCode::not_found) return core::Status::failure(loaded.error());
    ++state_.revision; started_ = true; return core::Status::success();
}
core::Status DeviceIdentityComponent::stop() noexcept {
    const std::lock_guard lock{mutex_}; started_ = false; return core::Status::success();
}
DeviceNameSnapshot DeviceIdentityComponent::snapshot() const noexcept {
    const std::lock_guard lock{mutex_}; return state_;
}
core::Status DeviceIdentityComponent::read_parameter(std::string_view id, core::ScalarValue& value) noexcept {
    if (id == "type") { value = core::ScalarValue::from_string(type_); return core::Status::success(); }
    return failure(core::ErrorCode::invalid_argument, "owned-read-required");
}
core::Status DeviceIdentityComponent::read_parameter_owned(std::string_view id, core::ScalarValue& value,
    std::span<char> output) noexcept {
    if (id != "name") return read_parameter(id, value);
    const std::lock_guard lock{mutex_};
    const std::string_view name{state_.name.data()};
    if (output.size() < name.size()) return failure(core::ErrorCode::capacity_exceeded, "name-buffer");
    std::copy(name.begin(), name.end(), output.begin());
    value = core::ScalarValue::from_string({output.data(), name.size()}); return core::Status::success();
}
core::Status DeviceIdentityComponent::write_parameter(std::string_view id, const core::ScalarValue& value) noexcept {
    if (id != "name" || value.type != core::ValueType::string || !valid_name(value.string))
        return failure(core::ErrorCode::invalid_argument, "invalid-name");
    const std::lock_guard lock{mutex_};
    if (!started_) return failure(core::ErrorCode::invalid_state, "not-started");
    if (value.string == state_.name.data()) return core::Status::success();
    const auto saved = settings_->save(descriptor_, std::as_bytes(std::span{value.string.data(), value.string.size()}));
    if (!saved) return saved;
    state_.name.fill(0); std::copy(value.string.begin(), value.string.end(), state_.name.begin());
    ++state_.revision; return core::Status::success();
}
} // namespace blip::storage
