#include "blip/fleet/esp_fleet_component.hpp"

#include "blip/transport/envelope.hpp"
#include "blip/transport/espnow_radio.hpp"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"

#include <algorithm>
#include <limits>
#include <cstring>

namespace blip::fleet {
namespace {
constexpr std::uint8_t kBroadcast[6]{255, 255, 255, 255, 255, 255};
constexpr std::array<core::MetadataEntry, 2> kMetadata{{
    {"ui_topic", "Fleet"}, {"ui_primary", "enabled,active,synchronized,is_leader"}}};
#if defined(BLIP_FLEET_WASM)
constexpr std::array<std::string_view, 2> kProvided{"fleet.coordination", "blip.fleet.v1"};
using WT = core::WasmValueType;
using WR = core::WasmArgumentRole;
constexpr std::array kScheduleArguments{
    core::WasmArgumentDescriptor{"component_offset", WT::i32, WR::utf8_offset}, core::WasmArgumentDescriptor{"component_bytes", WT::i32, WR::utf8_length},
    core::WasmArgumentDescriptor{"control_offset", WT::i32, WR::utf8_offset}, core::WasmArgumentDescriptor{"control_bytes", WT::i32, WR::utf8_length},
    core::WasmArgumentDescriptor{"value", WT::i32}, core::WasmArgumentDescriptor{"delay_ms", WT::i32}};
constexpr std::array kActionArguments{
    core::WasmArgumentDescriptor{"component_offset", WT::i32, WR::utf8_offset}, core::WasmArgumentDescriptor{"component_bytes", WT::i32, WR::utf8_length},
    core::WasmArgumentDescriptor{"control_offset", WT::i32, WR::utf8_offset}, core::WasmArgumentDescriptor{"control_bytes", WT::i32, WR::utf8_length},
    core::WasmArgumentDescriptor{"delay_ms", WT::i32}};
constexpr std::array kI32{WT::i32}, kI64{WT::i64};
constexpr std::array kScriptFunctions{
    core::WasmFunctionDescriptor{"ready", "Read active synchronized fleet clock readiness", {}, kI32, 2000},
    core::WasmFunctionDescriptor{"time_us", "Read synchronized fleet time; unavailable without clock lock", {}, kI64, 2000},
    core::WasmFunctionDescriptor{"leader", "Read leader node identifier (zero when inactive)", {}, kI64, 2000},
    core::WasmFunctionDescriptor{"schedule_i32", "Leader queues a signed i32 parameter write after 100..60000 ms; copied IDs at most 64 ASCII bytes; returns cue ID", kScheduleArguments, kI32, 2000},
    core::WasmFunctionDescriptor{"schedule_action", "Leader queues an action without arguments after 100..60000 ms; copied IDs at most 64 ASCII bytes; returns cue ID", kActionArguments, kI32, 2000}};
#else
constexpr std::array<std::string_view, 1> kProvided{"fleet.coordination"};
#endif
constexpr std::array<std::string_view, 3> kRequired{
    "control.dispatch", "storage.settings", "transport.wifi"};
constexpr auto integer(std::string_view id, std::string_view label, std::string_view unit = "") {
    return core::ParameterDescriptor{id, label, core::ValueType::integer,
        core::Access::read_only, false, core::ScalarValue::from_integer(0), {}, unit};
}
constexpr auto boolean(std::string_view id, std::string_view label) {
    return core::ParameterDescriptor{id, label, core::ValueType::boolean,
        core::Access::read_only, false, core::ScalarValue::from_bool(false), {}, ""};
}
constexpr std::array kParameters{
    core::ParameterDescriptor{"enabled", "Fleet enabled", core::ValueType::boolean,
        core::Access::read_write, true, core::ScalarValue::from_bool(false), {}, ""},
    core::ParameterDescriptor{"fleet_id", "Fleet identifier", core::ValueType::integer,
        core::Access::read_write, true, core::ScalarValue::from_integer(1),
        {true, 1, 4294967295.0, 1}, ""},
    core::ParameterDescriptor{"channel", "Routerless ESP-NOW channel", core::ValueType::integer,
        core::Access::read_write, true, core::ScalarValue::from_integer(1), {true, 1, 11, 1}, "channel"},
    boolean("active", "Fleet radio active"),
    integer("node_id", "Station node identifier"), integer("leader_id", "Leader identifier"),
    boolean("is_leader", "This node leads"), boolean("synchronized", "Fleet clock locked"),
    integer("pending", "Pending cues"),
    integer("time_us", "Fleet time", "us"), integer("offset_us", "Clock offset", "us"),
    integer("uncertainty_us", "Clock uncertainty", "us"), integer("sample_age_us", "Clock sample age", "us"),
    integer("leader_changes", "Leader changes"), integer("clock_samples", "Accepted clock samples"),
    integer("rejected", "Engine rejections"), integer("duplicates", "Duplicate cues"),
    integer("scheduled", "Cues queued"), integer("due", "Cues handed to executor"),
    integer("late", "Late cues discarded"), integer("cancelled", "Cues cancelled"),
    integer("executed", "Commands executed"), integer("execution_failed", "Commands failed"),
    integer("execution_dropped", "Executor drops"), integer("invalid_packets", "Invalid packets"),
    integer("send_failed", "UDP send failures"), integer("last_execution_us", "Last execution fleet time", "us"),
    integer("maximum_lateness_us", "Maximum execution lateness", "us"),
    integer("network_stack_headroom", "Network stack headroom", "bytes"),
    integer("executor_stack_headroom", "Executor stack headroom", "bytes")};
constexpr std::array<core::FieldDescriptor, 4> write_fields(core::ValueType type) {
    return {{{"component", core::ValueType::string, true},
             {"parameter", core::ValueType::string, true}, {"value", type, true},
             {"delay_ms", core::ValueType::integer, true}}};
}
constexpr auto kInteger = write_fields(core::ValueType::integer);
constexpr auto kBoolean = write_fields(core::ValueType::boolean);
constexpr auto kNumber = write_fields(core::ValueType::number);
constexpr auto kString = write_fields(core::ValueType::string);
constexpr std::array<core::FieldDescriptor, 3> kActionFields{{
    {"component", core::ValueType::string, true}, {"action", core::ValueType::string, true},
    {"delay_ms", core::ValueType::integer, true}}};
constexpr std::array<core::ActionDescriptor, 6> kActions{{
    {"schedule_write_integer", "Schedule an integer write", kInteger},
    {"schedule_write_boolean", "Schedule a boolean write", kBoolean},
    {"schedule_write_number", "Schedule a number write", kNumber},
    {"schedule_write_string", "Schedule a string write", kString},
    {"schedule_action", "Schedule an action without arguments", kActionFields},
    {"cancel_local", "Cancel this node's pending cues", {}}}};
constexpr core::ComponentDescriptor make_descriptor() {
    core::ComponentDescriptor result{};
    result.schema_version = 1;
    result.id = "blip.fleet";
    result.display_name = "Fleet coordination";
    result.description = "Routerless ESP-NOW broadcast clock and scheduled control commands";
    result.provided_services = kProvided;
    result.metadata = kMetadata;
    result.required_services = kRequired;
    result.parameters = kParameters;
    result.actions = kActions;
    result.settings = {2, 2};
    result.disable_policy = core::DisablePolicy::live;
    result.supports_restart = true;
    result.cost = {32768, sizeof(EspFleetComponent), 14336};
#if defined(BLIP_FLEET_WASM)
    result.wasm = {1, "blip.fleet.v1", kScriptFunctions};
    result.cost.flash_bytes += 6144;
#endif
    return result;
}
core::Error error(core::ErrorCode code, std::string_view operation, std::string_view detail) {
    return {core::ErrorDomain::transport, code, "blip.fleet", operation, detail};
}
core::Status failure(core::ErrorCode code, std::string_view operation, std::string_view detail) {
    return core::Status::failure(error(code, operation, detail));
}
std::uint64_t now_us() { return static_cast<std::uint64_t>(esp_timer_get_time()); }
std::uint32_t session() { auto value = esp_random(); return value == 0U ? 1U : value; }
} // namespace

const core::ComponentDescriptor EspFleetComponent::descriptor_{make_descriptor()};
const core::ComponentDescriptor& EspFleetComponent::descriptor() const noexcept { return descriptor_; }

core::Status EspFleetComponent::load_settings() noexcept {
    std::array<std::byte, 6> data{};
    const auto loaded = settings_->load(descriptor_, data);
    if (!loaded) {
        return loaded.error().code == core::ErrorCode::not_found
            ? core::Status::success() : core::Status::failure(loaded.error());
    }
    const bool previous = loaded.value().schema_version == 1U && loaded.value().payload_size == 5U;
    if ((!previous && (loaded.value().schema_version != 2U || loaded.value().payload_size != data.size())) || data[0] > std::byte{1})
        return failure(core::ErrorCode::corrupt_data, "load", "fleet-settings");
    std::uint32_t id{};
    for (std::size_t i = 0; i < 4; ++i)
        id |= std::to_integer<std::uint32_t>(data[i + 1]) << (8U * i);
    if (id == 0U) return failure(core::ErrorCode::corrupt_data, "load", "fleet-id");
    enabled_.store(data[0] == std::byte{1});
    fleet_id_.store(id);
    const auto channel = previous ? 1U : std::to_integer<std::uint8_t>(data[5]);
    if (channel < 1U || channel > 11U) return failure(core::ErrorCode::corrupt_data, "load", "fleet-channel");
    channel_.store(channel);
    return core::Status::success();
}

core::Status EspFleetComponent::save_settings(bool enabled, std::uint32_t id, std::uint8_t channel) noexcept {
    std::array<std::byte, 6> data{};
    data[0] = enabled ? std::byte{1} : std::byte{0};
    for (std::size_t i = 0; i < 4; ++i) data[i + 1] = static_cast<std::byte>(id >> (8U * i));
    data[5] = static_cast<std::byte>(channel);
    return settings_->save(descriptor_, data);
}

core::Status EspFleetComponent::start(const core::StartContext&) noexcept {
    if (started_.load() || !callbacks_quiesced()) return failure(core::ErrorCode::invalid_state, "start", "worker-active");
    const auto loaded = load_settings();
    if (!loaded) return loaded;
    std::array<std::uint8_t, 6> mac{};
    if (esp_read_mac(mac.data(), ESP_MAC_WIFI_STA) != ESP_OK)
        return failure(core::ErrorCode::start_failed, "start", "station-mac");
    node_ = 0;
    for (const auto value : mac) node_ = (node_ << 8U) | value;
    engine_mutex_ = xSemaphoreCreateMutex();
    settings_mutex_ = xSemaphoreCreateMutex();
    execution_queue_ = xQueueCreate(kMaximumCues, sizeof(Execution));
    receive_queue_ = xQueueCreate(16U, sizeof(Received));
    if (engine_mutex_ == nullptr || settings_mutex_ == nullptr || execution_queue_ == nullptr || receive_queue_ == nullptr) {
        static_cast<void>(stop());
        return failure(core::ErrorCode::start_failed, "start", "runtime-resources");
    }
    control_ready_.store(false);
    reconfigure_.store(true);
    started_.store(true);
    network_quiesced_.store(false);
    if (xTaskCreate(network_entry, "blip_fleet_net", 8192, this, 4, &network_task_) != pdPASS) {
        network_quiesced_.store(true);
        static_cast<void>(stop());
        return failure(core::ErrorCode::start_failed, "start", "network-task");
    }
    executor_quiesced_.store(false);
    if (xTaskCreate(executor_entry, "blip_fleet_exec", 6144, this, 4, &executor_task_) != pdPASS) {
        executor_quiesced_.store(true);
        static_cast<void>(stop());
        return failure(core::ErrorCode::start_failed, "start", "executor-task");
    }
    return core::Status::success();
}

core::Status EspFleetComponent::stop() noexcept {
#if defined(BLIP_FLEET_WASM)
    if (!script_admission_ || xSemaphoreTake(script_admission_, pdMS_TO_TICKS(2)) != pdTRUE)
        return failure(core::ErrorCode::stop_failed, "stop", "script-admission-busy");
#endif
    control_ready_.store(false);
    started_.store(false);
    active_.store(false);
#if defined(BLIP_FLEET_WASM)
    xSemaphoreGive(script_admission_);
#endif
    epoch_.fetch_add(1U);
    for (std::size_t i = 0; i < 50 && !callbacks_quiesced(); ++i) vTaskDelay(pdMS_TO_TICKS(10));
    if (!callbacks_quiesced()) return failure(core::ErrorCode::stop_failed, "stop", "worker-active");
    if (receive_queue_ != nullptr) { vQueueDelete(receive_queue_); receive_queue_ = nullptr; }
    if (execution_queue_ != nullptr) { vQueueDelete(execution_queue_); execution_queue_ = nullptr; }
    if (engine_mutex_ != nullptr) { vSemaphoreDelete(engine_mutex_); engine_mutex_ = nullptr; }
    if (settings_mutex_ != nullptr) { vSemaphoreDelete(settings_mutex_); settings_mutex_ = nullptr; }
    network_task_ = nullptr;
    executor_task_ = nullptr;
    return core::Status::success();
}

bool EspFleetComponent::callbacks_quiesced() const noexcept {
    return network_quiesced_.load() && executor_quiesced_.load();
}

#if defined(BLIP_FLEET_WASM)
core::Status EspFleetComponent::invoke(std::string_view name, wasm::CallContext& context,
    std::span<const wasm::Value> arguments, std::span<wasm::Value> output, std::size_t& count) noexcept {
    count = 0;
    const bool write = name == "schedule_i32", action = name == "schedule_action";
    const bool query = name == "ready" || name == "time_us" || name == "leader";
    if (!write && !action && !query) return failure(core::ErrorCode::not_found, "script", "unknown-function");
    if (arguments.size() != (write ? 6U : action ? 5U : 0U) || output.empty())
        return failure(core::ErrorCode::invalid_argument, "script", "signature");
    for (const auto& value : arguments)
        if (value.type != wasm::ValueType::i32 || value.bits > UINT32_MAX)
            return failure(core::ErrorCode::invalid_argument, "script", "argument-type");
    std::array<char, 64> component{}, control{}; std::size_t component_bytes{}, control_bytes{};
    core::ScalarValue value{};
    std::array<std::byte, kMaximumCueBytes> payload{}; std::size_t payload_bytes{};
    std::uint32_t delay{};
    if (!query) {
        const auto identifier = [](std::string_view id, bool dotted) {
            if (id.empty() || id[0] < 'a' || id[0] > 'z' || (dotted && id.find('.') == id.npos)) return false;
            for (const char c : id)
                if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || (dotted && c == '.'))) return false;
            return true;
        };
        auto status = context.read_utf8({static_cast<std::uint32_t>(arguments[0].bits), static_cast<std::uint32_t>(arguments[1].bits)}, component, component_bytes);
        if (!status) return status;
        status = context.read_utf8({static_cast<std::uint32_t>(arguments[2].bits), static_cast<std::uint32_t>(arguments[3].bits)}, control, control_bytes);
        if (!status) return status;
        const std::string_view component_id(component.data(), component_bytes), control_id(control.data(), control_bytes);
        delay = static_cast<std::uint32_t>(arguments.back().bits);
        if (!identifier(component_id, true) || !identifier(control_id, false) || component_id == descriptor_.id || delay < 100 || delay > 60000)
            return failure(core::ErrorCode::invalid_argument, "script", "cue-fields");
        if (write) value = core::ScalarValue::from_integer(std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(arguments[4].bits)));
        const transport::ControlMessage message{write ? core::ControlOperation::write_parameter : core::ControlOperation::invoke_action,
            core::ErrorDomain::none, core::ErrorCode::none, component_id, control_id, {},
            write ? std::span<const core::ScalarValue>{&value, 1} : std::span<const core::ScalarValue>{}};
        const auto encoded = transport::encode_control_message(message, payload);
        if (!encoded) return core::Status::failure(encoded.error());
        payload_bytes = encoded.value();
    }
    if (context.cancelled()) return failure(core::ErrorCode::cancelled, "script", "cancelled-admission");
    if (!script_admission_ || xSemaphoreTake(script_admission_, 0) != pdTRUE)
        return failure(core::ErrorCode::resource_unavailable, "script", "admission-busy");
    auto status = core::Status::success(); wasm::Value result{};
    if (!started_.load() || !control_ready_.load()) status = failure(core::ErrorCode::resource_unavailable, "script", "not-started");
    else if (!engine_mutex_ || xSemaphoreTake(engine_mutex_, 0) != pdTRUE) status = failure(core::ErrorCode::resource_unavailable, "script", "engine-busy");
    else {
        const bool ready = active_.load() && !reconfigure_.load() && engine_.synchronized();
        if (context.cancelled()) status = failure(core::ErrorCode::cancelled, "script", "cancelled-admission");
        else if (name == "ready") result = wasm::Value::i32(ready ? 1 : 0);
        else if (name == "leader") result = wasm::Value::i64(active_.load() ? engine_.leader() : 0);
        else if (!ready) status = failure(core::ErrorCode::resource_unavailable, "script", "clock-unavailable");
        else if (name == "time_us") result = wasm::Value::i64(engine_.time_us(now_us()));
        else {
            const auto scheduled = engine_.schedule(std::span<const std::byte>(payload).first(payload_bytes), static_cast<std::uint64_t>(delay) * 1000U, now_us());
            status = scheduled ? core::Status::success() : core::Status::failure(scheduled.error());
            if (scheduled) result = wasm::Value::i32(scheduled.value());
            update_epoch();
        }
        xSemaphoreGive(engine_mutex_);
    }
    xSemaphoreGive(script_admission_);
    if (!status) return status;
    output[0] = result; count = 1; return status;
}
#endif

