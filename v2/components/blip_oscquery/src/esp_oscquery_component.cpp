#include "blip/oscquery/esp_oscquery_component.hpp"

#include "blip/oscquery/oscquery.hpp"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <limits>

namespace blip::oscquery {
namespace {

constexpr char kTag[] = "blip_oscquery";
constexpr std::array<std::string_view, 2> kProvidedServices{"transport.osc", "discovery.oscquery"};
constexpr std::array<std::string_view, 4> kRequiredServices{"control.dispatch", "network.http",
                                                            "storage.web_assets", "firmware.ota"};
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

class HttpChunkSink final : public TextSink, public resources::SnapshotSink {
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

[[nodiscard]] bool request_accepts_html(httpd_req_t* request) noexcept {
    std::array<char, 128> accept{};
    const std::size_t size = httpd_req_get_hdr_value_len(request, "Accept");
    return size != 0U && size < accept.size() &&
           httpd_req_get_hdr_value_str(request, "Accept", accept.data(), accept.size()) == ESP_OK &&
           std::string_view{accept.data(), size}.find("text/html") != std::string_view::npos;
}

template <std::size_t Capacity>
[[nodiscard]] bool read_header(httpd_req_t* request, const char* name,
                               std::array<char, Capacity>& output,
                               std::string_view& value) noexcept {
    const std::size_t size = httpd_req_get_hdr_value_len(request, name);
    if (size == 0U || size >= output.size() ||
        httpd_req_get_hdr_value_str(request, name, output.data(), output.size()) != ESP_OK) {
        return false;
    }
    value = {output.data(), size};
    return true;
}

[[nodiscard]] bool json_string(std::string_view source, std::string_view key,
                               std::string_view& value) noexcept {
    std::array<char, 48> pattern{};
    if (key.size() + 2U > pattern.size()) return false;
    pattern[0] = '\"';
    std::copy(key.begin(), key.end(), pattern.begin() + 1);
    pattern[key.size() + 1U] = '\"';
    const auto found = source.find({pattern.data(), key.size() + 2U});
    if (found == std::string_view::npos) return false;
    const auto colon = source.find(':', found + key.size() + 2U);
    const auto opening = colon == std::string_view::npos ? colon : source.find('\"', colon + 1U);
    const auto closing = opening == std::string_view::npos ? opening : source.find('\"', opening + 1U);
    if (opening == std::string_view::npos || closing == std::string_view::npos) return false;
    value = source.substr(opening + 1U, closing - opening - 1U);
    return value.find_first_of("\\\"\r\n\0") == std::string_view::npos;
}

[[nodiscard]] bool json_u32(std::string_view source, std::string_view key,
                            std::uint32_t& value) noexcept {
    std::array<char, 48> pattern{};
    if (key.size() + 2U > pattern.size()) return false;
    pattern[0] = '\"';
    std::copy(key.begin(), key.end(), pattern.begin() + 1);
    pattern[key.size() + 1U] = '\"';
    const auto found = source.find({pattern.data(), key.size() + 2U});
    const auto colon = found == std::string_view::npos ? found : source.find(':', found + key.size() + 2U);
    if (colon == std::string_view::npos) return false;
    std::size_t cursor = colon + 1U;
    while (cursor < source.size() && (source[cursor] == ' ' || source[cursor] == '\t')) ++cursor;
    const auto parsed = std::from_chars(source.data() + cursor, source.data() + source.size(), value);
    return parsed.ec == std::errc{};
}

[[nodiscard]] const core::ParameterDescriptor*
resource_parameter(const core::RegistryView& registry, std::string_view owner,
                   std::string_view& component, std::string_view& parameter) noexcept {
    const auto separator = owner.rfind(':');
    if (separator == std::string_view::npos) return nullptr;
    component = owner.substr(0U, separator);
    parameter = owner.substr(separator + 1U);
    for (std::size_t index = 0; index < registry.component_count(); ++index) {
        const auto& descriptor = registry.component_descriptor(index);
        if (descriptor.id != component) continue;
        const auto found = std::find_if(descriptor.parameters.begin(), descriptor.parameters.end(),
                                        [parameter](const core::ParameterDescriptor& candidate) {
                                            return candidate.id == parameter;
                                        });
        return found == descriptor.parameters.end() ? nullptr : &*found;
    }
    return nullptr;
}

class ResourceControlHooks final : public resources::TransactionHooks {
  public:
    ResourceControlHooks(core::ControlService& controls, std::string_view requester_component,
                         std::string_view requester_parameter, std::string_view previous_component,
                         std::string_view previous_parameter, std::int64_t target_gpio,
                         resources::ReassignmentKind kind) noexcept
        : controls_(&controls), requester_component_(requester_component),
          requester_parameter_(requester_parameter), previous_component_(previous_component),
          previous_parameter_(previous_parameter), target_gpio_(target_gpio), kind_(kind) {}

