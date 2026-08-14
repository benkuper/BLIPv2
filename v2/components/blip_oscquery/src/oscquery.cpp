#include "blip/oscquery/oscquery.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace blip::oscquery {
namespace {

[[nodiscard]] core::Status query_failure(core::ErrorCode code, std::string_view operation,
                                         std::string_view detail) noexcept {
    return core::Status::failure(
        {core::ErrorDomain::transport, code, "blip.oscquery", operation, detail});
}

class Writer {
  public:
    explicit Writer(TextSink& sink) noexcept : sink_(&sink) {}

    [[nodiscard]] bool append(std::string_view text) noexcept {
        if (!ok_ || !sink_->write(text)) {
            ok_ = false;
        }
        return ok_;
    }

    [[nodiscard]] bool quoted(std::string_view text) noexcept {
        if (!append("\"")) {
            return false;
        }
        for (const unsigned char character : text) {
            switch (character) {
            case '\"':
                if (!append("\\\"")) {
                    return false;
                }
                break;
            case '\\':
                if (!append("\\\\")) {
                    return false;
                }
                break;
            case '\b':
                if (!append("\\b")) {
                    return false;
                }
                break;
            case '\f':
                if (!append("\\f")) {
                    return false;
                }
                break;
            case '\n':
                if (!append("\\n")) {
                    return false;
                }
                break;
            case '\r':
                if (!append("\\r")) {
                    return false;
                }
                break;
            case '\t':
                if (!append("\\t")) {
                    return false;
                }
                break;
            default:
                if (character < 0x20U) {
                    constexpr char hex[] = "0123456789abcdef";
                    const std::array<char, 6> escape{{'\\', 'u', '0', '0',
                                                      hex[(character >> 4U) & 0x0fU],
                                                      hex[character & 0x0fU]}};
                    if (!append({escape.data(), escape.size()})) {
                        return false;
                    }
                } else {
                    const char value = static_cast<char>(character);
                    if (!append({&value, 1U})) {
                        return false;
                    }
                }
            }
        }
        return append("\"");
    }

    template <typename Number> [[nodiscard]] bool number(Number value) noexcept {
        std::array<char, 40> buffer{};
        const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
        return result.ec == std::errc{} &&
               append({buffer.data(), static_cast<std::size_t>(result.ptr - buffer.data())});
    }

    [[nodiscard]] bool ok() const noexcept { return ok_; }

  private:
    TextSink* sink_{};
    bool ok_{true};
};

[[nodiscard]] std::string_view metadata_value(const core::ComponentDescriptor& descriptor,
                                              std::string_view key) noexcept {
    for (const auto& metadata : descriptor.metadata) {
        if (metadata.key == key) {
            return metadata.value;
        }
    }
    return {};
}

[[nodiscard]] bool component_path(const core::ComponentDescriptor& descriptor,
                                  std::span<char> output, std::size_t& size) noexcept {
    const auto legacy = metadata_value(descriptor, "legacy_path");
    if (!legacy.empty()) {
        if (legacy.size() >= output.size() || legacy.front() != '/') {
            return false;
        }
        std::copy(legacy.begin(), legacy.end(), output.begin());
        size = legacy.size();
    } else {
        if (descriptor.id.size() + 1U >= output.size()) {
            return false;
        }
        output[0] = '/';
        size = 1U;
        for (const char character : descriptor.id) {
            output[size++] = character == '.' ? '/' : character;
        }
    }
    output[size] = '\0';
    return size > 1U && output[size - 1U] != '/';
}

[[nodiscard]] std::string_view next_segment(std::string_view full_path,
                                            std::string_view prefix) noexcept {
    std::string_view remainder{};
    if (prefix.empty()) {
        if (full_path.size() < 2U || full_path.front() != '/') {
            return {};
        }
        remainder = full_path.substr(1U);
    } else {
        if (full_path.size() <= prefix.size() + 1U || !full_path.starts_with(prefix) ||
            full_path[prefix.size()] != '/') {
            return {};
        }
        remainder = full_path.substr(prefix.size() + 1U);
    }
    const auto slash = remainder.find('/');
    return remainder.substr(0, slash);
}

[[nodiscard]] char type_tag(core::ValueType type) noexcept {
    switch (type) {
    case core::ValueType::boolean:
        return 'b';
    case core::ValueType::integer:
        return 'i';
    case core::ValueType::number:
        return 'f';
    case core::ValueType::string:
        return 's';
    }
    return '?';
}

[[nodiscard]] bool scalar(Writer& writer, const core::ScalarValue& value) noexcept {
    switch (value.type) {
    case core::ValueType::boolean:
        return writer.append(value.boolean ? "true" : "false");
    case core::ValueType::integer:
        return writer.number(value.integer);
    case core::ValueType::number:
        return std::isfinite(value.number) && writer.number(value.number);
    case core::ValueType::string:
        return writer.quoted(value.string);
    }
    return false;
}

class TreeWriter {
  public:
    TreeWriter(const core::RegistryView& registry, core::ControlService& controls,
               bool include_config, TextSink& sink) noexcept
        : registry_(&registry), controls_(&controls), include_config_(include_config),
          writer_(sink) {}

