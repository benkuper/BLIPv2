#include "blip/ddp/esp_ddp_component.hpp"

#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include <array>
#include <limits>

namespace blip::ddp {
namespace {
constexpr char kTag[] = "blip_ddp";
constexpr std::array<std::string_view, 1> kProvided{"input.pixel-stream.ddp"};
constexpr std::array<std::string_view, 2> kRequired{"transport.wifi", "output.pixel-strip"};
constexpr std::array<core::MetadataEntry, 2> kMetadata{{
    {"udp_port", "4048"}, {"mapping", "destination=1,offset=0,rgb8"},
}};
constexpr std::array<core::ParameterDescriptor, 7> kParameters{{
    {"port", "DDP UDP port", core::ValueType::integer, core::Access::read_only, false,
     core::ScalarValue::from_integer(kPort), {}, ""},
    {"accepted_packets", "Accepted pixel packets", core::ValueType::integer,
     core::Access::read_only, false, core::ScalarValue::from_integer(0), {}, "packets"},
    {"rejected_packets", "Rejected pixel packets", core::ValueType::integer,
     core::Access::read_only, false, core::ScalarValue::from_integer(0), {}, "packets"},
    {"invalid_packets", "Invalid DDP packets", core::ValueType::integer,
     core::Access::read_only, false, core::ScalarValue::from_integer(0), {}, "packets"},
    {"stale_packets", "Stale sequence packets", core::ValueType::integer,
     core::Access::read_only, false, core::ScalarValue::from_integer(0), {}, "packets"},
    {"output_rejections", "Packets rejected by pixel output", core::ValueType::integer,
     core::Access::read_only, false, core::ScalarValue::from_integer(0), {}, "packets"},
    {"worker_stack_headroom", "DDP worker stack headroom", core::ValueType::integer,
     core::Access::read_only, false, core::ScalarValue::from_integer(0),
     {true, 0, EspDdpComponent::kTaskStackBytes, 1}, "bytes"},
}};
constexpr std::array<core::DiagnosticDescriptor, 5> kDiagnostics{{
    {"accepted_packets", core::ValueType::integer, "packets"},
    {"rejected_packets", core::ValueType::integer, "packets"},
    {"invalid_packets", core::ValueType::integer, "packets"},
    {"stale_packets", core::ValueType::integer, "packets"},
    {"output_rejections", core::ValueType::integer, "packets"},
}};
[[nodiscard]] constexpr core::ComponentDescriptor make_descriptor() noexcept {
    core::ComponentDescriptor descriptor{};
    descriptor.schema_version = 1U;
    descriptor.id = "blip.input.ddp";
    descriptor.display_name = "DDP pixel input";
    descriptor.description = "Bounded RGB8 DDP input for the pixel stream layer";
    descriptor.metadata = kMetadata;
    descriptor.provided_services = kProvided;
    descriptor.required_services = kRequired;
    descriptor.parameters = kParameters;
    descriptor.diagnostics = kDiagnostics;
    descriptor.settings = {1U, 1U};
    descriptor.disable_policy = core::DisablePolicy::live;
    descriptor.supports_restart = true;
    descriptor.cost = {8192U, 2048U, EspDdpComponent::kTaskStackBytes};
    return descriptor;
}
[[nodiscard]] core::Status failure(core::ErrorCode code, std::string_view operation,
                                   std::string_view detail) noexcept {
    return core::Status::failure({core::ErrorDomain::transport, code, "blip.input.ddp",
                                  operation, detail});
}
void increment(std::atomic<std::uint32_t>& value) noexcept {
    auto current = value.load();
    while (current != std::numeric_limits<std::uint32_t>::max() &&
           !value.compare_exchange_weak(current, current + 1U)) {
    }
}
} // namespace

const core::ComponentDescriptor EspDdpComponent::descriptor_{make_descriptor()};

const core::ComponentDescriptor& EspDdpComponent::descriptor() const noexcept {
    return descriptor_;
}

core::Status EspDdpComponent::start(const core::StartContext&) noexcept {
    if (started_.load()) {
        return core::Status::success();
    }
    sequences_.reset();
    socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (socket_ < 0) {
        return failure(core::ErrorCode::start_failed, "start", "udp-socket");
    }
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
    task_ = xTaskCreateStatic(task_entry, "blip_ddp", task_stack_.size(), this, 4,
                              task_stack_.data(), &task_storage_);
    if (task_ == nullptr) {
        started_.store(false);
        task_quiesced_.store(true);
        close(socket_);
        socket_ = -1;
        return failure(core::ErrorCode::start_failed, "start", "udp-task");
    }
    ESP_LOGI(kTag, "DDP UDP port=%u ready", static_cast<unsigned>(kPort));
    return core::Status::success();
}

core::Status EspDdpComponent::stop() noexcept {
    if (!started_.exchange(false) && task_quiesced_.load()) {
        return core::Status::success();
    }
    for (std::size_t attempt = 0U; attempt < 50U && !task_quiesced_.load(); ++attempt) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!task_quiesced_.load()) {
        return failure(core::ErrorCode::stop_failed, "stop", "task-active");
    }
    close(socket_);
    socket_ = -1;
    task_ = nullptr;
    return core::Status::success();
}

bool EspDdpComponent::callbacks_quiesced() const noexcept { return task_quiesced_.load(); }

core::Status EspDdpComponent::read_parameter(std::string_view id,
                                              core::ScalarValue& output) noexcept {
    if (id == "port") {
        output = core::ScalarValue::from_integer(kPort);
    } else if (id == "accepted_packets") {
        output = core::ScalarValue::from_integer(accepted_.load());
    } else if (id == "rejected_packets") {
        output = core::ScalarValue::from_integer(rejected_.load());
    } else if (id == "invalid_packets") {
        output = core::ScalarValue::from_integer(invalid_.load());
    } else if (id == "stale_packets") {
        output = core::ScalarValue::from_integer(stale_.load());
    } else if (id == "output_rejections") {
        output = core::ScalarValue::from_integer(output_rejections_.load());
    } else if (id == "worker_stack_headroom") {
        const auto words = task_ == nullptr ? 0U : uxTaskGetStackHighWaterMark(task_);
        output = core::ScalarValue::from_integer(words * sizeof(StackType_t));
    } else {
        return failure(core::ErrorCode::not_found, "read-parameter", id);
    }
    return core::Status::success();
}

void EspDdpComponent::task_entry(void* context) noexcept {
    static_cast<EspDdpComponent*>(context)->run();
}

void EspDdpComponent::run() noexcept {
    while (started_.load()) {
        const auto received = recvfrom(socket_, packet_.data(), packet_.size(), 0, nullptr,
                                       nullptr);
        if (received <= 0) {
            continue;
        }
        const auto update = map(
            std::span<const std::byte>{packet_.data(), static_cast<std::size_t>(received)},
            mapping_);
        const auto now_us = static_cast<std::uint64_t>(esp_timer_get_time());
        if (!update) {
            increment(invalid_);
            increment(rejected_);
            continue;
        }
        if (!sequences_.accept(update.value().sequence, now_us)) {
            increment(stale_);
            increment(rejected_);
            continue;
        }
        if (!output_->ingest_stream(0U, update.value().start_pixel,
                                     update.value().channels, update.value().channels_per_pixel,
                                     update.value().sixteen_bit, now_us)) {
            increment(output_rejections_);
            increment(rejected_);
            continue;
        }
        increment(accepted_);
    }
    task_quiesced_.store(true);
    vTaskDelete(nullptr);
}

} // namespace blip::ddp
