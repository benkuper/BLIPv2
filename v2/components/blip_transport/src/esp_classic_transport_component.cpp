#include "blip/transport/esp_classic_transport_component.hpp"

#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_log.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>

namespace blip::transport {
namespace {

constexpr char kTag[] = "blip_classic";
constexpr std::array<std::string_view, 1> kProvidedServices{"transport.classic"};
constexpr std::array<std::string_view, 2> kRequiredServices{"control.dispatch", "storage.settings"};
constexpr std::array<core::ParameterDescriptor, 8> kParameters{{
    {"enabled", "Classic Bluetooth SPP enabled", core::ValueType::boolean,
     core::Access::read_write, true, core::ScalarValue::from_bool(false), {}, ""},
    {"active", "SPP service active", core::ValueType::boolean, core::Access::read_only,
     false, core::ScalarValue::from_bool(false), {}, ""},
    {"discoverable", "Classic Bluetooth discoverable", core::ValueType::boolean,
     core::Access::read_only, false, core::ScalarValue::from_bool(false), {}, ""},
    {"address", "Classic Bluetooth address", core::ValueType::string,
     core::Access::read_only, false, core::ScalarValue::from_string(""), {}, ""},
    {"connected", "SPP client connected", core::ValueType::boolean, core::Access::read_only,
     false, core::ScalarValue::from_bool(false), {}, ""},
    {"received_requests", "SPP control requests received", core::ValueType::integer,
     core::Access::read_only, false, core::ScalarValue::from_integer(0), {}, "requests"},
    {"sent_responses", "SPP control responses sent", core::ValueType::integer,
     core::Access::read_only, false, core::ScalarValue::from_integer(0), {}, "responses"},
    {"rejected_frames", "SPP frames rejected", core::ValueType::integer,
     core::Access::read_only, false, core::ScalarValue::from_integer(0), {}, "frames"},
}};

[[nodiscard]] constexpr core::ComponentDescriptor classic_descriptor() noexcept {
    core::ComponentDescriptor result{};
    result.schema_version = 1;
    result.id = "blip.transport.classic";
    result.display_name = "Classic Bluetooth SPP control";
    result.description =
        "Opt-in original ESP32 SPP service using the COBS-framed BLIP control envelope";
    result.provided_services = kProvidedServices;
    result.required_services = kRequiredServices;
    result.parameters = kParameters;
    result.settings = {1, 1};
    result.disable_policy = core::DisablePolicy::live;
    result.supports_restart = true;
    result.cost = {98304, 4096, EspClassicTransportComponent::kWorkerStackBytes};
    return result;
}

[[nodiscard]] core::Error classic_error(core::ErrorCode code, std::string_view operation,
                                        std::string_view detail) noexcept {
    return {core::ErrorDomain::transport, code, "blip.transport.classic", operation, detail};
}

void saturating_increment(std::atomic<std::uint32_t>& value) noexcept {
    auto current = value.load();
    while (current != std::numeric_limits<std::uint32_t>::max() &&
           !value.compare_exchange_weak(current, current + 1U)) {
    }
}

} // namespace

const core::ComponentDescriptor EspClassicTransportComponent::descriptor_{classic_descriptor()};
EspClassicTransportComponent* EspClassicTransportComponent::instance_{};

EspClassicTransportComponent::EspClassicTransportComponent(
    core::ControlService& controls, storage::SettingsStore& settings) noexcept
    : settings_(&settings),
      endpoint_(controls, decode_buffer_, envelope_buffer_, payload_buffer_) {}

const core::ComponentDescriptor& EspClassicTransportComponent::descriptor() const noexcept {
    return descriptor_;
}

core::Status EspClassicTransportComponent::load_enabled() noexcept {
    std::array<std::byte, 1> data{};
    const auto loaded = settings_->load(descriptor_, data);
    if (!loaded) {
        if (loaded.error().code == core::ErrorCode::not_found) {
            desired_enabled_.store(false);
            return core::Status::success();
        }
        return core::Status::failure(loaded.error());
    }
    if (loaded.value().payload_size != data.size() || data[0] > std::byte{1}) {
        return core::Status::failure(
            classic_error(core::ErrorCode::corrupt_data, "load", "enabled-setting"));
    }
    desired_enabled_.store(data[0] == std::byte{1});
    return core::Status::success();
}

core::Status EspClassicTransportComponent::save_enabled(bool enabled) noexcept {
    const std::array<std::byte, 1> data{enabled ? std::byte{1} : std::byte{0}};
    return settings_->save(descriptor_, data);
}

core::Status EspClassicTransportComponent::start(const core::StartContext&) noexcept {
    if (started_.load()) {
        return core::Status::success();
    }
    if (instance_ != nullptr && instance_ != this) {
        return core::Status::failure(
            classic_error(core::ErrorCode::resource_conflict, "start", "spp-instance"));
    }
    const auto loaded = load_enabled();
    if (!loaded) {
        return loaded;
    }
    receive_stream_ = xStreamBufferCreate(2048, 1);
    if (receive_stream_ == nullptr) {
        return core::Status::failure(
            classic_error(core::ErrorCode::start_failed, "start", "receive-buffer"));
    }
    instance_ = this;
    quiesced_.store(false);
    control_ready_.store(false);
    started_.store(true);
    if (xTaskCreate(worker_entry, "blip_classic", kWorkerStackBytes, this, 4,
                    &worker_task_) != pdPASS) {
        started_.store(false);
        quiesced_.store(true);
        instance_ = nullptr;
        vStreamBufferDelete(receive_stream_);
        receive_stream_ = nullptr;
        return core::Status::failure(
            classic_error(core::ErrorCode::start_failed, "start", "worker-task"));
    }
    return core::Status::success();
}

core::Status EspClassicTransportComponent::stop() noexcept {
    if (!started_.exchange(false) && quiesced_.load()) {
        return core::Status::success();
    }
    if (worker_task_ != nullptr) {
        xTaskNotifyGive(worker_task_);
    }
    for (std::size_t attempt = 0; attempt < 500U && !quiesced_.load(); ++attempt) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!quiesced_.load()) {
        return core::Status::failure(
            classic_error(core::ErrorCode::stop_failed, "stop", "worker-timeout"));
    }
    worker_task_ = nullptr;
    instance_ = nullptr;
    if (receive_stream_ != nullptr) {
        vStreamBufferDelete(receive_stream_);
        receive_stream_ = nullptr;
    }
    return core::Status::success();
}

