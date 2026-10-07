#include "blip/wasm/service.hpp"
#include "blip/wasm/wamr_runtime.hpp"
#include "fixtures.hpp"
#include "esp_heap_caps.h"
#include "esp_pthread.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <atomic>
#include <cstdio>
#include <limits>
#include <pthread.h>

namespace {
using namespace blip::wasm;
alignas(8) std::array<std::byte, 98304> pool{};
alignas(8) std::array<std::byte, 16384> modules{};
WamrRuntime runtime{};
std::atomic<bool> done{};
std::atomic<std::int64_t> call_started{};
std::atomic<bool> cancel_phase{};
std::atomic<unsigned> cancel_requests{};
std::uint32_t failures{};
std::uint32_t checks{};
std::int64_t maximum_trap_us{};
constexpr std::uint32_t worker_stack_bytes = 16384;

void check(bool valid, const char* name) {
    ++checks;
    if (!valid) {
        ++failures;
        std::printf("SERVICE {\"type\":\"failure\",\"check\":\"%s\"}\n", name);
    }
}
bool echo(Service& service, std::uint32_t value) {
    const std::array args{Value::i32(value)};
    std::array<Value, 1> results{};
    std::size_t count{};
    return service.call("echo", args, {}, results, count) && count == 1 && results[0].bits == value;
}
void* worker(void*) {
    Service service(runtime, pool, modules);
    std::printf("SERVICE {\"type\":\"start\",\"chip\":\"%s\",\"idf\":\"%s\",\"engine\":\"%s\",\"cpu_mhz\":%d,\"pool_bytes\":98304,\"module_bytes\":16384,\"wasm_stack_bytes\":4096,\"native_stack_bytes\":%lu,\"fixture_sha256\":\"%s\"}\n",
                CONFIG_IDF_TARGET, esp_get_idf_version(), runtime.name().data(),
                CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ, static_cast<unsigned long>(worker_stack_bytes), fixtures::workloads_sha256);
    check(service.start().ok(), "start");
    if (service.snapshot().state == State::stopped) { done.store(true); return nullptr; }
    // A second engine cannot overwrite the global pool while one is alive.
    WamrRuntime second;
    check(!second.initialize(pool, {}), "one-runtime-owner");
    second.shutdown();
    check(service.load(fixtures::workloads).ok(), "load");
    check(echo(service, 0xfedcba98), "echo-bits");
    const std::array mixed{Value::i32(0xfedcba98), Value::i64(0xfedcba9876543210ULL),
                          Value{ValueType::f32, 0xffc01234}, Value{ValueType::f64, 0xfff8000000001234ULL}};
    std::array<Value, 8> results{};
    std::size_t count{};
    bool roundtrip = service.call("mixed", mixed, {}, results, count).ok() && count == mixed.size();
    for (std::size_t i = 0; i < mixed.size(); ++i) roundtrip &= results[i].type == mixed[i].type && results[i].bits == mixed[i].bits;
    check(roundtrip, "numeric-multivalue-roundtrip");
    check(service.call("word", {}, {}, results, count).ok() && count == 1 && results[0].bits == 0, "memory-zero");
    check(service.call("grow", {}, {}, results, count).ok() && count == 1 && results[0].bits == 0xffffffffU, "memory-cap");
    const auto generation = service.snapshot().generation;
    check(!service.call("missing", {}, {}, results, count) && service.snapshot().state == State::loaded, "missing-export");
    check(!service.call("too_many", {}, {}, results, count) && service.snapshot().state == State::loaded, "signature-cap");
    const std::array bad{Value::i64(1)};
    check(!service.call("echo", bad, {}, results, count) && service.snapshot().generation == generation, "invalid-argument");
    for (const auto* name : {"trap", "invalid", "divide", "recursion", "spin"}) {
        check(service.load(fixtures::workloads).ok(), "fault-load");
        const std::array args{Value::i32(0)};
        const auto began = esp_timer_get_time();
        call_started.store(began);
        const auto status = service.call(name, std::string_view(name) == "divide" ? std::span<const Value>(args) : std::span<const Value>{}, {}, results, count);
        call_started.store(0);
        const auto elapsed = esp_timer_get_time() - began;
        if (elapsed > maximum_trap_us) maximum_trap_us = elapsed;
        check(!status && count == 0 && service.snapshot().state == State::faulted, name);
        check(!echo(service, 1), "fault-blocks-calls");
        const auto previous = service.snapshot();
        check(previous.runtime.fault[0] != 0, "fault-owned");
        check(service.load(fixtures::workloads).ok() && echo(service, 91), "fault-reload-recovers");
    }
    check(service.load(fixtures::workloads).ok(), "cancel-load");
    const auto began = esp_timer_get_time();
    call_started.store(began);
    cancel_phase.store(true);
    const auto cancelled = service.call("spin", {}, {100000000, 0, nullptr}, results, count);
    cancel_phase.store(false);
    call_started.store(0);
    const auto cancel_us = esp_timer_get_time() - began;
    check(!cancelled && cancelled.error().code == blip::core::ErrorCode::cancelled && count == 0 && cancel_requests.load() > 0, "async-cancellation");
    check(service.load(fixtures::workloads).ok() && echo(service, 92), "cancel-reload-recovers");
    for (const auto module : {std::span<const std::byte>(fixtures::start_loop), std::span<const std::byte>(fixtures::post_loop),
                             std::span<const std::byte>(fixtures::ctor_loop), std::span<const std::byte>(fixtures::initialize_loop),
                             std::span<const std::byte>(fixtures::imported), std::span<const std::byte>(fixtures::large_memory)}) {
        call_started.store(esp_timer_get_time());
        check(!service.load(module) && service.snapshot().state == State::ready, "rejected-module");
        call_started.store(0);
        check(service.load(fixtures::workloads).ok() && echo(service, 93), "rejected-module-recovers");
    }
    service.unload();
    const auto heap_after_warmup = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    std::size_t heap_min = heap_after_warmup;
    std::size_t heap_max = heap_after_warmup;
    std::uint32_t used_unloaded{};
    std::uint32_t peak{};
    for (unsigned i = 0; i < 100; ++i) {
        check(service.load(fixtures::workloads).ok() && echo(service, i), "repeat-load-call");
        peak = service.snapshot().runtime.peak_bytes;
        service.unload();
        const auto unloaded = runtime.snapshot();
        if (i == 0) used_unloaded = unloaded.used_bytes;
        check(unloaded.used_bytes == used_unloaded, "pool-non-growing");
        const auto heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
        if (heap < heap_min) heap_min = heap;
        if (heap > heap_max) heap_max = heap;
        check(heap == heap_after_warmup, "sdk-heap-non-growing");
        vTaskDelay(1);
    }
    service.stop();
    const auto heap_after_stop = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    for (unsigned i = 0; i < 20; ++i) {
        check(service.start().ok() && service.load(fixtures::workloads).ok() && echo(service, i), "repeat-start");
        service.stop();
        check(heap_caps_get_free_size(MALLOC_CAP_8BIT) == heap_after_stop, "sdk-restart-non-growing");
    }
    std::printf("SERVICE {\"type\":\"complete\",\"checks\":%lu,\"failures\":%lu,\"load_unload_cycles\":100,\"start_stop_cycles\":20,\"heap_after_warmup\":%lu,\"heap_min\":%lu,\"heap_max\":%lu,\"heap_after_stop\":%lu,\"unloaded_pool_used\":%lu,\"pool_peak\":%lu,\"stack_headroom_bytes\":%lu,\"maximum_trap_us\":%lld,\"cancel_us\":%lld,\"cancel_requests\":%u}\n",
                static_cast<unsigned long>(checks), static_cast<unsigned long>(failures),
                static_cast<unsigned long>(heap_after_warmup), static_cast<unsigned long>(heap_min), static_cast<unsigned long>(heap_max),
                static_cast<unsigned long>(heap_after_stop), static_cast<unsigned long>(used_unloaded), static_cast<unsigned long>(peak),
                static_cast<unsigned long>(uxTaskGetStackHighWaterMark(nullptr)), static_cast<long long>(maximum_trap_us),
                static_cast<long long>(cancel_us), cancel_requests.load());
    done.store(true);
    return nullptr;
}
}
extern "C" void app_main() {
    vTaskDelay(pdMS_TO_TICKS(1500));
    vTaskPrioritySet(nullptr, 5);
    auto config = esp_pthread_get_default_config();
    config.stack_size = worker_stack_bytes;
    config.prio = 3;
    config.thread_name = "blip-wasm-test";
    if (esp_pthread_set_cfg(&config) != ESP_OK) { std::printf("SERVICE {\"type\":\"fatal\",\"stage\":\"pthread-config\"}\n"); return; }
    pthread_t thread{};
    if (pthread_create(&thread, nullptr, worker, nullptr)) { std::printf("SERVICE {\"type\":\"fatal\",\"stage\":\"pthread-create\"}\n"); return; }
    // Independent supervisor remains responsive during a hostile guest loop.
    const auto began = esp_timer_get_time();
    while (!done.load()) {
        const auto call = call_started.load();
        const auto now = esp_timer_get_time();
        if ((call && now - call > 100000) || now - began > 30000000) {
            std::printf("SERVICE {\"type\":\"supervisor-failure\",\"elapsed_us\":%lld}\n", static_cast<long long>(call ? now - call : now - began));
            esp_restart();
        }
        if (call && cancel_phase.load() && now - call > 5000) {
            runtime.request_cancel();
            cancel_requests.fetch_add(1);
        }
        vTaskDelay(1);
    }
    pthread_join(thread, nullptr);
}
