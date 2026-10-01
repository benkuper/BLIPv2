#include "blip/transport/esp_ble_transport_component.hpp"

#include "esp_log.h"
#include "host/ble_att.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include <algorithm>
#include <cstring>

namespace blip::transport {
namespace {

constexpr char kTag[] = "blip_ble";
constexpr std::uint16_t kNoConnection = 0xffffU;
constexpr std::array<std::string_view, 1> kProvidedServices{"transport.ble"};
constexpr std::array<std::string_view, 2> kRequiredServices{"control.dispatch", "storage.settings"};
constexpr std::array<core::ParameterDescriptor, 8> kParameters{{
    {"enabled", "BLE enabled (live suspension)", core::ValueType::boolean,
     core::Access::read_write, true, core::ScalarValue::from_bool(false), {}, ""},
    {"active", "NimBLE stack active", core::ValueType::boolean, core::Access::read_only,
     false, core::ScalarValue::from_bool(false), {}, ""},
    {"advertising", "BLE advertising", core::ValueType::boolean, core::Access::read_only,
     false, core::ScalarValue::from_bool(false), {}, ""},
    {"connected", "BLE central connected", core::ValueType::boolean, core::Access::read_only,
     false, core::ScalarValue::from_bool(false), {}, ""},
    {"subscribed", "BLE indications subscribed", core::ValueType::boolean,
     core::Access::read_only, false, core::ScalarValue::from_bool(false), {}, ""},
    {"received_requests", "BLE control requests received", core::ValueType::integer,
     core::Access::read_only, false, core::ScalarValue::from_integer(0), {}, "requests"},
    {"sent_responses", "BLE control responses acknowledged", core::ValueType::integer,
     core::Access::read_only, false, core::ScalarValue::from_integer(0), {}, "responses"},
    {"rejected_chunks", "BLE chunks rejected", core::ValueType::integer,
     core::Access::read_only, false, core::ScalarValue::from_integer(0), {}, "chunks"},
}};

constexpr ble_uuid128_t kServiceUuid = BLE_UUID128_INIT(
    0x00, 0x43, 0xd2, 0x13, 0xd7, 0x8b, 0x3a, 0x8a,
    0xf4, 0x43, 0xcf, 0x64, 0xc0, 0x83, 0x6a, 0x6a);
constexpr ble_uuid128_t kRxUuid = BLE_UUID128_INIT(
    0x01, 0x43, 0xd2, 0x13, 0xd7, 0x8b, 0x3a, 0x8a,
    0xf4, 0x43, 0xcf, 0x64, 0xc0, 0x83, 0x6a, 0x6a);
constexpr ble_uuid128_t kTxUuid = BLE_UUID128_INIT(
    0x02, 0x43, 0xd2, 0x13, 0xd7, 0x8b, 0x3a, 0x8a,
    0xf4, 0x43, 0xcf, 0x64, 0xc0, 0x83, 0x6a, 0x6a);
std::uint16_t tx_value_handle{};

[[nodiscard]] core::Error ble_error(core::ErrorCode code, std::string_view operation,
                                    std::string_view detail) noexcept {
    return {core::ErrorDomain::transport, code, "blip.transport.ble", operation, detail};
}

[[nodiscard]] constexpr core::ComponentDescriptor ble_descriptor() noexcept {
    core::ComponentDescriptor result{};
    result.schema_version = 1;
    result.id = "blip.transport.ble";
    result.display_name = "NimBLE control transport";
    result.description =
        "Opt-in BLE control service. Disabling stops advertising and deinitializes NimBLE; "
        "excluding BLE from the build removes its code and controller configuration.";
    result.provided_services = kProvidedServices;
    result.required_services = kRequiredServices;
    result.parameters = kParameters;
    result.settings = {1, 1};
    result.disable_policy = core::DisablePolicy::live;
    result.supports_restart = true;
    result.cost = {65536, 4096, EspBleTransportComponent::kWorkerStackBytes};
    return result;
}

} // namespace

const core::ComponentDescriptor EspBleTransportComponent::descriptor_{ble_descriptor()};
EspBleTransportComponent* EspBleTransportComponent::instance_{};

EspBleTransportComponent::EspBleTransportComponent(core::ControlService& controls,
                                                   storage::SettingsStore& settings) noexcept
    : controls_(&controls), settings_(&settings), endpoint_(controls, payload_buffer_) {}

const core::ComponentDescriptor& EspBleTransportComponent::descriptor() const noexcept {
    return descriptor_;
}

core::Status EspBleTransportComponent::load_enabled() noexcept {
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
            ble_error(core::ErrorCode::corrupt_data, "load", "enabled-setting"));
    }
    desired_enabled_.store(data[0] == std::byte{1});
    return core::Status::success();
}

