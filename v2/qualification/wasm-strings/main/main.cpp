#include "blip/wasm/esp_wasm_component.hpp"
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
#include <cstring>
#include <pthread.h>

namespace {
using namespace blip::wasm;
using blip::core::ErrorCode;
std::byte *pool{}, *module{}, *linear{};
WamrRuntime runtime;
std::atomic<bool> done{};
unsigned checks{}, failures{};
std::int64_t maximum_read_us{}, maximum_write_us{};
void check(bool valid, const char* name) {
    ++checks;
    if (!valid) { ++failures; std::printf("STRINGS {\"type\":\"failure\",\"check\":\"%s\"}\n", name); }
}
StringRef span(Service& service, const char* name) {
    std::array<Value, 2> result{};
    std::size_t count{};
    const auto status = service.call(name, {}, {}, result, count);
    check(status.ok() && count == 2 && result[0].type == ValueType::i32 && result[1].type == ValueType::i32, "guest-pointer-length-result");
    return {static_cast<std::uint32_t>(result[0].bits), static_cast<std::uint32_t>(result[1].bits)};
}
void read(Service& service, StringRef ref, std::string_view expected, const char* name) {
    std::array<char, kMaximumStringBytes + 1> output{}; output.fill('!');
    std::size_t count = 999;
    const auto began = esp_timer_get_time();
    const auto status = service.read_utf8(ref, service.snapshot().generation, output, count);
    maximum_read_us = std::max(maximum_read_us, esp_timer_get_time() - began);
    check(status.ok() && count == expected.size() && std::string_view(output.data(), count) == expected, name);
    check(output[count <= kMaximumStringBytes ? count : 0] == '!', "no-terminator-or-overwrite");
}
void rejected(Service& service, StringRef ref, ErrorCode code, const char* name) {
    std::array<char, kMaximumStringBytes + 1> output{}; output.fill('!');
    std::size_t count = 999;
    const auto status = service.read_utf8(ref, service.snapshot().generation, output, count);
    check(!status && status.error().code == code && count == 0, name);
    check(std::all_of(output.begin(), output.end(), [](char value) { return value == '!'; }), "failed-read-preserves-output");
    check(service.snapshot().state == State::loaded, "failed-read-does-not-fault-guest");
}
void write(Service& service, std::uint32_t offset, std::string_view text) {
    const auto began = esp_timer_get_time();
    check(service.write_utf8(offset, service.snapshot().generation, text).ok(), "write-valid-utf8");
    maximum_write_us = std::max(maximum_write_us, esp_timer_get_time() - began);
    read(service, {offset, static_cast<std::uint32_t>(text.size())}, text, "write-read-roundtrip");
}
void* worker(void*) {
    Service service(runtime, {pool, EspWasmComponent::kEnginePoolBytes}, {module, EspWasmComponent::kModuleBytes},
                    {linear, EspWasmComponent::kLinearBytes});
    check(service.start().ok(), "start");
    check(service.load(fixtures::strings).ok(), "load-string-fixture");
    if (service.snapshot().state != State::loaded) { service.stop(); done.store(true); return nullptr; }
    read(service, span(service, "ascii"), "No terminator", "unterminated-ascii");
    constexpr char unicode[] = "\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80";
    read(service, span(service, "unicode"), std::string_view(unicode, 9), "multibyte-utf8");
    constexpr std::array nul{'A','\0','Z'};
    read(service, span(service, "nul"), {nul.data(), nul.size()}, "embedded-nul");
    read(service, span(service, "end"), "Z", "last-byte-unterminated");
    read(service, {65536,0}, {}, "empty-at-end");
    read(service, {0,0}, {}, "empty-at-zero");
    std::array<char, kMaximumStringBytes> long_text{}; long_text.fill('L');
    read(service, span(service, "long"), {long_text.data(),long_text.size()}, "maximum-length");
    for (const char* name : {"overlong","surrogate","too_high","truncated","continuation","invalid_lead"})
        rejected(service, span(service, name), ErrorCode::invalid_argument, name);
    rejected(service, span(service, "wrapped"), ErrorCode::invalid_argument, "wrapped-pointer");
    for (const StringRef ref : {StringRef{65535,2}, {65537,0}, {UINT32_MAX,0}, {UINT32_MAX,1}})
        rejected(service, ref, ErrorCode::invalid_argument, "read-bounds");
    rejected(service, {0,257}, ErrorCode::capacity_exceeded, "long-read-limit");
    rejected(service, {1,UINT32_MAX}, ErrorCode::capacity_exceeded, "wrapped-length");
    std::array<char, 2> short_output{'!','!'};
    std::size_t short_count = 999;
    check(!service.read_utf8({0,3}, service.snapshot().generation, short_output, short_count) && short_count == 0 &&
          short_output[0] == '!' && short_output[1] == '!', "short-destination-atomicity");
    write(service, 513, std::string_view(unicode, 9)); // Unaligned ESP32 IRAM copy.
    write(service, 65535, "X");
    const auto generation = service.snapshot().generation;
    check(!service.write_utf8(65535, generation, "BAD"), "cross-boundary-write-rejected");
    read(service, {65535,1}, "X", "cross-boundary-write-does-not-mutate");
    check(service.write_utf8(65536, generation, {}).ok(), "empty-write-at-end");
    for (const auto offset : std::array<std::uint32_t, 3>{65536, 65537, UINT32_MAX})
        check(!service.write_utf8(offset, generation, "X"), "write-bounds");
    check(!service.write_utf8(65537, generation, {}), "empty-write-beyond-end");
    constexpr std::array malformed{'\xe0','\x80','\x80'};
    check(!service.write_utf8(0, generation, {malformed.data(),malformed.size()}), "malformed-write-rejected");
    read(service, {0,13}, "No terminator", "malformed-write-does-not-mutate");
    std::array<char, kMaximumStringBytes + 1> too_long{}; too_long.fill('Q');
    check(!service.write_utf8(0, generation, {too_long.data(),too_long.size()}), "long-write-rejected");
    read(service, {0,13}, "No terminator", "long-write-does-not-mutate");
    std::array<Value, 1> result{};
    std::size_t count{};
    const std::array peek{Value::i32(65535)};
    check(service.call("peek", peek, {}, result, count).ok() && count == 1 && result[0].bits == 'X', "guest-observes-host-write");
    check(service.call("grow", {}, {}, result, count).ok() && count == 1 && result[0].bits == UINT32_MAX, "memory-growth-cap");
    check(service.load(fixtures::strings).ok(), "reload");
    std::array<char, 32> output{}; output.fill('!');
    count = 999;
    check(!service.read_utf8({0,13}, generation, output, count) && count == 0, "stale-generation-read");
    check(!service.write_utf8(0, generation, "wrong"), "stale-generation-write");
    read(service, span(service, "ascii"), "No terminator", "reload-data-restored");
    read(service, span(service, "end"), "Z", "reload-last-byte-restored");
    service.unload();
    check(!runtime.read_memory(0, {}), "unloaded-runtime-read-denied");
    check(!runtime.write_memory(0, {}), "unloaded-runtime-write-denied");
    check(service.load(fixtures::no_memory).ok(), "load-without-memory");
    rejected(service, {0,0}, ErrorCode::not_found, "absent-memory-empty-denied");
    check(!service.write_utf8(0, service.snapshot().generation, {}), "absent-memory-write-denied");
    check(service.load(fixtures::growing).ok(), "load-zero-memory");
    read(service, {0,0}, {}, "zero-memory-empty");
    rejected(service, {1,0}, ErrorCode::invalid_argument, "zero-memory-offset");
    rejected(service, {0,1}, ErrorCode::invalid_argument, "zero-memory-nonempty");
    check(!service.write_utf8(0, service.snapshot().generation, "X"), "zero-memory-write-denied");
    check(service.call("grow", {}, {}, result, count).ok() && count == 1 && result[0].bits == 0, "zero-to-one-growth");
    write(service, 0, "X");
    check(service.call("peek", {}, {}, result, count).ok() && count == 1 && result[0].bits == 'X', "grown-memory-guest-observes-write");
    service.unload();
    const auto baseline = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    auto minimum = baseline, maximum = baseline;
    std::uint32_t pool_peak{}, unloaded_pool{};
    for (unsigned cycle = 0; cycle < 100; ++cycle) {
        check(service.load(fixtures::strings).ok(), "repeat-load");
        read(service, span(service, "unicode"), std::string_view(unicode, 9), "repeat-guest-string");
        write(service, 513, std::string_view(unicode, 9));
        pool_peak = service.snapshot().runtime.peak_bytes;
        service.unload();
        const auto used = runtime.snapshot().used_bytes;
        if (cycle == 0) unloaded_pool = used;
        check(used == unloaded_pool, "repeat-unloaded-pool-stable");
        const auto heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        minimum = std::min(minimum, heap); maximum = std::max(maximum, heap);
        check(heap == baseline, "repeat-sdk-heap-stable");
        vTaskDelay(1);
    }
    service.stop();
    check(!service.read_utf8({0,0}, service.snapshot().generation, {}, count) && count == 0, "stopped-service-read-denied");
    check(heap_caps_check_integrity_all(true), "heap-integrity");
    std::printf("STRINGS {\"type\":\"complete\",\"checks\":%u,\"failures\":%u,\"reload_cycles\":100,\"heap_baseline\":%u,\"heap_min\":%u,\"heap_max\":%u,\"pool_peak\":%u,\"unloaded_pool_used\":%u,\"worker_headroom\":%u,\"maximum_read_us\":%lld,\"maximum_write_us\":%lld}\n",
        checks, failures, static_cast<unsigned>(baseline), static_cast<unsigned>(minimum), static_cast<unsigned>(maximum),
        static_cast<unsigned>(pool_peak), static_cast<unsigned>(unloaded_pool), static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)),
        maximum_read_us, maximum_write_us);
    done.store(true);
    return nullptr;
}
}
extern "C" void app_main() {
    vTaskDelay(pdMS_TO_TICKS(1500));
    std::printf("STRINGS {\"type\":\"start\",\"chip\":\"%s\",\"idf\":\"%s\",\"abi_version\":%u,\"maximum_string_bytes\":256,\"engine_pool_bytes\":%u,\"linear_bytes\":%u,\"worker_stack_bytes\":8192,\"fixture_sha256\":\"%s\"}\n",
        CONFIG_IDF_TARGET, esp_get_idf_version(), static_cast<unsigned>(kStringAbiVersion), static_cast<unsigned>(EspWasmComponent::kEnginePoolBytes),
        static_cast<unsigned>(EspWasmComponent::kLinearBytes), fixtures::strings_sha256);
    pool = static_cast<std::byte*>(heap_caps_aligned_alloc(8, EspWasmComponent::kEnginePoolBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    module = static_cast<std::byte*>(heap_caps_malloc(EspWasmComponent::kModuleBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
#if defined(CONFIG_IDF_TARGET_ESP32)
    linear = static_cast<std::byte*>(heap_caps_malloc(EspWasmComponent::kLinearBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_IRAM_8BIT));
#endif
    if (!pool || !module || (EspWasmComponent::kLinearBytes && !linear)) {
        std::printf("STRINGS {\"type\":\"fatal\",\"check\":\"startup-buffers\"}\n");
        heap_caps_free(pool); heap_caps_free(module); heap_caps_free(linear); return;
    }
    auto config = esp_pthread_get_default_config();
    config.stack_size = 8192; config.prio = 2; config.thread_name = "blip_strings"; config.pin_to_core = -1; config.inherit_cfg = false;
    pthread_t thread{};
    if (esp_pthread_set_cfg(&config) != ESP_OK || pthread_create(&thread, nullptr, worker, nullptr) != 0) {
        std::printf("STRINGS {\"type\":\"fatal\",\"check\":\"worker-create\"}\n");
        heap_caps_free(pool); heap_caps_free(module); heap_caps_free(linear); return;
    }
    const auto until = esp_timer_get_time() + 20000000;
    while (!done.load() && esp_timer_get_time() < until) vTaskDelay(pdMS_TO_TICKS(10));
    if (!done.load()) { std::printf("STRINGS {\"type\":\"fatal\",\"check\":\"worker-timeout\"}\n"); esp_restart(); }
    pthread_join(thread, nullptr);
    heap_caps_free(pool); heap_caps_free(module); heap_caps_free(linear);
}