core::Status EspFleetComponent::write_parameter(std::string_view id, const core::ScalarValue& value) noexcept {
    if (!started_.load()) return failure(core::ErrorCode::invalid_state, "write", "not-started");
    if ((id != "enabled" && id != "fleet_id" && id != "channel") ||
        (id == "enabled" && value.type != core::ValueType::boolean) ||
        (id == "fleet_id" && (value.type != core::ValueType::integer || value.integer < 1 ||
                              value.integer > std::numeric_limits<std::uint32_t>::max())) ||
        (id == "channel" && (value.type != core::ValueType::integer || value.integer < 1 || value.integer > 11)))
        return failure(core::ErrorCode::invalid_argument, "write", "invalid-setting");
    xSemaphoreTake(settings_mutex_, portMAX_DELAY);
    const bool enabled = id == "enabled" ? value.boolean : enabled_.load();
    const auto fleet_id = id == "fleet_id" ? static_cast<std::uint32_t>(value.integer) : fleet_id_.load();
    const auto channel = id == "channel" ? static_cast<std::uint8_t>(value.integer) : channel_.load();
    const auto saved = save_settings(enabled, fleet_id, channel);
    if (saved) {
        // Fence due commands immediately, before the network task resets its clock epoch.
        epoch_.fetch_add(1U);
        active_.store(false);
        enabled_.store(enabled);
        fleet_id_.store(fleet_id);
        channel_.store(channel);
        reconfigure_.store(true);
    }
    xSemaphoreGive(settings_mutex_);
    return saved;
}

