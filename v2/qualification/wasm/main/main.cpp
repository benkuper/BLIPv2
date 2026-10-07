#include "bench_runtime.hpp"
#include "workloads.hpp"
#include "esp_attr.h"
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
#include <iterator>
#include <pthread.h>

namespace {
RTC_NOINIT_ATTR std::uint32_t reset_after_spin;
constexpr std::uint32_t reset_marker = 0x42574153;
std::atomic<bool> done{};
std::atomic<std::int64_t> spin_started{};
std::atomic<std::uint32_t> supervisor_ticks{};
std::uint32_t failures{};
constexpr std::uint32_t native_stack_bytes = 32768;

#if BLIP_BENCH_WAMR || BLIP_BENCH_WASM3
void fault(const char* name) {
    char error[128]{};
    std::uint32_t result{};
    const auto start = esp_timer_get_time();
    const bool trapped = !bench::call(name, 0, result, error, sizeof(error));
    const auto duration = esp_timer_get_time() - start;
    char recovery_error[128]{};
    const bool recovered = bench::call("noop", 73, result, recovery_error, sizeof(recovery_error)) && result == 73;
    failures += !trapped || !recovered;
    // Runtime error strings in these fixtures have no quotes; bounded output.
    std::printf("BENCH {\"type\":\"fault\",\"name\":\"%s\",\"trapped\":%s,\"recovered\":%s,\"elapsed_us\":%lld,\"error\":\"%s\"}\n", name, trapped ? "true" : "false", recovered ? "true" : "false", static_cast<long long>(duration), error);
}
#endif

void measure(const char* name, std::uint32_t argument, std::uint32_t expected, unsigned batch = 1) {
    std::int64_t durations[21]{};
    char error[128]{};
    std::uint32_t result{};
    bool valid = bench::call(name, argument, result, error, sizeof(error)) && result == expected;
    for (auto& duration : durations) {
        const auto start = esp_timer_get_time();
        for (unsigned i = 0; i < batch; ++i)
            valid &= bench::call(name, argument, result, error, sizeof(error)) && result == expected;
        duration = esp_timer_get_time() - start;
        vTaskDelay(1);
    }
    std::sort(std::begin(durations), std::end(durations));
    failures += !valid;
    std::printf("BENCH {\"type\":\"timing\",\"name\":\"%s\",\"argument\":%lu,\"batch\":%u,\"samples\":21,\"min_us\":%lld,\"median_us\":%lld,\"max_us\":%lld,\"result\":%lu,\"valid\":%s}\n", name, static_cast<unsigned long>(argument), batch, static_cast<long long>(durations[0]), static_cast<long long>(durations[10]), static_cast<long long>(durations[20]), static_cast<unsigned long>(result), valid ? "true" : "false");
}

void* worker(void*) {
    std::printf("BENCH {\"type\":\"start\",\"engine\":\"%s\",\"chip\":\"%s\",\"idf\":\"%s\",\"cpu_mhz\":%d,\"native_stack_bytes\":%lu,\"wasm_stack_bytes\":%lu,\"fixture_sha256\":\"%s\"}\n", BLIP_BENCH_ENGINE, CONFIG_IDF_TARGET, esp_get_idf_version(), CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ, static_cast<unsigned long>(native_stack_bytes), static_cast<unsigned long>(bench::wasm_stack_bytes), workload_sha256);
    char error[128]{};
    const auto before = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    const auto start = esp_timer_get_time();
    if (!bench::load(workload_bytes, sizeof(workload_bytes), error, sizeof(error))) {
        std::printf("BENCH {\"type\":\"load_error\",\"error\":\"%s\"}\n", error);
        failures++;
        done.store(true);
        return nullptr;
    }
    std::printf("BENCH {\"type\":\"load\",\"elapsed_us\":%lld,\"heap_before\":%lu,\"heap_after\":%lu,\"reserved_pool_bytes\":%lu,\"pool_used_bytes\":%lu,\"pool_peak_bytes\":%lu}\n", static_cast<long long>(esp_timer_get_time() - start), static_cast<unsigned long>(before), static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_8BIT)), static_cast<unsigned long>(bench::reserved_bytes()), static_cast<unsigned long>(bench::used_bytes()), static_cast<unsigned long>(bench::peak_bytes()));
    const bool zero_initialized = bench::initial_memory_is_zero();
    failures += !zero_initialized;
    std::printf("BENCH {\"type\":\"memory_initialization\",\"zero_initialized\":%s}\n", zero_initialized ? "true" : "false");
    measure("noop", 7, 7, 100);
    measure("integer", 4096, bench::integer_work(4096));
    float float_expected = 32640.0F;
    std::uint32_t float_bits{};
    std::memcpy(&float_bits, &float_expected, sizeof(float_bits));
    measure("float", 2048, float_bits);
    measure("pixels", 256, bench::pixel_work(256));
    measure("host_calls", 512, 512 * 515 / 2);
    std::uint32_t growth{};
    const bool grow_rejected = bench::call("grow", 0, growth, error, sizeof(error)) && growth == 0xffffffffU;
    failures += !grow_rejected;
    std::printf("BENCH {\"type\":\"memory_growth\",\"rejected\":%s}\n", grow_rejected ? "true" : "false");
