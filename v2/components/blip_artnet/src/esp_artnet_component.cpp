#include "blip/artnet/esp_artnet_component.hpp"

#include "esp_mac.h"
#include "esp_timer.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include <array>
#include <cstring>
#include <limits>

namespace blip::artnet {
namespace {
constexpr std::array<std::string_view, 1> kProvided{"input.pixel-stream.artnet"};
constexpr std::array<std::string_view, 2> kRequired{"transport.wifi", "output.pixel-strip"};
constexpr std::array<core::MetadataEntry, 3> kMetadata{
    {{"udp_port", "6454"}, {"protocol", "Art-Net-4"}, {"optional", "true"}}};
constexpr std::array<core::ParameterDescriptor, 4> kParameters{{
    {"port",
     "Art-Net UDP port",
     core::ValueType::integer,
     core::Access::read_only,
     false,
     core::ScalarValue::from_integer(kPort),
     {},
     ""},
    {"accepted_packets",
     "Accepted Art-Net packets",
     core::ValueType::integer,
     core::Access::read_only,
     false,
     core::ScalarValue::from_integer(0),
     {},
     "packets"},
    {"rejected_packets",
     "Rejected Art-Net packets",
     core::ValueType::integer,
     core::Access::read_only,
     false,
     core::ScalarValue::from_integer(0),
     {},
     "packets"},
    {"discovery_replies",
     "ArtPoll replies",
     core::ValueType::integer,
     core::Access::read_only,
     false,
     core::ScalarValue::from_integer(0),
     {},
     "packets"},
}};
constexpr std::array<core::DiagnosticDescriptor, 3> kDiagnostics{{
    {"accepted_packets", core::ValueType::integer, "packets"},
    {"rejected_packets", core::ValueType::integer, "packets"},
    {"discovery_replies", core::ValueType::integer, "packets"},
}};
[[nodiscard]] constexpr core::ComponentDescriptor make_descriptor() noexcept {
    core::ComponentDescriptor descriptor{};
    descriptor.schema_version = 1U;
    descriptor.id = "blip.input.artnet";
    descriptor.display_name = "Art-Net input";
    descriptor.description = "Optional Art-Net DMX input and node discovery";
    descriptor.metadata = kMetadata;
    descriptor.provided_services = kProvided;
    descriptor.required_services = kRequired;
    descriptor.parameters = kParameters;
    descriptor.diagnostics = kDiagnostics;
    descriptor.settings = {1U, 1U};
    descriptor.disable_policy = core::DisablePolicy::live;
    descriptor.supports_restart = true;
    descriptor.cost = {16384U, 6144U, EspArtNetComponent::kTaskStackBytes};
    return descriptor;
}
[[nodiscard]] core::Status failure(core::ErrorCode code, std::string_view operation,
                                   std::string_view detail) noexcept {
    return core::Status::failure(
        {core::ErrorDomain::transport, code, "blip.input.artnet", operation, detail});
}
void increment(std::atomic<std::uint32_t>& value) noexcept {
    auto current = value.load();
    while (current != std::numeric_limits<std::uint32_t>::max() &&
           !value.compare_exchange_weak(current, current + 1U)) {
    }
}
} // namespace

const core::ComponentDescriptor EspArtNetComponent::descriptor_{make_descriptor()};

const core::ComponentDescriptor& EspArtNetComponent::descriptor() const noexcept {
    return descriptor_;
}

core::Status EspArtNetComponent::start(const core::StartContext&) noexcept {
    if (started_.load())
        return core::Status::success();
    socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (socket_ < 0)
        return failure(core::ErrorCode::start_failed, "start", "udp-socket");
    const int reuse = 1;
    static_cast<void>(setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)));
    const int broadcast = 1;
    static_cast<void>(setsockopt(socket_, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast)));
    const timeval timeout{.tv_sec = 0, .tv_usec = 100000};
    static_cast<void>(setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(kPort);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(socket_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        close(socket_);
        socket_ = -1;
        return failure(core::ErrorCode::resource_conflict, "start", "udp-port-in-use");
    }
    started_.store(true);
    task_quiesced_.store(false);
    task_ = xTaskCreateStatic(task_entry, "blip_artnet", sizeof(task_stack_), this, 4,
                              task_stack_.data(), &task_storage_);
    if (task_ == nullptr) {
        started_.store(false);
        task_quiesced_.store(true);
        close(socket_);
        socket_ = -1;
        return failure(core::ErrorCode::start_failed, "start", "udp-task");
    }
    return core::Status::success();
}

