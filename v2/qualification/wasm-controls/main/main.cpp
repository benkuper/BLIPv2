#include "blip/wasm/script_manifest.hpp"
#include "esp_heap_caps.h"
#include "esp_pthread.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <pthread.h>

int blip_manifest_run_tests();
namespace {
constexpr unsigned kStackBytes = 20480;
blip::wasm::ScriptManifest schema;
// A valid empty Wasm module with an owned boolean control declaration.
constexpr std::array fixture{
    std::byte{0}, std::byte{0x61}, std::byte{0x73}, std::byte{0x6d},
    std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0},
    std::byte{0}, std::byte{40}, std::byte{16},
    std::byte{'b'}, std::byte{'l'}, std::byte{'i'}, std::byte{'p'}, std::byte{'.'},
    std::byte{'c'}, std::byte{'o'}, std::byte{'n'}, std::byte{'t'}, std::byte{'r'}, std::byte{'o'}, std::byte{'l'}, std::byte{'s'},
    std::byte{'.'}, std::byte{'v'}, std::byte{'1'},
    std::byte{'B'}, std::byte{'C'}, std::byte{'M'}, std::byte{'1'}, std::byte{1},
    std::byte{0}, std::byte{5}, std::byte{'a'}, std::byte{'r'}, std::byte{'m'}, std::byte{'e'}, std::byte{'d'},
    std::byte{5}, std::byte{'A'}, std::byte{'r'}, std::byte{'m'}, std::byte{'e'}, std::byte{'d'},
    std::byte{0}, std::byte{2}, std::byte{0}, std::byte{1}, std::byte{0}};
void* run(void*) {
    vTaskDelay(pdMS_TO_TICKS(2000));
    std::printf("CONTROLS {\"type\":\"start\",\"chip\":\"%s\",\"idf\":\"%s\",\"stack_bytes\":%u,\"schema_bytes\":%u}\n",
        CONFIG_IDF_TARGET, esp_get_idf_version(), kStackBytes, static_cast<unsigned>(sizeof(schema)));
    if (blip_manifest_run_tests() != 0) {
        std::printf("CONTROLS {\"type\":\"fatal\",\"detail\":\"shared-cases\"}\n"); return nullptr;
    }
    const auto baseline = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    auto minimum = baseline, maximum = baseline;
    std::uint64_t maximum_us{};
    for (unsigned i = 0; i < 1000; ++i) {
        const auto began = esp_timer_get_time();
        const auto status = blip::wasm::parse_script_manifest(fixture, schema);
        maximum_us = std::max(maximum_us, static_cast<std::uint64_t>(esp_timer_get_time() - began));
        if (!status || schema.size() != 1 || !schema.default_value(0).boolean) {
            std::printf("CONTROLS {\"type\":\"fatal\",\"detail\":\"repeated-parser\"}\n"); return nullptr;
        }
        const auto heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        minimum = std::min(minimum, heap); maximum = std::max(maximum, heap);
    }
    std::printf("CONTROLS {\"type\":\"complete\",\"cases\":12,\"parse_cycles\":1000,\"maximum_parse_us\":%llu,"
        "\"heap_baseline\":%u,\"heap_min\":%u,\"heap_max\":%u,\"stack_headroom\":%u,\"failures\":0}\n",
        static_cast<unsigned long long>(maximum_us), static_cast<unsigned>(baseline), static_cast<unsigned>(minimum),
        static_cast<unsigned>(maximum), static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    return nullptr;
}
} // namespace
extern "C" void app_main() {
    auto configuration = esp_pthread_get_default_config();
    configuration.stack_size = kStackBytes; configuration.prio = 2; configuration.thread_name = "manifest_tests";
    if (esp_pthread_set_cfg(&configuration) != ESP_OK) {
        std::printf("CONTROLS {\"type\":\"fatal\",\"detail\":\"thread-configuration\"}\n"); return;
    }
    pthread_t thread;
    if (pthread_create(&thread, nullptr, run, nullptr) != 0) {
        std::printf("CONTROLS {\"type\":\"fatal\",\"detail\":\"thread-create\"}\n"); return;
    }
    pthread_join(thread, nullptr);
}
