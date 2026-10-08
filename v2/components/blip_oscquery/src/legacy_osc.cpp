#include "blip/oscquery/legacy_osc.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace blip::oscquery {
namespace {

[[nodiscard]] core::Status failure(core::ErrorCode code, std::string_view operation,
                                   std::string_view detail) noexcept {
    return core::Status::failure(
        {core::ErrorDomain::transport, code, "blip.oscquery", operation, detail});
}

[[nodiscard]] std::string_view legacy_path(const core::ComponentDescriptor& descriptor) noexcept {
    for (const auto& metadata : descriptor.metadata) {
        if (metadata.key == "legacy_path") {
            return metadata.value;
        }
    }
    return {};
}

[[nodiscard]] bool component_path(const core::ComponentDescriptor& descriptor,
                                  std::span<char> output, std::size_t& size) noexcept {
    const auto legacy = legacy_path(descriptor);
    if (!legacy.empty()) {
        if (legacy.size() >= output.size() || legacy.front() != '/') {
            return false;
        }
        std::copy(legacy.begin(), legacy.end(), output.begin());
        size = legacy.size();
        output[size] = '\0';
        return true;
    }
    if (descriptor.id.size() + 1U >= output.size()) {
        return false;
    }
    output[0] = '/';
    size = 1U;
    for (const char character : descriptor.id) {
        output[size++] = character == '.' ? '/' : character;
    }
    output[size] = '\0';
    return true;
}

[[nodiscard]] bool prefix_matches(std::string_view address, std::string_view prefix) noexcept {
    return address.size() > prefix.size() && address.starts_with(prefix) &&
           address[prefix.size()] == '/';
}

[[nodiscard]] const core::ParameterDescriptor*
find_parameter(const core::ComponentDescriptor& descriptor, std::string_view id) noexcept {
    const auto found =
        std::find_if(descriptor.parameters.begin(), descriptor.parameters.end(),
                     [id](const core::ParameterDescriptor& value) { return value.id == id; });
    return found == descriptor.parameters.end() ? nullptr : &*found;
}

[[nodiscard]] const core::LegacyParameterAlias*
find_legacy_parameter(const core::ComponentDescriptor& descriptor, std::string_view id) noexcept {
    const auto found =
        std::find_if(descriptor.legacy_parameters.begin(), descriptor.legacy_parameters.end(),
                     [id](const core::LegacyParameterAlias& value) { return value.id == id; });
    return found == descriptor.legacy_parameters.end() ? nullptr : &*found;
}

[[nodiscard]] const core::ActionDescriptor* find_action(const core::ComponentDescriptor& descriptor,
                                                        std::string_view id) noexcept {
    const auto found =
        std::find_if(descriptor.actions.begin(), descriptor.actions.end(),
                     [id](const core::ActionDescriptor& value) { return value.id == id; });
    return found == descriptor.actions.end() ? nullptr : &*found;
}

[[nodiscard]] bool to_core_value(const OscValue& input, core::ScalarValue& output) noexcept {
    switch (input.type) {
    case OscValueType::boolean:
        output = core::ScalarValue::from_bool(input.boolean);
        return true;
    case OscValueType::int32:
        output = core::ScalarValue::from_integer(input.integer);
        return true;
    case OscValueType::float32:
        if (!std::isfinite(input.number)) {
            return false;
        }
        output = core::ScalarValue::from_number(input.number);
        return true;
    case OscValueType::string:
        output = core::ScalarValue::from_string(input.string);
        return true;
    case OscValueType::rgba:
    case OscValueType::midi:
    case OscValueType::timetag:
        return false;
    }
    return false;
}

[[nodiscard]] bool to_osc_value(const core::ScalarValue& input, OscValue& output) noexcept {
    switch (input.type) {
    case core::ValueType::boolean:
        output = OscValue::from_bool(input.boolean);
        return true;
    case core::ValueType::integer:
        if (input.integer < std::numeric_limits<std::int32_t>::min() ||
            input.integer > std::numeric_limits<std::int32_t>::max()) {
            return false;
        }
        output = OscValue::from_integer(static_cast<std::int32_t>(input.integer));
        return true;
    case core::ValueType::number:
        if (!std::isfinite(input.number) ||
            std::abs(input.number) > std::numeric_limits<float>::max()) {
            return false;
        }
        output = OscValue::from_number(static_cast<float>(input.number));
        return true;
    case core::ValueType::string:
        if (input.string.size() > kMaxOscStringBytes) {
            return false;
        }
        output = OscValue::from_string(input.string);
        return true;
    }
    return false;
}

[[nodiscard]] bool same_scalar(const core::ScalarValue& left,
                               const core::ScalarValue& right) noexcept {
    if (left.type != right.type) {
        return false;
    }
    switch (left.type) {
    case core::ValueType::boolean:
        return left.boolean == right.boolean;
    case core::ValueType::integer:
        return left.integer == right.integer;
    case core::ValueType::number:
        return left.number == right.number;
    case core::ValueType::string:
        return left.string == right.string;
    }
    return false;
}

[[nodiscard]] bool legacy_to_core(const OscValue& input, const core::LegacyParameterAlias* alias,
                                  core::ScalarValue& output) noexcept {
    if (alias == nullptr || alias->enum_values.empty()) {
        return to_core_value(input, output);
    }
    if (input.type != OscValueType::string) {
        return false;
    }
    for (const auto& value : alias->enum_values) {
        if (value.label == input.string) {
            output = value.canonical_value;
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool core_to_legacy(const core::ScalarValue& input,
                                  const core::LegacyParameterAlias* alias,
                                  OscValue& output) noexcept {
    if (alias == nullptr || alias->enum_values.empty()) {
        return to_osc_value(input, output);
    }
    for (const auto& value : alias->enum_values) {
        if (same_scalar(value.canonical_value, input)) {
            output = OscValue::from_string(value.label);
            return true;
        }
    }
    return false;
}

} // namespace

LegacyOscEndpoint::LegacyOscEndpoint(const core::RegistryView& registry,
                                     core::ControlService& controls,
                                     DeviceIdentity identity) noexcept
    : registry_(&registry), controls_(&controls), identity_(identity) {}

core::Status LegacyOscEndpoint::handle(const OscMessage& request, std::string_view local_ip,
                                       bool udp_feedback, OscMessage& response,
                                       bool& should_reply) noexcept {
    response = {};
    should_reply = false;
    if (request.address == "/yo") {
        if (request.argument_count > 1U ||
            (request.argument_count == 1U && request.arguments[0].type != OscValueType::string)) {
            return failure(core::ErrorCode::invalid_argument, "discovery", "host");
        }
        response.address = "/wassup";
        response.arguments[0] = OscValue::from_string(local_ip);
        response.arguments[1] = OscValue::from_string(identity_.id);
        response.arguments[2] = OscValue::from_string(identity_.type);
        response.arguments[3] = OscValue::from_string(identity_.name);
        response.arguments[4] = OscValue::from_string(identity_.version);
        response.argument_count = 5U;
        should_reply = true;
        return core::Status::success();
    }
    if (request.address == "/ping") {
        if (request.argument_count > 1U ||
            (request.argument_count == 1U && request.arguments[0].type != OscValueType::string)) {
            return failure(core::ErrorCode::invalid_argument, "liveness", "host");
        }
        response.address = "/pong";
        response.arguments[0] = OscValue::from_string(identity_.id);
        response.argument_count = 1U;
        should_reply = true;
        return core::Status::success();
    }
    const auto status = handle_control(request, udp_feedback, response);
    should_reply = static_cast<bool>(status);
    return status;
}

core::Status LegacyOscEndpoint::handle_control(const OscMessage& request, bool udp_feedback,
                                               OscMessage& response) noexcept {
    if (request.argument_count > kMaxOscControlArguments) {
        return failure(core::ErrorCode::capacity_exceeded, "route", "too-many-arguments");
    }
    const core::ComponentDescriptor* matched{};
    std::size_t matched_size{};
    std::array<char, kMaxOscAddressBytes + 1U> path{};
    for (std::size_t index = 0; index < registry_->component_count(); ++index) {
        const auto& descriptor = registry_->component_descriptor(index);
        std::size_t path_size{};
        if (!component_path(descriptor, path, path_size)) {
            return failure(core::ErrorCode::capacity_exceeded, "route", "component-path");
        }
        const std::string_view candidate{path.data(), path_size};
        if (candidate.size() > matched_size && prefix_matches(request.address, candidate)) {
            matched = &descriptor;
            matched_size = candidate.size();
        }
    }
    if (matched == nullptr) {
        return failure(core::ErrorCode::not_found, "route", "component");
    }
    const std::string_view control_id = request.address.substr(matched_size + 1U);
    if (control_id.empty() || control_id.find('/') != std::string_view::npos) {
        return failure(core::ErrorCode::not_found, "route", "control");
    }

    core::ControlRequest control{};
    control.component_id = matched->id;
    const auto* alias = find_legacy_parameter(*matched, control_id);
    const auto* parameter =
        find_parameter(*matched, alias == nullptr ? control_id : alias->parameter_id);
    control.control_id = parameter == nullptr ? control_id : parameter->id;
    const auto* action = find_action(*matched, control_id);
    core::DynamicSchemaLease schema;
    core::ParameterDescriptor dynamic_parameter{};
    core::ActionDescriptor dynamic_action{};
    std::array<core::FieldDescriptor, core::kMaxControlValues> dynamic_fields{};
    if (!parameter && !action) {
        const auto acquired = registry_->acquire_dynamic_schema(matched->id, schema);
        if (!acquired) return acquired;
        for (std::size_t i = 0; i < schema.size(); ++i) {
            if (schema.id(i) != control_id) continue;
            if (schema.kind(i) == core::DynamicControlKind::parameter) {
                const auto projected = schema.parameter(i, dynamic_parameter);
                if (!projected) return projected;
                parameter = &dynamic_parameter;
            } else if (schema.kind(i) == core::DynamicControlKind::action) {
                const auto projected = schema.action(i, dynamic_fields, dynamic_action);
                if (!projected) return projected;
                action = &dynamic_action;
            }
            control.generation = schema.generation(); break;
        }
    }
    if (parameter != nullptr) {
        control.operation = request.argument_count == 0U ? core::ControlOperation::read_parameter
                                                         : core::ControlOperation::write_parameter;
    } else if (action != nullptr) {
        control.operation = core::ControlOperation::invoke_action;
    } else {
        return failure(core::ErrorCode::not_found, "route", "control");
    }

    std::array<core::ScalarValue, core::kMaxControlValues> values{};
    for (std::size_t index = 0; index < request.argument_count; ++index) {
        if (!legacy_to_core(request.arguments[index], parameter == nullptr ? nullptr : alias,
                            values[index])) {
            return failure(core::ErrorCode::invalid_argument, "route", "argument-type");
        }
    }
    control.values = {values.data(), request.argument_count};
    core::ControlResponse control_response{};
    const auto status = controls_->execute(control, control_response);
    if (!status) {
        return status;
    }

    if (request.address.size() >= response_address_.size()) {
        return failure(core::ErrorCode::capacity_exceeded, "feedback", "address");
    }
    std::copy(request.address.begin(), request.address.end(), response_address_.begin());
    response_address_[request.address.size()] = '\0';
    response.address = {response_address_.data(), request.address.size()};
    std::size_t output_index{};
    if (udp_feedback) {
        response.arguments[output_index++] = OscValue::from_string(identity_.id);
    }
    if (control_response.value_count != 0U) {
        for (std::size_t index = 0; index < control_response.value_count; ++index) {
            if (!core_to_legacy(control_response.values[index], alias,
                                response.arguments[output_index++])) {
                return failure(core::ErrorCode::validation_failed, "feedback", "result-type");
            }
        }
    } else if (control.operation == core::ControlOperation::write_parameter) {
        for (std::size_t index = 0; index < request.argument_count; ++index) {
            response.arguments[output_index++] = request.arguments[index];
        }
    }
    response.argument_count = output_index;
    std::size_t string_bytes{};
    for (std::size_t i = 0; i < output_index; ++i) {
        if (response.arguments[i].type != OscValueType::string) continue;
        if (response.arguments[i].string.size() > response_strings_.size() - string_bytes)
            return failure(core::ErrorCode::capacity_exceeded, "feedback", "string-storage");
        string_bytes += response.arguments[i].string.size();
    }
    std::array<char, core::kControlResponseStringBytes> strings{};
    string_bytes = 0;
    for (std::size_t i = 0; i < output_index; ++i) {
        auto& value = response.arguments[i]; if (value.type != OscValueType::string) continue;
        if (!value.string.empty()) std::memcpy(strings.data() + string_bytes, value.string.data(), value.string.size());
        value.string = {response_strings_.data() + string_bytes, value.string.size()}; string_bytes += value.string.size();
    }
    response_strings_ = strings;
    return core::Status::success();
}

} // namespace blip::oscquery