core::Result<std::uint32_t> EspFleetComponent::schedule(const core::ControlRequest& request,
                                                       std::uint32_t delay_ms) noexcept {
    if (!control_ready_.load() || !active_.load() || request.operation == core::ControlOperation::read_parameter)
        return core::Result<std::uint32_t>::failure(error(core::ErrorCode::invalid_state, "schedule", "fleet-inactive"));
    // Self-reconfiguration is not a synchronized cue: it invalidates the current epoch.
    if (request.component_id == descriptor_.id)
        return core::Result<std::uint32_t>::failure(error(core::ErrorCode::invalid_argument, "schedule", "fleet-command"));
    std::array<std::byte, kMaximumCueBytes> payload{};
    const transport::ControlMessage message{request.operation, core::ErrorDomain::none,
        core::ErrorCode::none, request.component_id, request.control_id, {}, request.values};
    const auto encoded = transport::encode_control_message(message, payload);
    if (!encoded) return core::Result<std::uint32_t>::failure(encoded.error());
    xSemaphoreTake(engine_mutex_, portMAX_DELAY);
    const auto scheduled = active_.load() && !reconfigure_.load()
        ? engine_.schedule(std::span<const std::byte>{payload.data(), encoded.value()},
                           static_cast<std::uint64_t>(delay_ms) * 1000U, now_us())
        : core::Result<std::uint32_t>::failure(error(core::ErrorCode::invalid_state, "schedule", "fleet-reconfiguring"));
    update_epoch();
    xSemaphoreGive(engine_mutex_);
    return scheduled;
}