#if BLIP_BENCH_WAMR || BLIP_BENCH_WASM3
    for (const auto* name : {"trap", "invalid", "divide", "recursion"}) fault(name);
#endif
    std::printf("BENCH {\"type\":\"memory\",\"heap_free\":%lu,\"heap_minimum\":%lu,\"pool_used_bytes\":%lu,\"pool_peak_bytes\":%lu,\"stack_headroom_bytes\":%lu,\"probe_calls\":%lu,\"failures\":%lu}\n", static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_8BIT)), static_cast<unsigned long>(heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT)), static_cast<unsigned long>(bench::used_bytes()), static_cast<unsigned long>(bench::peak_bytes()), static_cast<unsigned long>(uxTaskGetStackHighWaterMark(nullptr)), static_cast<unsigned long>(bench::probe_calls()), static_cast<unsigned long>(failures));
#if BLIP_BENCH_WAMR || BLIP_BENCH_WASM3
    spin_started.store(esp_timer_get_time());
    std::uint32_t result{};
    const bool bounded = !bench::call("spin", 0, result, error, sizeof(error), 10000);
    const auto elapsed = esp_timer_get_time() - spin_started.load();
    spin_started.store(0);
    const bool recovered = bench::call("noop", 91, result, error, sizeof(error)) && result == 91;
    failures += !bounded || !recovered;
    std::printf("BENCH {\"type\":\"infinite_loop\",\"bounded\":%s,\"recovered\":%s,\"instruction_limit\":10000,\"elapsed_us\":%lld,\"supervisor_ticks\":%lu,\"reset_required\":false}\n", bounded ? "true" : "false", recovered ? "true" : "false", static_cast<long long>(elapsed), static_cast<unsigned long>(supervisor_ticks.load()));
#endif
    bench::close();
    std::printf("BENCH {\"type\":\"cleanup\",\"heap_free\":%lu,\"heap_before\":%lu,\"failures\":%lu}\n", static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_8BIT)), static_cast<unsigned long>(before), static_cast<unsigned long>(failures));
    done.store(true);
    return nullptr;
}
}

extern "C" void app_main() {
    // Allow the host to open the console after esptool releases reset.
    vTaskDelay(pdMS_TO_TICKS(1500));
    if (reset_after_spin == reset_marker && esp_reset_reason() == ESP_RST_SW) {
        reset_after_spin = 0;
        std::printf("BENCH {\"type\":\"done\",\"reset_recovery\":true}\n");
        return;
    }
    reset_after_spin = 0;
    auto config = esp_pthread_get_default_config();
    config.stack_size = native_stack_bytes;
    config.prio = 3;
    config.thread_name = "wasm-bench";
    if (esp_pthread_set_cfg(&config) != ESP_OK) return;
    pthread_t thread{};
    if (pthread_create(&thread, nullptr, worker, nullptr) != 0) return;
    // main priority 1 would be starved by a script on a single-core C6.
    vTaskPrioritySet(nullptr, 5);
    while (!done.load()) {
        ++supervisor_ticks;
        const auto spin = spin_started.load();
        if (spin && esp_timer_get_time() - spin > 100000) {
            std::printf("BENCH {\"type\":\"infinite_loop\",\"bounded\":false,\"recovered\":false,\"elapsed_us\":%lld,\"supervisor_ticks\":%lu,\"reset_required\":true}\n", static_cast<long long>(esp_timer_get_time() - spin), static_cast<unsigned long>(supervisor_ticks.load()));
            reset_after_spin = reset_marker;
            std::fflush(stdout);
            esp_restart();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    pthread_join(thread, nullptr);
    std::printf("BENCH {\"type\":\"done\",\"failures\":%lu,\"reset_recovery\":false}\n", static_cast<unsigned long>(failures));
}
