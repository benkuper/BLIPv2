#include "blip/wasm/capability.hpp"
#include "blip/wasm/esp_wasm_component.hpp"
#include "blip/wasm/wamr_runtime.hpp"
#include "fixtures.hpp"
#include "wasm_export.h"
#include "esp_heap_caps.h"
#include "esp_pthread.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <algorithm>
#include <cstdio>
#include <pthread.h>

namespace {
using namespace blip::wasm;
using blip::core::ErrorCode;
using blip::core::Status;
using blip::core::WasmArgumentDescriptor;
using blip::core::WasmArgumentRole;
constexpr std::array i32_arg{WasmArgumentDescriptor{"value", ValueType::i32}};
constexpr std::array i64_arg{WasmArgumentDescriptor{"value", ValueType::i64}};
constexpr std::array f32_arg{WasmArgumentDescriptor{"value", ValueType::f32}};
constexpr std::array f64_arg{WasmArgumentDescriptor{"value", ValueType::f64}};
constexpr std::array mixed_args{WasmArgumentDescriptor{"a", ValueType::i32}, WasmArgumentDescriptor{"b", ValueType::i64},
    WasmArgumentDescriptor{"c", ValueType::f32}, WasmArgumentDescriptor{"d", ValueType::f64},
    WasmArgumentDescriptor{"e", ValueType::i32}, WasmArgumentDescriptor{"f", ValueType::i64},
    WasmArgumentDescriptor{"g", ValueType::f32}, WasmArgumentDescriptor{"h", ValueType::f64}};
constexpr std::array wide_args{WasmArgumentDescriptor{"a", ValueType::i64}, WasmArgumentDescriptor{"b", ValueType::i64},
    WasmArgumentDescriptor{"c", ValueType::i64}, WasmArgumentDescriptor{"d", ValueType::i64},
    WasmArgumentDescriptor{"e", ValueType::i64}, WasmArgumentDescriptor{"f", ValueType::i64},
    WasmArgumentDescriptor{"g", ValueType::i64}, WasmArgumentDescriptor{"h", ValueType::i64}};
constexpr std::array text_args{WasmArgumentDescriptor{"offset", ValueType::i32, WasmArgumentRole::utf8_offset},
    WasmArgumentDescriptor{"bytes", ValueType::i32, WasmArgumentRole::utf8_length}, WasmArgumentDescriptor{"destination", ValueType::i32}};
constexpr std::array i32_result{ValueType::i32}, i64_result{ValueType::i64}, f32_result{ValueType::f32}, f64_result{ValueType::f64};
constexpr std::array alpha_functions{
    blip::core::WasmFunctionDescriptor{"echo", "Raw i32", i32_arg, i32_result, 2000},
    blip::core::WasmFunctionDescriptor{"wide", "Raw i64", i64_arg, i64_result, 2000},
    blip::core::WasmFunctionDescriptor{"float", "Raw f32", f32_arg, f32_result, 2000},
    blip::core::WasmFunctionDescriptor{"double", "Raw f64", f64_arg, f64_result, 2000},
    blip::core::WasmFunctionDescriptor{"text", "Checked text copy", text_args, i32_result, 2000},
    blip::core::WasmFunctionDescriptor{"noop", "Void", {}, {}, 2000},
    blip::core::WasmFunctionDescriptor{"fail", "Injected failure", {}, i32_result, 2000},
    blip::core::WasmFunctionDescriptor{"slow", "Injected slow callback", {}, i32_result, 2000}};
constexpr std::array beta_functions{blip::core::WasmFunctionDescriptor{"echo", "Independent owner", i32_arg, i32_result, 2000},
    blip::core::WasmFunctionDescriptor{"mixed", "Eight mixed numeric arguments", mixed_args, i64_result, 2000},
    blip::core::WasmFunctionDescriptor{"wide8", "Eight wide numeric arguments", wide_args, i64_result, 2000}};
std::atomic<unsigned> register_attempt{}, fail_register{};
std::atomic<bool> entered{}, cancellation_issued{}, done{};
WamrRuntime runtime;
class Provider final : public blip::core::Component, public CapabilityProvider {
  public:
    explicit Provider(bool beta) noexcept : beta_(beta) {
        descriptor_.schema_version = 1; descriptor_.id = beta ? "test.beta" : "test.alpha";
        descriptor_.display_name = descriptor_.id; descriptor_.settings = {1, 1}; descriptor_.supports_resume = true;
        descriptor_.wasm = {1, beta ? "test.beta.v1" : "test.alpha.v1", beta ?
            std::span<const blip::core::WasmFunctionDescriptor>(beta_functions) :
            std::span<const blip::core::WasmFunctionDescriptor>(alpha_functions)};
    }
    const blip::core::ComponentDescriptor& descriptor() const noexcept override { return descriptor_; }
    CapabilityProvider* wasm_provider() noexcept override { return this; }
    Status start(const blip::core::StartContext&) noexcept override { enabled_.store(true); return Status::success(); }
    Status suspend() noexcept override { enabled_.store(false); return Status::success(); }
    Status resume() noexcept override { enabled_.store(true); return Status::success(); }
    Status stop() noexcept override { enabled_.store(false); return Status::success(); }
    bool available() const noexcept override { return enabled_.load(); }
    Status invoke(std::string_view name, CallContext& context, std::span<const Value> arguments,
        std::span<Value> output, std::size_t& count) noexcept override {
        count = 0;
        if (reenter) return runtime.invoke("noop", {}, {}, output, count);
        if (name == "fail") return failure(ErrorCode::queue_full);
        if (name == "noop") return Status::success();
        if (name == "text") {
            std::array<char, kMaximumStringBytes> text{}; std::size_t bytes{};
            auto status = context.read_utf8({static_cast<std::uint32_t>(arguments[0].bits), static_cast<std::uint32_t>(arguments[1].bits)}, text, bytes);
            if (!status) return status;
            status = context.write_utf8(static_cast<std::uint32_t>(arguments[2].bits), {text.data(), bytes});
            if (!status) return status;
            output[0] = Value::i32(static_cast<std::uint32_t>(bytes));
        } else if (name == "slow") {
            if (cooperative) {
                entered.store(true);
                const auto until = esp_timer_get_time() + 200000;
                while (!context.cancelled() && esp_timer_get_time() < until) vTaskDelay(1);
                entered.store(false);
                if (context.cancelled()) return failure(ErrorCode::cancelled);
            } else vTaskDelay(pdMS_TO_TICKS(10));
            output[0] = Value::i32(1);
        } else if (name == "mixed" || name == "wide8") {
            std::uint64_t bits{};
            for (std::size_t i = 0; i < arguments.size(); ++i) bits ^= arguments[i].bits + i;
            output[0] = Value::i64(bits);
        } else {
            output[0] = arguments[0];
            if (beta_) output[0].bits = (output[0].bits + 1) & 0xffffffffU;
        }
        if (wrong_result) output[0] = Value::f64(1);
        count = 1; return Status::success();
    }
    bool cooperative{}, reenter{}, wrong_result{};
  private:
    static Status failure(ErrorCode code) noexcept { return Status::failure({blip::core::ErrorDomain::control, code, "test.alpha", "invoke", "injected"}); }
    blip::core::ComponentDescriptor descriptor_{};
    std::atomic<bool> enabled_{};
    bool beta_{};
};
Provider alpha{false}, beta{true};
blip::core::Registry<2> registry;
CapabilityRegistry capabilities;
std::byte *pool{}, *module{}, *linear{};
unsigned checks{}, failures{};
void check(bool valid, const char* name) {
    ++checks; if (!valid) { ++failures; std::printf("IMPORTS {\"type\":\"failure\",\"check\":\"%s\"}\n", name); }
}
void reload(Service& service) { check(service.load(fixtures::valid).ok(), "reload-valid-module"); }
void numeric(Service& service, std::string_view name, Value input, Value expected) {
    const std::array arguments{input}; std::array<Value, 1> output{}; std::size_t count{};
    const auto status = service.call(name, arguments, {}, output, count);
    check(status.ok() && count == 1 && output[0].type == expected.type && output[0].bits == expected.bits, "numeric-import-bits");
}
void eight_arguments(Service& service, std::string_view name, const std::array<Value, 8>& arguments) {
    std::uint64_t expected{};
    for (std::size_t i = 0; i < arguments.size(); ++i) expected ^= arguments[i].bits + i;
    std::array<Value, 1> output{}; std::size_t count{};
    const auto status = service.call(name, arguments, {}, output, count);
    check(status.ok() && count == 1 && output[0].type == ValueType::i64 && output[0].bits == expected, "eight-argument-import-bits");
}
void rejected_call(Service& service, const char* name, ErrorCode code, std::span<const Value> arguments = {}) {
    std::array<Value, 1> output{Value::i32(999)}; std::size_t count = 99;
    const auto status = service.call(name, arguments, {}, output, count);
    check(!status && status.error().code == code && count == 0 && output[0].bits == 999 && service.snapshot().state == State::faulted, "rejected-import-faults-without-result");
    reload(service);
}
void copies(Service& service) {
    check(service.write_utf8(522, service.snapshot().generation, "!").ok(), "prepare-copy-sentinel");
    const std::array args{Value::i32(0), Value::i32(9), Value::i32(513)};
    std::array<Value, 1> output{}; std::size_t count{};
    check(service.call("text", args, {}, output, count).ok() && count == 1 && output[0].bits == 9, "guest-to-provider-text");
    std::array<char, 10> copied{}; std::size_t bytes{};
    check(service.read_utf8({513,10}, service.snapshot().generation, copied, bytes).ok() &&
        std::string_view(copied.data(), bytes) == std::string_view("\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80!",10), "exact-copy-no-terminator");
    numeric(service, "peek", Value::i32(513), Value::i32(0xc3));
}
void* worker(void*) {
    check(registry.add(beta).ok() && registry.add(alpha).ok() && registry.validate().ok() && capabilities.bind(registry).ok(), "bind-independent-owners");
    Service service(runtime, {pool, EspWasmComponent::kEnginePoolBytes}, {module, EspWasmComponent::kModuleBytes}, {linear, EspWasmComponent::kLinearBytes});
    check(runtime.configure_capabilities(&capabilities).ok() && service.start().ok(), "register-native-imports");
    const auto idle_pool = runtime.snapshot().used_bytes;
    check(idle_pool > 0 && idle_pool < 8192, "bounded-persistent-registration");
    reload(service); const auto before = runtime.snapshot().native_calls;
    check(before == 0, "loading-does-not-execute-imports");
    const std::array echo_arg{Value::i32(40)};
    rejected_call(service, "echo", ErrorCode::resource_unavailable, echo_arg);
    check(registry.start_all().ok(), "start-providers");
    numeric(service,"echo",Value::i32(0xffffffffU),Value::i32(0xffffffffU));
    numeric(service,"wide",Value::i64(0xffffffffffffffffULL),Value::i64(0xffffffffffffffffULL));
    numeric(service,"float",{ValueType::f32,0x7fc01234U},{ValueType::f32,0x7fc01234U});
    numeric(service,"double",{ValueType::f64,0x7ff8000012345678ULL},{ValueType::f64,0x7ff8000012345678ULL});
    numeric(service,"beta",Value::i32(40),Value::i32(41)); copies(service);
    eight_arguments(service, "mixed", {Value::i32(0xffffffffU), Value::i64(0xffffffffffffffffULL),
        Value{ValueType::f32,0x7fc01234U}, Value{ValueType::f64,0x7ff8000012345678ULL},
        Value::i32(42), Value::i64(0xfedcba9876543210ULL), Value{ValueType::f32,0x80000000U}, Value{ValueType::f64,0x8000000000000000ULL}});
    eight_arguments(service, "wide8", {Value::i64(0xffffffffffffffffULL), Value::i64(0xfedcba9876543210ULL),
        Value::i64(0x8000000000000000ULL), Value::i64(0x0123456789abcdefULL),
        Value::i64(0x1020304050607080ULL), Value::i64(0x8877665544332211ULL), Value::i64(1), Value::i64(0)});
    std::array<Value, 1> output{}; std::size_t count{};
    check(service.call("noop", {}, {}, {}, count).ok() && count == 0, "void-import");
    for (const auto& args : {std::array{Value::i32(32),Value::i32(3),Value::i32(513)},
                            std::array{Value::i32(0xffffffffU),Value::i32(8),Value::i32(513)}})
        rejected_call(service,"text",ErrorCode::invalid_argument,args);
    const std::array long_args{Value::i32(0),Value::i32(257),Value::i32(513)};
    rejected_call(service,"text",ErrorCode::capacity_exceeded,long_args);
    const std::array empty_args{Value::i32(65536),Value::i32(0),Value::i32(65536)};
    check(service.call("text", empty_args, {}, output, count).ok() && count == 1 && output[0].bits == 0, "empty-end-string-import");
    rejected_call(service,"fail",ErrorCode::queue_full);
    alpha.wrong_result = true; rejected_call(service,"echo",ErrorCode::verification_failed,echo_arg); alpha.wrong_result = false;
    alpha.reenter = true; rejected_call(service,"echo",ErrorCode::recursive_dispatch,echo_arg); alpha.reenter = false;
    rejected_call(service,"slow",ErrorCode::budget_exceeded);
    alpha.cooperative = true; rejected_call(service,"slow",ErrorCode::cancelled); alpha.cooperative = false;
    check(cancellation_issued.load(), "active-native-cancellation-issued");
    const auto loop_status = service.call("loop", {}, {1000,0,nullptr}, {}, count);
    check(!loop_status && loop_status.error().code == ErrorCode::budget_exceeded && count == 0 &&
        service.snapshot().state == State::faulted, "fuel-bounds-import-loop");
    reload(service);
    check(registry.suspend_all().ok(), "suspend-providers"); rejected_call(service,"echo",ErrorCode::resource_unavailable,echo_arg);
    check(registry.resume_all().ok(), "resume-providers"); numeric(service,"echo",Value::i32(40),Value::i32(40));
    const std::array negative{std::span<const std::byte>(fixtures::unknown), std::span<const std::byte>(fixtures::version),
        std::span<const std::byte>(fixtures::alias), std::span<const std::byte>(fixtures::module_nul), std::span<const std::byte>(fixtures::field_nul),
        std::span<const std::byte>(fixtures::argument_type), std::span<const std::byte>(fixtures::result_type), std::span<const std::byte>(fixtures::many_results),
        std::span<const std::byte>(fixtures::memory_import), std::span<const std::byte>(fixtures::table_import), std::span<const std::byte>(fixtures::global_import),
        std::span<const std::byte>(fixtures::start), std::span<const std::byte>(fixtures::constructors), std::span<const std::byte>(fixtures::export_nul),
        std::span<const std::byte>(fixtures::imports_33)};
    for (const auto bytes : negative) {
        const auto calls = runtime.snapshot().native_calls;
        const auto status = service.load(bytes);
        check(!status && service.snapshot().state == State::ready && runtime.snapshot().native_calls == calls, "invalid-import-module-never-executes");
        check(runtime.snapshot().used_bytes == idle_pool, "rejected-load-restores-idle-registration");
    }
    check(service.load(fixtures::imports_32).ok(), "maximum-import-count"); numeric(service,"echo",Value::i32(123),Value::i32(123));
    service.unload();
    const auto baseline = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    auto minimum = baseline, maximum = baseline; std::uint32_t peak{};
    for (unsigned cycle = 0; cycle < 100; ++cycle) {
        reload(service); copies(service); peak = runtime.snapshot().peak_bytes; service.unload();
        check(runtime.snapshot().used_bytes == idle_pool, "idle-registration-stable");
        const auto heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        minimum = std::min(minimum, heap); maximum = std::max(maximum, heap);
        check(heap == baseline, "stable-sdk-heap"); vTaskDelay(1);
    }
    const auto measured = runtime.snapshot(); service.stop();
    const auto stopped_heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    for (unsigned fail_at : {1U, 2U}) {
        fail_register.store(fail_at); register_attempt.store(0);
        check(runtime.configure_capabilities(&capabilities).ok() && !service.start(), "injected-registration-failure");
        check(service.snapshot().state == State::stopped && runtime.snapshot().reserved_bytes == 0, "failed-registration-releases-runtime");
        check(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) == stopped_heap, "failed-registration-sdk-heap-stable");
        fail_register.store(0); check(runtime.configure_capabilities(&capabilities).ok() && service.start().ok(), "registration-failure-retry");
        reload(service); numeric(service,"echo",Value::i32(40),Value::i32(40)); service.stop();
    }
    for (unsigned cycle = 0; cycle < 20; ++cycle) {
        check(runtime.configure_capabilities(&capabilities).ok() && service.start().ok(), "repeat-engine-registration");
        reload(service); numeric(service,"echo",Value::i32(40),Value::i32(40)); service.stop();
        check(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) == stopped_heap, "repeat-engine-sdk-heap-stable");
    }
    check(registry.stop_all().ok(), "stop-providers-after-runtime"); check(heap_caps_check_integrity_all(true), "heap-integrity");
    std::printf("IMPORTS {\"type\":\"complete\",\"checks\":%u,\"failures\":%u,\"reload_cycles\":100,\"engine_cycles\":20,\"heap_baseline\":%u,\"heap_min\":%u,\"heap_max\":%u,\"pool_peak\":%u,\"idle_pool_used\":%u,\"worker_headroom\":%u,\"native_calls\":%u,\"native_failures\":%u,\"native_maximum_us\":%u,\"native_imports_exercised\":true}\n",
        checks, failures, static_cast<unsigned>(baseline), static_cast<unsigned>(minimum), static_cast<unsigned>(maximum),
        static_cast<unsigned>(peak), static_cast<unsigned>(idle_pool), static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)),
        static_cast<unsigned>(measured.native_calls), static_cast<unsigned>(measured.native_failures), static_cast<unsigned>(measured.native_maximum_us));
    done.store(true); return nullptr;
}
} // namespace
extern "C" bool __real_wasm_runtime_register_natives_raw(const char*, NativeSymbol*, std::uint32_t);
extern "C" bool __wrap_wasm_runtime_register_natives_raw(const char* module_name, NativeSymbol* symbols, std::uint32_t count) {
    const auto attempt = register_attempt.fetch_add(1) + 1;
    if (fail_register.load() == attempt) return false;
    return __real_wasm_runtime_register_natives_raw(module_name, symbols, count);
}
extern "C" void app_main() {
    vTaskDelay(pdMS_TO_TICKS(1500));
    std::printf("IMPORTS {\"type\":\"start\",\"chip\":\"%s\",\"idf\":\"%s\",\"worker_stack_bytes\":8192,\"fixture_sha256\":\"%s\"}\n", CONFIG_IDF_TARGET, esp_get_idf_version(), fixtures::valid_sha256);
    pool = static_cast<std::byte*>(heap_caps_aligned_alloc(8, EspWasmComponent::kEnginePoolBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    module = static_cast<std::byte*>(heap_caps_malloc(EspWasmComponent::kModuleBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
#if defined(CONFIG_IDF_TARGET_ESP32)
    linear = static_cast<std::byte*>(heap_caps_malloc(EspWasmComponent::kLinearBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_IRAM_8BIT));
#endif
    if (!pool || !module || (EspWasmComponent::kLinearBytes && !linear)) {
        std::printf("IMPORTS {\"type\":\"fatal\",\"check\":\"startup-buffers\"}\n");
        heap_caps_free(pool); heap_caps_free(module); heap_caps_free(linear); return;
    }
    auto config = esp_pthread_get_default_config(); config.stack_size = 8192; config.prio = 2;
    config.thread_name = "blip_imports"; config.pin_to_core = -1; config.inherit_cfg = false;
    pthread_t thread{};
    if (esp_pthread_set_cfg(&config) != ESP_OK || pthread_create(&thread, nullptr, worker, nullptr) != 0) {
        std::printf("IMPORTS {\"type\":\"fatal\",\"check\":\"worker-create\"}\n"); heap_caps_free(pool); heap_caps_free(module); heap_caps_free(linear); return;
    }
    const auto until = esp_timer_get_time() + 20000000;
    while (!done.load() && esp_timer_get_time() < until) {
        if (entered.load()) { cancellation_issued.store(true); runtime.request_cancel(); }
        vTaskDelay(1);
    }
    if (!done.load()) { std::printf("IMPORTS {\"type\":\"fatal\",\"check\":\"worker-timeout\"}\n"); esp_restart(); }
    pthread_join(thread, nullptr); heap_caps_free(pool); heap_caps_free(module); heap_caps_free(linear);
}