core::Status EspFleetComponent::invoke_action(std::string_view id, std::span<const core::ScalarValue> arguments,
                                             std::span<core::ScalarValue> outputs, std::size_t& output_count) noexcept {
    output_count = 0;
    if (!started_.load()) return failure(core::ErrorCode::invalid_state, "action", "not-started");
    if (id == "cancel_local" && arguments.empty()) {
        xSemaphoreTake(engine_mutex_, portMAX_DELAY);
        engine_.cancel_all();
        epoch_.fetch_add(1U);
        xSemaphoreGive(engine_mutex_);
        return core::Status::success();
    }
    const bool action = id == "schedule_action";
    core::ValueType type{};
    if (id == "schedule_write_integer") type = core::ValueType::integer;
    else if (id == "schedule_write_boolean") type = core::ValueType::boolean;
    else if (id == "schedule_write_number") type = core::ValueType::number;
    else if (id == "schedule_write_string") type = core::ValueType::string;
    else if (!action) return failure(core::ErrorCode::not_found, "action", "unknown-action");
    const auto count = action ? 3U : 4U;
    if (arguments.size() != count || arguments[0].type != core::ValueType::string ||
        arguments[1].type != core::ValueType::string || (!action && arguments[2].type != type) ||
        arguments.back().type != core::ValueType::integer || arguments.back().integer < 100 ||
        arguments.back().integer > 60000 || outputs.empty())
        return failure(core::ErrorCode::invalid_argument, "action", "invalid-arguments");
    const core::ControlRequest request{action ? core::ControlOperation::invoke_action : core::ControlOperation::write_parameter,
        arguments[0].string, arguments[1].string, action ? std::span<const core::ScalarValue>{} : arguments.subspan(2, 1)};
    const auto result = schedule(request, static_cast<std::uint32_t>(arguments.back().integer));
    if (!result) return core::Status::failure(result.error());
    outputs[0] = core::ScalarValue::from_integer(result.value());
    output_count = 1;
    return core::Status::success();
}

