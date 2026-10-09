#pragma once

#include "blip/core/component.hpp"
#include "blip/core/control.hpp"
#include "blip/fleet/fleet.hpp"
#include "blip/network/esp_wifi_component.hpp"
#include "blip/storage/settings_store.hpp"
#include "blip/transport/espnow_frames.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_now.h"
#if defined(BLIP_FLEET_WASM)
#include "blip/wasm/capability.hpp"
#endif

#include <array>
#include <atomic>

namespace blip::fleet {

class EspFleetComponent final : public core::Component
#if defined(BLIP_FLEET_WASM)
    , public wasm::CapabilityProvider
#endif
{
  public:
    static constexpr std::size_t kNetworkStackBytes = 6144, kExecutorStackBytes = 6144;
    EspFleetComponent(core::ControlService& controls, storage::SettingsStore& settings,
                      network::EspWifiComponent& wifi) noexcept
        : controls_(&controls), settings_(&settings), wifi_(&wifi) {
#if defined(BLIP_FLEET_WASM)
        script_admission_ = xSemaphoreCreateMutexStatic(&script_admission_storage_);
#endif
    }
    [[nodiscard]] const core::ComponentDescriptor& descriptor() const noexcept override;
    [[nodiscard]] core::Status start(const core::StartContext&) noexcept override;
    [[nodiscard]] core::Status stop() noexcept override;
    [[nodiscard]] core::Status suspend() noexcept override { return stop(); }
    [[nodiscard]] bool callbacks_quiesced() const noexcept override;
    [[nodiscard]] core::Status read_parameter(std::string_view, core::ScalarValue&) noexcept override;
    [[nodiscard]] core::Status write_parameter(std::string_view, const core::ScalarValue&) noexcept override;
    [[nodiscard]] core::Status invoke_action(std::string_view, std::span<const core::ScalarValue>,
        std::span<core::ScalarValue>, std::size_t&) noexcept override;
    [[nodiscard]] core::Result<std::uint32_t> schedule(const core::ControlRequest&,
                                                      std::uint32_t delay_ms) noexcept;
    void enable_control() noexcept { control_ready_.store(true); }
#if defined(BLIP_FLEET_WASM)
    wasm::CapabilityProvider* wasm_provider() noexcept override { return this; }
    bool available() const noexcept override { return started_.load() && control_ready_.load(); }
    core::Status invoke(std::string_view, wasm::CallContext&, std::span<const wasm::Value>,
        std::span<wasm::Value>, std::size_t&) noexcept override;
#endif

  private:
    struct Received {
        std::uint64_t source{}, timestamp_us{};
        std::uint16_t size{};
        std::array<std::byte, transport::kEspNowPacketBytes> bytes{};
    };
    struct Execution { Cue cue{}; std::uint32_t epoch{}; };
    static void network_entry(void*) noexcept;
    static void executor_entry(void*) noexcept;
    void run_network() noexcept;
    void run_executor() noexcept;
    void send(Message&) noexcept;
    static void receive_callback(void*, const esp_now_recv_info_t*, const std::uint8_t*, int) noexcept;
    void update_epoch() noexcept;
    [[nodiscard]] core::Status load_settings() noexcept;
    [[nodiscard]] core::Status save_settings(bool, std::uint32_t, std::uint8_t) noexcept;
    static const core::ComponentDescriptor descriptor_;

    core::ControlService* controls_;
    storage::SettingsStore* settings_;
    network::EspWifiComponent* wifi_;
    Engine engine_{};
    transport::EspNowReassemble reassembly_{};
    std::uint64_t reassembly_source_{};
    SemaphoreHandle_t engine_mutex_{};
    SemaphoreHandle_t settings_mutex_{};
    QueueHandle_t execution_queue_{};
    QueueHandle_t receive_queue_{};
    TaskHandle_t network_task_{};
    TaskHandle_t executor_task_{};
    std::uint32_t transmit_sequence_{};
    std::uint32_t transmit_session_{};
    std::uint64_t node_{};
    std::uint64_t epoch_leader_{};
    std::uint32_t epoch_session_{};
    bool epoch_sync_{};
    std::atomic<std::uint32_t> epoch_{};
    std::atomic<bool> started_{};
    std::atomic<bool> network_quiesced_{true};
    std::atomic<bool> executor_quiesced_{true};
    std::atomic<bool> control_ready_{};
    std::atomic<bool> enabled_{};
    std::atomic<std::uint32_t> fleet_id_{1U};
    std::atomic<bool> reconfigure_{};
    std::atomic<bool> active_{};
    std::atomic<std::uint8_t> channel_{1U};
    std::atomic<std::uint32_t> executed_{};
    std::atomic<std::uint32_t> execution_failed_{};
    std::atomic<std::uint32_t> execution_dropped_{};
    std::atomic<std::uint32_t> invalid_packets_{};
    std::atomic<std::uint32_t> send_failed_{};
    std::atomic<std::uint64_t> last_execution_us_{};
    std::atomic<std::uint32_t> maximum_lateness_us_{};
#if defined(BLIP_FLEET_WASM)
    StaticSemaphore_t script_admission_storage_{};
    SemaphoreHandle_t script_admission_{}; // Permanent gate protects admission vs resource teardown.
#endif
};

} // namespace blip::fleet
