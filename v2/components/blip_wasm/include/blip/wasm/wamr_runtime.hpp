#pragma once

#include "blip/wasm/runtime.hpp"
#include <mutex>

namespace blip::wasm {

// WAMR implementation only; clients and capability providers use Runtime.
// There is one pool/runtime per firmware. All lifecycle methods run on the
// worker. A supervisor may request_cancel while invoke is running. Destroying
// the adapter requires stop/shutdown and a quiescent supervisor first.
class WamrRuntime final : public Runtime {
  public:
    WamrRuntime() noexcept = default;
    WamrRuntime(const WamrRuntime&) = delete;
    WamrRuntime& operator=(const WamrRuntime&) = delete;
    std::string_view name() const noexcept override { return "wamr-2.4.5-fast-metered"; }
    core::Status configure_capabilities(CapabilityRegistry*) noexcept override;
    core::Status initialize(std::span<std::byte> pool, Limits limits,
                            std::span<std::byte> linear_memory = {}) noexcept override;
    core::Status load(std::span<std::byte> module) noexcept override;
    void unload() noexcept override;
    void shutdown() noexcept override;
    core::Result<Signature> signature(std::string_view name) noexcept override;
    core::Status invoke(std::string_view name, std::span<const Value> arguments,
                        ExecutionBudget budget, std::span<Value> results,
                        std::size_t& result_count) noexcept override;
    void request_cancel() noexcept override;
    RuntimeSnapshot snapshot() const noexcept override;
    core::Status read_memory(std::uint32_t offset, std::span<std::byte> output) noexcept override;
    core::Status write_memory(std::uint32_t offset, std::span<const std::byte> input) noexcept override;

  private:
    friend struct WamrNativeBridge;
    core::Status install_capabilities() noexcept;
    void release_capabilities() noexcept;
    void raw_capability(void* environment, std::size_t binding, std::uint64_t* values) noexcept;
    CapabilityRegistry* capabilities_{};
    void* imports_{}; // Opaque allocation from the fixed WAMR pool, owned by the adapter.
    std::atomic<bool> provider_cancelled_{};
    const std::atomic<bool>* invocation_cancellation_{}; // Worker-confined, borrowed for this invocation only.
    bool provider_active_{};
    core::ErrorCode provider_error_{core::ErrorCode::none};
    std::uint32_t native_calls_{}, native_failures_{}, native_maximum_us_{};
    // Only this implementation casts opaque handles to WAMR types.
    void* module_{};
    void* instance_{};
    void* environment_{};
    bool initialized_{};
    bool running_{}; // Protected by cancel_mutex_, together with instance_.
    bool cancelled_{};
    std::mutex cancel_mutex_{};
    Limits limits_{};
    std::uint32_t pool_bytes_{};
    std::array<char, kMaximumDiagnosticBytes> fault_{};
    // Checks the current instance/extent before producing a private native view.
    core::Result<std::byte*> memory_range(std::uint32_t offset, std::size_t bytes) noexcept;
};

} // namespace blip::wasm
