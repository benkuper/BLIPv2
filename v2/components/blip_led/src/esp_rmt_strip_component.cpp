#include "blip/led/esp_rmt_strip_component.hpp"

#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "soc/soc_caps.h"

#include <array>
#include <limits>

#ifndef BLIP_LED_DEFAULT_GPIO
#define BLIP_LED_DEFAULT_GPIO 2
#endif

namespace blip::led {
namespace {

constexpr char kTag[] = "blip_rmt_strip";
constexpr std::array<std::string_view, 1> kProvidedServices{"output.pixel-strip"};
constexpr std::array<std::string_view, 1> kRequiredServices{"storage.settings"};
constexpr std::array<std::string_view, 4> kRmtAlternatives{"rmt.tx0", "rmt.tx1", "rmt.tx2",
                                                           "rmt.tx3"};
constexpr std::array<core::ResourceRequest, 1> kResources{{
    {core::ResourceClass::rmt, "waveform", core::OwnershipMode::exclusive, kRmtAlternatives, 0U, 1U,
     0U, 0x02U, 0U, false},
}};
constexpr std::array<core::MetadataEntry, 7> kMetadata{{
    {"backend", "native-rmt"},
    {"qualification", "ADR-0007"},
    {"lane_count", "1"},
    {"protocol_values", "0=ws2812-grb,1=sk6812-grbw"},
    {"gpio_parameter", "pin"},
    {"maximum_pixels", "1024"},
    {"rmt_dma", "disabled-m3-baseline"},
}};
constexpr std::array<core::ParameterDescriptor, 14> kParameters{{
    {"enabled",
     "Strip enabled",
     core::ValueType::boolean,
     core::Access::read_write,
     true,
     core::ScalarValue::from_bool(false),
     {},
     ""},
    {"pin",
     "Data GPIO",
     core::ValueType::integer,
     core::Access::read_write,
     true,
     core::ScalarValue::from_integer(BLIP_LED_DEFAULT_GPIO),
     {true, 0, 63, 1},
     ""},
    {"protocol",
     "Pixel protocol",
     core::ValueType::integer,
     core::Access::read_write,
     true,
     core::ScalarValue::from_integer(0),
     {true, 0, 1, 1},
     ""},
    {"pixels",
     "Pixel count",
     core::ValueType::integer,
     core::Access::read_write,
     true,
     core::ScalarValue::from_integer(1),
     {true, 1, kMaximumStripPixels, 1},
     "pixels"},
    {"brightness",
     "Global brightness",
     core::ValueType::integer,
     core::Access::read_write,
     true,
     core::ScalarValue::from_integer(255),
     {true, 0, 255, 1},
     ""},
    {"red",
     "Red",
     core::ValueType::integer,
     core::Access::read_write,
     true,
     core::ScalarValue::from_integer(0),
     {true, 0, 255, 1},
     ""},
    {"green",
     "Green",
     core::ValueType::integer,
     core::Access::read_write,
     true,
     core::ScalarValue::from_integer(0),
     {true, 0, 255, 1},
     ""},
    {"blue",
     "Blue",
     core::ValueType::integer,
     core::Access::read_write,
     true,
     core::ScalarValue::from_integer(0),
     {true, 0, 255, 1},
     ""},
    {"white",
     "White",
     core::ValueType::integer,
     core::Access::read_write,
     true,
     core::ScalarValue::from_integer(0),
     {true, 0, 255, 1},
     ""},
    {"applied_frames",
     "Applied frames",
     core::ValueType::integer,
     core::Access::read_only,
     false,
     core::ScalarValue::from_integer(0),
     {},
     "frames"},
    {"failed_frames",
     "Failed frames",
     core::ValueType::integer,
     core::Access::read_only,
     false,
     core::ScalarValue::from_integer(0),
     {},
     "frames"},
    {"last_frame_us",
     "Last frame completion",
     core::ValueType::integer,
     core::Access::read_only,
     false,
     core::ScalarValue::from_integer(0),
     {},
     "us"},
    {"worker_stack_headroom",
     "Output worker stack headroom",
     core::ValueType::integer,
     core::Access::read_only,
     false,
     core::ScalarValue::from_integer(0),
     {true, 0, EspRmtStripComponent::kTaskStackBytes, 1},
     "bytes"},
    {"backend",
     "Selected backend",
     core::ValueType::string,
     core::Access::read_only,
     false,
     core::ScalarValue::from_string("native-rmt"),
     {},
     ""},
}};
constexpr std::array<core::ActionDescriptor, 1> kActions{{
    {"blackout", "Set all channels to zero", {}},
}};
constexpr std::array<core::DiagnosticDescriptor, 3> kDiagnostics{{
    {"applied_frames", core::ValueType::integer, "frames"},
    {"failed_frames", core::ValueType::integer, "frames"},
    {"last_frame_us", core::ValueType::integer, "us"},
}};

[[nodiscard]] constexpr core::ComponentDescriptor strip_descriptor() noexcept {
    core::ComponentDescriptor descriptor{};
    descriptor.schema_version = 1U;
    descriptor.id = "blip.output.strip0";
    descriptor.display_name = "Pixel strip 0";
    descriptor.description = "Qualified single-lane WS2812/SK6812 native RMT output";
    descriptor.metadata = kMetadata;
    descriptor.provided_services = kProvidedServices;
    descriptor.required_services = kRequiredServices;
    descriptor.parameters = kParameters;
    descriptor.actions = kActions;
    descriptor.diagnostics = kDiagnostics;
    descriptor.resources = kResources;
    descriptor.settings = {1U, 1U};
    descriptor.disable_policy = core::DisablePolicy::live;
    descriptor.supports_restart = true;
    descriptor.cost = {32768U, 12288U, EspRmtStripComponent::kTaskStackBytes};
    return descriptor;
}

[[nodiscard]] core::Error output_error(core::ErrorCode code, std::string_view operation,
                                       std::string_view detail) noexcept {
    return {core::ErrorDomain::transport, code, "blip.output.strip0", operation, detail};
}

void saturating_increment(std::atomic<std::uint32_t>& value) noexcept {
    std::uint32_t current = value.load();
    while (current != std::numeric_limits<std::uint32_t>::max() &&
           !value.compare_exchange_weak(current, current + 1U)) {
    }
}

} // namespace

const core::ComponentDescriptor EspRmtStripComponent::descriptor_{strip_descriptor()};

EspRmtStripComponent::EspRmtStripComponent(storage::SettingsStore& settings) noexcept
    : settings_(&settings) {}

const core::ComponentDescriptor& EspRmtStripComponent::descriptor() const noexcept {
    return descriptor_;
}

bool EspRmtStripComponent::lock_config() noexcept {
    return config_mutex_ != nullptr && xSemaphoreTake(config_mutex_, portMAX_DELAY) == pdTRUE;
}

void EspRmtStripComponent::unlock_config() noexcept {
    if (config_mutex_ != nullptr) {
        static_cast<void>(xSemaphoreGive(config_mutex_));
    }
}

core::Status EspRmtStripComponent::load_config() noexcept {
    const auto loaded = settings_->load(descriptor_, settings_buffer_);
    if (!loaded) {
        if (loaded.error().code == core::ErrorCode::not_found) {
            config_ = {};
            config_.gpio = BLIP_LED_DEFAULT_GPIO;
            return validate_strip_config(config_);
        }
        return core::Status::failure(loaded.error());
    }
    const auto decoded = decode_strip_config(
        std::span<const std::byte>{settings_buffer_.data(), loaded.value().payload_size});
    if (!decoded) {
        return core::Status::failure(decoded.error());
    }
    config_ = decoded.value();
    return core::Status::success();
}

core::Status EspRmtStripComponent::save_config(const StripConfig& config) noexcept {
    const auto encoded = encode_strip_config(config, settings_buffer_);
    if (!encoded) {
        return core::Status::failure(encoded.error());
    }
    return settings_->save(descriptor_,
                           std::span<const std::byte>{settings_buffer_.data(), encoded.value()});
}

core::Status EspRmtStripComponent::initialize_output(const StripConfig& config) noexcept {
    if (channel_ != nullptr || encoder_ != nullptr) {
        return core::Status::failure(
            output_error(core::ErrorCode::invalid_state, "initialize", "already-initialized"));
    }
    rmt_tx_channel_config_t channel_config{};
    channel_config.gpio_num = static_cast<gpio_num_t>(config.gpio);
    channel_config.clk_src = RMT_CLK_SRC_DEFAULT;
    channel_config.resolution_hz = 10'000'000U;
    channel_config.mem_block_symbols = SOC_RMT_MEM_WORDS_PER_CHANNEL;
    channel_config.trans_queue_depth = 2U;
    channel_config.flags.with_dma = false;
    if (rmt_new_tx_channel(&channel_config, &channel_) != ESP_OK) {
        channel_ = nullptr;
        return core::Status::failure(
            output_error(core::ErrorCode::resource_unavailable, "initialize", "rmt-channel"));
    }
    const auto timing = strip_timing(config.protocol);
    rmt_bytes_encoder_config_t encoder_config{};
    encoder_config.bit0.duration0 = timing.zero_high_ticks;
    encoder_config.bit0.level0 = 1U;
    encoder_config.bit0.duration1 = timing.zero_low_ticks;
    encoder_config.bit0.level1 = 0U;
    encoder_config.bit1.duration0 = timing.one_high_ticks;
    encoder_config.bit1.level0 = 1U;
    encoder_config.bit1.duration1 = timing.one_low_ticks;
    encoder_config.bit1.level1 = 0U;
    encoder_config.flags.msb_first = true;
    if (rmt_new_bytes_encoder(&encoder_config, &encoder_) != ESP_OK ||
        rmt_enable(channel_) != ESP_OK) {
        if (encoder_ != nullptr) {
            static_cast<void>(rmt_del_encoder(encoder_));
            encoder_ = nullptr;
        }
        static_cast<void>(rmt_del_channel(channel_));
        channel_ = nullptr;
        return core::Status::failure(
            output_error(core::ErrorCode::start_failed, "initialize", "rmt-encoder"));
    }
    return core::Status::success();
}

void EspRmtStripComponent::deinitialize_output() noexcept {
    if (channel_ != nullptr) {
        static_cast<void>(rmt_tx_wait_all_done(channel_, 100));
        static_cast<void>(rmt_disable(channel_));
        static_cast<void>(rmt_del_channel(channel_));
        channel_ = nullptr;
    }
    if (encoder_ != nullptr) {
        static_cast<void>(rmt_del_encoder(encoder_));
        encoder_ = nullptr;
    }
}

core::Status EspRmtStripComponent::transmit(const StripConfig& config,
                                            std::size_t payload_size) noexcept {
    if (channel_ == nullptr || encoder_ == nullptr) {
        return core::Status::failure(
            output_error(core::ErrorCode::invalid_state, "transmit", "output-not-initialized"));
    }
    rmt_transmit_config_t transmit_config{};
    transmit_config.flags.eot_level = 0U;
    const auto started_at = esp_timer_get_time();
    if (rmt_transmit(channel_, encoder_, pixel_buffer_.data(), payload_size, &transmit_config) !=
        ESP_OK) {
        return core::Status::failure(
            output_error(core::ErrorCode::io_failed, "transmit", "submit-failed"));
    }
    const int timeout_ms = static_cast<int>(config.pixel_count / 20U + 100U);
    if (rmt_tx_wait_all_done(channel_, timeout_ms) != ESP_OK) {
        return core::Status::failure(
            output_error(core::ErrorCode::io_failed, "transmit", "completion-timeout"));
    }
    esp_rom_delay_us(strip_timing(config.protocol).reset_us);
    last_frame_us_.store(static_cast<std::uint32_t>(esp_timer_get_time() - started_at));
    return core::Status::success();
}

core::Status EspRmtStripComponent::start(const core::StartContext&) noexcept {
    if (started_.load()) {
        return core::Status::success();
    }
    config_mutex_ = xSemaphoreCreateMutexStatic(&config_mutex_storage_);
    request_mutex_ = xSemaphoreCreateMutexStatic(&request_mutex_storage_);
    completion_ = xSemaphoreCreateBinaryStatic(&completion_storage_);
    if (config_mutex_ == nullptr || request_mutex_ == nullptr || completion_ == nullptr) {
        return core::Status::failure(
            output_error(core::ErrorCode::start_failed, "start", "mutex-create-failed"));
    }
    auto status = load_config();
    if (!status) {
        config_mutex_ = nullptr;
        request_mutex_ = nullptr;
        completion_ = nullptr;
        return status;
    }
    if (config_.enabled) {
        status = initialize_output(config_);
        if (!status) {
            config_mutex_ = nullptr;
            request_mutex_ = nullptr;
            completion_ = nullptr;
            return status;
        }
    }
    started_.store(true);
    task_quiesced_.store(false);
    task_ = xTaskCreateStatic(task_entry, "blip_led", task_stack_.size(), this, 5,
                              task_stack_.data(), &task_storage_);
    if (task_ == nullptr) {
        started_.store(false);
        task_quiesced_.store(true);
        deinitialize_output();
        config_mutex_ = nullptr;
        request_mutex_ = nullptr;
        completion_ = nullptr;
        return core::Status::failure(
            output_error(core::ErrorCode::start_failed, "start", "task-create-failed"));
    }
    xTaskNotifyGive(task_);
    ESP_LOGI(kTag, "ready pin=%u protocol=%u pixels=%u enabled=%u",
             static_cast<unsigned>(config_.gpio), static_cast<unsigned>(config_.protocol),
             static_cast<unsigned>(config_.pixel_count), config_.enabled ? 1U : 0U);
    return core::Status::success();
}

core::Status EspRmtStripComponent::stop() noexcept {
    if (!started_.exchange(false) && task_quiesced_.load()) {
        return core::Status::success();
    }
    if (task_ != nullptr) {
        xTaskNotifyGive(task_);
    }
    for (std::size_t attempt = 0; attempt < 200U && !task_quiesced_.load(); ++attempt) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    task_ = nullptr;
    deinitialize_output();
    config_mutex_ = nullptr;
    request_mutex_ = nullptr;
    completion_ = nullptr;
    return task_quiesced_.load() ? core::Status::success()
                                 : core::Status::failure(output_error(core::ErrorCode::stop_failed,
                                                                      "stop", "task-active"));
}

bool EspRmtStripComponent::callbacks_quiesced() const noexcept { return task_quiesced_.load(); }

core::Status EspRmtStripComponent::apply_config_locked(const StripConfig& candidate) noexcept {
    const auto valid = validate_strip_config(candidate);
    if (!valid) {
        return valid;
    }
    while (completion_ != nullptr && xSemaphoreTake(completion_, 0) == pdTRUE) {
    }
    if (!started_.load() || !lock_config()) {
        return core::Status::failure(
            output_error(core::ErrorCode::invalid_state, "configure", "not-started"));
    }
    const StripConfig previous = config_;
    if (candidate == previous) {
        unlock_config();
        return core::Status::success();
    }
    const bool transport_changed =
        candidate.gpio != previous.gpio || candidate.protocol != previous.protocol;
    if (transport_changed && previous.enabled && candidate.enabled) {
        unlock_config();
        return core::Status::failure(output_error(core::ErrorCode::invalid_state, "configure",
                                                  "disable-before-pin-or-protocol-change"));
    }
    pending_config_ = candidate;
    ++next_update_id_;
    if (next_update_id_ == 0U) {
        ++next_update_id_;
    }
    pending_update_id_ = next_update_id_;
    const std::uint32_t update_id = pending_update_id_;
    reconfigure_pending_ = true;
    unlock_config();
    if (task_ != nullptr) {
        xTaskNotifyGive(task_);
    }
    core::Status status = core::Status::failure(
        output_error(core::ErrorCode::io_failed, "configure", "commit-timeout"));
    const TickType_t started_at = xTaskGetTickCount();
    const TickType_t timeout = pdMS_TO_TICKS(10000);
    bool complete{};
    while (!complete && completion_ != nullptr) {
        const TickType_t now = xTaskGetTickCount();
        const TickType_t elapsed = now - started_at;
        if (elapsed >= timeout || xSemaphoreTake(completion_, timeout - elapsed) != pdTRUE) {
            break;
        }
        if (lock_config()) {
            complete = completed_update_id_ == update_id;
            if (complete) {
                status = completed_update_status_;
            }
            unlock_config();
        }
    }
    return status;
}

void EspRmtStripComponent::process_pending_config() noexcept {
    StripConfig candidate{};
    StripConfig previous{};
    std::uint32_t update_id{};
    if (!lock_config()) {
        return;
    }
    if (!reconfigure_pending_) {
        unlock_config();
        return;
    }
    candidate = pending_config_;
    previous = config_;
    update_id = pending_update_id_;
    unlock_config();

    const bool transport_changed =
        candidate.gpio != previous.gpio || candidate.protocol != previous.protocol;
    const bool platform_changed = candidate.enabled != previous.enabled || transport_changed;
    core::Status status = core::Status::success();
    if (platform_changed) {
        deinitialize_output();
        if (candidate.enabled) {
            status = initialize_output(candidate);
        }
    }
    if (status) {
        status = save_config(candidate);
    }
    if (!status && platform_changed) {
        deinitialize_output();
        if (previous.enabled && !initialize_output(previous)) {
            ESP_LOGE(kTag, "failed to restore prior output after configuration failure");
        }
    }
    if (lock_config()) {
        if (status) {
            config_ = candidate;
        }
        completed_update_id_ = update_id;
        completed_update_status_ = status;
        reconfigure_pending_ = false;
        unlock_config();
    }
    if (completion_ != nullptr) {
        static_cast<void>(xSemaphoreGive(completion_));
    }
}

core::Status EspRmtStripComponent::read_parameter(std::string_view id,
                                                  core::ScalarValue& output) noexcept {
    if (id == "applied_frames") {
        output = core::ScalarValue::from_integer(applied_frames_.load());
        return core::Status::success();
    }
    if (id == "failed_frames") {
        output = core::ScalarValue::from_integer(failed_frames_.load());
        return core::Status::success();
    }
    if (id == "last_frame_us") {
        output = core::ScalarValue::from_integer(last_frame_us_.load());
        return core::Status::success();
    }
    if (id == "worker_stack_headroom") {
        const auto words = task_ == nullptr ? 0U : uxTaskGetStackHighWaterMark(task_);
        output = core::ScalarValue::from_integer(words * sizeof(StackType_t));
        return core::Status::success();
    }
    if (id == "backend") {
        output = core::ScalarValue::from_string("native-rmt");
        return core::Status::success();
    }
    if (!started_.load() || !lock_config()) {
        return core::Status::failure(
            output_error(core::ErrorCode::invalid_state, "read-parameter", "not-started"));
    }
    if (id == "enabled") {
        output = core::ScalarValue::from_bool(config_.enabled);
    } else if (id == "pin") {
        output = core::ScalarValue::from_integer(config_.gpio);
    } else if (id == "protocol") {
        output = core::ScalarValue::from_integer(static_cast<std::uint8_t>(config_.protocol));
    } else if (id == "pixels") {
        output = core::ScalarValue::from_integer(config_.pixel_count);
    } else if (id == "brightness") {
        output = core::ScalarValue::from_integer(config_.brightness);
    } else if (id == "red") {
        output = core::ScalarValue::from_integer(config_.red);
    } else if (id == "green") {
        output = core::ScalarValue::from_integer(config_.green);
    } else if (id == "blue") {
        output = core::ScalarValue::from_integer(config_.blue);
    } else if (id == "white") {
        output = core::ScalarValue::from_integer(config_.white);
    } else {
        unlock_config();
        return core::Status::failure(
            output_error(core::ErrorCode::not_found, "read-parameter", id));
    }
    unlock_config();
    return core::Status::success();
}

core::Status EspRmtStripComponent::write_parameter(std::string_view id,
                                                   const core::ScalarValue& value) noexcept {
    if (request_mutex_ == nullptr ||
        xSemaphoreTake(request_mutex_, pdMS_TO_TICKS(10000)) != pdTRUE) {
        return core::Status::failure(
            output_error(core::ErrorCode::invalid_state, "write-parameter", "not-started"));
    }
    if (!started_.load() || !lock_config()) {
        static_cast<void>(xSemaphoreGive(request_mutex_));
        return core::Status::failure(
            output_error(core::ErrorCode::invalid_state, "write-parameter", "not-started"));
    }
    StripConfig candidate = config_;
    unlock_config();
    if (id == "enabled" && value.type == core::ValueType::boolean) {
        candidate.enabled = value.boolean;
    } else if (id == "pin" && value.type == core::ValueType::integer && value.integer >= 0 &&
               value.integer <= 63) {
        candidate.gpio = static_cast<std::uint8_t>(value.integer);
    } else if (id == "protocol" && value.type == core::ValueType::integer && value.integer >= 0 &&
               value.integer <= 1) {
        candidate.protocol = static_cast<StripProtocol>(value.integer);
    } else if (id == "pixels" && value.type == core::ValueType::integer && value.integer >= 1 &&
               value.integer <= static_cast<std::int64_t>(kMaximumStripPixels)) {
        candidate.pixel_count = static_cast<std::uint16_t>(value.integer);
    } else if ((id == "brightness" || id == "red" || id == "green" || id == "blue" ||
                id == "white") &&
               value.type == core::ValueType::integer && value.integer >= 0 &&
               value.integer <= 255) {
        auto& channel = id == "brightness" ? candidate.brightness
                        : id == "red"      ? candidate.red
                        : id == "green"    ? candidate.green
                        : id == "blue"     ? candidate.blue
                                           : candidate.white;
        channel = static_cast<std::uint8_t>(value.integer);
    } else {
        static_cast<void>(xSemaphoreGive(request_mutex_));
        return core::Status::failure(
            output_error(core::ErrorCode::validation_failed, "write-parameter", "invalid-value"));
    }
    const auto status = apply_config_locked(candidate);
    static_cast<void>(xSemaphoreGive(request_mutex_));
    return status;
}

core::Status EspRmtStripComponent::invoke_action(std::string_view id,
                                                 std::span<const core::ScalarValue> arguments,
                                                 std::span<core::ScalarValue>,
                                                 std::size_t& output_count) noexcept {
    output_count = 0U;
    if (id != "blackout" || !arguments.empty()) {
        return core::Status::failure(
            output_error(core::ErrorCode::invalid_argument, "invoke-action", "invalid-action"));
    }
    if (request_mutex_ == nullptr ||
        xSemaphoreTake(request_mutex_, pdMS_TO_TICKS(10000)) != pdTRUE) {
        return core::Status::failure(
            output_error(core::ErrorCode::invalid_state, "invoke-action", "not-started"));
    }
    if (!started_.load() || !lock_config()) {
        static_cast<void>(xSemaphoreGive(request_mutex_));
        return core::Status::failure(
            output_error(core::ErrorCode::invalid_state, "invoke-action", "not-started"));
    }
    StripConfig candidate = config_;
    unlock_config();
    candidate.red = 0U;
    candidate.green = 0U;
    candidate.blue = 0U;
    candidate.white = 0U;
    const auto status = apply_config_locked(candidate);
    static_cast<void>(xSemaphoreGive(request_mutex_));
    return status;
}

void EspRmtStripComponent::run() noexcept {
    while (started_.load()) {
        static_cast<void>(ulTaskNotifyTake(pdTRUE, portMAX_DELAY));
        if (!started_.load()) {
            break;
        }
        process_pending_config();
        StripConfig snapshot{};
        if (!lock_config()) {
            saturating_increment(failed_frames_);
            continue;
        }
        snapshot = config_;
        unlock_config();
        if (!snapshot.enabled) {
            continue;
        }
        const auto payload = fill_solid_frame(snapshot, pixel_buffer_);
        if (!payload) {
            saturating_increment(failed_frames_);
            continue;
        }
        const auto status = transmit(snapshot, payload.value());
        if (status) {
            saturating_increment(applied_frames_);
        } else {
            saturating_increment(failed_frames_);
        }
    }
    task_quiesced_.store(true);
    vTaskDelete(nullptr);
}

void EspRmtStripComponent::task_entry(void* context) noexcept {
    static_cast<EspRmtStripComponent*>(context)->run();
}

} // namespace blip::led
