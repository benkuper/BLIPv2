#include "blip/core/control.hpp"
#include "blip/oscquery/legacy_osc.hpp"
#include "esp_heap_caps.h"
#include "esp_pthread.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <algorithm>
#include <cstdio>
#include <pthread.h>

int blip_dynamic_controls_run_tests();
namespace {
constexpr unsigned kStackBytes = 20480, kCases = 6, kRepeatCycles = 20;
static_assert(sizeof(blip::core::ControlResponse) <= 1024);
static_assert(sizeof(blip::oscquery::LegacyOscEndpoint) <= 1024);
void* run(void*) {
    vTaskDelay(pdMS_TO_TICKS(2000));
    std::printf("DYNSCHEMA {\"type\":\"start\",\"chip\":\"%s\",\"idf\":\"%s\",\"stack_bytes\":%u,"
        "\"response_bytes\":%u,\"osc_endpoint_bytes\":%u}\n",
        CONFIG_IDF_TARGET, esp_get_idf_version(), kStackBytes,
        static_cast<unsigned>(sizeof(blip::core::ControlResponse)),
        static_cast<unsigned>(sizeof(blip::oscquery::LegacyOscEndpoint)));
    if (blip_dynamic_controls_run_tests() != 0) {
        std::printf("DYNSCHEMA {\"type\":\"fatal\",\"detail\":\"shared-cases\"}\n"); return nullptr;
    }
    const auto baseline = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    auto minimum = baseline, maximum = baseline;
    std::uint64_t maximum_us{};
    for (unsigned i = 0; i < kRepeatCycles; ++i) {
        const auto began = esp_timer_get_time();
        if (blip_dynamic_controls_run_tests() != 0) {
            std::printf("DYNSCHEMA {\"type\":\"fatal\",\"detail\":\"repeated-suite\"}\n"); return nullptr;
        }
        maximum_us = std::max(maximum_us, static_cast<std::uint64_t>(esp_timer_get_time() - began));
        const auto heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        minimum = std::min(minimum, heap); maximum = std::max(maximum, heap);
    }
    std::printf("DYNSCHEMA {\"type\":\"complete\",\"cases\":%u,\"repeat_cycles\":%u,\"case_passes\":%u,"
        "\"maximum_suite_us\":%llu,\"heap_baseline\":%u,\"heap_min\":%u,\"heap_max\":%u,"
        "\"stack_headroom\":%u,\"failures\":0}\n",
        kCases, kRepeatCycles, kCases * (kRepeatCycles + 1), static_cast<unsigned long long>(maximum_us),
        static_cast<unsigned>(baseline), static_cast<unsigned>(minimum), static_cast<unsigned>(maximum),
        static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    return nullptr;
}
} // namespace
extern "C" void app_main() {
    auto configuration = esp_pthread_get_default_config();
    configuration.stack_size = kStackBytes; configuration.prio = 2; configuration.thread_name = "schema_tests";
    if (esp_pthread_set_cfg(&configuration) != ESP_OK) {
        std::printf("DYNSCHEMA {\"type\":\"fatal\",\"detail\":\"thread-configuration\"}\n"); return;
    }
    pthread_t thread;
    if (pthread_create(&thread, nullptr, run, nullptr) != 0) {
        std::printf("DYNSCHEMA {\"type\":\"fatal\",\"detail\":\"thread-create\"}\n"); return;
    }
    pthread_join(thread, nullptr);
}