core::Status EspArtNetComponent::stop() noexcept {
    if (!started_.exchange(false) && task_quiesced_.load())
        return core::Status::success();
    for (std::size_t attempt = 0; attempt < 50U && !task_quiesced_.load(); ++attempt)
        vTaskDelay(pdMS_TO_TICKS(10));
    if (!task_quiesced_.load())
        return failure(core::ErrorCode::stop_failed, "stop", "task-active");
    if (socket_ >= 0) {
        close(socket_);
        socket_ = -1;
    }
    task_ = nullptr;
    return core::Status::success();
}

bool EspArtNetComponent::callbacks_quiesced() const noexcept { return task_quiesced_.load(); }

core::Status EspArtNetComponent::read_parameter(std::string_view id,
                                                core::ScalarValue& output) noexcept {
    if (id == "port")
        output = core::ScalarValue::from_integer(kPort);
    else if (id == "accepted_packets")
        output = core::ScalarValue::from_integer(accepted_.load());
    else if (id == "rejected_packets")
        output = core::ScalarValue::from_integer(rejected_.load());
    else if (id == "discovery_replies")
        output = core::ScalarValue::from_integer(discovery_replies_.load());
    else
        return failure(core::ErrorCode::not_found, "read-parameter", id);
    return core::Status::success();
}

void EspArtNetComponent::task_entry(void* context) noexcept {
    static_cast<EspArtNetComponent*>(context)->run();
}

void EspArtNetComponent::run() noexcept {
    while (started_.load()) {
        sockaddr_storage source{};
        socklen_t source_size = sizeof(source);
        const auto received = recvfrom(socket_, packet_.data(), packet_.size(), 0,
                                       reinterpret_cast<sockaddr*>(&source), &source_size);
        if (received <= 0)
            continue;
        const auto packet = parse(
            std::span<const std::byte>{packet_.data(), static_cast<std::size_t>(received)});
        if (!packet) {
            increment(rejected_);
            continue;
        }
        if (packet.value().kind == PacketKind::poll) {
            std::array<char, 16> ip_text{};
            std::size_t ip_size{};
            in_addr ip{};
            if (!wifi_->local_ipv4(ip_text, ip_size) ||
                inet_pton(AF_INET, ip_text.data(), &ip) != 1 ||
                esp_read_mac(identity_.mac.data(), ESP_MAC_WIFI_STA) != ESP_OK) {
                increment(rejected_);
                continue;
            }
            std::memcpy(identity_.ipv4.data(), &ip.s_addr, identity_.ipv4.size());
            const auto encoded = encode_poll_reply(identity_, response_);
            if (!encoded || sendto(socket_, response_.data(), encoded.value(), 0,
                                   reinterpret_cast<const sockaddr*>(&source), source_size) < 0) {
                increment(rejected_);
                continue;
            }
            increment(discovery_replies_);
            increment(accepted_);
            continue;
        }
        if (packet.value().kind == PacketKind::sync ||
            packet.value().universe != mapping_.universe) {
            increment(accepted_);
            continue;
        }
        core::ScalarValue configured_pixels{};
        if (!output_->read_parameter("pixels", configured_pixels) ||
            configured_pixels.type != core::ValueType::integer ||
            configured_pixels.integer <= mapping_.start_pixel) {
            increment(rejected_);
            continue;
        }
        const auto maximum_pixels =
            static_cast<std::size_t>(configured_pixels.integer - mapping_.start_pixel);
        const auto update = map_dmx(packet.value(), mapping_, maximum_pixels);
        if (!update) {
            increment(rejected_);
            continue;
        }
        if (!update.value().channels.empty() &&
            !output_->ingest_stream(update.value().sequence, update.value().start_pixel,
                                    update.value().channels,
                                    update.value().channels_per_pixel,
                                    update.value().sixteen_bit,
                                    static_cast<std::uint64_t>(esp_timer_get_time()))) {
            increment(rejected_);
            continue;
        }
        increment(accepted_);
    }
    task_quiesced_.store(true);
    vTaskDelete(nullptr);
}

} // namespace blip::artnet
