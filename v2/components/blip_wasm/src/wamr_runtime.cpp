#include "blip/wasm/wamr_runtime.hpp"
#include "wasm_export.h"
#include "linear_arena.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>

#if WASM_ENABLE_THREAD_MGR == 0 || WASM_ENABLE_INSTRUCTION_METERING == 0
#error "BLIP WAMR requires instruction metering and the internal cancellation manager"
#endif

namespace blip::wasm {
namespace {
std::atomic<WamrRuntime*> owner{};
core::Status failure(core::ErrorCode code, std::string_view operation, std::string_view detail) noexcept {
    return core::Status::failure({core::ErrorDomain::control, code, "blip.wasm", operation, detail});
}
bool export_name(std::string_view name, std::array<char, kMaximumExportNameBytes + 1>& target) noexcept {
    if (name.empty() || name.size() > kMaximumExportNameBytes || name.find('\0') != name.npos) return false;
    std::memcpy(target.data(), name.data(), name.size());
    return true;
}
bool type(wasm_valkind_t source, ValueType& target) noexcept {
    switch (source) {
    case WASM_I32: target = ValueType::i32; return true;
    case WASM_I64: target = ValueType::i64; return true;
    case WASM_F32: target = ValueType::f32; return true;
    case WASM_F64: target = ValueType::f64; return true;
    default: return false;
    }
}
// This is a bounded section walk, not a substitute for WAMR validation. WAMR
// runs start sections during instantiation with unlimited fuel: reject them
// before loading. Reject malformed/truncated length encodings as well.
bool passive_module(std::span<const std::byte> bytes) noexcept {
    constexpr std::array header{std::byte{0}, std::byte{0x61}, std::byte{0x73}, std::byte{0x6d},
                                std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0}};
    if (bytes.size() < header.size() || !std::equal(header.begin(), header.end(), bytes.begin())) return false;
    std::size_t cursor = header.size();
    while (cursor < bytes.size()) {
        const auto id = std::to_integer<unsigned>(bytes[cursor++]);
        if (id == 8) return false;
        std::uint32_t length{};
        bool complete{};
        for (unsigned i = 0; i < 5 && cursor < bytes.size(); ++i) {
            const auto b = std::to_integer<unsigned>(bytes[cursor++]);
            if (i == 4 && (b & 0xf0)) return false;
            length |= (b & 0x7fU) << (i * 7);
            if (!(b & 0x80)) { complete = true; break; }
        }
        if (!complete || length > bytes.size() - cursor) return false;
        cursor += length;
    }
    return true;
}
bool automatic_entry(std::string_view name) noexcept {
    return name == "__post_instantiate" || name == "__wasm_call_ctors" || name == "_initialize";
}
}

core::Status WamrRuntime::initialize(std::span<std::byte> pool, Limits limits,
                                     std::span<std::byte> linear_memory) noexcept {
    if (initialized_) return failure(core::ErrorCode::invalid_state, "initialize", "already-initialized");
    if (pool.size() < 16384 || pool.size() > std::numeric_limits<std::uint32_t>::max() ||
        reinterpret_cast<std::uintptr_t>(pool.data()) % 8 || limits.linear_memory_bytes != 65536 ||
        limits.wasm_stack_bytes < 2048 || limits.wasm_stack_bytes > 8192 ||
        limits.maximum_module_bytes < 8 || limits.maximum_module_bytes > 16384)
        return failure(core::ErrorCode::invalid_argument, "initialize", "unsupported-pool-or-limits");
    if (!linear_memory.empty()) {
        const auto begin = reinterpret_cast<std::uintptr_t>(pool.data());
        const auto linear_begin = reinterpret_cast<std::uintptr_t>(linear_memory.data());
        if (linear_memory.size() != 65536 || linear_begin % 4 ||
            pool.size() > std::numeric_limits<std::uint32_t>::max() - linear_memory.size() ||
            (begin <= linear_begin ? linear_begin - begin < pool.size() : begin - linear_begin < linear_memory.size()) ||
            WASM_ENABLE_FAST_INTERP == 0)
            return failure(core::ErrorCode::invalid_argument, "initialize", "unsupported-linear-arena");
    }
    WamrRuntime* expected{};
    if (!owner.compare_exchange_strong(expected, this))
        return failure(core::ErrorCode::resource_conflict, "initialize", "runtime-already-owned");
    blip_wasm_linear_arena(linear_memory.empty() ? nullptr : linear_memory.data(), linear_memory.size());
    RuntimeInitArgs args{};
    args.mem_alloc_type = Alloc_With_Pool;
    args.mem_alloc_option.pool.heap_buf = pool.data();
    args.mem_alloc_option.pool.heap_size = static_cast<std::uint32_t>(pool.size());
    if (!wasm_runtime_full_init(&args)) {
        blip_wasm_linear_arena(nullptr, 0);
        owner.store(nullptr);
        return failure(core::ErrorCode::start_failed, "initialize", "runtime-initialization");
    }
    initialized_ = true;
    pool_bytes_ = static_cast<std::uint32_t>(pool.size() + linear_memory.size());
    limits_ = limits;
    fault_.fill(0);
    return core::Status::success();
}

