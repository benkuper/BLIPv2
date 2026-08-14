#include "blip/oscquery/esp_oscquery_component.hpp"

#include "blip/oscquery/oscquery.hpp"
#include "esp_log.h"
#include "esp_mac.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>

namespace blip::oscquery {
namespace {

constexpr char kTag[] = "blip_oscquery";
constexpr std::array<std::string_view, 2> kProvidedServices{"transport.osc", "discovery.oscquery"};
constexpr std::array<std::string_view, 2> kRequiredServices{"control.dispatch", "network.http"};
constexpr std::array<core::MetadataEntry, 4> kMetadata{{
    {"legacy_path", "/comm/osc"},
    {"osc_transport", "udp"},
    {"osc_port", "9000"},
    {"oscquery_transport", "http-websocket"},
}};
constexpr std::array<core::ParameterDescriptor, 5> kParameters{{
    {"port",
     "OSC UDP port",
     core::ValueType::integer,
     core::Access::read_only,
     false,
     core::ScalarValue::from_integer(EspOscQueryComponent::kOscPort),
     {true, EspOscQueryComponent::kOscPort, EspOscQueryComponent::kOscPort, 1},
     "port"},
    {"received_packets",
     "Received OSC packets",
     core::ValueType::integer,
     core::Access::read_only,
     false,
     core::ScalarValue::from_integer(0),
     {},
     "packets"},
    {"rejected_packets",
     "Rejected OSC packets",
     core::ValueType::integer,
     core::Access::read_only,
     false,
     core::ScalarValue::from_integer(0),
     {},
     "packets"},
    {"task_stack_headroom",
     "OSC UDP task stack headroom",
     core::ValueType::integer,
     core::Access::read_only,
     false,
     core::ScalarValue::from_integer(0),
     {true, 0, EspOscQueryComponent::kTaskStackBytes, 1},
     "bytes"},
    {"http_stack_headroom",
     "OSCQuery HTTP task stack headroom",
     core::ValueType::integer,
     core::Access::read_only,
     false,
     core::ScalarValue::from_integer(0),
     {true, 0, network::EspWifiComponent::kPortalTaskStackBytes, 1},
     "bytes"},
}};
constexpr std::array<core::DiagnosticDescriptor, 4> kDiagnostics{{
    {"received_packets", core::ValueType::integer, "packets"},
    {"rejected_packets", core::ValueType::integer, "packets"},
    {"active_http_callbacks", core::ValueType::integer, "callbacks"},
    {"http_stack_headroom", core::ValueType::integer, "bytes"},
}};

[[nodiscard]] constexpr core::ComponentDescriptor oscquery_descriptor() noexcept {
    core::ComponentDescriptor descriptor{};
    descriptor.schema_version = 1;
    descriptor.id = "blip.oscquery";
    descriptor.display_name = "OSC and OSCQuery";
    descriptor.description =
        "Bounded OSC UDP control and registry-generated OSCQuery HTTP/WebSocket discovery";
    descriptor.metadata = kMetadata;
    descriptor.provided_services = kProvidedServices;
    descriptor.required_services = kRequiredServices;
    descriptor.parameters = kParameters;
    descriptor.diagnostics = kDiagnostics;
    descriptor.settings = {1, 1};
    descriptor.disable_policy = core::DisablePolicy::live;
    descriptor.supports_restart = true;
    descriptor.cost = {131072, 16384, EspOscQueryComponent::kTaskStackBytes};
    return descriptor;
}

[[nodiscard]] core::Status component_failure(core::ErrorCode code, std::string_view operation,
                                             std::string_view detail) noexcept {
    return core::Status::failure(
        {core::ErrorDomain::transport, code, "blip.oscquery", operation, detail});
}

void saturating_increment(std::atomic<std::uint32_t>& value) noexcept {
    std::uint32_t current = value.load();
    while (current != std::numeric_limits<std::uint32_t>::max() &&
           !value.compare_exchange_weak(current, current + 1U)) {
    }
}

void record_minimum(std::atomic<std::uint32_t>& value, std::uint32_t sample) noexcept {
    std::uint32_t current = value.load();
    while (sample < current && !value.compare_exchange_weak(current, sample)) {
    }
}

class HttpChunkSink final : public TextSink {
  public:
    explicit HttpChunkSink(httpd_req_t* request) noexcept : request_(request) {}