bool EspClassicTransportComponent::callbacks_quiesced() const noexcept {
    return quiesced_.load() && !stack_started_.load();
}

void EspClassicTransportComponent::enable_control() noexcept {
    control_ready_.store(true);
    if (worker_task_ != nullptr) {
        xTaskNotifyGive(worker_task_);
    }
}

core::Status EspClassicTransportComponent::read_parameter(std::string_view id,
                                                           core::ScalarValue& output) noexcept {
    if (!started_.load()) {
        return core::Status::failure(
            classic_error(core::ErrorCode::invalid_state, "read", "not-started"));
    }
    if (id == "enabled") {
        output = core::ScalarValue::from_bool(desired_enabled_.load());
    } else if (id == "active") {
        output = core::ScalarValue::from_bool(active_.load());
    } else if (id == "discoverable") {
        output = core::ScalarValue::from_bool(discoverable_.load());
    } else if (id == "address") {
        const auto* address = active_.load() ? esp_bt_dev_get_address() : nullptr;
        if (address != nullptr) {
            std::snprintf(address_text_.data(), address_text_.size(),
                          "%02X:%02X:%02X:%02X:%02X:%02X", address[0], address[1],
                          address[2], address[3], address[4], address[5]);
        } else {
            address_text_[0] = '\0';
        }
        output = core::ScalarValue::from_string(address_text_.data());
    } else if (id == "connected") {
        output = core::ScalarValue::from_bool(connection_handle_.load() != 0U);
    } else if (id == "received_requests") {
        output = core::ScalarValue::from_integer(received_requests_.load());
    } else if (id == "sent_responses") {
        output = core::ScalarValue::from_integer(sent_responses_.load());
    } else if (id == "rejected_frames") {
        output = core::ScalarValue::from_integer(rejected_frames_.load());
    } else {
        return core::Status::failure(
            classic_error(core::ErrorCode::not_found, "read", "parameter-not-found"));
    }
    return core::Status::success();
}

core::Status EspClassicTransportComponent::write_parameter(
    std::string_view id, const core::ScalarValue& value) noexcept {
    if (!started_.load() || id != "enabled" || value.type != core::ValueType::boolean) {
        return core::Status::failure(classic_error(core::ErrorCode::invalid_argument, "write",
                                                   "enabled-boolean-required"));
    }
    const auto saved = save_enabled(value.boolean);
    if (!saved) {
        return saved;
    }
    desired_enabled_.store(value.boolean);
    if (worker_task_ != nullptr) {
        xTaskNotifyGive(worker_task_);
    }
    return core::Status::success();
}

