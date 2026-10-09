#pragma once
#include "blip/core/component.hpp"
#include "blip/network/esp_wifi_component.hpp"
#include "blip/ota/release_catalog.hpp"
#include "blip/ota/release_policy.hpp"
#include "blip/storage/web_asset_store.hpp"
#include "blip/storage/settings_store.hpp"
#include <atomic>
#include <mutex>
#include "freertos/timers.h"
#include "freertos/semphr.h"

namespace blip::ota {
enum class ReleaseOperation : std::uint8_t { check, firmware, web, first_run };
struct ReleaseProgress {
    bool busy{}, firmware_available{}, web_available{};
    const char* state{};
    const char* error{};
    int http_status{};
    std::uint32_t received{}, expected{}, installed_firmware{}, installed_web{};
    ReleaseText<31> firmware_candidate{}, web_candidate{};
    std::string_view firmware_version{};
};
class EspReleaseComponent final : public core::Component {
  public:
    EspReleaseComponent(ReleaseIdentity identity, network::EspWifiComponent& wifi,
                         storage::WebAssetStore& assets, storage::SettingsStore& settings,
                         UpdateService& updates) noexcept;
    [[nodiscard]] core::Status request(ReleaseOperation) noexcept;
    [[nodiscard]] ReleaseProgress progress() noexcept;
    [[nodiscard]] const ReleaseIdentity& identity() const noexcept { return identity_; }
    [[nodiscard]] core::Status begin_manual_transfer() noexcept;
    void end_manual_transfer() noexcept;
    const core::ComponentDescriptor& descriptor() const noexcept override;
    core::Status start(const core::StartContext&) noexcept override;
    core::Status stop() noexcept override;
    bool callbacks_quiesced() const noexcept override { return !busy_.load() && timer_quiesced_.load(); }
    core::Status read_parameter_owned(std::string_view, core::ScalarValue&, std::span<char>) noexcept override;
    core::Status write_parameter(std::string_view, const core::ScalarValue&) noexcept override;
    core::Status invoke_action(std::string_view, std::span<const core::ScalarValue>,
                               std::span<core::ScalarValue>, std::size_t&) noexcept override;
  private:
    static void task_entry(void*) noexcept;
    static void timer_entry(TimerHandle_t) noexcept;
    static void timer_barrier(void*, std::uint32_t) noexcept;
    core::Status request_locked(ReleaseOperation) noexcept;
    void check() noexcept;
    void fetch_catalog() noexcept;
    void install(bool firmware) noexcept;
    void result(const char* state, const char* error, int http_status = 0) noexcept;
    core::ComponentDescriptor descriptor_{};
    const ReleaseIdentity identity_;
    network::EspWifiComponent* wifi_;
    storage::WebAssetStore* assets_;
    storage::SettingsStore* settings_;
    UpdateService* updates_;
    ReleasePolicy policy_{default_release_policy()};
    std::mutex mutex_{};
    bool started_{};
    std::atomic<bool> busy_{}, cancelled_{};
    unsigned manual_transfers_{}; // Component mutex; HTTP callbacks own scoped leases.
    std::atomic<bool> timer_quiesced_{true};
    StaticTimer_t timer_storage_{};
    TimerHandle_t timer_{};
    StaticSemaphore_t barrier_storage_{};
    SemaphoreHandle_t barrier_{};
    std::int64_t next_check_us_{};
    const char* state_{"not-checked"};
    const char* error_{""};
    int http_status_{};
    ReleaseOperation operation_{ReleaseOperation::check};
    std::uint32_t received_{}, expected_{};
    std::atomic<std::uint32_t> worker_headroom_{};
    ReleaseCatalog catalog_{};
    bool firmware_available_{}, web_available_{};
};
} // namespace blip::ota