core::Status EspBleTransportComponent::save_enabled(bool enabled) noexcept {
    const std::array<std::byte, 1> data{enabled ? std::byte{1} : std::byte{0}};
    return settings_->save(descriptor_, data);
}

core::Status EspBleTransportComponent::start(const core::StartContext&) noexcept {
    if (started_.load()) {
        return core::Status::success();
    }
    if (instance_ != nullptr && instance_ != this) {
        return core::Status::failure(
            ble_error(core::ErrorCode::resource_conflict, "start", "nimble-instance"));
    }
    const auto loaded = load_enabled();
    if (!loaded) {
        return loaded;
    }
    instance_ = this;
    control_ready_.store(false);
    quiesced_.store(false);
    started_.store(true);
    if (xTaskCreate(worker_entry, "blip_ble", kWorkerStackBytes, this, 4,
                    &worker_task_) != pdPASS) {
        started_.store(false);
        quiesced_.store(true);
        instance_ = nullptr;
        return core::Status::failure(
            ble_error(core::ErrorCode::start_failed, "start", "worker-task"));
    }
    return core::Status::success();
}

core::Status EspBleTransportComponent::stop() noexcept {
    if (!started_.exchange(false) && quiesced_.load()) {
        return core::Status::success();
    }
    if (worker_task_ != nullptr) {
        xTaskNotifyGive(worker_task_);
    }
    for (std::size_t attempt = 0; attempt < 200U && !quiesced_.load(); ++attempt) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (!quiesced_.load()) {
        return core::Status::failure(
            ble_error(core::ErrorCode::stop_failed, "stop", "worker-timeout"));
    }
    worker_task_ = nullptr;
    instance_ = nullptr;
    return core::Status::success();
}

bool EspBleTransportComponent::callbacks_quiesced() const noexcept {
    return quiesced_.load() && !host_running_.load();
}

void EspBleTransportComponent::enable_control() noexcept {
    control_ready_.store(true);
    if (worker_task_ != nullptr) {
        xTaskNotifyGive(worker_task_);
    }
}

core::Status EspBleTransportComponent::read_parameter(std::string_view id,
                                                       core::ScalarValue& output) noexcept {
    if (!started_.load()) {
        return core::Status::failure(
            ble_error(core::ErrorCode::invalid_state, "read", "not-started"));
    }
    if (id == "enabled") {
        output = core::ScalarValue::from_bool(desired_enabled_.load());
    } else if (id == "active") {
        output = core::ScalarValue::from_bool(active_.load());
    } else if (id == "advertising") {
        output = core::ScalarValue::from_bool(advertising_.load());
    } else if (id == "connected") {
        output = core::ScalarValue::from_bool(connection_handle_.load() != kNoConnection);
    } else if (id == "subscribed") {
        output = core::ScalarValue::from_bool(subscribed_.load());
    } else if (id == "received_requests") {
        output = core::ScalarValue::from_integer(received_requests_.load());
    } else if (id == "sent_responses") {
        output = core::ScalarValue::from_integer(sent_responses_.load());
    } else if (id == "rejected_chunks") {
        output = core::ScalarValue::from_integer(rejected_chunks_.load());
    } else {
        return core::Status::failure(
            ble_error(core::ErrorCode::not_found, "read", "parameter-not-found"));
    }
    return core::Status::success();
}

