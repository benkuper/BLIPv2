#pragma once
// Opt-in production-owner test. No qualification controls enter normal images.
#include "blip/ota/esp_release_component.hpp"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include <cstdio>

namespace blip::qualification {
inline bool qualify_release_worker(ota::EspReleaseComponent& worker) noexcept {
    vTaskDelay(pdMS_TO_TICKS(2000)); // Allow USB console enumeration and capture.
    unsigned checks{};
    const auto check = [&](bool passed, const char* name) {
        ++checks;
        if (!passed) std::printf("RELEASE_WORKER {\"type\":\"failure\",\"check\":\"%s\"}\n", name);
        return passed;
    };
    const auto tasks_before = uxTaskGetNumberOfTasks();
    const auto heap_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    std::printf("RELEASE_WORKER {\"type\":\"start\",\"chip\":\"%s\",\"idf\":\"%s\",\"stack_bytes\":%u}\n",
        CONFIG_IDF_TARGET, esp_get_idf_version(), static_cast<unsigned>(ota::EspReleaseComponent::kWorkerStackBytes));
    for (unsigned cycle = 0; cycle < 30; ++cycle) {
        if (!check(worker.stop().ok() && worker.callbacks_quiesced() && !worker.progress().busy,
                   "stopped-worker-quiesced")) return false;
        if (!check(worker.stop().ok(), "stop-idempotent")) return false;
        if (!check(!worker.request(ota::ReleaseOperation::check), "stopped-worker-refuses-request")) return false;
        if (!check(worker.start(core::StartContext{}).ok() && !worker.callbacks_quiesced(),
                   "static-worker-restarted")) return false;
        const auto tasks = uxTaskGetNumberOfTasks();
        if (!check(worker.start(core::StartContext{}).ok() && uxTaskGetNumberOfTasks() == tasks,
                   "start-idempotent")) return false;
        if (!check(worker.begin_manual_transfer().ok(), "manual-transfer-admitted")) return false;
        if (!check(!worker.request(ota::ReleaseOperation::check), "manual-transfer-excludes-download")) return false;
        worker.end_manual_transfer();
        if (cycle % 10 == 9) {
            if (!check(worker.request(ota::ReleaseOperation::check).ok(), "queued-request-admitted")) return false;
            if (!check(worker.stop().ok() && worker.callbacks_quiesced() && !worker.progress().busy &&
                       std::string_view(worker.progress().state) != "checking",
                       "queued-worker-cancelled-and-joined")) return false;
            if (!check(worker.start(core::StartContext{}).ok(), "restart-after-cancellation")) return false;
        }
        std::printf("RELEASE_WORKER {\"type\":\"cycle\",\"cycle\":%u}\n", cycle + 1);
    }
    core::ScalarValue headroom;
    if (!check(worker.read_parameter_owned("worker_stack_headroom", headroom, {}).ok(), "worker-headroom-readable")) return false;
    std::printf("RELEASE_WORKER {\"type\":\"complete\",\"checks\":%u,\"cycles\":30,\"queued_cancellations\":3,"
        "\"tasks_before\":%u,\"tasks_after\":%u,\"heap_before\":%u,\"heap_after\":%u,\"worker_headroom\":%u,\"main_headroom\":%u}\n",
        checks, static_cast<unsigned>(tasks_before), static_cast<unsigned>(uxTaskGetNumberOfTasks()),
        static_cast<unsigned>(heap_before), static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
        static_cast<unsigned>(headroom.integer), static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    return true;
}
} // namespace blip::qualification
