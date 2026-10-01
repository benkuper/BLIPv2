#include "blip/transport/esp_espnow_transport_component.hpp"

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace blip::transport {
namespace {

constexpr char kTag[] = "blip_espnow";
constexpr std::array<std::string_view, 1> kProvidedServices{"transport.espnow"};
constexpr std::array<std::string_view, 3> kRequiredServices{
    "control.dispatch", "storage.settings", "transport.wifi"};
constexpr std::array<core::ParameterDescriptor, 8> kParameters{{
    {"enabled", "ESP-NOW enabled", core::ValueType::boolean, core::Access::read_write,
     true, core::ScalarValue::from_bool(false), {}, ""},
    {"active", "ESP-NOW radio active", core::ValueType::boolean, core::Access::read_only,
     false, core::ScalarValue::from_bool(false), {}, ""},
    {"address", "Station MAC address", core::ValueType::string, core::Access::read_only,
     false, core::ScalarValue::from_string(""), {}, ""},
    {"peer_mac", "Allowed peer station MAC", core::ValueType::string,
     core::Access::read_write, true, core::ScalarValue::from_string(""), {}, ""},
    {"received_requests", "Control requests received", core::ValueType::integer,
     core::Access::read_only, false, core::ScalarValue::from_integer(0), {}, "requests"},
    {"sent_responses", "Control responses acknowledged", core::ValueType::integer,
     core::Access::read_only, false, core::ScalarValue::from_integer(0), {}, "responses"},
    {"rejected_packets", "Packets rejected or dropped", core::ValueType::integer,
     core::Access::read_only, false, core::ScalarValue::from_integer(0), {}, "packets"},
    {"retries", "Whole message retries", core::ValueType::integer,
     core::Access::read_only, false, core::ScalarValue::from_integer(0), {}, "retries"},
}};
constexpr std::array<core::FieldDescriptor, 2> kRemoteReadArguments{{
    {"component", core::ValueType::string, true},
    {"parameter", core::ValueType::string, true},
}};
constexpr std::array<core::FieldDescriptor, 3> kRemoteWriteArguments{{
    {"component", core::ValueType::string, true},
    {"parameter", core::ValueType::string, true},
    {"value", core::ValueType::string, true},
}};
constexpr std::array<core::FieldDescriptor, 3> kRemoteWriteIntegerArguments{{
    {"component", core::ValueType::string, true},
    {"parameter", core::ValueType::string, true},
    {"value", core::ValueType::integer, true},
}};
constexpr std::array<core::FieldDescriptor, 3> kRemoteWriteBooleanArguments{{
    {"component", core::ValueType::string, true},
    {"parameter", core::ValueType::string, true},
    {"value", core::ValueType::boolean, true},
}};
constexpr std::array<core::FieldDescriptor, 3> kRemoteWriteNumberArguments{{
    {"component", core::ValueType::string, true},
    {"parameter", core::ValueType::string, true},
    {"value", core::ValueType::number, true},
}};
constexpr std::array<core::ActionDescriptor, 5> kActions{{
    {"remote_read", "Read a parameter from the configured peer", kRemoteReadArguments},
    {"remote_write", "Write a string parameter on the configured peer", kRemoteWriteArguments},
    {"remote_write_integer", "Write an integer parameter on the configured peer",
     kRemoteWriteIntegerArguments},
    {"remote_write_boolean", "Write a boolean parameter on the configured peer",
     kRemoteWriteBooleanArguments},
    {"remote_write_number", "Write a number parameter on the configured peer",
     kRemoteWriteNumberArguments},
}};

[[nodiscard]] constexpr core::ComponentDescriptor make_descriptor() noexcept {
    core::ComponentDescriptor result{};
    result.schema_version = 1;
    result.id = "blip.transport.espnow";
    result.display_name = "ESP-NOW V2 control";
    result.description = "Opt-in one-peer V2 control transport on the Wi-Fi station channel";
    result.provided_services = kProvidedServices;
    result.required_services = kRequiredServices;
    result.parameters = kParameters;
    result.actions = kActions;
    result.settings = {1, 1};
    result.disable_policy = core::DisablePolicy::live;
    result.supports_restart = true;
    result.cost = {32768, 8192, EspEspNowTransportComponent::kWorkerStackBytes};
    return result;
}

[[nodiscard]] core::Error failure(core::ErrorCode code, std::string_view operation,
                                  std::string_view detail) noexcept {
    return {core::ErrorDomain::transport, code, "blip.transport.espnow", operation, detail};
}

[[nodiscard]] int hex_digit(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

[[nodiscard]] bool parse_mac(std::string_view input, std::uint64_t& output) noexcept {
    if (input.size() != 17U) return false;
    output = 0;
    for (std::size_t i = 0; i < 6U; ++i) {
        if (i != 0U && input[i * 3U - 1U] != ':') return false;
        const int hi = hex_digit(input[i * 3U]);
        const int lo = hex_digit(input[i * 3U + 1U]);
        if (hi < 0 || lo < 0) return false;
        output |= static_cast<std::uint64_t>((hi << 4) | lo) << (i * 8U);
    }
    return output != 0U && (output & 1U) == 0U;
}

void unpack_mac(std::uint64_t packed, std::uint8_t* output) noexcept {
    for (std::size_t i = 0; i < 6U; ++i) {
        output[i] = static_cast<std::uint8_t>(packed >> (i * 8U));
    }
}

void format_mac(std::uint64_t packed, std::array<char, 18>& output) noexcept {
    std::uint8_t bytes[6]{};
    unpack_mac(packed, bytes);
    std::snprintf(output.data(), output.size(), "%02x:%02x:%02x:%02x:%02x:%02x",
                  bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5]);
}

[[nodiscard]] std::uint32_t now_ms() noexcept {
    return static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
}

} // namespace

const core::ComponentDescriptor EspEspNowTransportComponent::descriptor_{make_descriptor()};
EspEspNowTransportComponent* EspEspNowTransportComponent::instance_{};

EspEspNowTransportComponent::EspEspNowTransportComponent(
    core::ControlService& controls, storage::SettingsStore& settings,
    network::EspWifiComponent& wifi) noexcept
    : settings_(&settings), wifi_(&wifi), endpoint_(controls, payload_buffer_) {}

const core::ComponentDescriptor& EspEspNowTransportComponent::descriptor() const noexcept {
    return descriptor_;
}

core::Status EspEspNowTransportComponent::load_settings() noexcept {
    std::array<std::byte, 7> data{};
    const auto loaded = settings_->load(descriptor_, data);
    if (!loaded) {
        // Earlier imported ESP-NOW settings use this component ID but carry a
        // different, larger payload. V2 starts disabled until configured.
        if (loaded.error().code == core::ErrorCode::not_found ||
            (loaded.error().code == core::ErrorCode::capacity_exceeded &&
             loaded.error().detail == "output-too-small")) {
            return core::Status::success();
        }
        return core::Status::failure(loaded.error());
    }
    if (loaded.value().payload_size != data.size() ||
        loaded.value().schema_version != descriptor_.settings.schema_version) {
        return core::Status::success();
    }
    if (data[0] > std::byte{1}) {
        return core::Status::failure(failure(core::ErrorCode::corrupt_data, "load", "enabled"));
    }
    std::uint64_t peer{};
    for (std::size_t i = 0; i < 6U; ++i) {
        peer |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(data[i + 1U]))
                << (i * 8U);
    }
    if (peer != 0U && (peer & 1U) != 0U) {
        return core::Status::failure(failure(core::ErrorCode::corrupt_data, "load", "peer-mac"));
    }
    peer_packed_.store(peer);
    enabled_.store(data[0] == std::byte{1});
    return core::Status::success();
}