    [[nodiscard]] core::Status run(resources::TransactionStage stage,
                                   const resources::ReassignmentRequest&) noexcept override {
        if (stage == resources::TransactionStage::stop_components) {
            auto status = read(requester_component_, requester_parameter_, requester_old_);
            return status ? read(previous_component_, previous_parameter_, previous_old_) : status;
        }
        if (stage == resources::TransactionStage::persist_settings) {
            auto status = write(requester_component_, requester_parameter_, target_gpio_);
            if (!status) return status;
            requester_written_ = true;
            const std::int64_t previous_value =
                kind_ == resources::ReassignmentKind::swap ? requester_old_ : -1;
            status = write(previous_component_, previous_parameter_, previous_value);
            if (status) previous_written_ = true;
            return status;
        }
        return core::Status::success();
    }

    void rollback(resources::TransactionStage, const resources::ReassignmentRequest&) noexcept override {
        if (previous_written_) static_cast<void>(write(previous_component_, previous_parameter_, previous_old_));
        if (requester_written_) static_cast<void>(write(requester_component_, requester_parameter_, requester_old_));
    }

  private:
    [[nodiscard]] core::Status read(std::string_view component, std::string_view parameter,
                                    std::int64_t& value) noexcept {
        core::ControlResponse response{};
        const auto status = controls_->execute(
            {core::ControlOperation::read_parameter, component, parameter, {}}, response);
        if (!status) return status;
        if (response.value_count != 1U || response.values[0].type != core::ValueType::integer) {
            return core::Status::failure({core::ErrorDomain::resource,
                                          core::ErrorCode::validation_failed, component,
                                          "resource.reassign", "pin-not-integer"});
        }
        value = response.values[0].integer;
        return core::Status::success();
    }
    [[nodiscard]] core::Status write(std::string_view component, std::string_view parameter,
                                     std::int64_t value) noexcept {
        const core::ScalarValue scalar = core::ScalarValue::from_integer(value);
        core::ControlResponse response{};
        return controls_->execute(
            {core::ControlOperation::write_parameter, component, parameter,
             std::span<const core::ScalarValue>{&scalar, 1U}}, response);
    }

    core::ControlService* controls_{};
    std::string_view requester_component_{};
    std::string_view requester_parameter_{};
    std::string_view previous_component_{};
    std::string_view previous_parameter_{};
    std::int64_t target_gpio_{};
    std::int64_t requester_old_{};
    std::int64_t previous_old_{};
    resources::ReassignmentKind kind_{};
    bool requester_written_{};
    bool previous_written_{};
};

} // namespace

const core::ComponentDescriptor EspOscQueryComponent::descriptor_{oscquery_descriptor()};

EspOscQueryComponent::EspOscQueryComponent(const core::RegistryView& registry,
                                           core::ControlService& controls,
                                           network::EspWifiComponent& wifi,
                                           storage::WebAssetStore& web_assets,
                                           ota::UpdateService& updates,
                                           resources::DeviceBroker& resources,
                                           resources::BoardManifest board) noexcept
    : registry_(&registry), controls_(&controls), wifi_(&wifi), web_assets_(&web_assets),
      updates_(&updates), resources_(&resources), board_(board),
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
    task_ = xTaskCreateStatic(task_entry, "blip_osc", sizeof(task_stack_), this, 4,
                              task_stack_.data(), &task_storage_);
    if (task_ == nullptr) {
        started_.store(false);
        task_quiesced_.store(true);
        wifi_->clear_http_root_delegate(*this);
        close(socket_);
        socket_ = -1;
        return component_failure(core::ErrorCode::start_failed, "start", "udp-task");
    }
    const auto discovery = wifi_->advertise_osc(kOscPort);
    if (!discovery) {
        static_cast<void>(stop());
        return discovery;
    }
    ESP_LOGI(kTag, "OSC UDP port=%u and OSCQuery HTTP/WebSocket ready",
             static_cast<unsigned>(kOscPort));
    return core::Status::success();
}

core::Status EspOscQueryComponent::stop() noexcept {
    wifi_->withdraw_osc();
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
    const std::string_view request_uri{request->uri};
    const std::size_t query_separator = request_uri.find('?');
    const std::string_view path = request_uri.substr(0U, query_separator);
    const std::string_view query_text{query.data(), query_size};
    switch (route_http_get(path, !query_text.empty(), request_accepts_html(request))) {
    case HttpGetSurface::web_asset:
        return handle_asset_get(request, path);
    case HttpGetSurface::asset_status:
        return handle_asset_status(request);
    case HttpGetSurface::update_status:
        return handle_update_status(request);
    case HttpGetSurface::resource_status:
        return handle_resource_status(request);
    case HttpGetSurface::not_found:
        return httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, "unknown endpoint");
    case HttpGetSurface::oscquery:
        break;
    }
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "Access-Control-Allow-Origin", "*");
    HttpChunkSink sink{request};
    core::Status status = core::Status::success();
    if (query_text == "HOST_INFO") {
        status = write_oscquery_host_info(identity_, sink);
    } else if (query_text.empty() || query_text == "config=1" || query_text == "config=0") {
        status = write_oscquery_tree(*registry_, *controls_, query_text != "config=0", sink);
    } else {
        return httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, "unknown OSCQuery query");
    }
    return status ? sink.finish() : ESP_FAIL;
}