core::Status EspFleetComponent::read_parameter(std::string_view id, core::ScalarValue& output) noexcept {
    if (id == "enabled") output = core::ScalarValue::from_bool(enabled_.load());
    else if (id == "fleet_id") output = core::ScalarValue::from_integer(fleet_id_.load());
    else if (id == "channel") output = core::ScalarValue::from_integer(channel_.load());
    else if (id == "active") output = core::ScalarValue::from_bool(active_.load());
    else if (id == "node_id") output = core::ScalarValue::from_integer(node_);
    else if (id == "executed") output = core::ScalarValue::from_integer(executed_.load());
    else if (id == "execution_failed") output = core::ScalarValue::from_integer(execution_failed_.load());
    else if (id == "execution_dropped") output = core::ScalarValue::from_integer(execution_dropped_.load());
    else if (id == "invalid_packets") output = core::ScalarValue::from_integer(invalid_packets_.load());
    else if (id == "send_failed") output = core::ScalarValue::from_integer(send_failed_.load());
    else if (id == "last_execution_us") output = core::ScalarValue::from_integer(last_execution_us_.load());
    else if (id == "maximum_lateness_us") output = core::ScalarValue::from_integer(maximum_lateness_us_.load());
    else if (id == "network_stack_headroom" || id == "executor_stack_headroom") {
        const auto task = id == "network_stack_headroom" ? network_task_ : executor_task_;
        output = core::ScalarValue::from_integer(task == nullptr ? 0U : uxTaskGetStackHighWaterMark(task));
    } else {
        if (engine_mutex_ == nullptr) return failure(core::ErrorCode::invalid_state, "read", "not-started");
        xSemaphoreTake(engine_mutex_, portMAX_DELAY);
        const auto& metrics = engine_.metrics();
        bool found = true;
        if (id == "leader_id") output = core::ScalarValue::from_integer(engine_.leader());
        else if (id == "is_leader") output = core::ScalarValue::from_bool(active_.load() && engine_.is_leader());
        else if (id == "synchronized") output = core::ScalarValue::from_bool(active_.load() && engine_.synchronized());
        else if (id == "pending") output = core::ScalarValue::from_integer(engine_.pending());
        else if (id == "time_us") output = core::ScalarValue::from_integer(engine_.time_us(now_us()));
        else if (id == "offset_us") output = core::ScalarValue::from_integer(engine_.offset_us());
        else if (id == "uncertainty_us") output = core::ScalarValue::from_integer(engine_.uncertainty_us());
        else if (id == "sample_age_us") output = core::ScalarValue::from_integer(engine_.sample_age_us(now_us()));
        else if (id == "leader_changes") output = core::ScalarValue::from_integer(metrics.leader_changes);
        else if (id == "clock_samples") output = core::ScalarValue::from_integer(metrics.clock_samples);
        else if (id == "rejected") output = core::ScalarValue::from_integer(metrics.rejected);
        else if (id == "duplicates") output = core::ScalarValue::from_integer(metrics.duplicates);
        else if (id == "scheduled") output = core::ScalarValue::from_integer(metrics.scheduled);
        else if (id == "due") output = core::ScalarValue::from_integer(metrics.due);
        else if (id == "late") output = core::ScalarValue::from_integer(metrics.late);
        else if (id == "cancelled") output = core::ScalarValue::from_integer(metrics.cancelled);
        else found = false;
        xSemaphoreGive(engine_mutex_);
        if (!found) return failure(core::ErrorCode::not_found, "read", "unknown-parameter");
    }
    return core::Status::success();
}