core::Status EspEspNowTransportComponent::save_settings(bool enabled,
                                                        std::uint64_t peer) noexcept {
    std::array<std::byte, 7> data{};
    data[0] = enabled ? std::byte{1} : std::byte{0};
    for (std::size_t i = 0; i < 6U; ++i) {
        data[i + 1U] = static_cast<std::byte>((peer >> (i * 8U)) & 0xffU);
    }
    return settings_->save(descriptor_, data);
}

core::Status EspEspNowTransportComponent::start(const core::StartContext&) noexcept {
    if (started_.load()) return core::Status::success();
    if (instance_ != nullptr && instance_ != this) {
        return core::Status::failure(failure(core::ErrorCode::resource_conflict,
                                             "start", "instance"));
    }
    const auto loaded = load_settings();
    if (!loaded) return loaded;
    receive_queue_ = xQueueCreate(8, sizeof(ReceivedPacket));
    request_done_ = xSemaphoreCreateBinary();
    if (receive_queue_ == nullptr || request_done_ == nullptr) {
        if (receive_queue_ != nullptr) vQueueDelete(receive_queue_);
        if (request_done_ != nullptr) vSemaphoreDelete(request_done_);
        receive_queue_ = nullptr;
        request_done_ = nullptr;
        return core::Status::failure(failure(core::ErrorCode::start_failed,
                                             "start", "queue-or-semaphore"));
    }
    std::uint8_t own_mac[6]{};
    if (esp_wifi_get_mac(WIFI_IF_STA, own_mac) == ESP_OK) {
        std::uint64_t packed{};
        for (std::size_t i = 0; i < 6U; ++i) {
            packed |= static_cast<std::uint64_t>(own_mac[i]) << (i * 8U);
        }
        format_mac(packed, address_text_);
    }
    session_ = esp_random();
    if (session_ == 0U) session_ = 1U;
    next_sequence_ = esp_random();
    if (next_sequence_ == 0U) next_sequence_ = 1U;
    instance_ = this;
    quiesced_.store(false);
    started_.store(true);
    if (xTaskCreate(worker_entry, "blip_espnow", kWorkerStackBytes, this, 4,
                    &worker_task_) != pdPASS) {
        started_.store(false);
        quiesced_.store(true);
        instance_ = nullptr;
        vQueueDelete(receive_queue_);
        vSemaphoreDelete(request_done_);
        receive_queue_ = nullptr;
        request_done_ = nullptr;
        return core::Status::failure(failure(core::ErrorCode::start_failed,
                                             "start", "worker-task"));
    }
    return core::Status::success();
}

