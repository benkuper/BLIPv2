#include "blip/wasm/service.hpp"
#include <algorithm>
#include <cstring>
#include <limits>

namespace blip::wasm {
namespace {
core::Status failure(core::ErrorCode code, std::string_view operation,
                     std::string_view detail) noexcept {
    return core::Status::failure({core::ErrorDomain::control, code, "blip.wasm", operation, detail});
}
bool valid_type(ValueType type) noexcept {
    return type == ValueType::i32 || type == ValueType::i64 || type == ValueType::f32 || type == ValueType::f64;
}
void increment(std::uint32_t& value) noexcept {
    if (value != std::numeric_limits<std::uint32_t>::max()) ++value;
}
}

core::Status Service::start(Limits limits) noexcept {
    if (snapshot_.state != State::stopped)
        return failure(core::ErrorCode::invalid_state, "start", "already-started");
    if (pool_.empty() || module_storage_.empty() || limits.maximum_module_bytes < 8 ||
        limits.maximum_module_bytes > module_storage_.size() || !limits.linear_memory_bytes ||
        !limits.wasm_stack_bytes)
        return failure(core::ErrorCode::invalid_argument, "start", "invalid-limits");
    if (!linear_memory_.empty() && linear_memory_.size() < limits.linear_memory_bytes)
        return failure(core::ErrorCode::invalid_argument, "start", "linear-arena-too-small");
    const auto status = runtime_->initialize(pool_, limits, linear_memory_);
    if (!status) { runtime_->shutdown(); return status; }
    limits_ = limits;
    snapshot_.state = State::ready;
    snapshot_.loaded_bytes = 0;
    snapshot_.runtime = runtime_->snapshot();
    return core::Status::success();
}

core::Status Service::load(std::span<const std::byte> bytes) noexcept {
    if (snapshot_.state == State::stopped)
        return failure(core::ErrorCode::invalid_state, "load", "not-started");
    constexpr std::array header{std::byte{0}, std::byte{0x61}, std::byte{0x73}, std::byte{0x6d},
                                std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0}};
    // Validate request-level bounds before replacing the current module. Full
    // binary validation belongs to the engine and can still reject the module.
    if (bytes.size() < header.size() || bytes.size() > limits_.maximum_module_bytes)
        return failure(core::ErrorCode::capacity_exceeded, "load", "module-size");
    if (!std::equal(header.begin(), header.end(), bytes.begin()))
        return failure(core::ErrorCode::corrupt_data, "load", "wasm-version-or-magic");
    if (snapshot_.generation == std::numeric_limits<std::uint32_t>::max())
        return failure(core::ErrorCode::generation_exhausted, "load", "module-generation");
    const auto previous_generation = snapshot_.generation;
    unload();
    if (snapshot_.generation == previous_generation) increment(snapshot_.generation);
    // memmove allows a caller to reload a view of the existing scratch bytes.
    std::memmove(module_storage_.data(), bytes.data(), bytes.size());
    const auto status = runtime_->load(module_storage_.first(bytes.size()));
    if (!status) {
        snapshot_.runtime = runtime_->snapshot();
        runtime_->unload();
        return status;
    }
    snapshot_.state = State::loaded;
    snapshot_.loaded_bytes = static_cast<std::uint32_t>(bytes.size());
    snapshot_.runtime = runtime_->snapshot();
    return core::Status::success();
}

core::Status Service::call(std::string_view name, std::span<const Value> arguments,
                           ExecutionBudget budget, std::span<Value> results,
                           std::size_t& result_count) noexcept {
    result_count = 0;
    if (snapshot_.state != State::loaded)
        return failure(core::ErrorCode::invalid_state, "call", "module-not-runnable");
    if (name.empty() || name.size() > kMaximumExportNameBytes || name.find('\0') != name.npos ||
        arguments.size() > kMaximumArguments || budget.instructions == 0 ||
        budget.instructions > static_cast<std::uint32_t>(std::numeric_limits<int>::max()))
        return failure(core::ErrorCode::invalid_argument, "call", "invalid-call");
    if (budget.cancellation && budget.cancellation->load())
        return failure(core::ErrorCode::cancelled, "call", "cancelled-before-call");
    const auto signature = runtime_->signature(name);
    if (!signature) return core::Status::failure(signature.error());
    const auto& expected = signature.value();
    if (expected.argument_count > kMaximumArguments || expected.result_count > kMaximumResults)
        return failure(core::ErrorCode::capacity_exceeded, "call", "function-signature-limit");
    if (arguments.size() != expected.argument_count || results.size() < expected.result_count)
        return failure(core::ErrorCode::invalid_argument, "call", "function-arity");
    for (std::size_t i = 0; i < arguments.size(); ++i)
        if (!valid_type(arguments[i].type) || arguments[i].type != expected.arguments[i] ||
            ((arguments[i].type == ValueType::i32 || arguments[i].type == ValueType::f32) && arguments[i].bits > 0xffffffffU))
            return failure(core::ErrorCode::invalid_argument, "call", "argument-type-or-bits");
    for (std::size_t i = 0; i < expected.result_count; ++i)
        if (!valid_type(expected.results[i]))
            return failure(core::ErrorCode::invalid_argument, "call", "unsupported-result-type");
    increment(snapshot_.calls);
    const auto status = runtime_->invoke(name, arguments, budget,
                                         results.first(expected.result_count), result_count);
    if (!status || result_count != expected.result_count) {
        snapshot_.runtime = runtime_->snapshot();
        snapshot_.state = State::faulted;
        increment(snapshot_.faults);
        result_count = 0;
        return status ? failure(core::ErrorCode::verification_failed, "call", "runtime-result-count") : status;
    }
    for (std::size_t i = 0; i < result_count; ++i) {
        if (results[i].type != expected.results[i] ||
            ((results[i].type == ValueType::i32 || results[i].type == ValueType::f32) && results[i].bits > 0xffffffffU)) {
            snapshot_.runtime = runtime_->snapshot();
            snapshot_.state = State::faulted;
            increment(snapshot_.faults);
            result_count = 0;
            return failure(core::ErrorCode::verification_failed, "call", "runtime-result-type-or-bits");
        }
    }
    snapshot_.runtime = runtime_->snapshot();
    return core::Status::success();
}

void Service::unload() noexcept {
    if (snapshot_.state == State::loaded || snapshot_.state == State::faulted) {
        runtime_->unload();
        increment(snapshot_.generation);
        snapshot_.runtime = runtime_->snapshot();
    }
    snapshot_.loaded_bytes = 0;
    if (snapshot_.state != State::stopped) snapshot_.state = State::ready;
}
void Service::stop() noexcept {
    if (snapshot_.state == State::stopped) return;
    unload();
    runtime_->shutdown();
    snapshot_.runtime = runtime_->snapshot();
    snapshot_.state = State::stopped;
}
Snapshot Service::snapshot() const noexcept { return snapshot_; }

} // namespace blip::wasm