void EspFleetComponent::network_entry(void* context) noexcept { static_cast<EspFleetComponent*>(context)->run_network(); }
void EspFleetComponent::executor_entry(void* context) noexcept { static_cast<EspFleetComponent*>(context)->run_executor(); }

void EspFleetComponent::send(Message& message) noexcept {
    std::array<std::byte, kMaximumPacketBytes> envelope{};
    std::array<std::byte, transport::kEspNowPacketBytes> packet{};
    // Timestamp immediately before framing, without a per-device send queue.
    if (message.kind == Kind::beacon) message.sent_us = now_us();
    const auto encoded = encode(message, envelope);
    transport::EspNowTransmit transmit{};
    if (++transmit_sequence_ == 0U) ++transmit_sequence_;
    if (!encoded || !transmit.begin(transmit_session_, transmit_sequence_,
        std::span<const std::byte>{envelope.data(), encoded.value()})) {
        send_failed_.fetch_add(1U);
        return;
    }
    for (std::size_t i = 0; i < transmit.fragment_count(); ++i) {
        const auto size = transmit.fragment(i, packet);
        if (esp_now_send(kBroadcast, reinterpret_cast<const std::uint8_t*>(packet.data()), size) != ESP_OK)
            send_failed_.fetch_add(1U);
    }
}

