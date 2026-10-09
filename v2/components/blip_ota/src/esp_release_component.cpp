#include "blip/ota/esp_release_component.hpp"
#include "blip/ota/esp_release_http.hpp"
#include "blip/ota/release_image.hpp"
#include "blip/ota/sha256.hpp"
#include "esp_heap_caps.h"
#include "sdkconfig.h"
#include "esp_netif_sntp.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include <algorithm>
#include <array>
#include <ctime>
#include <new>
#include <memory>

namespace blip::ota {
namespace {
constexpr std::array<std::string_view, 1> provided{"firmware.release-check"};
constexpr std::array<std::string_view, 4> required{"transport.wifi", "storage.files.internal", "firmware.ota", "storage.settings"};
constexpr std::array<core::MetadataEntry, 1> metadata{{{"ui_topic", "Updates"}}};
constexpr core::ParameterDescriptor text(std::string_view id, std::string_view label) {
    return {id, label, core::ValueType::string, core::Access::read_only, false, core::ScalarValue::from_string(""), {}, ""};
}
constexpr core::ParameterDescriptor boolean(std::string_view id, std::string_view label) {
    return {id, label, core::ValueType::boolean, core::Access::read_only, false, core::ScalarValue::from_bool(false), {}, ""};
}
constexpr std::array<core::ParameterDescriptor, 15> parameters{{
    {"worker_stack_headroom", "Update worker stack headroom", core::ValueType::integer,
     core::Access::read_only, false, core::ScalarValue::from_integer(0), {}, "bytes"},
    text("state", "Release check state"), text("last_error", "Release check error"),
    {"endpoint", "Release endpoint", core::ValueType::string, core::Access::read_write, true, core::ScalarValue::from_string(kDefaultReleaseEndpoint), {}, ""},
    {"channel", "Release channel", core::ValueType::string, core::Access::read_write, true, core::ScalarValue::from_string("stable"), {}, ""},
    text("firmware_candidate", "Published firmware version"), text("web_candidate", "Published interface version"),
    boolean("firmware_available", "Firmware update available"), boolean("web_available", "Interface update available"),
    {"http_status", "Release HTTP status", core::ValueType::integer, core::Access::read_only, false, core::ScalarValue::from_integer(0), {}, ""},
    {"received_bytes", "Downloaded bytes", core::ValueType::integer, core::Access::read_only, false, core::ScalarValue::from_integer(0), {}, "bytes"},
    {"expected_bytes", "Download size", core::ValueType::integer, core::Access::read_only, false, core::ScalarValue::from_integer(0), {}, "bytes"},
    {"interval_hours", "Automatic check interval (0 disables)", core::ValueType::integer, core::Access::read_write, true, core::ScalarValue::from_integer(24), {true, 0, 168, 1}, "hours"},
    {"automatic_firmware", "Install firmware automatically", core::ValueType::boolean, core::Access::read_write, true, core::ScalarValue::from_bool(false), {}, ""},
    {"automatic_web", "Install interface automatically", core::ValueType::boolean, core::Access::read_write, true, core::ScalarValue::from_bool(false), {}, ""},
}};
constexpr std::array<core::ActionDescriptor, 4> actions{{{"check", "Check for updates", {}},
    {"install_firmware", "Install firmware update", {}}, {"install_web", "Install interface update", {}},
    {"cancel", "Cancel update", {}}}};
core::ComponentDescriptor make_descriptor() {
    core::ComponentDescriptor output{};
    output.schema_version = 1; output.id = "blip.updates"; output.display_name = "Device updates";
    output.description = "Public device-specific release checks";
    output.provided_services = provided; output.required_services = required;
    output.metadata = metadata; output.parameters = parameters; output.actions = actions;
    output.settings = {1, 1}; output.disable_policy = core::DisablePolicy::reboot_required;
    output.cost = {262144, 2048, 6144};
    return output;
}
core::Status failure(core::ErrorCode code, std::string_view detail) {
    return core::Status::failure({core::ErrorDomain::transport, code, "blip.updates", "check", detail});
}
}
EspReleaseComponent::EspReleaseComponent(ReleaseIdentity identity, network::EspWifiComponent& wifi,
    storage::WebAssetStore& assets, storage::SettingsStore& settings, UpdateService& updates) noexcept
    : descriptor_(make_descriptor()), identity_(identity), wifi_(&wifi), assets_(&assets), settings_(&settings), updates_(&updates) {
    timer_ = xTimerCreateStatic("blip_updates", pdMS_TO_TICKS(30000), pdTRUE, this, timer_entry, &timer_storage_);
    barrier_ = xSemaphoreCreateBinaryStatic(&barrier_storage_);
}
const core::ComponentDescriptor& EspReleaseComponent::descriptor() const noexcept { return descriptor_; }
core::Status EspReleaseComponent::start(const core::StartContext&) noexcept {
    std::lock_guard guard(mutex_);
    std::array<std::byte, kReleasePolicyBytes> bytes{};
    const auto loaded = settings_->load(descriptor_, bytes);
    if (loaded) {
        if (!decode_release_policy(std::span<const std::byte>(bytes).first(loaded.value().payload_size), policy_))
            return failure(core::ErrorCode::corrupt_data, "release-settings");
    } else if (loaded.error().code != core::ErrorCode::not_found) return core::Status::failure(loaded.error());
    if (!timer_ || !barrier_) return failure(core::ErrorCode::resource_unavailable, "timer-resources");
    started_ = true; next_check_us_ = esp_timer_get_time() + 60000000; timer_quiesced_.store(false);
    if (xTimerStart(timer_, 0) != pdPASS) {
        started_ = false; timer_quiesced_.store(true); return failure(core::ErrorCode::resource_unavailable, "timer-queue");
    }
    return core::Status::success();
}
core::Status EspReleaseComponent::stop() noexcept {
    { std::lock_guard guard(mutex_); started_ = false; cancelled_.store(true); }
    if (!timer_quiesced_.load()) {
        static_cast<void>(xSemaphoreTake(barrier_, 0));
        if (xTimerStop(timer_, pdMS_TO_TICKS(100)) != pdPASS ||
            xTimerPendFunctionCall(timer_barrier, this, 0, pdMS_TO_TICKS(100)) != pdPASS ||
            xSemaphoreTake(barrier_, pdMS_TO_TICKS(1000)) != pdTRUE)
            return failure(core::ErrorCode::stop_failed, "timer-not-quiesced");
        timer_quiesced_.store(true);
    }
    for (unsigned index = 0; index < 1500 && busy_.load(); ++index) vTaskDelay(pdMS_TO_TICKS(10));
    if (busy_.load()) return failure(core::ErrorCode::stop_failed, "worker-active");
    return core::Status::success();
}
core::Status EspReleaseComponent::read_parameter_owned(std::string_view id, core::ScalarValue& output, std::span<char> storage) noexcept {
    std::lock_guard guard(mutex_);
    if (!started_) return failure(core::ErrorCode::invalid_state, "not-started");
    if (id == "firmware_available") output = core::ScalarValue::from_bool(firmware_available_);
    else if (id == "web_available") output = core::ScalarValue::from_bool(web_available_);
    else if (id == "http_status") output = core::ScalarValue::from_integer(http_status_);
    else if (id == "received_bytes") output = core::ScalarValue::from_integer(received_);
    else if (id == "expected_bytes") output = core::ScalarValue::from_integer(expected_);
    else if (id == "worker_stack_headroom") output = core::ScalarValue::from_integer(worker_headroom_.load());
    else if (id == "interval_hours") output = core::ScalarValue::from_integer(policy_.interval_hours);
    else if (id == "automatic_firmware") output = core::ScalarValue::from_bool(policy_.automatic_firmware);
    else if (id == "automatic_web") output = core::ScalarValue::from_bool(policy_.automatic_web);
    else {
        std::string_view value;
        if (id == "state") value = state_;
        else if (id == "last_error") value = error_;
        else if (id == "endpoint") value = policy_.endpoint.view();
        else if (id == "channel") value = policy_.channel.view();
        else if (id == "firmware_candidate") value = catalog_.firmware.version.view();
        else if (id == "web_candidate") value = catalog_.web.version.view();
        else return failure(core::ErrorCode::not_found, "unknown-parameter");
        if (value.size() > storage.size()) return failure(core::ErrorCode::capacity_exceeded, "response-storage");
        std::copy(value.begin(), value.end(), storage.begin());
        output = core::ScalarValue::from_string({storage.data(), value.size()});
    }
    return core::Status::success();
}
core::Status EspReleaseComponent::write_parameter(std::string_view id, const core::ScalarValue& value) noexcept {
    std::lock_guard guard(mutex_);
    if (!started_ || busy_.load()) return failure(core::ErrorCode::invalid_state, "worker-active-or-stopped");
    auto candidate = policy_;
    bool assigned = false;
    if (value.type == core::ValueType::string) {
        if (id == "endpoint") assigned = candidate.endpoint.assign(value.string);
        else if (id == "channel") assigned = candidate.channel.assign(value.string);
    }
    if (id == "interval_hours" && value.type == core::ValueType::integer && value.integer >= 0 && value.integer <= 168) {
        candidate.interval_hours = static_cast<std::uint32_t>(value.integer); assigned = true;
    }
    if (value.type == core::ValueType::boolean) {
        if (id == "automatic_firmware") { candidate.automatic_firmware = value.boolean; assigned = true; }
        else if (id == "automatic_web") { candidate.automatic_web = value.boolean; assigned = true; }
    }
    std::array<std::byte, kReleasePolicyBytes> bytes{};
    if (!assigned || !encode_release_policy(candidate, bytes)) return failure(core::ErrorCode::invalid_argument, "invalid-release-settings");
    const auto saved = settings_->save(descriptor_, bytes);
    if (!saved) return saved;
    policy_ = candidate; catalog_ = {}; firmware_available_ = web_available_ = false;
    state_ = "not-checked"; error_ = ""; http_status_ = 0;
    return core::Status::success();
}
core::Status EspReleaseComponent::invoke_action(std::string_view id, std::span<const core::ScalarValue> arguments,
                                                std::span<core::ScalarValue>, std::size_t& count) noexcept {
    count = 0;
    if (!arguments.empty()) return failure(core::ErrorCode::invalid_argument, "action-arguments");
    if (id == "cancel") { cancelled_.store(true); return core::Status::success(); }
    if (id == "check") return request(ReleaseOperation::check);
    if (id == "install_firmware") return request(ReleaseOperation::firmware);
    if (id == "install_web") return request(ReleaseOperation::web);
    return failure(core::ErrorCode::not_found, "unknown-action");
}
ReleaseProgress EspReleaseComponent::progress() noexcept {
    std::lock_guard guard(mutex_);
    return {busy_.load(), firmware_available_, web_available_, state_, error_, http_status_,
        received_, expected_, identity_.firmware_code, assets_->info().bundle_version,
        catalog_.firmware.version, catalog_.web.version, identity_.firmware_version};
}
core::Status EspReleaseComponent::request(ReleaseOperation operation) noexcept {
    std::lock_guard guard(mutex_);
    return request_locked(operation);
}
core::Status EspReleaseComponent::begin_manual_transfer() noexcept {
    std::lock_guard guard(mutex_);
    if (!started_ || busy_.load()) return failure(core::ErrorCode::resource_unavailable, "update-worker-active");
    ++manual_transfers_;
    return core::Status::success();
}
void EspReleaseComponent::end_manual_transfer() noexcept {
    std::lock_guard guard(mutex_);
    if (manual_transfers_) --manual_transfers_;
}
core::Status EspReleaseComponent::request_locked(ReleaseOperation operation) noexcept {
    if (!started_) return failure(core::ErrorCode::invalid_state, "not-started");
    if (busy_.load()) return failure(core::ErrorCode::resource_unavailable, "check-active");
    if (manual_transfers_) return failure(core::ErrorCode::resource_unavailable, "file-transfer-active");
    auto identity = identity_; identity.web_code = assets_->info().bundle_version;
    if ((operation == ReleaseOperation::firmware && !firmware_update_available(catalog_, identity)) ||
        (operation == ReleaseOperation::web && !web_update_available(catalog_, identity)))
        return failure(core::ErrorCode::invalid_state, "no-eligible-update");
    busy_.store(true); cancelled_.store(false); operation_ = operation; received_ = expected_ = 0;
    if (operation == ReleaseOperation::check || operation == ReleaseOperation::first_run) {
        catalog_ = {}; firmware_available_ = web_available_ = false;
    }
    state_ = operation == ReleaseOperation::firmware ? "downloading-firmware" :
             operation == ReleaseOperation::web ? "downloading-web" : "checking";
    error_ = ""; http_status_ = 0;
    // Final bundle validation reaches the filesystem's stat/open/CRC stack.
    // A 4 KiB worker passes HTTP checks but overflows on actual external media.
    if (xTaskCreate(task_entry, "blip_release", 6144, this, 2, nullptr) != pdPASS) {
        busy_.store(false); state_ = "error"; error_ = "worker-memory";
        return failure(core::ErrorCode::resource_unavailable, "worker-memory");
    }
    return core::Status::success();
}
void EspReleaseComponent::timer_entry(TimerHandle_t timer) noexcept {
    auto* self = static_cast<EspReleaseComponent*>(pvTimerGetTimerID(timer));
    // The shared timer task never waits for settings I/O or another callback.
    if (!self->mutex_.try_lock()) return;
    std::lock_guard guard(self->mutex_, std::adopt_lock);
    if (!self->started_ || self->busy_.load() || esp_timer_get_time() < self->next_check_us_ ||
        self->wifi_->connection_state() != network::WifiConnectionState::connected) return;
    std::unique_lock asset_guard(self->assets_->mutex(), std::try_to_lock);
    if (!asset_guard.owns_lock()) return;
    const bool first_run = !self->assets_->active();
    if (!first_run && !self->policy_.interval_hours) return;
    self->next_check_us_ = esp_timer_get_time() + (first_run ? 300000000LL :
        static_cast<std::int64_t>(self->policy_.interval_hours) * 3600000000LL);
    if (!self->request_locked(first_run ? ReleaseOperation::first_run : ReleaseOperation::check))
        self->next_check_us_ = esp_timer_get_time() + 60000000;
}
void EspReleaseComponent::timer_barrier(void* context, std::uint32_t) noexcept {
    xSemaphoreGive(static_cast<EspReleaseComponent*>(context)->barrier_);
}
void EspReleaseComponent::result(const char* state, const char* error, int http_status) noexcept {
    std::lock_guard guard(mutex_); state_ = state; error_ = error; http_status_ = http_status;
}
void EspReleaseComponent::task_entry(void* context) noexcept {
    auto* self = static_cast<EspReleaseComponent*>(context);
    self->check();
    self->worker_headroom_.store(uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t));
    self->busy_.store(false); vTaskDelete(nullptr);
}
void EspReleaseComponent::check() noexcept {
    if (wifi_->connection_state() != network::WifiConnectionState::connected) { result("error", "network-unavailable"); return; }
#if defined(CONFIG_ESP_HTTP_CLIENT_ENABLE_HTTPS)
    if (policy_.endpoint.view().starts_with("https://") &&
        (heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < 49152 ||
         heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < 17408)) {
        result("deferred", "insufficient-connection-memory"); return;
    }
    bool sntp_owned = false;
    if (policy_.endpoint.view().starts_with("https://") && std::time(nullptr) < 1704067200) {
        const esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        if (esp_netif_sntp_init(&config) != ESP_OK) { result("error", "clock-service"); return; }
        sntp_owned = true;
        const auto deadline = esp_timer_get_time() + 15000000;
        while (!cancelled_.load() && std::time(nullptr) < 1704067200 && esp_timer_get_time() < deadline)
            static_cast<void>(esp_netif_sntp_sync_wait(pdMS_TO_TICKS(200)));
    }
    if (sntp_owned) esp_netif_sntp_deinit();
    if (cancelled_.load()) { result("cancelled", ""); return; }
    if (policy_.endpoint.view().starts_with("https://") && std::time(nullptr) < 1704067200) {
        result("error", "clock-sync-timeout"); return;
    }
#endif
    if (operation_ == ReleaseOperation::firmware || operation_ == ReleaseOperation::web) {
        install(operation_ == ReleaseOperation::firmware);
    } else {
        fetch_catalog();
        if (operation_ == ReleaseOperation::first_run && !assets_->active() && web_available_ && !cancelled_.load()) install(false);
        else if (!cancelled_.load()) {
            if (policy_.automatic_web && web_available_) install(false);
            if (policy_.automatic_firmware && firmware_available_ && !cancelled_.load()) install(true);
        }
    }
}
void EspReleaseComponent::fetch_catalog() noexcept {
    std::unique_ptr<char[]> url(new (std::nothrow) char[1024]);
    if (!url) { result("error", "query-memory"); return; }
    auto identity = identity_; identity.web_code = assets_->info().bundle_version;
    identity.channel = policy_.channel.view(); // Configuration writes reject while this worker owns it.
    std::size_t written{};
    auto status = build_release_query(policy_.endpoint.view(), identity, {url.get(), 1024}, written);
    EspReleaseHttp connection;
    if (status) status = connection.open(url.get(), kMaximumReleaseCatalogBytes);
    url.reset(); // HTTP client owns its parsed URL.
    if (!status) { result("error", status.error().detail.data(), connection.http_status()); return; }
    const auto capacity = static_cast<std::size_t>(connection.body_length() > 0
        ? connection.body_length() : kMaximumReleaseCatalogBytes) + 1;
    std::unique_ptr<char[]> json(new (std::nothrow) char[capacity]);
    if (!json) { result("error", "catalog-memory"); return; }
    std::size_t total{};
    while (status) {
        const auto read = connection.read(std::as_writable_bytes(std::span(json.get(), capacity)).subspan(total), cancelled_);
        if (!read) { status = core::Status::failure(read.error()); break; }
        if (!read.value()) break;
        total += read.value();
    }
    connection.close();
    const auto* partition = esp_ota_get_next_update_partition(nullptr);
    if (status) {
        std::lock_guard guard(mutex_);
        status = decode_release_catalog({json.get(), total}, catalog_);
        if (status) status = validate_release_catalog(catalog_, identity, partition ? partition->size : 0,
                                                       storage::kMaxWebAssetBundleBytes);
        if (!status) catalog_ = {};
    }
    if (!status) { result(cancelled_.load() ? "cancelled" : "error", cancelled_.load() ? "" : "invalid-catalog-or-transfer", connection.http_status()); return; }
    {
        std::lock_guard guard(mutex_);
        http_status_ = connection.http_status();
        firmware_available_ = firmware_update_available(catalog_, identity);
        web_available_ = web_update_available(catalog_, identity);
        state_ = !catalog_.firmware.present && !catalog_.web.present ? "unpublished" :
                 firmware_available_ || web_available_ ? "updates-available" : "current";
        error_ = "";
    }
}
void EspReleaseComponent::install(bool firmware) noexcept {
    const auto& artifact = firmware ? catalog_.firmware : catalog_.web;
    struct DownloadScratch {
        // Identity prefix is consumed before streaming; reuse its storage.
        std::array<std::byte, 512> buffer{};
        ArtifactSha256 hash{};
    };
    static_assert(kReleaseImagePrefixBytes <= 512);
    auto* scratch = new (std::nothrow) DownloadScratch;
    if (!scratch) { result("error", "download-memory"); return; }
    const auto identity = [&] { auto value = identity_; value.web_code = assets_->info().bundle_version; return value; }();
    EspReleaseHttp connection;
    auto status = connection.open(artifact.url.bytes.data(), artifact.bytes, artifact.bytes);
    const auto prefix_size = firmware ? kReleaseImagePrefixBytes : storage::kWebAssetBundleHeaderBytes;
    std::size_t prefix_received{};
    while (status && prefix_received < prefix_size) {
        const auto read = connection.read(std::span<std::byte>(scratch->buffer).subspan(prefix_received, prefix_size - prefix_received), cancelled_);
        if (!read) { status = core::Status::failure(read.error()); break; }
        if (!read.value()) { status = failure(core::ErrorCode::verification_failed, "truncated-prefix"); break; }
        prefix_received += read.value();
    }
    if (status && firmware) status = validate_firmware_release_prefix(std::span<const std::byte>(scratch->buffer).first(prefix_size), identity, artifact);
    if (status && !firmware) {
        const auto word = [&](std::size_t offset) {
            std::uint32_t value{};
            for (unsigned byte = 0; byte < 4; ++byte)
                value |= std::to_integer<std::uint32_t>(scratch->buffer[offset + byte]) << (8 * byte);
            return value;
        };
        if (word(0) != 0x42574c42U || word(8) != artifact.code || word(24) != artifact.bytes)
            status = failure(core::ErrorCode::verification_failed, "web-prefix-identity");
    }
    if (status && cancelled_.load()) status = failure(core::ErrorCode::cancelled, "cancelled-before-install");
    bool owns_transaction = false;
    if (status) {
        if (firmware) status = updates_->begin({artifact.bytes, artifact.sha256, identity.project,
            artifact.version.view(), identity.target, identity.profile});
        else status = assets_->begin_install(artifact.bytes);
        owns_transaction = status.ok();
    }
    const auto append = [&](std::span<const std::byte> bytes) {
        auto written = core::Status::success();
        if (firmware) {
            std::lock_guard guard(updates_->mutex());
            written = updates_->append(bytes);
            if (!written) { updates_->cancel(); owns_transaction = false; }
        } else {
            std::lock_guard guard(assets_->mutex());
            written = assets_->append_install(bytes);
            if (!written) { assets_->cancel_install(); owns_transaction = false; }
        }
        if (written) {
            scratch->hash.update(bytes);
            std::lock_guard guard(mutex_); received_ += static_cast<std::uint32_t>(bytes.size());
        }
        return written;
    };
    if (status) {
        scratch->hash.reset();
        { std::lock_guard guard(mutex_); expected_ = artifact.bytes; received_ = 0;
          state_ = firmware ? "downloading-firmware" : "downloading-web"; http_status_ = connection.http_status(); }
        status = append(std::span<const std::byte>(scratch->buffer).first(prefix_size));
    }
    while (status) {
        const auto read = connection.read(scratch->buffer, cancelled_);
        if (!read) { status = core::Status::failure(read.error()); break; }
        if (!read.value()) break;
        status = append(std::span<const std::byte>(scratch->buffer).first(read.value()));
        taskYIELD();
    }
    connection.close();
    if (status && (cancelled_.load() || scratch->hash.finish() != artifact.sha256))
        status = failure(core::ErrorCode::verification_failed, "cancelled-or-sha256");
    if (status) {
        // finish() owns validation, atomic activation and cleanup under the
        // transaction lock. Never cancel somebody else's subsequent transfer.
        if (firmware) status = updates_->finish();
        else {
            const auto installed = assets_->finish_install();
            if (!installed) status = core::Status::failure(installed.error());
        }
        owns_transaction = false;
    }
    if (owns_transaction) {
        if (firmware) updates_->cancel();
        else assets_->cancel_install();
    }
    delete scratch;
    if (!status) {
        result(cancelled_.load() ? "cancelled" : "error", cancelled_.load() ? "" : "artifact-transfer-or-validation", connection.http_status());
        return;
    }
    if (firmware) {
        result("restarting", "", connection.http_status());
        vTaskDelay(pdMS_TO_TICKS(250)); esp_restart();
    } else {
        std::lock_guard guard(mutex_);
        auto installed_identity = identity_; installed_identity.web_code = assets_->info().bundle_version;
        web_available_ = web_update_available(catalog_, installed_identity);
        firmware_available_ = firmware_update_available(catalog_, installed_identity);
        state_ = "web-installed"; error_ = ""; http_status_ = connection.http_status();
    }
}
} // namespace blip::ota