bool EspClassicTransportComponent::initialize_stack() noexcept {
    if (!ble_memory_released_) {
        if (esp_bt_controller_mem_release(ESP_BT_MODE_BLE) != ESP_OK) {
            ESP_LOGE(kTag, "BLE controller memory release failed");
            return false;
        }
        ble_memory_released_ = true;
    }
    esp_bt_controller_config_t controller_config = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    if (esp_bt_controller_init(&controller_config) != ESP_OK) {
        ESP_LOGE(kTag, "controller initialization failed");
        return false;
    }
    controller_initialized_ = true;
    if (esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT) != ESP_OK) {
        ESP_LOGE(kTag, "controller enable failed");
        deinitialize_stack();
        return false;
    }
    controller_enabled_ = true;
    esp_bluedroid_config_t bluedroid_config = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    bluedroid_config.ssp_en = true;
    if (esp_bluedroid_init_with_cfg(&bluedroid_config) != ESP_OK) {
        ESP_LOGE(kTag, "Bluedroid initialization failed");
        deinitialize_stack();
        return false;
    }
    bluedroid_initialized_ = true;
    if (esp_bluedroid_enable() != ESP_OK) {
        ESP_LOGE(kTag, "Bluedroid enable failed");
        deinitialize_stack();
        return false;
    }
    bluedroid_enabled_ = true;
    esp_bt_io_cap_t capability = ESP_BT_IO_CAP_NONE;
    esp_spp_cfg_t spp_config{};
    spp_config.mode = ESP_SPP_MODE_CB;
    spp_config.enable_l2cap_ertm = true;
    if (esp_bt_gap_register_callback(gap_callback) != ESP_OK ||
        esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &capability,
                                      sizeof(capability)) != ESP_OK ||
        esp_spp_register_callback(spp_callback) != ESP_OK ||
        esp_spp_enhanced_init(&spp_config) != ESP_OK) {
        ESP_LOGE(kTag, "SPP registration failed");
        deinitialize_stack();
        return false;
    }
    spp_initialized_ = true;
    spp_uninitialized_.store(false);
    stack_started_.store(true);
    ESP_LOGI(kTag, "Classic SPP stack initialized");
    return true;
}

void EspClassicTransportComponent::deinitialize_stack() noexcept {
    active_.store(false);
    discoverable_.store(false);
    connection_handle_.store(0U);
    if (bluedroid_enabled_) {
        static_cast<void>(esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE,
                                                    ESP_BT_NON_DISCOVERABLE));
    }
    if (spp_initialized_) {
        spp_uninitialized_.store(false);
        if (esp_spp_deinit() == ESP_OK) {
            for (std::size_t attempt = 0; attempt < 200U && !spp_uninitialized_.load(); ++attempt) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
        spp_initialized_ = false;
    }
    if (bluedroid_enabled_) {
        static_cast<void>(esp_bluedroid_disable());
        bluedroid_enabled_ = false;
    }
    if (bluedroid_initialized_) {
        static_cast<void>(esp_bluedroid_deinit());
        bluedroid_initialized_ = false;
    }
    if (controller_enabled_) {
        static_cast<void>(esp_bt_controller_disable());
        controller_enabled_ = false;
    }
    if (controller_initialized_) {
        static_cast<void>(esp_bt_controller_deinit());
        controller_initialized_ = false;
    }
    stack_started_.store(false);
}

bool EspClassicTransportComponent::write_response(std::size_t size) noexcept {
    const auto handle = connection_handle_.load();
    if (handle == 0U) {
        return false;
    }
    for (std::size_t attempt = 0; attempt < 200U && write_congested_.load() &&
                                  connection_handle_.load() == handle; ++attempt) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (write_congested_.load() || connection_handle_.load() != handle) {
        return false;
    }
    write_complete_.store(false);
    write_ok_.store(false);
    if (esp_spp_write(handle, static_cast<int>(size),
                      reinterpret_cast<std::uint8_t*>(response_frame_.data())) != ESP_OK) {
        return false;
    }
    for (std::size_t attempt = 0; attempt < 200U && !write_complete_.load() &&
                                  connection_handle_.load() == handle; ++attempt) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return write_complete_.load() && write_ok_.load() && connection_handle_.load() == handle;
}

void EspClassicTransportComponent::process_bytes(std::span<const std::byte> input) noexcept {
    for (const auto byte : input) {
        if (byte == std::byte{0}) {
            if (discard_until_delimiter_) {
                discard_until_delimiter_ = false;
                incoming_size_ = 0;
                continue;
            }
            if (incoming_size_ == 0U || !control_ready_.load()) {
                incoming_size_ = 0;
                continue;
            }
            response_frame_[0] = std::byte{0};
            const auto response = endpoint_.handle_frame(
                {incoming_frame_.data(), incoming_size_},
                std::span<std::byte>{response_frame_}.subspan(1U));
            incoming_size_ = 0;
            if (!response) {
                saturating_increment(rejected_frames_);
                continue;
            }
            saturating_increment(received_requests_);
            if (write_response(response.value() + 1U)) {
                saturating_increment(sent_responses_);
            } else {
                saturating_increment(rejected_frames_);
            }
            continue;
        }
        if (discard_until_delimiter_) {
            continue;
        }
        if (incoming_size_ == incoming_frame_.size()) {
            incoming_size_ = 0;
            discard_until_delimiter_ = true;
            saturating_increment(overflow_frames_);
            continue;
        }
        incoming_frame_[incoming_size_++] = byte;
    }
}