core::Status EspEspNowTransportComponent::stop() noexcept {
    started_.store(false);
    if (worker_task_ != nullptr) xTaskNotifyGive(worker_task_);
    for (std::size_t i = 0; i < 200U && !quiesced_.load(); ++i) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (!quiesced_.load()) {
        return core::Status::failure(failure(core::ErrorCode::stop_failed,
                                             "stop", "worker-timeout"));
    }
    worker_task_ = nullptr;
    instance_ = nullptr;
    if (receive_queue_ != nullptr) vQueueDelete(receive_queue_);
    if (request_done_ != nullptr) vSemaphoreDelete(request_done_);
    receive_queue_ = nullptr;
    request_done_ = nullptr;
    return core::Status::success();
}

bool EspEspNowTransportComponent::callbacks_quiesced() const noexcept {
    return quiesced_.load();
}

void EspEspNowTransportComponent::enable_control() noexcept {
    control_ready_.store(true);
    if (worker_task_ != nullptr) xTaskNotifyGive(worker_task_);
}

core::Status EspEspNowTransportComponent::read_parameter(
    std::string_view id, core::ScalarValue& output) noexcept {
    if (id == "enabled") output = core::ScalarValue::from_bool(enabled_.load());
    else if (id == "active") output = core::ScalarValue::from_bool(active_.load());
    else if (id == "address") output = core::ScalarValue::from_string(address_text_.data());
    else if (id == "peer_mac") {
        const auto peer = peer_packed_.load();
        if (peer != 0U) format_mac(peer, peer_text_);
        else peer_text_[0] = '\0';
        output = core::ScalarValue::from_string(peer_text_.data());
    } else if (id == "received_requests")
        output = core::ScalarValue::from_integer(received_requests_.load());
    else if (id == "sent_responses")
        output = core::ScalarValue::from_integer(sent_responses_.load());
    else if (id == "rejected_packets")
        output = core::ScalarValue::from_integer(rejected_packets_.load());
    else if (id == "retries") output = core::ScalarValue::from_integer(retries_.load());
    else return core::Status::failure(failure(core::ErrorCode::not_found,
                                              "read", "parameter"));
    return core::Status::success();
}