    [[nodiscard]] bool write(std::string_view text) noexcept override {
        while (ok_ && !text.empty()) {
            const std::size_t copied = std::min(text.size(), buffer_.size() - size_);
            std::copy_n(text.begin(), copied, buffer_.begin() + size_);
            size_ += copied;
            text.remove_prefix(copied);
            if (size_ == buffer_.size()) {
                flush();
            }
        }
        return ok_;
    }

    [[nodiscard]] esp_err_t finish() noexcept {
        flush();
        return ok_ ? httpd_resp_send_chunk(request_, nullptr, 0) : ESP_FAIL;
    }

  private:
    void flush() noexcept {
        if (ok_ && size_ != 0U &&
            httpd_resp_send_chunk(request_, buffer_.data(), size_) != ESP_OK) {
            ok_ = false;
        }
        size_ = 0;
    }

    httpd_req_t* request_{};
    std::array<char, 512> buffer_{};
    std::size_t size_{};
    bool ok_{true};
};

} // namespace

const core::ComponentDescriptor EspOscQueryComponent::descriptor_{oscquery_descriptor()};

EspOscQueryComponent::EspOscQueryComponent(const core::RegistryView& registry,
                                           core::ControlService& controls,
                                           network::EspWifiComponent& wifi) noexcept
    : registry_(&registry), controls_(&controls), wifi_(&wifi),
      identity_{
          {device_id_.data(), device_id_.size() - 1U}, "BLIP V2", "BLIP V2", "0.1.0", kOscPort},
      udp_endpoint_(registry, controls, identity_),
      websocket_endpoint_(registry, controls, identity_) {}

const core::ComponentDescriptor& EspOscQueryComponent::descriptor() const noexcept {
    return descriptor_;
}

core::Status EspOscQueryComponent::start(const core::StartContext&) noexcept {
    if (started_.load()) {
        return core::Status::success();
    }
    std::array<std::uint8_t, 6> mac{};
    if (esp_read_mac(mac.data(), ESP_MAC_WIFI_STA) != ESP_OK) {
        return component_failure(core::ErrorCode::start_failed, "start", "read-mac");
    }
    const int identity_size =
        std::snprintf(device_id_.data(), device_id_.size(), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0],
                      mac[1], mac[2], mac[3], mac[4], mac[5]);
    if (identity_size != static_cast<int>(device_id_.size() - 1U)) {
        return component_failure(core::ErrorCode::start_failed, "start", "format-identity");
    }

    socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (socket_ < 0) {
        return component_failure(core::ErrorCode::start_failed, "start", "udp-socket");
    }
    const int reuse = 1;
    static_cast<void>(setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)));
    const timeval timeout{.tv_sec = 0, .tv_usec = 250000};
    static_cast<void>(setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(kOscPort);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(socket_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        close(socket_);
        socket_ = -1;
        return component_failure(core::ErrorCode::start_failed, "start", "udp-bind");
    }
    if (!wifi_->set_http_root_delegate(*this)) {
        close(socket_);
        socket_ = -1;
        return component_failure(core::ErrorCode::resource_conflict, "start", "http-root");
    }
    started_.store(true);
    task_quiesced_.store(false);
    task_ = xTaskCreateStatic(task_entry, "blip_osc", task_stack_.size(), this, 4,
                              task_stack_.data(), &task_storage_);
    if (task_ == nullptr) {
        started_.store(false);
        task_quiesced_.store(true);
        wifi_->clear_http_root_delegate(*this);
        close(socket_);
        socket_ = -1;
        return component_failure(core::ErrorCode::start_failed, "start", "udp-task");
    }
    ESP_LOGI(kTag, "OSC UDP port=%u and OSCQuery HTTP/WebSocket ready",
             static_cast<unsigned>(kOscPort));
    return core::Status::success();
}