void EspClassicTransportComponent::run() noexcept {
    std::array<std::byte, 128> input{};
    while (started_.load()) {
        if (desired_enabled_.load() && control_ready_.load() && !stack_started_.load()) {
            if (!initialize_stack()) {
                desired_enabled_.store(false);
            }
        }
        const auto received = xStreamBufferReceive(receive_stream_, input.data(), input.size(),
                                                   pdMS_TO_TICKS(50));
        if (input_overflow_.exchange(false)) {
            incoming_size_ = 0;
            discard_until_delimiter_ = true;
        }
        if (received != 0U) {
            process_bytes({input.data(), received});
        }
        if (!desired_enabled_.load() && (stack_started_.load() || controller_initialized_)) {
            deinitialize_stack();
        }
    }
    if (stack_started_.load() || controller_initialized_) {
        deinitialize_stack();
    }
    quiesced_.store(true);
    vTaskDelete(nullptr);
}

void EspClassicTransportComponent::on_spp_event(esp_spp_cb_event_t event,
                                                 esp_spp_cb_param_t* param) noexcept {
    switch (event) {
    case ESP_SPP_INIT_EVT:
        if (param->init.status != ESP_SPP_SUCCESS ||
            esp_spp_start_srv(ESP_SPP_SEC_AUTHENTICATE, ESP_SPP_ROLE_SLAVE, 0,
                              "BLIP-V2") != ESP_OK) {
            ESP_LOGE(kTag, "SPP server initialization failed");
            desired_enabled_.store(false);
        }
        break;
    case ESP_SPP_START_EVT:
        if (param->start.status == ESP_SPP_SUCCESS &&
            esp_bt_gap_set_device_name("BLIP-Classic") == ESP_OK &&
            esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE) ==
                ESP_OK) {
            active_.store(true);
            discoverable_.store(true);
            ESP_LOGI(kTag, "Classic SPP discoverable");
        } else {
            ESP_LOGE(kTag, "SPP server start failed");
            desired_enabled_.store(false);
        }
        break;
    case ESP_SPP_SRV_OPEN_EVT:
        if (param->srv_open.status == ESP_SPP_SUCCESS) {
            connection_handle_.store(param->srv_open.handle);
            write_congested_.store(false);
        }
        break;
    case ESP_SPP_CLOSE_EVT:
        connection_handle_.store(0U);
        write_ok_.store(false);
        write_complete_.store(true);
        input_overflow_.store(true);
        break;
    case ESP_SPP_DATA_IND_EVT:
        if (param->data_ind.status == ESP_SPP_SUCCESS &&
            param->data_ind.handle == connection_handle_.load()) {
            const auto written = xStreamBufferSend(receive_stream_, param->data_ind.data,
                                                   param->data_ind.len, 0);
            if (written != param->data_ind.len) {
                input_overflow_.store(true);
                saturating_increment(overflow_frames_);
            }
        }
        break;
    case ESP_SPP_WRITE_EVT:
        write_ok_.store(param->write.status == ESP_SPP_SUCCESS);
        write_congested_.store(param->write.cong);
        write_complete_.store(true);
        break;
    case ESP_SPP_CONG_EVT:
        write_congested_.store(param->cong.cong);
        break;
    case ESP_SPP_UNINIT_EVT:
        spp_uninitialized_.store(true);
        break;
    default:
        break;
    }
}

void EspClassicTransportComponent::on_gap_event(esp_bt_gap_cb_event_t event,
                                                 esp_bt_gap_cb_param_t* param) noexcept {
    if (event == ESP_BT_GAP_CFM_REQ_EVT) {
        static_cast<void>(esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true));
    }
}

void EspClassicTransportComponent::worker_entry(void* context) noexcept {
    static_cast<EspClassicTransportComponent*>(context)->run();
}

void EspClassicTransportComponent::spp_callback(esp_spp_cb_event_t event,
                                                 esp_spp_cb_param_t* param) noexcept {
    if (instance_ != nullptr) {
        instance_->on_spp_event(event, param);
    }
}

void EspClassicTransportComponent::gap_callback(esp_bt_gap_cb_event_t event,
                                                 esp_bt_gap_cb_param_t* param) noexcept {
    if (instance_ != nullptr) {
        instance_->on_gap_event(event, param);
    }
}

} // namespace blip::transport