void EspFleetComponent::receive_callback(void* context, const esp_now_recv_info_t* info,
                                         const std::uint8_t* data, int length) noexcept {
    auto* self = static_cast<EspFleetComponent*>(context);
    if (!self->active_.load() || info == nullptr || info->src_addr == nullptr ||
        data == nullptr || length <= 0 || length > static_cast<int>(transport::kEspNowPacketBytes)) return;
    Received received{};
    received.timestamp_us = now_us();
    for (std::size_t i = 0; i < 6; ++i) received.source = (received.source << 8U) | info->src_addr[i];
    received.size = static_cast<std::uint16_t>(length);
    std::memcpy(received.bytes.data(), data, static_cast<std::size_t>(length));
    if (xQueueSend(self->receive_queue_, &received, 0) != pdTRUE) self->invalid_packets_.fetch_add(1U);
}

void EspFleetComponent::update_epoch() noexcept {
    if (epoch_leader_ != engine_.leader() || epoch_session_ != engine_.leader_session() ||
        (epoch_sync_ && !engine_.synchronized())) {
        epoch_.fetch_add(1U);
        epoch_leader_ = engine_.leader();
        epoch_session_ = engine_.leader_session();
    }
    epoch_sync_ = engine_.synchronized();
}

void EspFleetComponent::run_network() noexcept {
    Message incoming{}, outgoing{};
    Received received{};
    Execution execution{};
    bool radio_lease{}, latency_lease{}, subscribed{};
    std::uint64_t activation_due{};
    auto deactivate = [&]() {
        active_.store(false);
        if (subscribed) { transport::EspNowRadio::shared().unsubscribe(this); subscribed = false; }
        if (latency_lease) { wifi_->release_low_latency(); latency_lease = false; }
        if (radio_lease) {
            if (!wifi_->release_autonomous_radio()) send_failed_.fetch_add(1U);
            radio_lease = false;
        }
        xSemaphoreTake(engine_mutex_, portMAX_DELAY);
        engine_.cancel_all();
        epoch_.fetch_add(1U);
        xSemaphoreGive(engine_mutex_);
        reassembly_.reset();
        reassembly_source_ = 0;
        while (xQueueReceive(receive_queue_, &received, 0) == pdTRUE) {}
    };
    while (started_.load()) {
        taskYIELD();
        const bool reconfigure = reconfigure_.exchange(false);
        const auto wifi_state = wifi_->connection_state();
        const bool radio_unavailable = wifi_state == network::WifiConnectionState::disabled ||
                                       wifi_state == network::WifiConnectionState::off;
        if (reconfigure || !enabled_.load() || !control_ready_.load() || radio_unavailable) {
            if (radio_lease || subscribed) deactivate();
            activation_due = now_us();
        }
        if (!subscribed && enabled_.load() && control_ready_.load() && !radio_unavailable && now_us() >= activation_due) {
            activation_due = now_us() + 1'000'000U;
            radio_lease = wifi_->acquire_autonomous_radio(channel_.load()).ok();
            if (radio_lease) latency_lease = wifi_->acquire_low_latency().ok();
            if (latency_lease) subscribed = transport::EspNowRadio::shared().subscribe(this, receive_callback, true).ok();
            if (!subscribed) deactivate();
            else {
                transmit_session_ = session();
                transmit_sequence_ = 0;
                xSemaphoreTake(engine_mutex_, portMAX_DELAY);
                static_cast<void>(engine_.start(fleet_id_.load(), node_, transmit_session_, now_us()));
                update_epoch();
                active_.store(true);
                xSemaphoreGive(engine_mutex_);
            }
        }
        if (!subscribed) { vTaskDelay(1); continue; }
        xSemaphoreTake(engine_mutex_, portMAX_DELAY);
        engine_.service(now_us());
        update_epoch();
        while (engine_.take_due(now_us(), execution.cue)) {
            execution.epoch = epoch_.load();
            if (xQueueSend(execution_queue_, &execution, 0) != pdTRUE) execution_dropped_.fetch_add(1U);
        }
        xSemaphoreGive(engine_mutex_);
        for (std::size_t i = 0; i < kMaximumCues + 1U; ++i) {
            xSemaphoreTake(engine_mutex_, portMAX_DELAY);
            const bool available = engine_.next_message(now_us(), outgoing);
            update_epoch();
            xSemaphoreGive(engine_mutex_);
            if (!available) break;
            send(outgoing);
        }
        // A finite receive wait services cues even when the radio is silent.
        if (xQueueReceive(receive_queue_, &received, 1) != pdTRUE) continue;
        transport::EspNowPacketView fragment{};
        if (!transport::decode_espnow_packet({received.bytes.data(), received.size}, fragment) ||
            fragment.kind != transport::EspNowPacketKind::data) { invalid_packets_.fetch_add(1U); continue; }
        if (received.source != reassembly_source_) {
            reassembly_.reset();
            reassembly_source_ = received.source;
        }
        const auto result = reassembly_.accept(fragment);
        if (result == transport::EspNowReceiveResult::rejected) { invalid_packets_.fetch_add(1U); continue; }
        if (result != transport::EspNowReceiveResult::complete) continue;
        if (!decode(reassembly_.envelope(), incoming) || incoming.sender != received.source ||
            incoming.fleet_id != fleet_id_.load()) { invalid_packets_.fetch_add(1U); continue; }
        xSemaphoreTake(engine_mutex_, portMAX_DELAY);
        static_cast<void>(engine_.receive(incoming, received.timestamp_us));
        engine_.service(now_us());
        update_epoch();
        xSemaphoreGive(engine_mutex_);
    }
    deactivate();
    network_quiesced_.store(true);
    vTaskDelete(nullptr);
}