core::Status EspBleTransportComponent::write_parameter(std::string_view id,
                                                        const core::ScalarValue& value) noexcept {
    if (!started_.load() || id != "enabled" || value.type != core::ValueType::boolean) {
        return core::Status::failure(
            ble_error(core::ErrorCode::invalid_argument, "write", "enabled-boolean-required"));
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

bool EspBleTransportComponent::initialize_nimble() noexcept {
    if (nimble_port_init() != ESP_OK) {
        ESP_LOGE(kTag, "NimBLE initialization failed");
        return false;
    }
    tx_value_handle = 0U;
    ble_hs_cfg.reset_cb = reset_callback;
    ble_hs_cfg.sync_cb = sync_callback;
#if CONFIG_BT_NIMBLE_GAP_SERVICE
    ble_svc_gap_init();
#endif
#if MYNEWT_VAL(BLE_GATTS)
    ble_svc_gatt_init();
#endif
    static const ble_gatt_chr_def characteristics[]{
        {&kRxUuid.u, gatt_callback, nullptr, nullptr, BLE_GATT_CHR_F_WRITE, 0, nullptr, nullptr},
        {&kTxUuid.u, gatt_callback, nullptr, nullptr, BLE_GATT_CHR_F_INDICATE, 0,
         &tx_value_handle, nullptr},
        {},
    };
    static const ble_gatt_svc_def services[]{
        {BLE_GATT_SVC_TYPE_PRIMARY, &kServiceUuid.u, nullptr, characteristics},
        {},
    };
    if (ble_gatts_count_cfg(services) != 0 || ble_gatts_add_svcs(services) != 0 ||
        ble_svc_gap_device_name_set("BLIP-BLE") != 0) {
        ESP_LOGE(kTag, "NimBLE service registration failed");
        static_cast<void>(nimble_port_deinit());
        return false;
    }
    nimble_port_freertos_init(host_entry);
    active_.store(true);
    ESP_LOGI(kTag, "NimBLE control service initialized");
    return true;
}

void EspBleTransportComponent::deinitialize_nimble() noexcept {
    active_.store(false);
    synced_.store(false);
    advertising_.store(false);
    subscribed_.store(false);
    if (connection_handle_.load() != kNoConnection) {
        static_cast<void>(ble_gap_terminate(connection_handle_.load(), BLE_ERR_REM_USER_CONN_TERM));
    }
    static_cast<void>(ble_gap_adv_stop());
    if (nimble_port_stop() != 0) {
        ESP_LOGE(kTag, "NimBLE stop failed");
        return;
    }
    for (std::size_t attempt = 0; attempt < 100U && host_running_.load(); ++attempt) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (nimble_port_deinit() != ESP_OK) {
        ESP_LOGE(kTag, "NimBLE deinit failed");
    }
    connection_handle_.store(kNoConnection);
    connection_epoch_.fetch_add(1U);
    ESP_LOGI(kTag, "NimBLE control service suspended");
}

void EspBleTransportComponent::advertise() noexcept {
    if (!active_.load() || !synced_.load() || !control_ready_.load() ||
        !desired_enabled_.load() || advertising_.load() ||
        connection_handle_.load() != kNoConnection) {
        return;
    }
    ble_hs_adv_fields fields{};
    static char kName[] = "BLIP-BLE";
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = reinterpret_cast<const std::uint8_t*>(kName);
    fields.name_len = sizeof(kName) - 1U;
    fields.name_is_complete = 1;
    if (ble_gap_adv_set_fields(&fields) != 0) {
        ESP_LOGE(kTag, "BLE advertising fields failed");
        return;
    }
    ble_gap_adv_params parameters{};
    parameters.conn_mode = BLE_GAP_CONN_MODE_UND;
    parameters.disc_mode = BLE_GAP_DISC_MODE_GEN;
    if (ble_gap_adv_start(own_address_type_, nullptr, BLE_HS_FOREVER,
                          &parameters, gap_callback, this) == 0) {
        advertising_.store(true);
    }
}

void EspBleTransportComponent::dispatch_request() noexcept {
    const std::size_t size = request_size_;
    const std::uint32_t epoch = request_epoch_;
    const std::uint8_t frame_id = request_frame_id_;
    if (epoch != connection_epoch_.load() || !subscribed_.load()) {
        busy_.store(false);
        return;
    }
    const auto response = endpoint_.handle_request(
        {request_buffer_.data(), size}, response_buffer_);
    if (!response || epoch != connection_epoch_.load() || !subscribed_.load() ||
        !response_fragments_.begin({response_buffer_.data(), response.value()}, frame_id)) {
        rejected_chunks_.fetch_add(1U);
        busy_.store(false);
        return;
    }
    response_epoch_ = epoch;
    send_next_fragment();
}

void EspBleTransportComponent::send_next_fragment() noexcept {
    const auto connection = connection_handle_.load();
    if (connection == kNoConnection || !subscribed_.load() ||
        response_epoch_ != connection_epoch_.load()) {
        response_fragments_.reset();
        busy_.store(false);
        return;
    }
    const auto mtu = ble_att_mtu(connection);
    if (mtu <= kBleChunkHeaderBytes + 3U) {
        response_fragments_.reset();
        busy_.store(false);
        return;
    }
    std::array<std::byte, kMaxBleChunkBytes> chunk{};
    const auto limit = std::min<std::size_t>(chunk.size(), mtu - 3U);
    const auto size = response_fragments_.next({chunk.data(), limit});
    if (!size) {
        response_fragments_.reset();
        busy_.store(false);
        return;
    }
    os_mbuf* packet = ble_hs_mbuf_from_flat(chunk.data(), size.value());
    if (packet == nullptr || ble_gatts_indicate_custom(connection, tx_value_handle, packet) != 0) {
        response_fragments_.reset();
        busy_.store(false);
        return;
    }
    waiting_for_ack_ = true;
}

void EspBleTransportComponent::run() noexcept {
    while (started_.load()) {
        if (busy_.load() && !request_pending_.load() &&
            (response_epoch_ != connection_epoch_.load() || !subscribed_.load()) &&
            !waiting_for_ack_.load()) {
            response_fragments_.reset();
            busy_.store(false);
        }
        if (desired_enabled_.load() && !active_.load()) {
            if (!initialize_nimble()) {
                desired_enabled_.store(false);
            }
        }
        if (request_pending_.exchange(false)) {
            dispatch_request();
        }
        if (ack_pending_.exchange(false) && waiting_for_ack_) {
            waiting_for_ack_ = false;
            if (ack_status_.load() == BLE_HS_EDONE && response_fragments_.acknowledge()) {
                if (response_fragments_.complete()) {
                    sent_responses_.fetch_add(1U);
                    response_fragments_.reset();
                    busy_.store(false);
                } else {
                    send_next_fragment();
                }
            } else {
                response_fragments_.reset();
                busy_.store(false);
            }
        }
        if (active_.load() && !desired_enabled_.load() && !busy_.load()) {
            deinitialize_nimble();
        }
        if (active_.load() && synced_.load()) {
            advertise();
        }
        static_cast<void>(ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100)));
    }
    if (active_.load()) {
        deinitialize_nimble();
    }
    quiesced_.store(true);
    vTaskDelete(nullptr);
}

