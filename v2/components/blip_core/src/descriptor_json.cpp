#include "blip/core/descriptor_json.hpp"

#include <charconv>
#include <array>
#include <cstdint>
#include <string_view>

namespace blip::core {
namespace {

class Writer {
  public:
    explicit Writer(std::span<char> output) noexcept : output_(output) {}

    [[nodiscard]] bool append(std::string_view text) noexcept {
        if (text.size() >= output_.size() - position_) {
            return false;
        }
        for (const char character : text) {
            output_[position_++] = character;
        }
        return true;
    }

    [[nodiscard]] bool quoted(std::string_view text) noexcept {
        if (!append("\"")) {
            return false;
        }
        for (const char character : text) {
            if (static_cast<unsigned char>(character) < 0x20) {
                constexpr char digits[] = "0123456789abcdef";
                const std::array escaped{'\\', 'u', '0', '0', digits[(static_cast<unsigned char>(character) >> 4) & 15],
                                        digits[static_cast<unsigned char>(character) & 15]};
                if (!append({escaped.data(), escaped.size()})) return false;
                continue;
            }
            if (character == '\"' || character == '\\') {
                if (!append("\\")) {
                    return false;
                }
            }
            if (!append(std::string_view{&character, 1})) {
                return false;
            }
        }
        return append("\"");
    }

    [[nodiscard]] bool unsigned_integer(std::uint32_t value) noexcept {
        char buffer[10]{};
        const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
        return result.ec == std::errc{} &&
               append(std::string_view{buffer, static_cast<std::size_t>(result.ptr - buffer)});
    }

    [[nodiscard]] bool signed_integer(std::int64_t value) noexcept {
        char buffer[20]{};
        const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
        return result.ec == std::errc{} &&
               append(std::string_view{buffer, static_cast<std::size_t>(result.ptr - buffer)});
    }

    [[nodiscard]] bool number(double value) noexcept {
        char buffer[32]{};
        const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
        return result.ec == std::errc{} &&
               append(std::string_view{buffer, static_cast<std::size_t>(result.ptr - buffer)});
    }

    [[nodiscard]] std::size_t finish() noexcept {
        output_[position_] = '\0';
        return position_;
    }