esp_err_t EspOscQueryComponent::handle_resource_status(httpd_req_t* request) noexcept {
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "Access-Control-Allow-Origin", "*");
    HttpChunkSink sink{request};
    const auto status = resources::write_resource_snapshot(*resources_, board_, sink);
    return status ? sink.finish() : ESP_FAIL;
}

esp_err_t EspOscQueryComponent::handle_resource_reassignment(httpd_req_t* request) noexcept {
    std::array<char, 512> body{};
    if (request->content_len == 0U || request->content_len >= body.size()) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "invalid reassignment body");
    }
    std::size_t received_total = 0U;
    while (received_total < request->content_len) {
        const int received = httpd_req_recv(request, body.data() + received_total,
                                            request->content_len - received_total);
        if (received <= 0) {
            return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "incomplete reassignment body");
        }
        received_total += static_cast<std::size_t>(received);
    }
    const std::string_view source{body.data(), received_total};
    std::string_view operation{};
    std::string_view requester{};
    std::string_view previous_owner{};
    std::string_view target_resource{};
    std::uint32_t revision{};
    if (!json_string(source, "operation", operation) ||
        !json_string(source, "requester", requester) ||
        !json_string(source, "previous_owner", previous_owner) ||
        !json_string(source, "target_resource", target_resource) ||
        !json_u32(source, "expected_revision", revision)) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "invalid reassignment fields");
    }
    const auto kind = operation == "swap" ? resources::ReassignmentKind::swap
                      : operation == "unassign-and-move"
                          ? resources::ReassignmentKind::unassign_and_move
                          : resources::ReassignmentKind::swap;
    if (operation != "swap" && operation != "unassign-and-move") {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "invalid reassignment operation");
    }
    std::string_view requester_component{};
    std::string_view requester_parameter{};
    std::string_view previous_component{};
    std::string_view previous_parameter{};
    const auto* requester_descriptor = resource_parameter(
        *registry_, requester, requester_component, requester_parameter);
    const auto* previous_descriptor = resource_parameter(
        *registry_, previous_owner, previous_component, previous_parameter);
    const auto* target = resources_->find_resource(target_resource);
    if (requester_descriptor == nullptr || previous_descriptor == nullptr || target == nullptr ||
        !requester_descriptor->resource_selector.present ||
        !previous_descriptor->resource_selector.present || target->gpio < 0 ||
        (kind == resources::ReassignmentKind::swap &&
         !requester_descriptor->resource_selector.supports_swap) ||
        (kind == resources::ReassignmentKind::unassign_and_move &&
         (!requester_descriptor->resource_selector.supports_move ||
          !previous_descriptor->resource_selector.optional))) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "reassignment not permitted");
    }
    const resources::ReassignmentRequest reassignment{
        revision,
        kind,
        requester,
        previous_owner,
        target_resource,
        requester_descriptor->resource_selector.required_capabilities,
        previous_descriptor->resource_selector.required_capabilities,
        previous_descriptor->resource_selector.optional,
    };
    ResourceControlHooks hooks{*controls_, requester_component, requester_parameter,
                               previous_component, previous_parameter, target->gpio, kind};
    const auto status = resources_->reassign(reassignment, hooks);
    if (!status) {
        if (status.error().detail == "stale-revision") {
            httpd_resp_set_status(request, "409 Conflict");
            return httpd_resp_sendstr(request, "stale-revision");
        }
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "reassignment rejected");
    }
    return handle_resource_status(request);
}

