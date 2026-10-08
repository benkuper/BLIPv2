#pragma once

#include "blip/core/descriptor.hpp"
#include "blip/core/error.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace blip::wasm {
inline constexpr std::string_view kScriptControlsSection = "blip.controls.v1";
inline constexpr std::size_t kMaximumScriptControls = 16;
inline constexpr std::size_t kMaximumScriptFields = 4;
inline constexpr std::size_t kScriptManifestTextBytes = 2048;
inline constexpr std::size_t kMaximumScriptManifestBytes = 4096;

struct ScriptTextRef { std::uint16_t offset{}, bytes{}; };
struct ScriptField {
    ScriptTextRef id{};
    core::ValueType type{core::ValueType::integer};
};
struct ScriptControl {
    core::DynamicControlKind kind{core::DynamicControlKind::parameter};
    core::ValueType type{core::ValueType::integer};
    core::Access access{core::Access::read_write};
    std::uint8_t field_count{};
    ScriptTextRef id{}, label{}, unit{}, callback{}, default_text{};
    std::uint64_t default_bits{};
    core::NumericBounds bounds{};
    std::array<ScriptField, kMaximumScriptFields> fields{};
};

// Owned, relocatable schema: no pointer/view into uploaded or guest memory.
// Put this fixed reservation in its owner, not on the script worker's stack.
class ScriptManifest final {
  public:
    [[nodiscard]] std::size_t size() const noexcept { return count_; }
    [[nodiscard]] const ScriptControl& control(std::size_t index) const noexcept { return controls_[index]; }
    [[nodiscard]] std::string_view text(ScriptTextRef) const noexcept;
    [[nodiscard]] core::ScalarValue default_value(std::size_t index) const noexcept;
    [[nodiscard]] core::Status parameter(std::size_t index, core::ParameterDescriptor&) const noexcept;
    // The returned descriptor borrows this manifest's text and the caller's
    // field array. Failure leaves both outputs unchanged.
    [[nodiscard]] core::Status action(std::size_t index, std::span<core::FieldDescriptor>,
                                      core::ActionDescriptor&) const noexcept;
    [[nodiscard]] core::Status event(std::size_t index, std::span<core::FieldDescriptor>,
                                     core::EventDescriptor&) const noexcept;

  private:
    friend class ScriptManifestDecoder;
    std::array<ScriptControl, kMaximumScriptControls> controls_{};
    std::array<char, kScriptManifestTextBytes> text_{};
    std::uint16_t count_{}, text_bytes_{};
};
static_assert(sizeof(ScriptManifest) <= 4096);

// Bounded metadata walk, not an engine validator or import policy check.
// An absent declaration section produces an empty schema. Malformed, duplicate
// or unsupported declarations leave output unchanged. Input must stay stable
// during both passes; no allocation or full-schema stack scratch is used.
[[nodiscard]] core::Status parse_script_manifest(std::span<const std::byte> module,
                                                 ScriptManifest& output) noexcept;
} // namespace blip::wasm