core::Status EspEspNowTransportComponent::write_parameter(
    std::string_view id, const core::ScalarValue& value) noexcept {
    const bool setting_enabled = id == "enabled" && value.type == core::ValueType::boolean;
    const bool setting_peer = id == "peer_mac" && value.type == core::ValueType::string;
    if (!setting_enabled && !setting_peer) {
        return core::Status::failure(failure(core::ErrorCode::invalid_argument,
                                             "write", "parameter-or-type"));
    }
    bool enabled = enabled_.load();
    auto peer = peer_packed_.load();
    if (setting_enabled) enabled = value.boolean;
    if (setting_peer && !parse_mac(value.string, peer)) {
        return core::Status::failure(failure(core::ErrorCode::invalid_argument,
                                             "write", "peer-mac"));
    }
    if (enabled && peer == 0U) {
        return core::Status::failure(failure(core::ErrorCode::invalid_state,
                                             "write", "peer-mac-required"));
    }
    const auto saved = save_settings(enabled, peer);
    if (!saved) return saved;
    peer_packed_.store(peer);
    enabled_.store(enabled);
    reconfigure_.store(true);
    if (worker_task_ != nullptr) xTaskNotifyGive(worker_task_);
    return core::Status::success();
}

core::Status EspEspNowTransportComponent::invoke_action(
    std::string_view id, std::span<const core::ScalarValue> arguments,
    std::span<core::ScalarValue> outputs, std::size_t& output_count) noexcept {
    output_count = 0;
    const bool read = id == "remote_read";
    const bool write_string = id == "remote_write";
    const bool write_integer = id == "remote_write_integer";
    const bool write_boolean = id == "remote_write_boolean";
    const bool write_number = id == "remote_write_number";
    const bool write = write_string || write_integer || write_boolean || write_number;
    if ((!read && !write) || arguments.size() != (read ? 2U : 3U) ||
        (read && outputs.empty()) ||
        arguments[0].type != core::ValueType::string ||
        arguments[1].type != core::ValueType::string ||
        (write && arguments[2].type !=
            (write_string ? core::ValueType::string
             : write_integer ? core::ValueType::integer
             : write_boolean ? core::ValueType::boolean
                             : core::ValueType::number)) ||
        arguments[0].string.empty() || arguments[1].string.empty() ||
        xTaskGetCurrentTaskHandle() == worker_task_) {
        return core::Status::failure(failure(core::ErrorCode::invalid_argument,
                                             "remote-read", "arguments"));
    }
    bool expected = false;
    if (!active_.load() ||
        !request_busy_.compare_exchange_strong(expected, true)) {
        return core::Status::failure(failure(core::ErrorCode::invalid_state,
                                             "remote-read", "inactive-or-busy"));
    }
    while (xSemaphoreTake(request_done_, 0) == pdTRUE) {}
    request_expects_value_ = read;
    const ControlMessage request_message{read ? core::ControlOperation::read_parameter
                                              : core::ControlOperation::write_parameter,
        core::ErrorDomain::none, core::ErrorCode::none,
        arguments[0].string, arguments[1].string, {},
        write ? arguments.subspan(2, 1) : std::span<const core::ScalarValue>{}};
    const auto payload = encode_control_message(request_message, send_payload_);
    if (!payload) {
        request_busy_.store(false);
        return core::Status::failure(payload.error());
    }
    request_id_ = esp_random();
    if (request_id_ == 0U) request_id_ = 1U;
    const auto encoded = encode_envelope(
        {EnvelopeKind::request, PayloadType::control, request_id_, 0,
         {send_payload_.data(), payload.value()}}, request_frame_);
    if (!encoded) {
        request_busy_.store(false);
        return core::Status::failure(encoded.error());
    }
    request_size_ = encoded.value();
    request_pending_.store(true);
    xTaskNotifyGive(worker_task_);
    if (xSemaphoreTake(request_done_, pdMS_TO_TICKS(3000)) != pdTRUE) {
        request_cancel_.store(true);
        xTaskNotifyGive(worker_task_);
        return core::Status::failure(failure(core::ErrorCode::invalid_state,
                                             "remote-read", "timeout"));
    }
    const bool success = request_success_.load();
    if (success && read) {
        outputs[0] = remote_value_;
        output_count = 1;
    }
    request_busy_.store(false);
    return success ? core::Status::success()
                   : core::Status::failure(failure(core::ErrorCode::invalid_state,
                                                    "remote-read", "peer-error"));
}

