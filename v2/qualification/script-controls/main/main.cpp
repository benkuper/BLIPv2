#include "blip/wasm/script_controls.hpp"
#include "blip/wasm/service.hpp"
#include "blip/wasm/wamr_runtime.hpp"
#include "fixtures.hpp"
#include "esp_heap_caps.h"
#include "esp_pthread.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <pthread.h>

int blip_script_controls_run_tests();
namespace {
using namespace blip::wasm;
namespace core = blip::core;
constexpr unsigned kStackBytes = 24576, kCases = 8, kRepeatCycles = 20, kEngineCycles = 20;
alignas(8) std::array<std::byte, 98304> pool{};
alignas(8) std::array<std::byte, 16384> module{};
WamrRuntime runtime;
ScriptControlStore store;
ScriptControlMessage message;
std::atomic<bool> done{};
unsigned checks{}, failures{};
void check(bool passed, const char* name) {
    ++checks;
    if (!passed) { ++failures; std::printf("SCRIPTSTORE {\"type\":\"failure\",\"check\":\"%s\"}\n", name); }
}
bool engine_cycle(Service& service, unsigned ticket) {
    store.retire();
    if (!store.prepare(fixtures::valid, {}) || !service.load(fixtures::valid) ||
        !store.publish(runtime, service.snapshot().generation)) return false;
    const auto schema = store.generation();
    std::array<char, 128> strings{}; core::ScalarValue out;
    if (!store.write_id(schema, "note", core::ScalarValue::from_string("changed")) ||
        !store.read_id(schema, "note", out, strings) || out.string != "changed") return false;
    const std::array fields{core::ScalarValue::from_bool(true), core::ScalarValue::from_integer(-9223372036854775807LL),
        core::ScalarValue::from_number(1.25), core::ScalarValue::from_string(std::string_view("a\0b", 3))};
    if (!store.enqueue_action(schema, "fire", fields, ticket, 7) || !store.take_action(message) ||
        message.ticket != ticket || message.module_generation != service.snapshot().generation) return false;
    // Model worker-owned conversion using the checked guest-memory API. This
    // qualifier does not instantiate the production EspWasmComponent.
    const auto address = store.action_buffer() + 3 * kScriptValueStringBytes;
    if (!service.write_utf8(address, message.module_generation, message.values[3].scalar().string)) return false;
    const std::array arguments{Value::i32(static_cast<std::uint32_t>(message.values[0].bits)), Value::i64(message.values[1].bits),
        Value{ValueType::f64, message.values[2].bits}, Value::i32(address), Value::i32(message.values[3].bytes)};
    std::array<Value, 1> results{}; std::size_t count{};
    if (!service.call(message.name.data(), arguments, {}, results, count) || count) return false;
    std::array<std::byte, 32> observed{};
    if (!runtime.read_memory(0, observed)) return false;
    const auto little = [&observed](unsigned offset, unsigned bytes) {
        std::uint64_t bits{}; for (unsigned i = 0; i < bytes; ++i) bits |= std::to_integer<std::uint64_t>(observed[offset + i]) << (8 * i);
        return bits;
    };
    if (little(0, 4) != 1 || little(8, 8) != message.values[1].bits || little(16, 8) != message.values[2].bits ||
        little(24, 4) != address || little(28, 4) != 3) return false;
    std::array<char, 3> copied{};
    if (!service.read_utf8({address, 3}, message.module_generation, copied, count) || count != 3 ||
        std::string_view(copied.data(), copied.size()) != std::string_view("a\0b", 3)) return false;
    store.retire(); service.unload(); return store.quiescent();
}
void* run(void*) {
    vTaskDelay(pdMS_TO_TICKS(2000));
    std::printf("SCRIPTSTORE {\"type\":\"start\",\"chip\":\"%s\",\"idf\":\"%s\",\"stack_bytes\":%u,\"store_bytes\":%u,"
        "\"message_bytes\":%u,\"value_bytes\":%u,\"fixture_sha256\":\"%s\"}\n", CONFIG_IDF_TARGET, esp_get_idf_version(), kStackBytes,
        static_cast<unsigned>(sizeof(ScriptControlStore)), static_cast<unsigned>(sizeof(ScriptControlMessage)),
        static_cast<unsigned>(sizeof(OwnedScriptValue)), fixtures::valid_sha256);
    if (blip_script_controls_run_tests()) { std::printf("SCRIPTSTORE {\"type\":\"fatal\",\"detail\":\"shared-cases\"}\n"); done.store(true); return nullptr; }
    // FreeRTOS's idle task reclaims the joined reader task's stack/TCB.
    // Sample after that normal cleanup, rather than accumulating deleted tasks.
    vTaskDelay(pdMS_TO_TICKS(20));
    const auto baseline = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    auto minimum = baseline, maximum = baseline; std::uint64_t maximum_us{};
    for (unsigned i = 0; i < kRepeatCycles; ++i) {
        const auto began = esp_timer_get_time();
        if (blip_script_controls_run_tests()) { std::printf("SCRIPTSTORE {\"type\":\"fatal\",\"detail\":\"repeated-suite\"}\n"); done.store(true); return nullptr; }
        maximum_us = std::max(maximum_us, static_cast<std::uint64_t>(esp_timer_get_time() - began));
        vTaskDelay(pdMS_TO_TICKS(20));
        const auto heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); minimum = std::min(minimum, heap); maximum = std::max(maximum, heap);
    }
    Service service(runtime, pool, module); check(service.start().ok(), "engine-start");
    check(engine_cycle(service, 1), "real-callback-and-owned-fields");
    const auto engine_baseline = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    auto engine_min = engine_baseline, engine_max = engine_baseline;
    for (unsigned i = 0; i < kEngineCycles; ++i) {
        check(engine_cycle(service, 2 + i), "repeated-real-callback");
        const auto heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); engine_min = std::min(engine_min, heap); engine_max = std::max(engine_max, heap);
    }
    check(store.prepare(fixtures::valid, {}) && service.load(fixtures::valid) && store.publish(runtime, service.snapshot().generation), "global-test-load");
    const auto global = runtime.immutable_i32_global(kScriptActionBufferGlobal);
    check(global && global.value() == 1024, "immutable-i32-global");
    const auto negative = runtime.immutable_i32_global("negative"); check(negative && negative.value() == 0xffffffffU, "global-unsigned-bits");
    check(!runtime.immutable_i32_global("absent"), "missing-global-rejected");
    check(!runtime.immutable_i32_global("mutable"), "mutable-global-rejected");
    check(!runtime.immutable_i32_global("wide"), "wide-global-rejected");
    store.retire(); service.unload();
    for (const auto fixture : {std::span<const std::byte>(fixtures::mutable_buffer), std::span<const std::byte>(fixtures::wide_buffer),
                              std::span<const std::byte>(fixtures::outside_buffer)}) {
        check(store.prepare(fixture, {}) && service.load(fixture) && !store.publish(runtime, service.snapshot().generation) && !store.generation(), "invalid-arena-not-published");
        service.unload();
    }
    const auto peak = runtime.snapshot().peak_bytes; store.retire(); service.stop();
    check(!runtime.immutable_i32_global(kScriptActionBufferGlobal), "unloaded-global-rejected");
    std::printf("SCRIPTSTORE {\"type\":\"complete\",\"cases\":%u,\"repeat_cycles\":%u,\"case_passes\":%u,\"maximum_suite_us\":%llu,"
        "\"heap_baseline\":%u,\"heap_min\":%u,\"heap_max\":%u,\"engine_checks\":%u,\"engine_cycles\":%u,\"engine_heap_baseline\":%u,"
        "\"engine_heap_min\":%u,\"engine_heap_max\":%u,\"pool_peak\":%u,\"stack_headroom\":%u,\"failures\":%u}\n",
        kCases, kRepeatCycles, kCases * (kRepeatCycles + 1), static_cast<unsigned long long>(maximum_us), static_cast<unsigned>(baseline),
        static_cast<unsigned>(minimum), static_cast<unsigned>(maximum), checks, kEngineCycles, static_cast<unsigned>(engine_baseline),
        static_cast<unsigned>(engine_min), static_cast<unsigned>(engine_max), static_cast<unsigned>(peak), static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)), failures);
    done.store(true); return nullptr;
}
} // namespace
extern "C" void app_main() {
    vTaskPrioritySet(nullptr, 5);
    auto configuration = esp_pthread_get_default_config(); configuration.stack_size = kStackBytes;
    configuration.prio = 2; configuration.thread_name = "script_tests";
    if (esp_pthread_set_cfg(&configuration) != ESP_OK) { std::printf("SCRIPTSTORE {\"type\":\"fatal\",\"detail\":\"thread-configuration\"}\n"); return; }
    pthread_t thread;
    if (pthread_create(&thread, nullptr, run, nullptr)) { std::printf("SCRIPTSTORE {\"type\":\"fatal\",\"detail\":\"thread-create\"}\n"); return; }
    const auto began = esp_timer_get_time();
    while (!done.load()) {
        if (esp_timer_get_time() - began > 30000000) { std::printf("SCRIPTSTORE {\"type\":\"fatal\",\"detail\":\"supervisor-timeout\"}\n"); esp_restart(); }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    pthread_join(thread, nullptr);
}