  private:
    std::span<char> output_{};
    std::size_t position_{};
};

[[nodiscard]] constexpr std::string_view value_type_name(ValueType type) noexcept {
    switch (type) {
    case ValueType::boolean:
        return "boolean";
    case ValueType::integer:
        return "integer";
    case ValueType::number:
        return "number";
    case ValueType::string:
        return "string";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view access_name(Access access) noexcept {
    switch (access) {
    case Access::read_only:
        return "read_only";
    case Access::write_only:
        return "write_only";
    case Access::read_write:
        return "read_write";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view
resource_class_name(ResourceClass resource_class) noexcept {
    switch (resource_class) {
    case ResourceClass::gpio:
        return "gpio";
    case ResourceClass::rmt:
        return "rmt";
    case ResourceClass::spi:
        return "spi";
    case ResourceClass::i2c:
        return "i2c";
    case ResourceClass::uart:
        return "uart";
    case ResourceClass::timer:
        return "timer";
    case ResourceClass::dma:
        return "dma";
    case ResourceClass::internal_memory:
        return "internal_memory";
    case ResourceClass::psram:
        return "psram";
    case ResourceClass::radio:
        return "radio";
    }
    return "unknown";
}

[[nodiscard]] bool scalar(Writer& writer, const ScalarValue& value) noexcept {
    switch (value.type) {
    case ValueType::boolean:
        return writer.append(value.boolean ? "true" : "false");
    case ValueType::integer:
        return writer.signed_integer(value.integer);
    case ValueType::number:
        return writer.number(value.number);
    case ValueType::string:
        return writer.quoted(value.string);
    }
    return false;
}

template <typename Range, typename WriteItem>
[[nodiscard]] bool array(Writer& writer, const Range& values, WriteItem write_item) noexcept {
    if (!writer.append("[")) {
        return false;
    }
    bool first = true;
    for (const auto& value : values) {
        if ((!first && !writer.append(",")) || !write_item(value)) {
            return false;
        }
        first = false;
    }
    return writer.append("]");
}

std::string_view wasm_type_name(WasmValueType type) noexcept {
    switch (type) {
    case WasmValueType::i32: return "i32";
    case WasmValueType::i64: return "i64";
    case WasmValueType::f32: return "f32";
    case WasmValueType::f64: return "f64";
    }
    return "unknown";
}
std::string_view wasm_role_name(WasmArgumentRole role) noexcept {
    switch (role) {
    case WasmArgumentRole::scalar: return "scalar";
    case WasmArgumentRole::utf8_offset: return "utf8_offset";
    case WasmArgumentRole::utf8_length: return "utf8_length";
    }
    return "unknown";
}
bool wasm_capability(Writer& writer, const WasmCapabilityDescriptor& capability) noexcept {
    if (capability.functions.empty()) return true;
    return writer.append(",\"wasm\":{\"abi_version\":") && writer.unsigned_integer(capability.abi_version) &&
        writer.append(",\"import_module\":") && writer.quoted(capability.import_module) &&
        writer.append(",\"functions\":") && array(writer, capability.functions, [&writer](const WasmFunctionDescriptor& function) noexcept {
            return writer.append("{\"id\":") && writer.quoted(function.id) &&
                writer.append(",\"description\":") && writer.quoted(function.description) &&
                writer.append(",\"arguments\":") && array(writer, function.arguments, [&writer](const WasmArgumentDescriptor& argument) noexcept {
                    return writer.append("{\"id\":") && writer.quoted(argument.id) && writer.append(",\"type\":") &&
                        writer.quoted(wasm_type_name(argument.type)) && writer.append(",\"role\":") &&
                        writer.quoted(wasm_role_name(argument.role)) && writer.append("}");
                }) && writer.append(",\"results\":") && array(writer, function.results, [&writer](WasmValueType type) noexcept {
                    return writer.quoted(wasm_type_name(type));
                }) && writer.append(",\"maximum_call_us\":") && writer.unsigned_integer(function.maximum_call_us) && writer.append("}");
        }) && writer.append("}");
}

} // namespace

Result<std::size_t> write_descriptor_json(const ComponentDescriptor& descriptor,
                                          std::span<char> output) noexcept {
    if (output.empty()) {
        return Result<std::size_t>::failure({ErrorDomain::descriptor,
                                             ErrorCode::serialization_overflow, descriptor.id,
                                             "descriptor.write_json", "empty-output"});
    }

    Writer writer{output};
    const auto string_array = [&writer](const auto& values) noexcept {
        return array(writer, values,
                     [&writer](std::string_view value) noexcept { return writer.quoted(value); });
    };

    const bool ok =
        writer.append("{\"schema_version\":") &&
        writer.unsigned_integer(descriptor.schema_version) && writer.append(",\"id\":") &&
        writer.quoted(descriptor.id) && writer.append(",\"display_name\":") &&
        writer.quoted(descriptor.display_name) && writer.append(",\"description\":") &&
        writer.quoted(descriptor.description) && writer.append(",\"services\":{") &&
        writer.append("\"provided\":") && string_array(descriptor.provided_services) &&
        writer.append(",\"required\":") && string_array(descriptor.required_services) &&
        writer.append(",\"optional\":") && string_array(descriptor.optional_services) &&
        writer.append("},\"metadata\":") &&
        array(writer, descriptor.metadata,
              [&writer](const MetadataEntry& metadata) {
                  return writer.append("{\"key\":") && writer.quoted(metadata.key) &&
                         writer.append(",\"value\":") && writer.quoted(metadata.value) &&
                         writer.append("}");
              }) &&
        writer.append(",\"parameters\":") &&
        array(writer, descriptor.parameters,
              [&writer](const ParameterDescriptor& parameter) {
                  return writer.append("{\"id\":") && writer.quoted(parameter.id) &&
                         writer.append(",\"label\":") && writer.quoted(parameter.label) &&
                         writer.append(",\"type\":") &&
                         writer.quoted(value_type_name(parameter.type)) &&
                         writer.append(",\"access\":") &&
                         writer.quoted(access_name(parameter.access)) &&
                         writer.append(",\"persisted\":") &&
                         writer.append(parameter.persisted ? "true" : "false") &&
                         writer.append(",\"default\":") &&
                         scalar(writer, parameter.default_value) && writer.append(",\"bounds\":") &&
                         (parameter.bounds.present
                              ? writer.append("{\"minimum\":") &&
                                    writer.number(parameter.bounds.minimum) &&
                                    writer.append(",\"maximum\":") &&
                                    writer.number(parameter.bounds.maximum) &&
                                    writer.append(",\"step\":") &&
                                    writer.number(parameter.bounds.step) && writer.append("}")
                              : writer.append("null")) &&
                         writer.append(",\"unit\":") && writer.quoted(parameter.unit) &&
                         writer.append("}");
              }) &&
        writer.append(",\"legacy_parameters\":") &&
        array(writer, descriptor.legacy_parameters,
              [&writer](const LegacyParameterAlias& alias) {
                  return writer.append("{\"parameter_id\":") && writer.quoted(alias.parameter_id) &&
                         writer.append(",\"id\":") && writer.quoted(alias.id) &&
                         writer.append(",\"label\":") && writer.quoted(alias.label) &&
                         writer.append(",\"type\":") &&
                         writer.quoted(value_type_name(alias.type)) &&
                         writer.append(",\"enum_values\":") &&
                         array(writer, alias.enum_values,
                               [&writer](const LegacyEnumValue& value) {
                                   return writer.append("{\"canonical_value\":") &&
                                          scalar(writer, value.canonical_value) &&
                                          writer.append(",\"label\":") &&
                                          writer.quoted(value.label) && writer.append("}");
                               }) &&
                         writer.append("}");
              }) &&
        writer.append(",\"actions\":") &&
        array(writer, descriptor.actions,
              [&writer](const ActionDescriptor& action) {
                  return writer.append("{\"id\":") && writer.quoted(action.id) &&
                         writer.append(",\"label\":") && writer.quoted(action.label) &&
                         writer.append(",\"arguments\":") &&
                         array(writer, action.arguments,
                               [&writer](const FieldDescriptor& field) {
                                   return writer.append("{\"id\":") && writer.quoted(field.id) &&
                                          writer.append(",\"type\":") &&
                                          writer.quoted(value_type_name(field.type)) &&
                                          writer.append(",\"required\":") &&
                                          writer.append(field.required ? "true}" : "false}");
                               }) &&
                         writer.append("}");
              }) &&
        writer.append(",\"events\":") &&
        array(writer, descriptor.events,
              [&writer](const EventDescriptor& event) {
                  return writer.append("{\"id\":") && writer.quoted(event.id) &&
                         writer.append(",\"fields\":") &&
                         array(writer, event.fields,
                               [&writer](const FieldDescriptor& field) {
                                   return writer.append("{\"id\":") && writer.quoted(field.id) &&
                                          writer.append(",\"type\":") &&
                                          writer.quoted(value_type_name(field.type)) &&
                                          writer.append(",\"required\":") &&
                                          writer.append(field.required ? "true}" : "false}");
                               }) &&
                         writer.append("}");
              }) &&
        writer.append(",\"diagnostics\":") &&
        array(writer, descriptor.diagnostics,
              [&writer](const DiagnosticDescriptor& diagnostic) {
                  return writer.append("{\"id\":") && writer.quoted(diagnostic.id) &&
                         writer.append(",\"type\":") &&
                         writer.quoted(value_type_name(diagnostic.type)) &&
                         writer.append(",\"unit\":") && writer.quoted(diagnostic.unit) &&
                         writer.append("}");
              }) &&
        writer.append(",\"resources\":") &&
        array(writer, descriptor.resources,
              [&writer](const ResourceRequest& resource) {
                  return writer.append("{\"class\":") &&
                         writer.quoted(resource_class_name(resource.resource_class)) &&
                         writer.append(",\"logical_name\":") &&
                         writer.quoted(resource.logical_name) &&
                         writer.append(",\"alternatives\":") &&
                         array(writer, resource.alternatives,
                               [&writer](std::string_view alternative) {
                                   return writer.quoted(alternative);
                               }) &&
                         writer.append(",\"amount\":") &&
                         writer.unsigned_integer(resource.amount) &&
                         writer.append(",\"live_reacquire\":") &&
                         writer.append(resource.live_reacquire ? "true}" : "false}");
              }) &&
        writer.append(",\"settings\":{\"schema_version\":") &&
        writer.unsigned_integer(descriptor.settings.schema_version) &&
        writer.append(",\"migration_version\":") &&
        writer.unsigned_integer(descriptor.settings.migration_version) && writer.append("}") &&
        writer.append(",\"disable_policy\":") &&
        writer.quoted(descriptor.disable_policy == DisablePolicy::live ? "live"
                                                                       : "reboot_required") &&
        writer.append(",\"supports_resume\":") &&
        writer.append(descriptor.supports_resume ? "true" : "false") &&
        writer.append(",\"supports_restart\":") &&
        writer.append(descriptor.supports_restart ? "true" : "false") &&
        writer.append(",\"cost\":{\"flash_bytes\":") &&
        writer.unsigned_integer(descriptor.cost.flash_bytes) &&
        writer.append(",\"static_ram_bytes\":") &&
        writer.unsigned_integer(descriptor.cost.static_ram_bytes) &&
        writer.append(",\"task_stack_bytes\":") &&
        writer.unsigned_integer(descriptor.cost.task_stack_bytes) && writer.append("}") &&
        wasm_capability(writer, descriptor.wasm) && writer.append("}");

    if (!ok) {
        return Result<std::size_t>::failure({ErrorDomain::descriptor,
                                             ErrorCode::serialization_overflow, descriptor.id,
                                             "descriptor.write_json", "output-capacity"});
    }
    return Result<std::size_t>::success(writer.finish());
}

} // namespace blip::core