    [[nodiscard]] core::Status write() noexcept {
        const auto valid = validate_paths();
        if (!valid) {
            return valid;
        }
        if (!writer_.append("{\"DESCRIPTION\":\"Root\",\"FULL_PATH\":\"\",\"ACCESS\":0,"
                            "\"BLIP_KIND\":\"root\",")) {
            return overflow();
        }
        bool first = true;
        if (!write_contents({}, nullptr, first, 0U) || !writer_.append("}")) {
            return overflow();
        }
        return core::Status::success();
    }

  private:
    [[nodiscard]] core::Status overflow() const noexcept {
        return query_failure(core::ErrorCode::serialization_overflow, "tree", "sink");
    }

    [[nodiscard]] core::Status validate_paths() noexcept {
        std::array<char, kMaxOscAddressBytes + 1U> left{};
        std::array<char, kMaxOscAddressBytes + 1U> right{};
        for (std::size_t index = 0; index < registry_->component_count(); ++index) {
            std::size_t left_size{};
            if (!component_path(registry_->component_descriptor(index), left, left_size)) {
                return query_failure(core::ErrorCode::validation_failed, "tree", "component-path");
            }
            for (std::size_t prior = 0; prior < index; ++prior) {
                std::size_t right_size{};
                if (!component_path(registry_->component_descriptor(prior), right, right_size)) {
                    return query_failure(core::ErrorCode::validation_failed, "tree",
                                         "component-path");
                }
                if (std::string_view{left.data(), left_size} ==
                    std::string_view{right.data(), right_size}) {
                    return query_failure(core::ErrorCode::duplicate_id, "tree", "legacy-path");
                }
            }
        }
        return core::Status::success();
    }

    [[nodiscard]] const core::ComponentDescriptor*
    descriptor_at_path(std::string_view path) noexcept {
        std::array<char, kMaxOscAddressBytes + 1U> candidate{};
        for (std::size_t index = 0; index < registry_->component_count(); ++index) {
            std::size_t size{};
            const auto& descriptor = registry_->component_descriptor(index);
            if (!component_path(descriptor, candidate, size)) {
                return nullptr;
            }
            if (std::string_view{candidate.data(), size} == path) {
                return &descriptor;
            }
        }
        return nullptr;
    }

    [[nodiscard]] bool begin_item(std::string_view id, bool& first) noexcept {
        if ((!first && !writer_.append(",")) || !writer_.quoted(id) || !writer_.append(":")) {
            return false;
        }
        first = false;
        return true;
    }

    [[nodiscard]] bool write_value(const core::ComponentDescriptor& descriptor,
                                   const core::ParameterDescriptor& parameter,
                                   core::ScalarValue& current, bool& present) noexcept {
        present = false;
        if (parameter.access == core::Access::write_only) {
            return true;
        }
        core::ControlResponse response{};
        const auto status = controls_->execute(
            {core::ControlOperation::read_parameter, descriptor.id, parameter.id, {}}, response);
        if (!status || response.value_count != 1U || response.values[0].type != parameter.type) {
            return true;
        }
        current = response.values[0];
        present = true;
        return true;
    }

    [[nodiscard]] static const core::LegacyParameterAlias*
    legacy_alias(const core::ComponentDescriptor& descriptor,
                 const core::ParameterDescriptor& parameter) noexcept {
        const auto found =
            std::find_if(descriptor.legacy_parameters.begin(), descriptor.legacy_parameters.end(),
                         [&parameter](const core::LegacyParameterAlias& alias) {
                             return alias.parameter_id == parameter.id;
                         });
        return found == descriptor.legacy_parameters.end() ? nullptr : &*found;
    }

