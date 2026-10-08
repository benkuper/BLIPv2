#include "blip/core/wasm_descriptor.hpp"
#include <array>
#include <charconv>

namespace blip::core {
namespace {
bool public_id(std::string_view id) noexcept {
    if (id.empty() || id.size() > 64 || id.front() < 'a' || id.front() > 'z') return false;
    for (const char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    return true;
}
bool valid_type(WasmValueType type) noexcept {
    return type == WasmValueType::i32 || type == WasmValueType::i64 ||
           type == WasmValueType::f32 || type == WasmValueType::f64;
}
bool component_name(std::string_view id) noexcept {
    if (id.size() < 3 || id.size() > 64 || id.find('.') == id.npos || id.front() < 'a' || id.front() > 'z') return false;
    for (const char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return false;
    return true;
}
}
bool valid_wasm_descriptor(std::string_view component_id, const WasmCapabilityDescriptor& capability) noexcept {
    if (capability.functions.empty()) return capability.abi_version == 0 && capability.import_module.empty();
    if (!capability.abi_version || capability.functions.size() > kMaximumWasmFunctionsPerComponent ||
        !component_name(component_id) ||
        capability.import_module.size() > kMaximumWasmImportNameBytes) return false;
    // Component IDs are validated by the core registry. Enforce ownership and a
    // canonical version suffix independently, including for defensive catalogs.
    std::array<char, 10> version{};
    const auto formatted = std::to_chars(version.data(), version.data() + version.size(), capability.abi_version);
    const std::string_view digits{version.data(), static_cast<std::size_t>(formatted.ptr - version.data())};
    if (formatted.ec != std::errc{} || capability.import_module.size() != component_id.size() + 2 + digits.size() ||
        !capability.import_module.starts_with(component_id) ||
        capability.import_module.substr(component_id.size(), 2) != ".v" ||
        capability.import_module.substr(component_id.size() + 2) != digits) return false;
    for (std::size_t i = 0; i < capability.functions.size(); ++i) {
        const auto& function = capability.functions[i];
        if (!public_id(function.id) || function.description.size() > kMaximumWasmDescriptionBytes ||
            !function.maximum_call_us || function.maximum_call_us > kMaximumWasmNativeCallUs ||
            function.arguments.size() > kMaximumWasmArguments || function.results.size() > 1) return false;
        for (std::size_t j = 0; j < i; ++j) if (function.id == capability.functions[j].id) return false;
        for (const auto result : function.results) if (!valid_type(result)) return false;
        for (std::size_t j = 0; j < function.arguments.size(); ++j) {
            const auto& arg = function.arguments[j];
            if (!public_id(arg.id) || !valid_type(arg.type)) return false;
            for (std::size_t prior = 0; prior < j; ++prior) if (arg.id == function.arguments[prior].id) return false;
            switch (arg.role) {
            case WasmArgumentRole::scalar: break;
            case WasmArgumentRole::utf8_offset:
                if (arg.type != WasmValueType::i32 || j + 1 == function.arguments.size() ||
                    function.arguments[j + 1].type != WasmValueType::i32 ||
                    function.arguments[j + 1].role != WasmArgumentRole::utf8_length) return false;
                break;
            case WasmArgumentRole::utf8_length:
                if (arg.type != WasmValueType::i32 || !j ||
                    function.arguments[j - 1].role != WasmArgumentRole::utf8_offset) return false;
                break;
            default: return false;
            }
        }
    }
    return true;
}
} // namespace blip::core