core::Status EspOscQueryComponent::stop() noexcept {
    if (!started_.exchange(false) && task_quiesced_.load()) {
        return core::Status::success();
    }
    wifi_->clear_http_root_delegate(*this);
    for (std::size_t attempt = 0; attempt < 120U && !task_quiesced_.load(); ++attempt) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (socket_ >= 0) {
        close(socket_);
        socket_ = -1;
    }
    task_ = nullptr;
    for (std::size_t attempt = 0; attempt < 100U && active_http_callbacks_.load() != 0U;
         ++attempt) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return callbacks_quiesced()
               ? core::Status::success()
               : component_failure(core::ErrorCode::stop_failed, "stop", "callbacks-active");
}

bool EspOscQueryComponent::callbacks_quiesced() const noexcept {
    return task_quiesced_.load() && active_http_callbacks_.load() == 0U;
}

core::Status EspOscQueryComponent::read_parameter(std::string_view id,
                                                  core::ScalarValue& output) noexcept {
    if (!started_.load()) {
        return component_failure(core::ErrorCode::invalid_state, "read-parameter", "not-started");
    }
    if (id == "port") {
        output = core::ScalarValue::from_integer(kOscPort);
    } else if (id == "received_packets") {
        output = core::ScalarValue::from_integer(received_packets_.load());
    } else if (id == "rejected_packets") {
        output = core::ScalarValue::from_integer(rejected_packets_.load());
    } else if (id == "task_stack_headroom") {
        output = core::ScalarValue::from_integer(task_stack_headroom_bytes());
    } else if (id == "http_stack_headroom") {
        output = core::ScalarValue::from_integer(http_stack_headroom_bytes_.load());
    } else {
        return component_failure(core::ErrorCode::not_found, "read-parameter", id);
    }
    return core::Status::success();
}

std::uint32_t EspOscQueryComponent::task_stack_headroom_bytes() const noexcept {
    return task_ == nullptr ? 0U
                            : static_cast<std::uint32_t>(uxTaskGetStackHighWaterMark(task_)) *
                                  sizeof(StackType_t);
}

std::string_view EspOscQueryComponent::local_ip() noexcept {
    std::size_t size{};
    if (!wifi_->local_ipv4(local_ip_buffer_, size)) {
        constexpr std::string_view unavailable{"0.0.0.0"};
        std::copy(unavailable.begin(), unavailable.end(), local_ip_buffer_.begin());
        size = unavailable.size();
        local_ip_buffer_[size] = '\0';
    }
    return {local_ip_buffer_.data(), size};
}

void EspOscQueryComponent::handle_udp_packet(std::span<const std::byte> packet, const void* source,
                                             std::size_t source_size) noexcept {
    saturating_increment(received_packets_);
    const auto decoded = decode_osc_message(packet);
    if (!decoded) {
        saturating_increment(rejected_packets_);
        return;
    }
    OscMessage response{};
    bool reply{};
    const auto status = udp_endpoint_.handle(decoded.value(), local_ip(), true, response, reply);
    if (!status) {
        saturating_increment(rejected_packets_);
        return;
    }
    if (!reply) {
        return;
    }
    const auto encoded = encode_osc_message(response, udp_response_);
    if (!encoded ||
        sendto(socket_, udp_response_.data(), encoded.value(), 0,
               static_cast<const sockaddr*>(source), static_cast<socklen_t>(source_size)) < 0) {
        saturating_increment(rejected_packets_);
    }
}

void EspOscQueryComponent::run() noexcept {
    while (started_.load()) {
        sockaddr_storage source{};
        socklen_t source_size = sizeof(source);
        const int received = recvfrom(socket_, udp_packet_.data(), udp_packet_.size(), 0,
                                      reinterpret_cast<sockaddr*>(&source), &source_size);
        if (received > 0) {
            handle_udp_packet({udp_packet_.data(), static_cast<std::size_t>(received)}, &source,
                              source_size);
        }
    }
    task_quiesced_.store(true);
    vTaskDelete(nullptr);
}

