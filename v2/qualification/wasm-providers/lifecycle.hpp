#pragma once
// Included only by the opt-in production-owner lifecycle qualification image.
#include "blip/wasm/capability.hpp"
#include "blip/wasm/esp_wasm_component.hpp"
#include "blip/led/esp_rmt_strip_component.hpp"
#include "blip/fleet/esp_fleet_component.hpp"
#include "esp_pthread.h"
#include "esp_system.h"
#include <cstdio>
#include <pthread.h>

namespace blip::qualification {
class NoMemory final : public wasm::GuestMemory {
    core::Status read_memory(std::uint32_t, std::span<std::byte>) noexcept override {
        return rejected();
    }
    core::Status write_memory(std::uint32_t, std::span<const std::byte>) noexcept override {
        return rejected();
    }
    static core::Status rejected() noexcept {
        return core::Status::failure({core::ErrorDomain::control, core::ErrorCode::invalid_argument,
            "qualification.providers", "memory", "no-memory"});
    }
};
struct ProviderLifecycle {
    wasm::EspWasmComponent& worker;
    led::EspRmtStripComponent& led;
    fleet::EspFleetComponent& fleet;
    const core::RegistryView& registry;
    unsigned checks{}, failures{};
    bool completed{};

    bool check(bool passed, const char* name) noexcept {
        ++checks;
        if (!passed) {
            ++failures;
            std::printf("PROVIDERS {\"type\":\"failure\",\"check\":\"%s\"}\n", name);
        }
        return passed;
    }
    void run() noexcept {
        std::printf("PROVIDERS {\"type\":\"start\",\"chip\":\"%s\",\"idf\":\"%s\"}\n",
            CONFIG_IDF_TARGET, esp_get_idf_version());
        if (!check(!worker.release_reservation(), "live-reservation-cannot-be-retired")) return;
        // Lifecycle operations join the actual script worker before another
        // exclusive qualification worker makes callbacks or retires owners.
        if (!check(worker.suspend().ok() && worker.callbacks_quiesced(), "consumer-quiesced-before-owners")) return;
        core::ScalarValue reserved;
        if (!check(worker.read_parameter("buffer_reserved", reserved).ok() && reserved.integer == 98304,
                   "fixed-reservation-retained-after-join")) return;
        wasm::CapabilityRegistry catalog;
        if (!check(catalog.bind(registry, true).ok() && catalog.size() == 9, "production-catalog-bound")) return;
        NoMemory memory;
        const auto call = [&](std::string_view module, std::string_view name, bool available) {
            const auto index = catalog.resolve(module, name);
            if (!check(index.ok(), "resolve-owner-query")) return false;
            std::array<wasm::Value, 1> result{wasm::Value::i32(0xabcdef01)};
            std::size_t count = 99;
            const auto status = catalog.invoke(index.value(), memory, {}, result, count);
            return check(available ? status.ok() && count == 1 :
                !status && status.error().code == core::ErrorCode::resource_unavailable && count == 0 &&
                result[0].bits == 0xabcdef01, available ? "live-owner-query" : "retired-owner-refused");
        };
        fleet.enable_control();
        for (unsigned cycle = 0; cycle < 4; ++cycle) {
            if (!check(led.available() && fleet.available(), "owners-live-after-worker-joined")) return;
            if (!call("blip.output.strip0.v1", "frames", true) || !call("blip.fleet.v1", "ready", true)) return;
            if (!check(led.suspend().ok() && led.callbacks_quiesced() && !led.available(), "render-owner-suspended")) return;
            if (!call("blip.output.strip0.v1", "frames", false)) return;
            if (!check(fleet.suspend().ok() && fleet.callbacks_quiesced() && !fleet.available(), "radio-owner-suspended")) return;
            if (!call("blip.fleet.v1", "ready", false)) return;
            if (!check(led.start(core::StartContext{}).ok(), "render-owner-restarted")) return;
            if (!check(fleet.start(core::StartContext{}).ok(), "radio-owner-restarted")) return;
            fleet.enable_control();
        }
        if (!call("blip.output.strip0.v1", "pending", true) || !call("blip.fleet.v1", "ready", true)) return;
        if (!check(worker.start(core::StartContext{}).ok() && !worker.callbacks_quiesced(), "consumer-restarted-after-owners")) return;
        completed = true;
        std::printf("PROVIDERS {\"type\":\"complete\",\"checks\":%u,\"failures\":%u,\"owner_cycles\":4,\"worker_headroom\":%u,\"automatic_resume_supported\":false}\n",
            checks, failures, static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    }
};
inline bool qualify_provider_lifecycle(wasm::EspWasmComponent& worker, led::EspRmtStripComponent& led,
    fleet::EspFleetComponent& fleet, const core::RegistryView& registry) noexcept {
    ProviderLifecycle trial{worker, led, fleet, registry};
    auto config = esp_pthread_get_default_config();
    esp_pthread_cfg_t previous{};
    if (esp_pthread_get_cfg(&previous) != ESP_OK) previous = config;
    config.stack_size = 8192; config.prio = 2; config.thread_name = "blip_provider_hil";
    config.pin_to_core = -1; config.inherit_cfg = false;
    pthread_t thread{};
    const bool created = esp_pthread_set_cfg(&config) == ESP_OK &&
        pthread_create(&thread, nullptr, [](void* data) -> void* {
            static_cast<ProviderLifecycle*>(data)->run(); return nullptr;
        }, &trial) == 0;
    static_cast<void>(esp_pthread_set_cfg(&previous));
    if (!created) return false;
    pthread_join(thread, nullptr);
    return trial.completed && trial.failures == 0;
}
} // namespace blip::qualification