core::Status WamrRuntime::load(std::span<std::byte> bytes) noexcept {
    if (!initialized_ || module_) return failure(core::ErrorCode::invalid_state, "load", "runtime-state");
    if (bytes.size() > limits_.maximum_module_bytes || !passive_module(bytes))
        return failure(core::ErrorCode::validation_failed, "load", "invalid-or-automatic-module");
    fault_.fill(0);
    module_ = wasm_runtime_load(reinterpret_cast<std::uint8_t*>(bytes.data()),
                                static_cast<std::uint32_t>(bytes.size()), fault_.data(), fault_.size());
    if (!module_) return failure(core::ErrorCode::corrupt_data, "load", "engine-validation");
    // Capability imports will be installed by their owners in 6.5. Until that
    // boundary exists no imported host function, memory or table is available.
    if (wasm_runtime_get_import_count(static_cast<wasm_module_t>(module_)) != 0) {
        unload();
        return failure(core::ErrorCode::validation_failed, "load", "imports-unavailable");
    }
    const auto count = wasm_runtime_get_export_count(static_cast<wasm_module_t>(module_));
    if (count < 0) { unload(); return failure(core::ErrorCode::corrupt_data, "load", "module-exports"); }
    for (std::int32_t i = 0; i < count; ++i) {
        wasm_export_t item{};
        wasm_runtime_get_export_type(static_cast<wasm_module_t>(module_), i, &item);
        if (item.kind == WASM_IMPORT_EXPORT_KIND_FUNC && automatic_entry(item.name)) {
            unload();
            return failure(core::ErrorCode::validation_failed, "load", "automatic-constructor");
        }
    }
    InstantiationArgs args{limits_.wasm_stack_bytes, 0, 1};
    instance_ = wasm_runtime_instantiate_ex(static_cast<wasm_module_t>(module_), &args,
                                          fault_.data(), fault_.size());
    if (instance_) environment_ = wasm_runtime_create_exec_env(static_cast<wasm_module_inst_t>(instance_), limits_.wasm_stack_bytes);
    if (!environment_) { unload(); return failure(core::ErrorCode::capacity_exceeded, "load", "instance-allocation"); }
    wasm_runtime_set_native_stack_boundary(static_cast<wasm_exec_env_t>(environment_),
                                           reinterpret_cast<std::uint8_t*>(xTaskGetStackStart(nullptr)) + 2048);
    return core::Status::success();
}