esp_err_t EspOscQueryComponent::handle_http_get(httpd_req_t* request) noexcept {
    std::array<char, 32> query{};
    const std::size_t query_size = httpd_req_get_url_query_len(request);
    if (query_size >= query.size() ||
        (query_size != 0U &&
         httpd_req_get_url_query_str(request, query.data(), query.size()) != ESP_OK)) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "invalid query");
    }
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "Access-Control-Allow-Origin", "*");
    HttpChunkSink sink{request};
    core::Status status = core::Status::success();
    const std::string_view query_text{query.data(), query_size};
    if (query_text == "HOST_INFO") {
        status = write_oscquery_host_info(identity_, sink);
    } else if (query_text.empty() || query_text == "config=1" || query_text == "config=0") {
        status = write_oscquery_tree(*registry_, *controls_, query_text != "config=0", sink);
    } else {
        return httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, "unknown OSCQuery query");
    }
    return status ? sink.finish() : ESP_FAIL;
}

esp_err_t EspOscQueryComponent::handle_websocket(httpd_req_t* request) noexcept {
    httpd_ws_frame_t frame{};
    if (httpd_ws_recv_frame(request, &frame, 0) != ESP_OK || frame.len > websocket_packet_.size()) {
        return ESP_FAIL;
    }
    frame.payload = reinterpret_cast<std::uint8_t*>(websocket_packet_.data());
    if (frame.len != 0U && httpd_ws_recv_frame(request, &frame, frame.len) != ESP_OK) {
        return ESP_FAIL;
    }
    if (frame.type == HTTPD_WS_TYPE_TEXT) {
        constexpr char debug[] = "{\"ok\":true,\"kind\":\"debug\"}";
        httpd_ws_frame_t reply{
            .final = true,
            .fragmented = false,
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = reinterpret_cast<std::uint8_t*>(const_cast<char*>(debug)),
            .len = sizeof(debug) - 1U,
        };
        return httpd_ws_send_frame(request, &reply);
    }
    if (frame.type != HTTPD_WS_TYPE_BINARY) {
        return ESP_OK;
    }
    const auto decoded =
        decode_osc_message({websocket_packet_.data(), static_cast<std::size_t>(frame.len)});
    OscMessage response{};
    bool should_reply{};
    if (!decoded ||
        !websocket_endpoint_.handle(decoded.value(), local_ip(), false, response, should_reply)) {
        saturating_increment(rejected_packets_);
        constexpr char error[] = "{\"ok\":false,\"error\":\"invalid-osc\"}";
        httpd_ws_frame_t reply{
            .final = true,
            .fragmented = false,
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = reinterpret_cast<std::uint8_t*>(const_cast<char*>(error)),
            .len = sizeof(error) - 1U,
        };
        return httpd_ws_send_frame(request, &reply);
    }
    saturating_increment(received_packets_);
    if (!should_reply) {
        return ESP_OK;
    }
    const auto encoded = encode_osc_message(response, websocket_response_);
    if (!encoded) {
        return ESP_FAIL;
    }
    httpd_ws_frame_t reply{
        .final = true,
        .fragmented = false,
        .type = HTTPD_WS_TYPE_BINARY,
        .payload = reinterpret_cast<std::uint8_t*>(websocket_response_.data()),
        .len = encoded.value(),
    };
    return httpd_ws_send_frame(request, &reply);
}

esp_err_t EspOscQueryComponent::handle_http_root(httpd_req_t* request) noexcept {
    active_http_callbacks_.fetch_add(1U);
    const int socket = httpd_req_to_sockfd(request);
    const bool websocket =
        socket >= 0 && httpd_ws_get_fd_info(request->handle, socket) == HTTPD_WS_CLIENT_WEBSOCKET;
    const esp_err_t result = websocket ? handle_websocket(request) : handle_http_get(request);
    record_minimum(http_stack_headroom_bytes_,
                   static_cast<std::uint32_t>(uxTaskGetStackHighWaterMark(nullptr)) *
                       sizeof(StackType_t));
    active_http_callbacks_.fetch_sub(1U);
    return result;
}

void EspOscQueryComponent::task_entry(void* context) noexcept {
    static_cast<EspOscQueryComponent*>(context)->run();
}

} // namespace blip::oscquery