esp_err_t EspOscQueryComponent::handle_update_status(httpd_req_t* request) noexcept {
    const auto& status = updates_->status();
    std::array<char, 192> response{};
    const int size = std::snprintf(
        response.data(), response.size(),
        "{\"state\":\"%s\",\"received_bytes\":%lu,\"expected_bytes\":%lu,"
        "\"signature_enforced\":%s}",
        ota::update_state_name(status.state), static_cast<unsigned long>(status.received_bytes),
        static_cast<unsigned long>(status.expected_bytes),
        status.signature_enforced ? "true" : "false");
    if (size <= 0 || static_cast<std::size_t>(size) >= response.size()) {
        return ESP_FAIL;
    }
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, response.data(), size);
}

esp_err_t EspOscQueryComponent::handle_firmware_upload(httpd_req_t* request) noexcept {
    std::array<char, 65> sha_text{};
    std::array<char, 32> project_text{};
    std::array<char, 32> version_text{};
    std::array<char, 16> target_text{};
    std::array<char, 32> profile_text{};
    std::string_view sha{};
    std::string_view project{};
    std::string_view version{};
    std::string_view target{};
    std::string_view profile{};
    if (!read_header(request, "X-BLIP-SHA256", sha_text, sha) ||
        !read_header(request, "X-BLIP-Project", project_text, project) ||
        !read_header(request, "X-BLIP-Version", version_text, version) ||
        !read_header(request, "X-BLIP-Target", target_text, target) ||
        !read_header(request, "X-BLIP-Profile", profile_text, profile)) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "missing update metadata");
    }
    const auto digest = ota::parse_sha256_hex(sha);
    if (!digest) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "invalid SHA-256");
    }
    const ota::UpdateManifest manifest{
        request->content_len, digest.value(), project, version, target, profile};
    auto status = updates_->begin(manifest);
    if (!status) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "update rejected");
    }
    std::size_t received_total{};
    std::size_t timeouts{};
    while (received_total < manifest.image_size) {
        const std::size_t requested =
            std::min(http_asset_buffer_.size(), manifest.image_size - received_total);
        const int received =
            httpd_req_recv(request, reinterpret_cast<char*>(http_asset_buffer_.data()), requested);
        if (received == HTTPD_SOCK_ERR_TIMEOUT && timeouts++ < 8U) {
            continue;
        }
        if (received <= 0) {
            updates_->cancel();
            return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "incomplete image");
        }
        timeouts = 0;
        status = updates_->append({http_asset_buffer_.data(), static_cast<std::size_t>(received)});
        if (!status) {
            return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                       "flash write failed");
        }
        received_total += static_cast<std::size_t>(received);
    }
    status = updates_->finish();
    if (!status) {
        const bool invalid = status.error().code == core::ErrorCode::corrupt_data ||
                             status.error().code == core::ErrorCode::incompatible_version ||
                             status.error().code == core::ErrorCode::verification_failed;
        return httpd_resp_send_err(
            request, invalid ? HTTPD_400_BAD_REQUEST : HTTPD_500_INTERNAL_SERVER_ERROR,
            invalid ? "image verification failed" : "image activation failed");
    }
    httpd_resp_set_status(request, "202 Accepted");
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    const esp_err_t sent =
        httpd_resp_send(request, "{\"accepted\":true,\"restarting\":true}", HTTPD_RESP_USE_STRLEN);
    if (sent == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
    }
    return sent;
}