    [[nodiscard]] static bool same_scalar(const core::ScalarValue& left,
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

    [[nodiscard]] bool write_parameter_value(const core::ScalarValue& current,
                                             const core::LegacyParameterAlias* alias) noexcept {
        if (alias == nullptr || alias->enum_values.empty()) {
            return scalar(writer_, current);
        }
        for (const auto& value : alias->enum_values) {
            if (same_scalar(current, value.canonical_value)) {
                return writer_.quoted(value.label);
            }
        }
        return false;
    }

    [[nodiscard]] bool write_parameter(const core::ComponentDescriptor& descriptor,
                                       const core::ParameterDescriptor& parameter,
                                       std::string_view path, bool& first) noexcept {
        if (!include_config_ && parameter.persisted) {
            return true;
        }
        const auto* alias = legacy_alias(descriptor, parameter);
        const std::string_view id = alias == nullptr ? parameter.id : alias->id;
        const std::string_view label = alias == nullptr ? parameter.label : alias->label;
        const core::ValueType type = alias == nullptr ? parameter.type : alias->type;
        if (!begin_item(id, first) || !writer_.append("{\"DESCRIPTION\":") ||
            !writer_.quoted(label) || !writer_.append(",\"ACCESS\":")) {
            return false;
        }
        const int access = parameter.access == core::Access::read_only ? 1 : 3;
        if (!writer_.number(access) || !writer_.append(",\"TYPE\":")) {
            return false;
        }
        core::ScalarValue current{};
        bool value_present{};
        if (!write_value(descriptor, parameter, current, value_present)) {
            return false;
        }
        char tag = type_tag(type);
        if (type == core::ValueType::boolean && value_present) {
            tag = current.boolean ? 'T' : 'F';
        }
        if (!writer_.quoted({&tag, 1U}) || !writer_.append(",\"FULL_PATH\":")) {
            return false;
        }
        if (!write_control_path(path, id)) {
            return false;
        }
        if (value_present && (!writer_.append(",\"VALUE\":[") ||
                              !write_parameter_value(current, alias) || !writer_.append("]"))) {
            return false;
        }
        if (alias != nullptr && !alias->enum_values.empty()) {
            if (!writer_.append(",\"RANGE\":[{\"VALS\":[")) {
                return false;
            }
            for (std::size_t index = 0; index < alias->enum_values.size(); ++index) {
                if ((index != 0U && !writer_.append(",")) ||
                    !writer_.quoted(alias->enum_values[index].label)) {
                    return false;
                }
            }
            if (!writer_.append("]}]")) {
                return false;
            }
        } else if (parameter.bounds.present &&
                   (!writer_.append(",\"RANGE\":[{\"MIN\":") ||
                    !writer_.number(parameter.bounds.minimum) || !writer_.append(",\"MAX\":") ||
                    !writer_.number(parameter.bounds.maximum) || !writer_.append("}]"))) {
            return false;
        }
        if (!writer_.append(",\"BLIP_KIND\":\"parameter\",\"BLIP_PERSISTED\":")) {
            return false;
        }
        if (!writer_.append(parameter.persisted ? "true" : "false") ||
            !writer_.append(",\"BLIP_READABLE\":") ||
            !writer_.append(parameter.access == core::Access::write_only ? "false" : "true") ||
            !writer_.append(",\"BLIP_WRITABLE\":") ||
            !writer_.append(parameter.access == core::Access::read_only ? "false" : "true")) {
            return false;
        }
        if (parameter.bounds.present &&
            (!writer_.append(",\"BLIP_STEP\":") || !writer_.number(parameter.bounds.step))) {
            return false;
        }
        if (!parameter.unit.empty() &&
            (!writer_.append(",\"BLIP_UNIT\":") || !writer_.quoted(parameter.unit))) {
            return false;
        }
        return writer_.append("}");
    }

    template <typename Fields>
    [[nodiscard]] bool write_type_fields(const Fields& fields, bool impulse_if_empty) noexcept {
        if (fields.empty()) {
            return writer_.quoted(impulse_if_empty ? "I" : "");
        }
        std::array<char, core::kMaxControlValues> tags{};
        if (fields.size() > tags.size()) {
            return false;
        }
        for (std::size_t index = 0; index < fields.size(); ++index) {
            tags[index] = type_tag(fields[index].type);
        }
        return writer_.quoted({tags.data(), fields.size()});
    }

    template <typename Fields>
    [[nodiscard]] bool write_field_schema(const Fields& fields) noexcept {
        if (!writer_.append(",\"BLIP_FIELDS\":[")) {
            return false;
        }
        for (std::size_t index = 0; index < fields.size(); ++index) {
            const char tag = type_tag(fields[index].type);
            if ((index != 0U && !writer_.append(",")) || !writer_.append("{\"ID\":") ||
                !writer_.quoted(fields[index].id) || !writer_.append(",\"TYPE\":") ||
                !writer_.quoted({&tag, 1U}) || !writer_.append(",\"REQUIRED\":") ||
                !writer_.append(fields[index].required ? "true" : "false") ||
                !writer_.append("}")) {
                return false;
            }
        }
        return writer_.append("]");
    }

    [[nodiscard]] bool write_action(const core::ActionDescriptor& action, std::string_view path,
                                    bool& first) noexcept {
        if (!begin_item(action.id, first) || !writer_.append("{\"DESCRIPTION\":") ||
            !writer_.quoted(action.label) || !writer_.append(",\"ACCESS\":3,\"TYPE\":") ||
            !write_type_fields(action.arguments, true) || !writer_.append(",\"FULL_PATH\":")) {
            return false;
        }
        return write_control_path(path, action.id) && writer_.append(",\"BLIP_KIND\":\"action\"") &&
               write_field_schema(action.arguments) && writer_.append("}");
    }

    [[nodiscard]] bool write_event(const core::EventDescriptor& event, std::string_view path,
                                   bool& first) noexcept {
        if (!begin_item(event.id, first) || !writer_.append("{\"DESCRIPTION\":") ||
            !writer_.quoted(event.id) || !writer_.append(",\"ACCESS\":1,\"TYPE\":") ||
            !write_type_fields(event.fields, false) || !writer_.append(",\"FULL_PATH\":")) {
            return false;
        }
        return write_control_path(path, event.id) && writer_.append(",\"BLIP_KIND\":\"event\"") &&
               write_field_schema(event.fields) && writer_.append("}");
    }

    [[nodiscard]] bool write_dynamic(const core::DynamicControl& control, std::string_view path,
                                     bool& first) noexcept {
        if (!begin_item(control.id, first) || !writer_.append("{\"DESCRIPTION\":") ||
            !writer_.quoted(control.id) || !writer_.append(",\"ACCESS\":")) {
            return false;
        }
        const bool event = control.kind == core::DynamicControlKind::event;
        if (!writer_.number(event ? 1 : 3) || !writer_.append(",\"TYPE\":")) {
            return false;
        }
        const char tag =
            control.kind == core::DynamicControlKind::action ? 'I' : type_tag(control.value_type);
        if (!writer_.quoted({&tag, 1U}) || !writer_.append(",\"FULL_PATH\":")) {
            return false;
        }
        if (!write_control_path(path, control.id) || !writer_.append(",\"BLIP_KIND\":")) {
            return false;
        }
        switch (control.kind) {
        case core::DynamicControlKind::parameter:
            return writer_.quoted("parameter") && writer_.append("}");
        case core::DynamicControlKind::action:
            return writer_.quoted("action") && writer_.append("}");
        case core::DynamicControlKind::event:
            return writer_.quoted("event") && writer_.append("}");
        }
        return false;
    }

    [[nodiscard]] bool write_control_path(std::string_view path, std::string_view id) noexcept {
        std::array<char, kMaxOscAddressBytes + 1U> full_path{};
        if (path.size() + id.size() + 1U >= full_path.size()) {
            return false;
        }
        std::copy(path.begin(), path.end(), full_path.begin());
        full_path[path.size()] = '/';
        std::copy(id.begin(), id.end(), full_path.begin() + path.size() + 1U);
        return writer_.quoted({full_path.data(), path.size() + id.size() + 1U});
    }

    [[nodiscard]] bool write_contents(std::string_view path,
                                      const core::ComponentDescriptor* descriptor, bool&,
                                      std::size_t depth) noexcept {
        if (depth > 16U || !writer_.append("\"CONTENTS\":{")) {
            return false;
        }
        bool contents_first = true;
        if (descriptor != nullptr) {
            for (const auto& parameter : descriptor->parameters) {
                if (!write_parameter(*descriptor, parameter, path, contents_first)) {
                    return false;
                }
            }
            for (const auto& action : descriptor->actions) {
                if (!write_action(action, path, contents_first)) {
                    return false;
                }
            }
            for (const auto& event : descriptor->events) {
                if (!write_event(event, path, contents_first)) {
                    return false;
                }
            }
            for (std::size_t index = 0; index < registry_->dynamic_control_count(); ++index) {
                const auto& control = registry_->dynamic_control(index);
                if (control.component_id == descriptor->id &&
                    !write_dynamic(control, path, contents_first)) {
                    return false;
                }
            }
        }

        std::array<char, kMaxOscAddressBytes + 1U> full{};
        std::array<char, kMaxOscAddressBytes + 1U> prior_full{};
        for (std::size_t index = 0; index < registry_->component_count(); ++index) {
            std::size_t full_size{};
            const auto& candidate_descriptor = registry_->component_descriptor(index);
            if (!component_path(candidate_descriptor, full, full_size)) {
                return false;
            }
            const auto segment = next_segment({full.data(), full_size}, path);
            if (segment.empty()) {
                continue;
            }
            bool first_segment = true;
            for (std::size_t prior = 0; prior < index; ++prior) {
                std::size_t prior_size{};
                if (!component_path(registry_->component_descriptor(prior), prior_full,
                                    prior_size)) {
                    return false;
                }
                if (next_segment({prior_full.data(), prior_size}, path) == segment) {
                    first_segment = false;
                    break;
                }
            }
            if (!first_segment || !begin_item(segment, contents_first)) {
                if (!first_segment) {
                    continue;
                }
                return false;
            }
            std::array<char, kMaxOscAddressBytes + 1U> child_path{};
            std::size_t child_size{};
            if (path.empty()) {
                child_path[0] = '/';
                std::copy(segment.begin(), segment.end(), child_path.begin() + 1U);
                child_size = segment.size() + 1U;
            } else {
                if (path.size() + segment.size() + 1U >= child_path.size()) {
                    return false;
                }
                std::copy(path.begin(), path.end(), child_path.begin());
                child_path[path.size()] = '/';
                std::copy(segment.begin(), segment.end(), child_path.begin() + path.size() + 1U);
                child_size = path.size() + segment.size() + 1U;
            }
            const std::string_view child{child_path.data(), child_size};
            const auto* child_descriptor = descriptor_at_path(child);
            const auto description =
                child_descriptor == nullptr ? segment : child_descriptor->display_name;
            if (!writer_.append("{\"DESCRIPTION\":") || !writer_.quoted(description) ||
                !writer_.append(",\"FULL_PATH\":") || !writer_.quoted(child) ||
                !writer_.append(",\"ACCESS\":0,\"BLIP_KIND\":")) {
                return false;
            }
            if (child_descriptor == nullptr) {
                if (!writer_.quoted("namespace")) {
                    return false;
                }
            } else if (!writer_.quoted("component") || !writer_.append(",\"BLIP_COMPONENT_ID\":") ||
                       !writer_.quoted(child_descriptor->id) ||
                       !writer_.append(",\"BLIP_SCHEMA_VERSION\":") ||
                       !writer_.number(child_descriptor->schema_version) ||
                       !writer_.append(",\"BLIP_DISABLE_POLICY\":") ||
                       !writer_.quoted(child_descriptor->disable_policy == core::DisablePolicy::live
                                           ? "live"
                                           : "reboot-required")) {
                return false;
            }
            if (!writer_.append(",")) {
                return false;
            }
            bool ignored{};
            if (!write_contents(child, child_descriptor, ignored, depth + 1U) ||
                !writer_.append("}")) {
                return false;
            }
        }
        return writer_.append("}");
    }

    const core::RegistryView* registry_{};
    core::ControlService* controls_{};
    bool include_config_{};
    Writer writer_;
};

} // namespace

core::Status write_oscquery_host_info(const DeviceIdentity& identity, TextSink& sink) noexcept {
    Writer writer{sink};
    if (!writer.append("{\"EXTENSIONS\":{\"ACCESS\":true,\"CLIPMODE\":false,"
                       "\"CRITICAL\":false,\"RANGE\":true,\"TAGS\":false,\"TYPE\":true,"
                       "\"UNIT\":false,\"VALUE\":true,\"LISTEN\":true,"
                       "\"PATH_ADDED\":true,\"PATH_REMOVED\":true,\"PATH_RENAMED\":true,"
                       "\"PATH_CHANGED\":false,\"BLIP_KIND\":true,\"BLIP_FIELDS\":true},"
                       "\"NAME\":") ||
        !writer.quoted(identity.name) || !writer.append(",\"VERSION\":") ||
        !writer.quoted(identity.version) || !writer.append(",\"DEVICE_TYPE\":") ||
        !writer.quoted(identity.type) || !writer.append(",\"DEVICE_ID\":") ||
        !writer.quoted(identity.id) || !writer.append(",\"OSC_PORT\":") ||
        !writer.number(identity.osc_port) || !writer.append(",\"OSC_TRANSPORT\":\"UDP\"}")) {
        return query_failure(core::ErrorCode::serialization_overflow, "host-info", "sink");
    }
    return core::Status::success();
}

core::Status write_oscquery_tree(const core::RegistryView& registry, core::ControlService& controls,
                                 bool include_config, TextSink& sink) noexcept {
    return TreeWriter{registry, controls, include_config, sink}.write();
}

} // namespace blip::oscquery
