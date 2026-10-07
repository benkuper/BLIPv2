#pragma once

#include "blip/core/error.hpp"
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace blip::wasm {

inline constexpr std::size_t kMaximumArguments = 8;
inline constexpr std::size_t kMaximumResults = 8;
inline constexpr std::size_t kMaximumExportNameBytes = 64;
inline constexpr std::size_t kMaximumDiagnosticBytes = 128;

enum class ValueType : std::uint8_t { i32, i64, f32, f64 };

// Preserve guest bit patterns, including NaNs and unsigned i32/i64 values.
// These are Wasm values, not the registry's user-facing scalar representation.
struct Value {
    ValueType type{ValueType::i32};
    std::uint64_t bits{};
    static constexpr Value i32(std::uint32_t value) noexcept { return {ValueType::i32, value}; }
    static constexpr Value i64(std::uint64_t value) noexcept { return {ValueType::i64, value}; }
    static constexpr Value f32(float value) noexcept { return {ValueType::f32, std::bit_cast<std::uint32_t>(value)}; }
    static constexpr Value f64(double value) noexcept { return {ValueType::f64, std::bit_cast<std::uint64_t>(value)}; }
};

struct Signature {
    std::array<ValueType, kMaximumArguments> arguments{};
    std::array<ValueType, kMaximumResults> results{};
    std::uint8_t argument_count{};
    std::uint8_t result_count{};
};

struct Limits {
    std::uint32_t linear_memory_bytes{65536};
    std::uint32_t wasm_stack_bytes{4096};
    std::uint32_t maximum_module_bytes{16384};
};

struct ExecutionBudget {
    std::uint32_t instructions{10000};
    // Absolute monotonic deadline. Backends without a deadline supervisor must
    // reject a nonzero value; they may never silently ignore it.
    std::uint64_t deadline_us{};
    // Admission check. For cancellation during execution the platform observes
    // this token and calls request_cancel(); no unsynchronized engine mutation.
    const std::atomic<bool>* cancellation{};
};

struct RuntimeSnapshot {
    std::uint32_t reserved_bytes{};
    std::uint32_t used_bytes{};
    std::uint32_t peak_bytes{};
    std::array<char, kMaximumDiagnosticBytes> fault{};
};

// Engine boundary. All operations except request_cancel are worker-confined.
// Pool and mutable module bytes are caller-owned and stay valid until shutdown
// and unload respectively. Failed initialize/load must leave no partial engine
// state. No engine handles or headers may escape this interface.
// load must not execute guest code: initialization uses a budgeted export.
class Runtime {
  public:
    virtual ~Runtime() = default;
    virtual std::string_view name() const noexcept = 0;
    // An optional, separately placed linear arena remains borrowed until
    // shutdown. Unsupported backends must reject it, not silently ignore it.
    virtual core::Status initialize(std::span<std::byte> pool, Limits limits,
                                   std::span<std::byte> linear_memory = {}) noexcept = 0;
    virtual core::Status load(std::span<std::byte> module) noexcept = 0;
    virtual void unload() noexcept = 0;
    virtual void shutdown() noexcept = 0;
    virtual core::Result<Signature> signature(std::string_view name) noexcept = 0;
    virtual core::Status invoke(std::string_view name, std::span<const Value> arguments,
                               ExecutionBudget budget, std::span<Value> results,
                               std::size_t& result_count) noexcept = 0;
    virtual void request_cancel() noexcept = 0;
    virtual RuntimeSnapshot snapshot() const noexcept = 0;
};

} // namespace blip::wasm
