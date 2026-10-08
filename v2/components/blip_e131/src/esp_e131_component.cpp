#include "blip/e131/esp_e131_component.hpp"

#include "esp_timer.h"
#include "lwip/inet.h"

#include <array>
#include <limits>

namespace blip::e131 {
namespace {
constexpr std::array<std::string_view, 1> kProvided{"input.pixel-stream.e131"};
constexpr std::array<std::string_view, 2> kRequired{"transport.wifi", "output.pixel-strip"};
constexpr std::array<core::MetadataEntry, 5> kMetadata{{
    {"ui_topic", "Lighting inputs"},
    {"udp_port", "5568"}, {"universe", "1"}, {"maximum_sources", "2"},
    {"merge", "highest-priority-then-HTP"},
}};
constexpr std::array<core::ParameterDescriptor, 13> kParameters{{
    {"port", "E1.31 UDP port", core::ValueType::integer, core::Access::read_only, false,
     core::ScalarValue::from_integer(kPort), {}, ""},
    {"universe", "E1.31 universe", core::ValueType::integer, core::Access::read_only, false,
     core::ScalarValue::from_integer(1), {}, ""},
    {"multicast_joined", "Universe multicast membership", core::ValueType::boolean,
     core::Access::read_only, false, core::ScalarValue::from_bool(false), {}, ""},
    {"accepted_packets", "Accepted E1.31 data packets", core::ValueType::integer,
     core::Access::read_only, false, core::ScalarValue::from_integer(0), {}, "packets"},
    {"rejected_packets", "Rejected E1.31 data packets", core::ValueType::integer,
     core::Access::read_only, false, core::ScalarValue::from_integer(0), {}, "packets"},
    {"invalid_packets", "Malformed E1.31 packets", core::ValueType::integer,
     core::Access::read_only, false, core::ScalarValue::from_integer(0), {}, "packets"},
    {"ignored_packets", "Other universe or unsupported synchronized/preview packets",
     core::ValueType::integer, core::Access::read_only, false,
     core::ScalarValue::from_integer(0), {}, "packets"},
    {"stale_packets", "Stale source sequence packets", core::ValueType::integer,
     core::Access::read_only, false, core::ScalarValue::from_integer(0), {}, "packets"},
    {"source_capacity", "Packets beyond two source limit", core::ValueType::integer,
     core::Access::read_only, false, core::ScalarValue::from_integer(0), {}, "packets"},
    {"output_rejections", "Merged frames rejected by pixel output", core::ValueType::integer,
     core::Access::read_only, false, core::ScalarValue::from_integer(0), {}, "frames"},
    {"active_sources", "Active sources at output priority", core::ValueType::integer,
     core::Access::read_only, false, core::ScalarValue::from_integer(0), {}, "sources"},
    {"join_failures", "Multicast join failures", core::ValueType::integer,
     core::Access::read_only, false, core::ScalarValue::from_integer(0), {}, "attempts"},
    {"worker_stack_headroom", "E1.31 worker stack headroom", core::ValueType::integer,
     core::Access::read_only, false, core::ScalarValue::from_integer(0),
     {true, 0, EspE131Component::kTaskStackBytes, 1}, "bytes"},
}};
constexpr std::array<core::DiagnosticDescriptor, 9> kDiagnostics{{
    {"accepted_packets", core::ValueType::integer, "packets"},
    {"rejected_packets", core::ValueType::integer, "packets"},
    {"invalid_packets", core::ValueType::integer, "packets"},
    {"ignored_packets", core::ValueType::integer, "packets"},
    {"stale_packets", core::ValueType::integer, "packets"},
    {"source_capacity", core::ValueType::integer, "packets"},
    {"output_rejections", core::ValueType::integer, "frames"},
    {"active_sources", core::ValueType::integer, "sources"},
    {"join_failures", core::ValueType::integer, "attempts"},
}};
[[nodiscard]] constexpr core::ComponentDescriptor make_descriptor() noexcept {
    core::ComponentDescriptor descriptor{};
    descriptor.schema_version = 1U;
    descriptor.id = "blip.input.e131";
    descriptor.display_name = "E1.31 pixel input";
    descriptor.description = "Optional bounded sACN universe 1 receiver";
    descriptor.metadata = kMetadata;
    descriptor.provided_services = kProvided;
    descriptor.required_services = kRequired;
    descriptor.parameters = kParameters;
    descriptor.diagnostics = kDiagnostics;
    descriptor.settings = {1U, 1U};
    descriptor.disable_policy = core::DisablePolicy::live;
    descriptor.supports_restart = true;
    descriptor.cost = {16384U, 3072U, EspE131Component::kTaskStackBytes};
    return descriptor;
}
[[nodiscard]] core::Status failure(core::ErrorCode code, std::string_view operation,
                                   std::string_view detail) noexcept {
    return core::Status::failure(
        {core::ErrorDomain::transport, code, "blip.input.e131", operation, detail});
}
void increment(std::atomic<std::uint32_t>& value) noexcept {
    auto current = value.load();
    while (current != std::numeric_limits<std::uint32_t>::max() &&
           !value.compare_exchange_weak(current, current + 1U)) {
    }
}
} // namespace

const core::ComponentDescriptor EspE131Component::descriptor_{make_descriptor()};

const core::ComponentDescriptor& EspE131Component::descriptor() const noexcept {
    return descriptor_;
}

core::Status EspE131Component::start(const core::StartContext&) noexcept {
    if (started_.load())
        return core::Status::success();
    mixer_ = SourceMixer{};
    socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (socket_ < 0)
        return failure(core::ErrorCode::start_failed, "start", "udp-socket");
    const int reuse = 1;
    static_cast<void>(setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)));
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
    last_join_attempt_us_ = 0U;
    multicast_joined_.store(false);
    active_sources_.store(0U);
    started_.store(true);
    task_quiesced_.store(false);
    task_ = xTaskCreateStatic(task_entry, "blip_e131", sizeof(task_stack_), this, 4,
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

core::Status EspE131Component::stop() noexcept {
    if (!started_.exchange(false) && task_quiesced_.load())
        return core::Status::success();
    for (std::size_t attempt = 0U; attempt < 50U && !task_quiesced_.load(); ++attempt)
        vTaskDelay(pdMS_TO_TICKS(10));
    if (!task_quiesced_.load())
        return failure(core::ErrorCode::stop_failed, "stop", "task-active");
    if (socket_ >= 0) {
        close(socket_);
        socket_ = -1;
    }
    multicast_joined_.store(false);
    task_ = nullptr;
    return core::Status::success();
}

bool EspE131Component::callbacks_quiesced() const noexcept { return task_quiesced_.load(); }

core::Status EspE131Component::read_parameter(std::string_view id,
                                              core::ScalarValue& output) noexcept {
    if (id == "port")
        output = core::ScalarValue::from_integer(kPort);
    else if (id == "universe")
        output = core::ScalarValue::from_integer(mapping_.universe);
    else if (id == "multicast_joined")
        output = core::ScalarValue::from_bool(multicast_joined_.load());
    else if (id == "accepted_packets")
        output = core::ScalarValue::from_integer(accepted_.load());
    else if (id == "rejected_packets")
        output = core::ScalarValue::from_integer(rejected_.load());
    else if (id == "invalid_packets")
        output = core::ScalarValue::from_integer(invalid_.load());
    else if (id == "ignored_packets")
        output = core::ScalarValue::from_integer(ignored_.load());
    else if (id == "stale_packets")
        output = core::ScalarValue::from_integer(stale_.load());
    else if (id == "source_capacity")
        output = core::ScalarValue::from_integer(source_capacity_.load());
    else if (id == "output_rejections")
        output = core::ScalarValue::from_integer(output_rejections_.load());
    else if (id == "active_sources")
        output = core::ScalarValue::from_integer(active_sources_.load());
    else if (id == "join_failures")
        output = core::ScalarValue::from_integer(join_failures_.load());
    else if (id == "worker_stack_headroom") {
        const auto words = task_ == nullptr ? 0U : uxTaskGetStackHighWaterMark(task_);
        output = core::ScalarValue::from_integer(words * sizeof(StackType_t));
    }
    else
        return failure(core::ErrorCode::not_found, "read-parameter", id);
    return core::Status::success();
}

void EspE131Component::task_entry(void* context) noexcept {
    static_cast<EspE131Component*>(context)->run();
}

void EspE131Component::maintain_membership(std::uint64_t now_us) noexcept {
    std::array<char, 16> local_text{};
    std::size_t text_size{};
    in_addr station_ip{};
    const bool connected = wifi_->connection_state() == network::WifiConnectionState::connected &&
                           wifi_->local_ipv4(local_text, text_size) &&
                           inet_pton(AF_INET, local_text.data(), &station_ip) == 1;
    if (multicast_joined_.load() &&
        (!connected || membership_.imr_interface.s_addr != station_ip.s_addr)) {
        static_cast<void>(setsockopt(socket_, IPPROTO_IP, IP_DROP_MEMBERSHIP,
                                     &membership_, sizeof(membership_)));
        multicast_joined_.store(false);
    }
    if (!connected || multicast_joined_.load() ||
        (last_join_attempt_us_ != 0U && now_us >= last_join_attempt_us_ &&
         now_us - last_join_attempt_us_ < 1'000'000U))
        return;
    last_join_attempt_us_ = now_us;
    membership_ = {};
    membership_.imr_multiaddr.s_addr = htonl(0xefff0000U | mapping_.universe);
    membership_.imr_interface.s_addr = station_ip.s_addr;
    if (setsockopt(socket_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership_,
                   sizeof(membership_)) == 0)
        multicast_joined_.store(true);
    else
        increment(join_failures_);
}

void EspE131Component::submit_merged(std::uint64_t now_us) noexcept {
    std::size_t channel_count{};
    std::size_t active_sources{};
    if (!mixer_.compose(merged_, channel_count, active_sources)) {
        active_sources_.store(0U);
        return;
    }
    active_sources_.store(active_sources);
    core::ScalarValue configured_pixels{};
    if (!output_->read_parameter("pixels", configured_pixels) ||
        configured_pixels.type != core::ValueType::integer ||
        configured_pixels.integer <= mapping_.start_pixel) {
        increment(output_rejections_);
        increment(rejected_);
        return;
    }
    const auto maximum_pixels =
        static_cast<std::size_t>(configured_pixels.integer - mapping_.start_pixel);
    const auto update = map_dmx(std::span<const std::byte>{merged_.data(), channel_count},
                                mapping_, maximum_pixels);
    if (!update ||
        !output_->ingest_stream(0U, update.value().start_pixel, update.value().channels,
                                update.value().channels_per_pixel,
                                update.value().sixteen_bit, now_us)) {
        increment(output_rejections_);
        increment(rejected_);
    }
}

void EspE131Component::run() noexcept {
    while (started_.load()) {
        const auto now_us = static_cast<std::uint64_t>(esp_timer_get_time());
        maintain_membership(now_us);
        const auto received = recvfrom(socket_, packet_.data(), packet_.size(), 0, nullptr, nullptr);
        const auto received_at_us = static_cast<std::uint64_t>(esp_timer_get_time());
        if (mixer_.expire(received_at_us))
            submit_merged(received_at_us);
        if (received <= 0) {
            continue;
        }
        const auto packet = parse(
            std::span<const std::byte>{packet_.data(), static_cast<std::size_t>(received)});
        if (!packet) {
            increment(invalid_);
            increment(rejected_);
            continue;
        }
        if (packet.value().universe != mapping_.universe) {
            increment(ignored_);
            continue;
        }
        const auto result = mixer_.ingest(packet.value(), received_at_us);
        if (result == SourceResult::ignored) {
            increment(ignored_);
            continue;
        }
        if (result == SourceResult::stale) {
            increment(stale_);
            increment(rejected_);
            continue;
        }
        if (result == SourceResult::capacity) {
            increment(source_capacity_);
            increment(rejected_);
            continue;
        }
        increment(accepted_);
        submit_merged(received_at_us);
    }
    task_quiesced_.store(true);
    vTaskDelete(nullptr);
}

} // namespace blip::e131
