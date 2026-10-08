#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace blip::core {
inline constexpr std::size_t kMaximumWasmFunctionsPerComponent = 8;
inline constexpr std::size_t kMaximumWasmArguments = 8;
inline constexpr std::size_t kMaximumWasmImportNameBytes = 80;
inline constexpr std::size_t kMaximumWasmDescriptionBytes = 256;
inline constexpr std::uint32_t kMaximumWasmNativeCallUs = 2000;
enum class WasmValueType : std::uint8_t { i32, i64, f32, f64 };
enum class WasmArgumentRole : std::uint8_t { scalar, utf8_offset, utf8_length };
struct WasmArgumentDescriptor {
    std::string_view id{};
    WasmValueType type{WasmValueType::i32};
    WasmArgumentRole role{WasmArgumentRole::scalar};
};
struct WasmFunctionDescriptor {
    std::string_view id{};
    std::string_view description{};
    std::span<const WasmArgumentDescriptor> arguments{};
    // Native imports have at most one numeric result. UTF-8 uses checked memory.
    std::span<const WasmValueType> results{};
    // Maximum scheduled task execution time, including timing overhead and
    // interrupts. Whole guest calls also need a separate wall-clock supervisor.
    std::uint32_t maximum_call_us{};
};
struct WasmCapabilityDescriptor {
    std::uint32_t abi_version{};
    // Exactly <owning component ID>.v<canonical decimal ABI version>.
    std::string_view import_module{};
    std::span<const WasmFunctionDescriptor> functions{};
};
[[nodiscard]] bool valid_wasm_descriptor(std::string_view component_id,
                                        const WasmCapabilityDescriptor&) noexcept;
} // namespace blip::core