void EspBleTransportComponent::on_gap_event(const ble_gap_event& event) noexcept {
    switch (event.type) {
    case BLE_GAP_EVENT_CONNECT:
        advertising_.store(false);
        if (event.connect.status == 0) {
            connection_handle_.store(event.connect.conn_handle);
            connection_epoch_.fetch_add(1U);
            subscribed_.store(false);
            request_assembler_.reset();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        connection_handle_.store(kNoConnection);
        connection_epoch_.fetch_add(1U);
        subscribed_.store(false);
        request_assembler_.reset();
        waiting_for_ack_ = false;
        ack_pending_.store(false);
        if (worker_task_ != nullptr) {
            xTaskNotifyGive(worker_task_);
        }
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event.subscribe.attr_handle == tx_value_handle &&
            event.subscribe.conn_handle == connection_handle_.load()) {
            subscribed_.store(event.subscribe.cur_indicate != 0U);
        }
        break;
    case BLE_GAP_EVENT_NOTIFY_TX:
        if (event.notify_tx.indication != 0U &&
            event.notify_tx.attr_handle == tx_value_handle &&
            event.notify_tx.conn_handle == connection_handle_.load() &&
            event.notify_tx.status != 0) {
            ack_status_.store(event.notify_tx.status);
            ack_pending_.store(true);
            if (worker_task_ != nullptr) {
                xTaskNotifyGive(worker_task_);
            }
        }
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertising_.store(false);
        break;
    default:
        break;
    }
}