esp_err_t EspOscQueryComponent::handle_asset_get(httpd_req_t* request,
                                                 std::string_view path) noexcept {
    const auto* asset = web_assets_->find(path);
    if (asset == nullptr) {
        return httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, "asset not found");
    }
    const auto content_type = storage::content_type_name(asset->content_type);
    const auto cache_control = storage::cache_control_name(asset->cache_policy);
    httpd_resp_set_type(request, content_type.data());
    httpd_resp_set_hdr(request, "Cache-Control", cache_control.data());
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    if (asset->encoding == storage::WebContentEncoding::gzip) {
        httpd_resp_set_hdr(request, "Content-Encoding", "gzip");
    }
    if (asset->content_type == storage::WebContentType::html) {
        httpd_resp_set_hdr(request, "Content-Security-Policy",
                           "default-src 'self'; connect-src 'self' ws: wss:; style-src 'self'; "
                           "img-src 'self' data:; base-uri 'none'; frame-ancestors 'none'");
        httpd_resp_set_hdr(request, "Vary", "Accept, Accept-Encoding");
    }
    std::array<char, 32> etag{};
    const int etag_size = std::snprintf(etag.data(), etag.size(), "\"%08lx-%lu\"",
                                        static_cast<unsigned long>(asset->crc32),
                                        static_cast<unsigned long>(asset->stored_size));
    if (etag_size <= 0 || static_cast<std::size_t>(etag_size) >= etag.size()) {
        return ESP_FAIL;
    }
    httpd_resp_set_hdr(request, "ETag", etag.data());
    std::size_t offset{};
    while (offset < asset->stored_size) {
        const auto loaded = web_assets_->read(*asset, offset, http_asset_buffer_);
        if (!loaded || loaded.value() == 0U ||
            httpd_resp_send_chunk(request, reinterpret_cast<const char*>(http_asset_buffer_.data()),
                                  loaded.value()) != ESP_OK) {
            return ESP_FAIL;
        }
        offset += loaded.value();
    }
    return httpd_resp_send_chunk(request, nullptr, 0);
}

esp_err_t EspOscQueryComponent::handle_asset_status(httpd_req_t* request) noexcept {
    if (!web_assets_->active()) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "assets unavailable");
    }
    const auto& info = web_assets_->info();
    std::array<char, 160> response{};
    const int size = std::snprintf(
        response.data(), response.size(),
        "{\"format\":%u,\"bundle_version\":%lu,\"assets\":%lu,\"bytes\":%lu,"
        "\"crc32\":\"%08lx\"}",
        static_cast<unsigned>(storage::kWebAssetBundleFormatVersion),
        static_cast<unsigned long>(info.bundle_version),
        static_cast<unsigned long>(info.asset_count), static_cast<unsigned long>(info.total_size),
        static_cast<unsigned long>(info.bundle_crc32));
    if (size <= 0 || static_cast<std::size_t>(size) >= response.size()) {
        return ESP_FAIL;
    }
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, response.data(), size);
}

esp_err_t EspOscQueryComponent::handle_asset_upload(httpd_req_t* request) noexcept {
    const std::size_t expected = request->content_len;
    auto status = web_assets_->begin_install(expected);
    if (!status) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "invalid bundle size");
    }
    std::size_t received_total{};
    std::size_t timeouts{};
    while (received_total < expected) {
        const std::size_t requested =
            std::min(http_asset_buffer_.size(), expected - received_total);
        const int received =
            httpd_req_recv(request, reinterpret_cast<char*>(http_asset_buffer_.data()), requested);
        if (received == HTTPD_SOCK_ERR_TIMEOUT && timeouts++ < 8U) {
            continue;
        }
        if (received <= 0) {
            web_assets_->cancel_install();
            return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "incomplete bundle");
        }
        timeouts = 0;
        status = web_assets_->append_install(
            {http_asset_buffer_.data(), static_cast<std::size_t>(received)});
        if (!status) {
            web_assets_->cancel_install();
            return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                       "bundle write failed");
        }
        received_total += static_cast<std::size_t>(received);
    }
    const auto installed = web_assets_->finish_install();
    if (!installed) {
        const auto code = installed.error().code;
        const bool invalid = code == core::ErrorCode::corrupt_data ||
                             code == core::ErrorCode::incompatible_version ||
                             code == core::ErrorCode::verification_failed;
        return httpd_resp_send_err(
            request, invalid ? HTTPD_400_BAD_REQUEST : HTTPD_500_INTERNAL_SERVER_ERROR,
            invalid ? "bundle verification failed" : "bundle commit failed");
    }
    httpd_resp_set_status(request, "204 No Content");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, nullptr, 0);
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
    esp_err_t result{};
    if (websocket) {
        result = handle_websocket(request);
    } else if (request->method == HTTP_PUT && std::string_view{request->uri} == "/api/web-assets") {
        result = handle_asset_upload(request);
    } else if (request->method == HTTP_PUT && std::string_view{request->uri} == "/api/firmware") {
        result = handle_firmware_upload(request);
    } else if (request->method == HTTP_POST &&
               std::string_view{request->uri} == "/api/resources/reassign") {
        result = handle_resource_reassignment(request);
    } else {
        result = handle_http_get(request);
    }
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