bool EspEspNowTransportComponent::activate() noexcept {
    if (esp_now_init() != ESP_OK) return false;
    esp_now_peer_info_t peer{};
    active_peer_ = peer_packed_.load();
    unpack_mac(active_peer_, peer.peer_addr);
    peer.channel = 0;
    peer.ifidx = WIFI_IF_STA;
    if (esp_now_add_peer(&peer) != ESP_OK ||
        esp_now_register_recv_cb(receive_callback) != ESP_OK) {
        static_cast<void>(esp_now_deinit());
        return false;
    }
    active_.store(true);
    ESP_LOGI(kTag, "V2 peer radio active");
    return true;
}

void EspEspNowTransportComponent::deactivate() noexcept {
    if (!active_.exchange(false)) return;
    static_cast<void>(esp_now_unregister_recv_cb());
    static_cast<void>(esp_now_deinit());
    receive_.reset();
    transmit_.reset();
    response_waiting_ = false;
    if (request_busy_.load()) complete_request(false);
    ESP_LOGI(kTag, "V2 peer radio stopped");
}

void EspEspNowTransportComponent::begin_transmit(
    std::span<const std::byte> envelope, bool response) noexcept {
    transmit_sequence_ = next_sequence_++;
    if (next_sequence_ == 0U) ++next_sequence_;
    if (transmit_sequence_ == 0U) transmit_sequence_ = next_sequence_++;
    if (!transmit_.begin(session_, transmit_sequence_, envelope)) return;
    transmit_is_response_ = response;
    attempts_ = 0;
    transmit_started_ms_ = 0;
}

void EspEspNowTransportComponent::send_fragments() noexcept {
    std::uint8_t peer[6]{};
    unpack_mac(active_peer_, peer);
    for (std::size_t i = 0; i < transmit_.fragment_count(); ++i) {
        const auto size = transmit_.fragment(i, send_packet_);
        if (size == 0U || esp_now_send(peer,
            reinterpret_cast<const std::uint8_t*>(send_packet_.data()), size) != ESP_OK) {
            ++rejected_packets_;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(12));
    }
    ++attempts_;
    if (attempts_ > 1U) ++retries_;
    transmit_started_ms_ = now_ms();
}

void EspEspNowTransportComponent::complete_request(
    bool success, const core::ScalarValue& value) noexcept {
    if (!request_busy_.load()) return;
    remote_value_ = value;
    if (success && value.type == core::ValueType::string) {
        const auto count = std::min(value.string.size(), response_text_.size() - 1U);
        std::memcpy(response_text_.data(), value.string.data(), count);
        response_text_[count] = '\0';
        remote_value_.string = {response_text_.data(), count};
    }
    request_success_.store(success);
    xSemaphoreGive(request_done_);
}

void EspEspNowTransportComponent::handle_message() noexcept {
    const auto envelope = decode_envelope(receive_.envelope());
    if (!envelope || envelope.value().payload_type != PayloadType::control) {
        ++rejected_packets_;
        return;
    }
    if (envelope.value().kind == EnvelopeKind::request) {
        const auto result = endpoint_.handle_request(receive_.envelope(), response_frame_);
        if (!result) {
            ++rejected_packets_;
            return;
        }
        ++received_requests_;
        response_size_ = result.value();
        response_waiting_ = true;
        if (!transmit_.pending()) {
            response_waiting_ = false;
            begin_transmit({response_frame_.data(), response_size_}, true);
        }
    } else if ((envelope.value().kind == EnvelopeKind::response ||
                envelope.value().kind == EnvelopeKind::error) &&
               request_busy_.load() && envelope.value().request_id == request_id_) {
        const auto message = decode_control_message(envelope.value().payload);
        if (!message) {
            complete_request(false);
        } else {
            const bool okay = envelope.value().kind == EnvelopeKind::response &&
                              message.value().error_code == core::ErrorCode::none &&
                              message.value().value_count == (request_expects_value_ ? 1U : 0U);
            complete_request(okay, okay && request_expects_value_
                                       ? message.value().values[0] : core::ScalarValue{});
        }
        if (transmit_.pending() && !transmit_is_response_) transmit_.reset();
    }
}