core::Result<Signature> WamrRuntime::signature(std::string_view name) noexcept {
    using Result = core::Result<Signature>;
    std::array<char, kMaximumExportNameBytes + 1> terminated{};
    if (!instance_ || !export_name(name, terminated))
        return Result::failure(failure(core::ErrorCode::invalid_argument, "signature", "module-or-name").error());
    const auto count = wasm_runtime_get_export_count(static_cast<wasm_module_t>(module_));
    for (std::int32_t i = 0; i < count; ++i) {
        wasm_export_t item{};
        wasm_runtime_get_export_type(static_cast<wasm_module_t>(module_), i, &item);
        if (item.kind != WASM_IMPORT_EXPORT_KIND_FUNC || name != item.name) continue;
        const auto arguments = wasm_func_type_get_param_count(item.u.func_type);
        const auto results = wasm_func_type_get_result_count(item.u.func_type);
        if (arguments > kMaximumArguments || results > kMaximumResults)
            return Result::failure(failure(core::ErrorCode::capacity_exceeded, "signature", "function-signature-limit").error());
        Signature out{};
        out.argument_count = static_cast<std::uint8_t>(arguments);
        out.result_count = static_cast<std::uint8_t>(results);
        for (std::uint32_t j = 0; j < arguments; ++j)
            if (!type(wasm_func_type_get_param_valkind(item.u.func_type, j), out.arguments[j]))
                return Result::failure(failure(core::ErrorCode::validation_failed, "signature", "unsupported-argument").error());
        for (std::uint32_t j = 0; j < results; ++j)
            if (!type(wasm_func_type_get_result_valkind(item.u.func_type, j), out.results[j]))
                return Result::failure(failure(core::ErrorCode::validation_failed, "signature", "unsupported-result").error());
        return Result::success(out);
    }
    return Result::failure(failure(core::ErrorCode::not_found, "signature", "export-not-found").error());
}

core::Status WamrRuntime::invoke(std::string_view name, std::span<const Value> arguments,
                                ExecutionBudget budget, std::span<Value> results,
                                std::size_t& result_count) noexcept {
    result_count = 0;
    const auto expected = signature(name);
    if (!expected) return core::Status::failure(expected.error());
    if (arguments.size() != expected.value().argument_count || results.size() < expected.value().result_count ||
        !budget.instructions || budget.instructions > static_cast<std::uint32_t>(std::numeric_limits<int>::max()))
        return failure(core::ErrorCode::invalid_argument, "invoke", "call-bounds");
    // A platform deadline supervisor is still needed for 6.3. Refuse a deadline
    // rather than accepting a request whose wall-clock budget is not enforced.
    if (budget.deadline_us) return failure(core::ErrorCode::invalid_argument, "invoke", "deadline-supervisor-required");
    std::array<char, kMaximumExportNameBytes + 1> terminated{};
    if (!export_name(name, terminated)) return failure(core::ErrorCode::invalid_argument, "invoke", "export-name");
    std::array<std::uint32_t, 2 * std::max(kMaximumArguments, kMaximumResults)> cells{};
    std::uint32_t cell_count{};
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const auto& value = arguments[i];
        const bool wide = value.type == ValueType::i64 || value.type == ValueType::f64;
        if (value.type != expected.value().arguments[i] || (!wide && value.bits > 0xffffffffU))
            return failure(core::ErrorCode::invalid_argument, "invoke", "argument-type-or-bits");
        cells[cell_count++] = static_cast<std::uint32_t>(value.bits);
        if (wide) cells[cell_count++] = static_cast<std::uint32_t>(value.bits >> 32);
    }
    const auto instance = static_cast<wasm_module_inst_t>(instance_);
    const auto environment = static_cast<wasm_exec_env_t>(environment_);
    const auto function = wasm_runtime_lookup_function(instance, terminated.data());
    if (!function) return failure(core::ErrorCode::not_found, "invoke", "export-not-found");
    {
        const std::lock_guard lock(cancel_mutex_);
        if (budget.cancellation && budget.cancellation->load())
            return failure(core::ErrorCode::cancelled, "invoke", "cancelled-before-call");
        wasm_runtime_clear_exception(instance);
        wasm_runtime_set_instruction_count_limit(environment, static_cast<int>(budget.instructions));
        cancelled_ = false;
        running_ = true;
    }
    bool success = wasm_runtime_call_wasm(environment, function, cell_count, cells.data());
    {
        const std::lock_guard lock(cancel_mutex_);
        running_ = false;
        if (cancelled_) success = false;
        if (!success) {
            const auto* exception = wasm_runtime_get_exception(instance);
            std::snprintf(fault_.data(), fault_.size(), "%s", exception ? exception : "cancelled");
            const bool exhausted = exception && std::strstr(exception, "instruction limit exceeded");
            return failure(cancelled_ ? core::ErrorCode::cancelled : exhausted ? core::ErrorCode::budget_exceeded : core::ErrorCode::verification_failed,
                           "invoke", cancelled_ ? "cancelled-execution" : "guest-trap");
        }
    }
    std::size_t cursor{};
    for (std::size_t i = 0; i < expected.value().result_count; ++i) {
        const auto kind = expected.value().results[i];
        std::uint64_t bits = cells[cursor++];
        if (kind == ValueType::i64 || kind == ValueType::f64) bits |= static_cast<std::uint64_t>(cells[cursor++]) << 32;
        results[i] = {kind, bits};
    }
    result_count = expected.value().result_count;
    return core::Status::success();
}