void EspFleetComponent::run_executor() noexcept {
    Execution execution{};
    while (started_.load()) {
        if (xQueueReceive(execution_queue_, &execution, pdMS_TO_TICKS(10)) != pdTRUE) continue;
        xSemaphoreTake(engine_mutex_, portMAX_DELAY);
        engine_.service(now_us());
        update_epoch();
        const auto time = engine_.time_us(now_us());
        const bool valid = active_.load() && control_ready_.load() && engine_.synchronized() &&
            execution.epoch == epoch_.load() && time >= execution.cue.deadline_us &&
            time - execution.cue.deadline_us <= kMaximumCueLatenessUs;
        xSemaphoreGive(engine_mutex_);
        if (!valid) { execution_dropped_.fetch_add(1U); continue; }
        const auto decoded = transport::decode_control_message(
            std::span<const std::byte>{execution.cue.bytes.data(), execution.cue.size});
        if (!decoded || decoded.value().error_code != core::ErrorCode::none ||
            decoded.value().operation == core::ControlOperation::read_parameter ||
            decoded.value().component_id == descriptor_.id) {
            execution_failed_.fetch_add(1U);
            continue;
        }
        const auto& message = decoded.value();
        const core::ControlRequest request{message.operation, message.component_id, message.control_id,
            std::span<const core::ScalarValue>{message.values.data(), message.value_count}};
        core::ControlResponse response{};
        last_execution_us_.store(time);
        const auto lateness = static_cast<std::uint32_t>(time - execution.cue.deadline_us);
        if (lateness > maximum_lateness_us_.load()) maximum_lateness_us_.store(lateness);
        if (controls_->execute(request, response)) executed_.fetch_add(1U);
        else execution_failed_.fetch_add(1U);
    }
    executor_quiesced_.store(true);
    vTaskDelete(nullptr);
}

} // namespace blip::fleet
