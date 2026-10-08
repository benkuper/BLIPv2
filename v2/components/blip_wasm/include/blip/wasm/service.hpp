#pragma once

#include "blip/wasm/runtime.hpp"

namespace blip::wasm {

enum class State : std::uint8_t { stopped, ready, loaded, faulted };
struct Snapshot {
    State state{State::stopped};
    std::uint32_t generation{};
    std::uint32_t loaded_bytes{};
    std::uint32_t calls{};
    std::uint32_t faults{};
    RuntimeSnapshot runtime{};
};

// Runtime-neutral lifecycle/policy service. The worker exclusively owns these
// methods; a platform queue transfers control requests to that worker. Module
// scratch and pool are borrowed allocations. The worker may replace the bounded
// module scratch only after unload; the engine pool stays fixed. No I/O,
// task creation, registration, allocator or render callback is hidden here.
// Explicitly stop on the worker before destroying the service/runtime/buffers.
class Service final {
  public:
    Service(Runtime& runtime, std::span<std::byte> pool,
            std::span<std::byte> module_storage, std::span<std::byte> linear_memory = {}) noexcept
        : runtime_(&runtime), pool_(pool), module_storage_(module_storage), linear_memory_(linear_memory) {}
    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;
    [[nodiscard]] core::Status start(Limits limits = {}) noexcept;
    [[nodiscard]] core::Status load(std::span<const std::byte> bytes) noexcept;
    // Worker-only, after unload. Detach with an empty span before freeing an
    // upload allocation; a later load checks both policy and borrowed capacity.
    [[nodiscard]] core::Status replace_module_storage(std::span<std::byte>) noexcept;
    [[nodiscard]] core::Status call(std::string_view name, std::span<const Value> arguments,
                                   ExecutionBudget budget, std::span<Value> results,
                                   std::size_t& result_count) noexcept;
    [[nodiscard]] core::Status read_utf8(StringRef, std::uint32_t generation, std::span<char> output,
                                         std::size_t& count) noexcept;
    [[nodiscard]] core::Status write_utf8(std::uint32_t offset, std::uint32_t generation, std::string_view input) noexcept;
    void unload() noexcept;
    void stop() noexcept;
    [[nodiscard]] Snapshot snapshot() const noexcept;

  private:
    Runtime* runtime_;
    std::span<std::byte> pool_;
    std::span<std::byte> module_storage_;
    std::span<std::byte> linear_memory_;
    Limits limits_{};
    Snapshot snapshot_{};
};

} // namespace blip::wasm
