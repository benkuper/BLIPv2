#pragma once
#include <cstddef>
#include <span>

namespace blip::wasm {
class CapabilityRegistry;
// Bounded policy walk before engine validation. Prevents automatic execution
// and C-string aliasing of exact import/export names; not a full Wasm parser.
[[nodiscard]] bool passive_module(std::span<const std::byte>, const CapabilityRegistry* = nullptr) noexcept;
} // namespace blip::wasm
