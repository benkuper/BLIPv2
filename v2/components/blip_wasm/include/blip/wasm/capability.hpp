#pragma once
#include "blip/core/registry.hpp"
#include "blip/wasm/runtime.hpp"

namespace blip::wasm {
inline constexpr std::size_t kMaximumCapabilityComponents = 64;
inline constexpr std::size_t kMaximumCapabilityProviders = 8;
inline constexpr std::size_t kMaximumCapabilityFunctions = 32;

// Borrowed only during a worker callback. Providers must not retain this object,
// guest-memory references or spans. It exposes no engine or invocation handles.
class CallContext final {
  public:
    CallContext(GuestMemory& memory, const std::atomic<bool>* cancellation = nullptr) noexcept
        : memory_(&memory), cancellation_(cancellation) {}
    CallContext(const CallContext&) = delete;
    CallContext& operator=(const CallContext&) = delete;
    [[nodiscard]] bool cancelled() const noexcept;
    [[nodiscard]] core::Status read_utf8(StringRef, std::span<char>, std::size_t&) noexcept;
    [[nodiscard]] core::Status write_utf8(std::uint32_t offset, std::string_view) noexcept;
  private:
    GuestMemory* memory_;
    const std::atomic<bool>* cancellation_;
};

// Owned by its component. Descriptors live exclusively in the core registry.
// Callbacks run on the script worker and must fit their declared time bound:
// immediate queries or bounded queue admission, never I/O, guest re-entry,
// allocation, render callbacks or unbounded waits. Availability is thread-safe;
// the owner must also synchronize admission with suspend/stop.
class CapabilityProvider {
  public:
    virtual ~CapabilityProvider() = default;
    [[nodiscard]] virtual bool available() const noexcept = 0;
    [[nodiscard]] virtual core::Status invoke(std::string_view function, CallContext&,
        std::span<const Value>, std::span<Value>, std::size_t& count) noexcept = 0;
};

// Immutable borrowed catalog: bind once before use; all registry descriptors and
// providers outlive this catalog and every callback. Operations are worker-only.
// Native bridge installation and runtime teardown are separate lifecycle steps.
class CapabilityRegistry final {
  public:
    CapabilityRegistry() noexcept = default;
    CapabilityRegistry(const CapabilityRegistry&) = delete;
    CapabilityRegistry& operator=(const CapabilityRegistry&) = delete;
    struct Binding {
        const core::ComponentDescriptor* component{};
        const core::WasmFunctionDescriptor* function{};
        CapabilityProvider* provider{};
    };
    [[nodiscard]] core::Status bind(const core::RegistryView&) noexcept;
    [[nodiscard]] bool bound() const noexcept { return bound_; }
    [[nodiscard]] std::size_t size() const noexcept { return bindings_.size(); }
    [[nodiscard]] const Binding& binding(std::size_t index) const noexcept { return bindings_[index]; }
    [[nodiscard]] core::Result<std::size_t> resolve(std::string_view module, std::string_view function) const noexcept;
    [[nodiscard]] core::Status check_import(std::string_view module, std::string_view function, const Signature&) const noexcept;
    [[nodiscard]] core::Status invoke(std::size_t index, GuestMemory&, std::span<const Value>,
        std::span<Value>, std::size_t& count, const std::atomic<bool>* cancellation = nullptr) noexcept;
  private:
    core::FixedVector<Binding, kMaximumCapabilityFunctions> bindings_{};
    bool bound_{}, invoking_{};
};
} // namespace blip::wasm
