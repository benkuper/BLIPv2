#include "blip/wasm/capability.hpp"
#include "blip/wasm/esp_wasm_component.hpp"
#include "blip/wasm/wamr_runtime.hpp"
#include "../../wasm-strings/main/fixtures.hpp"
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
constexpr std::array numeric{WasmArgumentDescriptor{"value", ValueType::i32}};
constexpr std::array text{
    WasmArgumentDescriptor{"offset", ValueType::i32, WasmArgumentRole::utf8_offset},
    WasmArgumentDescriptor{"bytes", ValueType::i32, WasmArgumentRole::utf8_length},
    WasmArgumentDescriptor{"destination", ValueType::i32}};
constexpr std::array result{ValueType::i32};
constexpr std::array functions{
    blip::core::WasmFunctionDescriptor{"echo", "Component identity echo", numeric, result, 100},
    blip::core::WasmFunctionDescriptor{"text", "Checked guest text copy", text, result, 1000}};
class Provider final : public blip::core::Component, public CapabilityProvider {
  public:
    Provider(std::string_view id, std::string_view module, unsigned bias) noexcept : bias_(bias) {
        descriptor_.schema_version = 1; descriptor_.id = id; descriptor_.display_name = id;
        descriptor_.settings = {1, 1}; descriptor_.supports_resume = true; descriptor_.wasm = {1, module, functions};
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
        if (name == "echo") output[0] = Value::i32(static_cast<std::uint32_t>(arguments[0].bits) + bias_);
        else {
            std::array<char, kMaximumStringBytes> buffer{}; std::size_t bytes{};
            auto status = context.read_utf8({static_cast<std::uint32_t>(arguments[0].bits), static_cast<std::uint32_t>(arguments[1].bits)}, buffer, bytes);
            if (!status) return status;
            status = context.write_utf8(static_cast<std::uint32_t>(arguments[2].bits), {buffer.data(), bytes});
            if (!status) return status;
            output[0] = Value::i32(static_cast<std::uint32_t>(bytes));
        }
        count = 1; return Status::success();
    }
  private:
    blip::core::ComponentDescriptor descriptor_{};
    std::atomic<bool> enabled_{};
    unsigned bias_{};
};
Provider alpha{"test.alpha", "test.alpha.v1", 1}, beta{"test.beta", "test.beta.v1", 2};
blip::core::Registry<2> registry;
CapabilityRegistry capabilities;
WamrRuntime runtime;
std::byte *pool{}, *module{}, *linear{};
std::atomic<bool> done{};
unsigned checks{}, failures{};
std::int64_t maximum_callback_us{};
void check(bool valid, const char* name) {
    ++checks; if (!valid) { ++failures; std::printf("CAPABILITIES {\"type\":\"failure\",\"check\":\"%s\"}\n", name); }
}
void copies(Service& service) {
    constexpr std::array args{Value::i32(32), Value::i32(9), Value::i32(513)};
    std::array<Value, 1> output{Value::i32(999)}; std::size_t count{};
    const auto began = esp_timer_get_time();
    check(capabilities.invoke(1, runtime, args, output, count).ok() && count == 1 && output[0].bits == 9, "provider-text-copy");
    maximum_callback_us = std::max(maximum_callback_us, esp_timer_get_time() - began);
    std::array<char, 9> copied{}; std::size_t bytes{};
    constexpr char unicode[] = "\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80";
    check(service.read_utf8({513,9}, service.snapshot().generation, copied, bytes).ok() && bytes == 9 &&
        std::string_view(copied.data(), bytes) == std::string_view(unicode, 9), "real-memory-copy-verified");
    constexpr std::array peek{Value::i32(513)};
    check(service.call("peek", peek, {}, output, count).ok() && count == 1 && output[0].bits == 0xc3, "guest-observes-provider-write");
}
void* worker(void*) {
    Service service(runtime, {pool, EspWasmComponent::kEnginePoolBytes}, {module, EspWasmComponent::kModuleBytes}, {linear, EspWasmComponent::kLinearBytes});
    check(registry.add(beta).ok() && registry.add(alpha).ok() && registry.validate().ok(), "register-independent-components");
    check(capabilities.bind(registry).ok() && capabilities.size() == 4, "bind-immutable-catalog");
    check(capabilities.binding(0).provider == &alpha && capabilities.binding(2).provider == &beta, "lexical-binding-order");
    check(service.start().ok() && service.load(fixtures::strings).ok(), "start-real-wamr");
    if (service.snapshot().state != State::loaded) { service.stop(); done.store(true); return nullptr; }
    std::array<Value, 1> output{Value::i32(999)}; std::size_t count{};
    constexpr std::array args{Value::i32(40)};
    check(capabilities.invoke(0, runtime, args, output, count).error().code == ErrorCode::resource_unavailable && count == 0 && output[0].bits == 999, "pre-start-unavailable");
    check(registry.start_all().ok(), "start-providers");
    check(capabilities.invoke(0, runtime, args, output, count).ok() && count == 1 && output[0].bits == 41, "alpha-result");
    check(capabilities.invoke(2, runtime, args, output, count).ok() && count == 1 && output[0].bits == 42, "beta-result");
    copies(service);
    constexpr std::array invalid{Value::i32(96), Value::i32(3), Value::i32(513)};
    output[0] = Value::i32(999);
    check(capabilities.invoke(1, runtime, invalid, output, count).error().code == ErrorCode::invalid_argument && count == 0 && output[0].bits == 999, "malformed-input-preserves-result");
    check(registry.suspend_all().ok(), "suspend-providers");
    check(!capabilities.invoke(0, runtime, args, output, count) && count == 0, "suspended-unavailable");
    check(registry.resume_all().ok(), "resume-providers");
    std::atomic<bool> cancelled{true};
    check(capabilities.invoke(0, runtime, args, output, count, &cancelled).error().code == ErrorCode::cancelled, "cancelled-admission");
    service.unload();
    const auto baseline = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    auto minimum = baseline, maximum = baseline; std::uint32_t peak{};
    for (unsigned cycle = 0; cycle < 100; ++cycle) {
        check(service.load(fixtures::strings).ok(), "reload"); copies(service);
        peak = service.snapshot().runtime.peak_bytes; service.unload();
        check(runtime.snapshot().used_bytes == 0, "unloaded-pool-empty");
        const auto heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        minimum = std::min(minimum, heap); maximum = std::max(maximum, heap);
        check(heap == baseline, "stable-sdk-heap"); vTaskDelay(1);
    }
    service.stop(); check(registry.stop_all().ok(), "stop-providers-after-runtime");
    check(!capabilities.invoke(0, runtime, args, output, count) && count == 0, "stopped-unavailable");
    check(heap_caps_check_integrity_all(true), "heap-integrity");
    std::printf("CAPABILITIES {\"type\":\"complete\",\"checks\":%u,\"failures\":%u,\"reload_cycles\":100,\"heap_baseline\":%u,\"heap_min\":%u,\"heap_max\":%u,\"pool_peak\":%u,\"worker_headroom\":%u,\"maximum_callback_us\":%lld,\"native_imports_exercised\":false}\n",
        checks, failures, static_cast<unsigned>(baseline), static_cast<unsigned>(minimum), static_cast<unsigned>(maximum),
        static_cast<unsigned>(peak), static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)), maximum_callback_us);
    done.store(true); return nullptr;
}
} // namespace
extern "C" void app_main() {
    vTaskDelay(pdMS_TO_TICKS(1500));
    std::printf("CAPABILITIES {\"type\":\"start\",\"chip\":\"%s\",\"idf\":\"%s\",\"worker_stack_bytes\":8192,\"catalog_bytes\":%u,\"fixture_sha256\":\"%s\"}\n",
        CONFIG_IDF_TARGET, esp_get_idf_version(), static_cast<unsigned>(sizeof(CapabilityRegistry)), fixtures::strings_sha256);
    pool = static_cast<std::byte*>(heap_caps_aligned_alloc(8, EspWasmComponent::kEnginePoolBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    module = static_cast<std::byte*>(heap_caps_malloc(EspWasmComponent::kModuleBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
#if defined(CONFIG_IDF_TARGET_ESP32)
    linear = static_cast<std::byte*>(heap_caps_malloc(EspWasmComponent::kLinearBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_IRAM_8BIT));
#endif
    if (!pool || !module || (EspWasmComponent::kLinearBytes && !linear)) {
        std::printf("CAPABILITIES {\"type\":\"fatal\",\"check\":\"startup-buffers\"}\n");
        heap_caps_free(pool); heap_caps_free(module); heap_caps_free(linear); return;
    }
    auto config = esp_pthread_get_default_config(); config.stack_size = 8192; config.prio = 2;
    config.thread_name = "blip_capabilities"; config.pin_to_core = -1; config.inherit_cfg = false;
    pthread_t thread{};
    if (esp_pthread_set_cfg(&config) != ESP_OK || pthread_create(&thread, nullptr, worker, nullptr) != 0) {
        std::printf("CAPABILITIES {\"type\":\"fatal\",\"check\":\"worker-create\"}\n");
        heap_caps_free(pool); heap_caps_free(module); heap_caps_free(linear); return;
    }
    const auto until = esp_timer_get_time() + 20000000;
    while (!done.load() && esp_timer_get_time() < until) vTaskDelay(pdMS_TO_TICKS(10));
    if (!done.load()) { std::printf("CAPABILITIES {\"type\":\"fatal\",\"check\":\"worker-timeout\"}\n"); esp_restart(); }
    pthread_join(thread, nullptr); heap_caps_free(pool); heap_caps_free(module); heap_caps_free(linear);
}
