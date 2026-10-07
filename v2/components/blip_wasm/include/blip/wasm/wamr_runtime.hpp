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
    core::Status initialize(std::span<std::byte> pool, Limits limits) noexcept override;
    core::Status load(std::span<std::byte> module) noexcept override;
    void unload() noexcept override;
    void shutdown() noexcept override;
    core::Result<Signature> signature(std::string_view name) noexcept override;
    core::Status invoke(std::string_view name, std::span<const Value> arguments,
                        ExecutionBudget budget, std::span<Value> results,
                        std::size_t& result_count) noexcept override;
    void request_cancel() noexcept override;
    RuntimeSnapshot snapshot() const noexcept override;

  private:
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
};

} // namespace blip::wasm
