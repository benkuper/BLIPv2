#pragma once
#include "blip/core/dynamic_schema.hpp"
#include "blip/wasm/runtime.hpp"
#include "blip/wasm/script_manifest.hpp"
#include <atomic>

namespace blip::wasm {
inline constexpr std::size_t kScriptValueStringBytes = 128;
inline constexpr std::size_t kScriptControlQueueCapacity = 4;
inline constexpr std::size_t kScriptActionBufferBytes = kMaximumScriptFields * kScriptValueStringBytes;
inline constexpr std::string_view kScriptActionBufferGlobal = "blip_controls_buffer_v1";

// Relocatable payloads. Views are constructed only while the owning value/message
// remains alive; neither queue stores input or guest pointers.
struct OwnedScriptValue {
    std::uint64_t bits{};
    std::array<char, kScriptValueStringBytes> text{};
    std::uint16_t bytes{};
    core::ValueType type{core::ValueType::integer};
    [[nodiscard]] core::ScalarValue scalar() const noexcept;
    void assign(const core::ScalarValue&) noexcept; // Caller validates type/length first.
};
struct ScriptControlMessage {
    std::array<OwnedScriptValue, kMaximumScriptFields> values{};
    std::array<char, kMaximumExportNameBytes + 1> name{};
    std::uint32_t generation{}, module_generation{}, epoch{}, ticket{};
    std::uint8_t index{}, count{};
};
static_assert(sizeof(ScriptControlMessage) <= 704);

// Owned by the production worker component. prepare/publish run on the worker;
// control/value/queue operations use immediate bounded admission from any task.
// retirement closes admission immediately without invalidating leased metadata.
class ScriptControlStore final : public core::DynamicSchemaSource {
  public:
    [[nodiscard]] core::Status prepare(std::span<const std::byte>, const core::ComponentDescriptor&) noexcept;
    [[nodiscard]] core::Status publish(Runtime&, std::uint32_t module_generation) noexcept;
    void retire() noexcept;
    [[nodiscard]] bool quiescent() const noexcept;
    [[nodiscard]] std::uint32_t generation() const noexcept { return active_.load(std::memory_order_acquire); }
    [[nodiscard]] std::uint32_t action_buffer() const noexcept { return action_buffer_; } // Worker only.
    [[nodiscard]] core::Status acquire(core::DynamicSchemaLease&) const noexcept override;
    void release(std::uint32_t) const noexcept override;
    [[nodiscard]] std::string_view id(std::size_t) const noexcept override;
    [[nodiscard]] core::DynamicControlKind kind(std::size_t) const noexcept override;
    [[nodiscard]] core::Status parameter(std::size_t, core::ParameterDescriptor&) const noexcept override;
    [[nodiscard]] core::Status action(std::size_t, std::span<core::FieldDescriptor>, core::ActionDescriptor&) const noexcept override;
    [[nodiscard]] core::Status event(std::size_t, std::span<core::FieldDescriptor>, core::EventDescriptor&) const noexcept override;
    [[nodiscard]] core::Status read(std::uint32_t, std::size_t, core::ScalarValue&, std::span<char>, bool guest = false) noexcept;
    [[nodiscard]] core::Status write(std::uint32_t, std::size_t, const core::ScalarValue&, bool guest = false) noexcept;
    [[nodiscard]] core::Status read_id(std::uint32_t, std::string_view, core::ScalarValue&, std::span<char>) noexcept;
    [[nodiscard]] core::Status write_id(std::uint32_t, std::string_view, const core::ScalarValue&) noexcept;
    [[nodiscard]] core::Status enqueue_action(std::uint32_t, std::string_view, std::span<const core::ScalarValue>,
                                               std::uint32_t ticket, std::uint32_t epoch) noexcept;
    [[nodiscard]] core::Status take_action(ScriptControlMessage&) noexcept;
    // Worker-confined builder; end_invocation also discards incomplete events.
    void end_invocation() noexcept { builder_token_.store(0, std::memory_order_release); }
    [[nodiscard]] core::Result<std::uint32_t> begin_event(std::uint32_t, std::size_t) noexcept;
    [[nodiscard]] core::Status event_bits(std::uint32_t, std::size_t, std::uint64_t) noexcept;
    [[nodiscard]] core::Status event_utf8(std::uint32_t, std::size_t, std::string_view) noexcept;
    [[nodiscard]] core::Status commit_event(std::uint32_t) noexcept;
    [[nodiscard]] core::Status take_event(ScriptControlMessage&) noexcept;

  private:
    class WriteGuard;
    class DataGuard;
    [[nodiscard]] core::Status pin(std::uint32_t, core::DynamicSchemaLease&) const noexcept;
    [[nodiscard]] std::size_t find(std::string_view) const noexcept; // Leased or exclusive.
    ScriptManifest schema_{};
    std::array<OwnedScriptValue, kMaximumScriptControls> values_{};
    std::array<ScriptControlMessage, kScriptControlQueueCapacity> actions_{}, events_{};
    ScriptControlMessage builder_{};
    mutable std::atomic<std::uint32_t> gate_{};
    mutable std::atomic_flag data_{};
    std::atomic<std::uint32_t> active_{}, builder_token_{};
    std::uint32_t sequence_{}, module_generation_{}, action_buffer_{}, event_sequence_{};
    std::uint8_t action_head_{}, action_count_{}, event_head_{}, event_count_{}, field_mask_{};
    bool prepared_{};
};
static_assert(sizeof(ScriptControlStore) <= 12288);
} // namespace blip::wasm