void EspEspNowTransportComponent::handle_packet(const ReceivedPacket& packet) noexcept {
    std::uint8_t peer[6]{};
    unpack_mac(active_peer_, peer);
    if (std::memcmp(peer, packet.source.data(), 6U) != 0) {
        ++rejected_packets_;
        return;
    }
    EspNowPacketView view{};
    if (!decode_espnow_packet({packet.bytes.data(), packet.size}, view)) {
        ++rejected_packets_;
        return;
    }
    if (view.kind == EspNowPacketKind::acknowledgement) {
        if (transmit_.acknowledge(view) && transmit_is_response_) ++sent_responses_;
        return;
    }
    const auto result = receive_.accept(view);
    if (result == EspNowReceiveResult::rejected) {
        ++rejected_packets_;
        return;
    }
    if (result == EspNowReceiveResult::complete ||
        result == EspNowReceiveResult::duplicate) {
        std::array<std::byte, kEspNowHeaderBytes> ack{};
        const auto size = encode_espnow_ack(view.session, view.sequence, ack);
        static_cast<void>(esp_now_send(peer,
            reinterpret_cast<const std::uint8_t*>(ack.data()), size));
        if (result == EspNowReceiveResult::complete) handle_message();
    }
}

void EspEspNowTransportComponent::run() noexcept {
    while (started_.load()) {
        const bool should_run = control_ready_.load() && enabled_.load() &&
            peer_packed_.load() != 0U &&
            wifi_->connection_state() == network::WifiConnectionState::connected;
        // Let a peer-initiated settings change finish its response before
        // deinitializing the radio or switching to the new peer address.
        const bool replying = transmit_.pending() && transmit_is_response_;
        if ((!should_run || reconfigure_.load()) && active_.load() && !replying) {
            reconfigure_.store(false);
            deactivate();
        }
        if (should_run && !active_.load()) static_cast<void>(activate());
        if (active_.load()) {
            if (request_cancel_.exchange(false)) {
                if (!transmit_is_response_) transmit_.reset();
                request_pending_.store(false);
                request_busy_.store(false);
            }
            if (request_pending_.exchange(false)) {
                begin_transmit({request_frame_.data(), request_size_}, false);
            }
            ReceivedPacket packet{};
            while (xQueueReceive(receive_queue_, &packet, 0) == pdTRUE) {
                handle_packet(packet);
            }
            if (response_waiting_ && !transmit_.pending()) {
                response_waiting_ = false;
                begin_transmit({response_frame_.data(), response_size_}, true);
            }
            if (transmit_.pending() &&
                (attempts_ == 0U || now_ms() - transmit_started_ms_ >= 250U)) {
                if (attempts_ >= 6U) {
                    transmit_.reset();
                    if (!transmit_is_response_) complete_request(false);
                } else {
                    send_fragments();
                }
            }
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
    }
    deactivate();
    quiesced_.store(true);
    vTaskDelete(nullptr);
}

void EspEspNowTransportComponent::worker_entry(void* context) noexcept {
    static_cast<EspEspNowTransportComponent*>(context)->run();
}

void EspEspNowTransportComponent::receive_callback(
    const esp_now_recv_info_t* info, const std::uint8_t* data, int length) noexcept {
    auto* self = instance_;
    if (self == nullptr || !self->active_.load() || info == nullptr ||
        info->src_addr == nullptr || data == nullptr || length <= 0 ||
        length > static_cast<int>(kEspNowPacketBytes)) return;
    ReceivedPacket packet{};
    std::memcpy(packet.source.data(), info->src_addr, 6U);
    packet.size = static_cast<std::uint16_t>(length);
    std::memcpy(packet.bytes.data(), data, static_cast<std::size_t>(length));
    if (xQueueSend(self->receive_queue_, &packet, 0) != pdTRUE) {
        ++self->rejected_packets_;
    }
}

} // namespace blip::transport