int EspBleTransportComponent::on_gatt_access(std::uint16_t connection,
                                              ble_gatt_access_ctxt& context) noexcept {
    if (context.op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_READ_NOT_PERMITTED;
    }
    if (!control_ready_.load() || !subscribed_.load() ||
        connection != connection_handle_.load() || busy_.load()) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    const auto length = OS_MBUF_PKTLEN(context.om);
    if (length <= kBleChunkHeaderBytes || length > kMaxBleChunkBytes) {
        rejected_chunks_.fetch_add(1U);
        request_assembler_.reset();
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    std::array<std::byte, kMaxBleChunkBytes> chunk{};
    std::uint16_t copied{};
    if (ble_hs_mbuf_to_flat(context.om, chunk.data(), chunk.size(), &copied) != 0 ||
        copied != length) {
        rejected_chunks_.fetch_add(1U);
        request_assembler_.reset();
        return BLE_ATT_ERR_UNLIKELY;
    }
    const auto assembled = request_assembler_.append({chunk.data(), copied});
    if (!assembled) {
        rejected_chunks_.fetch_add(1U);
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    if (assembled.value()) {
        const auto frame = request_assembler_.frame();
        busy_.store(true);
        std::memcpy(request_buffer_.data(), frame.data(), frame.size());
        request_size_ = frame.size();
        request_frame_id_ = request_assembler_.frame_id();
        request_epoch_ = connection_epoch_.load();
        request_assembler_.reset();
        received_requests_.fetch_add(1U);
        request_pending_.store(true);
        if (worker_task_ != nullptr) {
            xTaskNotifyGive(worker_task_);
        }
    }
    return 0;
}

void EspBleTransportComponent::worker_entry(void* context) noexcept {
    static_cast<EspBleTransportComponent*>(context)->run();
}

void EspBleTransportComponent::host_entry(void*) noexcept {
    if (instance_ != nullptr) {
        instance_->host_running_.store(true);
    }
    nimble_port_run();
    if (instance_ != nullptr) {
        instance_->host_running_.store(false);
    }
    nimble_port_freertos_deinit();
}

void EspBleTransportComponent::sync_callback() noexcept {
    if (instance_ == nullptr || ble_hs_util_ensure_addr(0) != 0 ||
        ble_hs_id_infer_auto(0, &instance_->own_address_type_) != 0) {
        return;
    }
    instance_->synced_.store(true);
    if (instance_->worker_task_ != nullptr) {
        xTaskNotifyGive(instance_->worker_task_);
    }
}

void EspBleTransportComponent::reset_callback(int reason) noexcept {
    ESP_LOGW(kTag, "NimBLE host reset reason=%d", reason);
    if (instance_ != nullptr) {
        instance_->synced_.store(false);
        instance_->advertising_.store(false);
    }
}

int EspBleTransportComponent::gap_callback(ble_gap_event* event, void* context) noexcept {
    if (event != nullptr && context != nullptr) {
        static_cast<EspBleTransportComponent*>(context)->on_gap_event(*event);
    }
    return 0;
}

int EspBleTransportComponent::gatt_callback(std::uint16_t connection, std::uint16_t,
                                             ble_gatt_access_ctxt* context, void*) noexcept {
    return instance_ == nullptr || context == nullptr
               ? BLE_ATT_ERR_UNLIKELY
               : instance_->on_gatt_access(connection, *context);
}

} // namespace blip::transport