core::Result<std::byte*> WamrRuntime::memory_range(std::uint32_t offset, std::size_t bytes) noexcept {
    using Result = core::Result<std::byte*>;
    if (!instance_) return Result::failure(failure(core::ErrorCode::invalid_state, "memory", "module-not-loaded").error());
    const auto memory = wasm_runtime_get_default_memory(static_cast<wasm_module_inst_t>(instance_));
    if (!memory) return Result::failure(failure(core::ErrorCode::not_found, "memory", "guest-memory-unavailable").error());
    const auto pages = wasm_memory_get_cur_page_count(memory), per_page = wasm_memory_get_bytes_per_page(memory);
    // Keep the full extent in 64 bits and refuse an unsupported memory width.
    if (per_page && pages > UINT32_MAX / per_page)
        return Result::failure(failure(core::ErrorCode::capacity_exceeded, "memory", "guest-memory-width").error());
    const auto extent = pages * per_page;
    if (offset > extent || bytes > extent - offset)
        return Result::failure(failure(core::ErrorCode::invalid_argument, "memory", "guest-memory-bounds").error());
    auto* base = static_cast<std::byte*>(wasm_memory_get_base_address(memory));
    if (!bytes) return Result::success(nullptr); // Do not form a pointer into an empty memory.
    if (!base) return Result::failure(failure(core::ErrorCode::invalid_state, "memory", "guest-memory-base").error());
    return Result::success(base + offset);
}
core::Status WamrRuntime::read_memory(std::uint32_t offset, std::span<std::byte> output) noexcept {
    const auto range = memory_range(offset, output.size());
    if (!range) return core::Status::failure(range.error());
    if (!output.empty()) std::memmove(output.data(), range.value(), output.size());
    return core::Status::success();
}
core::Status WamrRuntime::write_memory(std::uint32_t offset, std::span<const std::byte> input) noexcept {
    const auto range = memory_range(offset, input.size());
    if (!range) return core::Status::failure(range.error());
    if (!input.empty()) std::memmove(range.value(), input.data(), input.size());
    return core::Status::success();
}
void WamrRuntime::request_cancel() noexcept {
    const std::lock_guard lock(cancel_mutex_);
    if (running_) {
        cancelled_ = true;
        wasm_runtime_terminate(static_cast<wasm_module_inst_t>(instance_));
    }
}
void WamrRuntime::unload() noexcept {
    const std::lock_guard lock(cancel_mutex_);
    if (environment_) wasm_runtime_destroy_exec_env(static_cast<wasm_exec_env_t>(environment_));
    if (instance_) wasm_runtime_deinstantiate(static_cast<wasm_module_inst_t>(instance_));
    if (module_) wasm_runtime_unload(static_cast<wasm_module_t>(module_));
    environment_ = nullptr;
    instance_ = nullptr;
    module_ = nullptr;
}
void WamrRuntime::shutdown() noexcept {
    unload();
    if (initialized_) {
        wasm_runtime_destroy();
        blip_wasm_linear_arena(nullptr, 0);
        initialized_ = false;
        pool_bytes_ = 0;
        owner.store(nullptr);
    }
}
RuntimeSnapshot WamrRuntime::snapshot() const noexcept {
    RuntimeSnapshot out{};
    out.reserved_bytes = pool_bytes_;
    out.fault = fault_;
    mem_alloc_info_t info{};
    if (initialized_ && wasm_runtime_get_mem_alloc_info(&info)) {
        out.used_bytes = info.total_size - info.total_free_size + blip_wasm_linear_used();
        // Sum of each arena's high-water mark is a conservative total peak.
        out.peak_bytes = info.highmark_size + blip_wasm_linear_peak();
    }
    return out;
}
} // namespace blip::wasm
