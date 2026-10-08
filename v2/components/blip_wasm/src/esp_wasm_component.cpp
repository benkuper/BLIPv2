#include "blip/wasm/esp_wasm_component.hpp"
#include "blip/wasm/module_upload.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_pthread.h"
#include "esp_timer.h"
#include <algorithm>
#include <cstring>
#include <limits>

namespace blip::wasm {
namespace {
using core::ErrorCode;
core::Status failure(ErrorCode code, std::string_view operation, std::string_view detail) noexcept {
    return core::Status::failure({core::ErrorDomain::control, code, "blip.wasm", operation, detail});
}
struct Guard {
    SemaphoreHandle_t mutex;
    bool held;
    explicit Guard(SemaphoreHandle_t value, TickType_t wait = pdMS_TO_TICKS(2)) noexcept
        : mutex(value), held(value && xSemaphoreTake(value, wait) == pdTRUE) {}
    ~Guard() { if (held) xSemaphoreGive(mutex); }
};
constexpr std::array<std::string_view, 1> provided{"script.runtime"}, required{"control.dispatch"};
constexpr auto integer(std::string_view id, std::string_view label, std::string_view unit = "") {
    return core::ParameterDescriptor{id, label, core::ValueType::integer, core::Access::read_only,
        false, core::ScalarValue::from_integer(0), {}, unit};
}
constexpr std::array parameters{
    core::ParameterDescriptor{"state", "Script state", core::ValueType::string, core::Access::read_only,
        false, core::ScalarValue::from_string("stopped"), {}, ""},
    core::ParameterDescriptor{"instruction_budget", "Instruction budget per call", core::ValueType::integer,
        core::Access::read_write, false, core::ScalarValue::from_integer(10000), {true, 1, 1000000, 1}, "instructions"},
    core::ParameterDescriptor{"deadline_ms", "Execution deadline", core::ValueType::integer,
        core::Access::read_write, false, core::ScalarValue::from_integer(10), {true, 1, 50, 1}, "ms"},
    integer("generation", "Module generation"), integer("epoch", "Work epoch"),
    integer("loaded_bytes", "Loaded module bytes", "bytes"), integer("upload_received", "Uploaded bytes", "bytes"),
    integer("submitted", "Accepted requests"), integer("completed", "Completed requests"),
    integer("failed", "Failed requests"), integer("rejected", "Queue rejections"),
    integer("deadlines", "Execution deadline failures"), integer("cancelled", "Cancelled requests"),
    integer("pending", "Queued requests"), integer("last_request", "Last completed request"),
    integer("last_error_code", "Last request error code"), integer("last_elapsed_us", "Last request duration", "us"),
    integer("pool_reserved", "Reserved engine pool", "bytes"), integer("pool_used", "Used engine pool", "bytes"),
    integer("pool_peak", "Peak engine pool use", "bytes"),
    integer("buffer_reserved", "Fixed script buffer reservation", "bytes"),
    integer("native_calls", "Native capability calls"), integer("native_failures", "Native capability failures"),
    integer("native_maximum_us", "Largest native callback wall duration", "us"),
    integer("native_last_us", "Last native callback wall duration", "us"),
    integer("native_task_last_us", "Last native callback task duration", "us"),
    integer("native_task_maximum_us", "Largest native callback task duration", "us"),
    integer("worker_stack_headroom", "Worker stack headroom", "bytes"),
    integer("supervisor_stack_headroom", "Supervisor stack headroom", "bytes")};
constexpr std::array<core::FieldDescriptor, 2> begin_fields{{{"bytes", core::ValueType::integer, true}, {"crc32", core::ValueType::integer, true}}};
constexpr std::array<core::FieldDescriptor, 2> chunk_fields{{{"offset", core::ValueType::integer, true}, {"hex", core::ValueType::string, true}}};
constexpr std::array<core::FieldDescriptor, 1> call_fields{{{"export", core::ValueType::string, true}}};
constexpr std::array<core::FieldDescriptor, 2> i32_fields{{{"export", core::ValueType::string, true}, {"argument", core::ValueType::integer, true}}};
constexpr std::array<core::FieldDescriptor, 1> completion_fields{{{"request", core::ValueType::integer, true}}};
constexpr std::array<core::FieldDescriptor, 2> result_fields{{{"request", core::ValueType::integer, true}, {"index", core::ValueType::integer, true}}};
constexpr std::array<core::ActionDescriptor, 9> actions{{
    {"upload_begin", "Begin a module upload", begin_fields}, {"upload_chunk", "Upload a contiguous module chunk", chunk_fields},
    {"upload_commit", "Validate and load the uploaded module", {}}, {"call0", "Queue an export without arguments", call_fields},
    {"call_i32", "Queue an export with one i32 argument", i32_fields}, {"unload", "Queue module unload", {}},
    {"cancel_all", "Cancel active and queued work", {}}, {"completion", "Read request completion", completion_fields},
    {"result", "Read typed result bits", result_fields}}};
constexpr std::array<core::MetadataEntry, 7> metadata{{
    {"worker_priority", "2"}, {"supervisor_priority", "6"}, {"core_affinity", "none"},
    {"queue_capacity", "8"}, {"completion_capacity", "16"}, {"overflow_policy", "reject-new"},
    {"cost_ram_policy", "fixed startup buffers plus object; task stacks excluded"}}};
constexpr core::ComponentDescriptor make_descriptor() {
    core::ComponentDescriptor d{};
    d.schema_version = 1; d.id = "blip.wasm"; d.display_name = "WASM scripts";
    d.description = "Bounded worker, passive module loading and supervised script execution";
    d.provided_services = provided; d.required_services = required;
    d.parameters = parameters; d.actions = actions; d.metadata = metadata;
    d.settings = {1, 1}; d.disable_policy = core::DisablePolicy::live; d.supports_restart = true;
    // Report the maximum admitted upload reservation; actual reserved bytes
    // track its bounded worker-owned allocation. Stacks are counted separately.
    d.cost = {131072, sizeof(EspWasmComponent) +
        EspWasmComponent::kPoolBytes + EspWasmComponent::kModuleBytes,
        EspWasmComponent::kWorkerStackBytes + EspWasmComponent::kSupervisorStackBytes};
    return d;
}
std::uint32_t elapsed(std::uint64_t begin) {
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(esp_timer_get_time() - begin, 0xffffffffU));
}
int nibble(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}
}
const core::ComponentDescriptor EspWasmComponent::base_descriptor_{make_descriptor()};
const core::ComponentDescriptor& EspWasmComponent::descriptor() const noexcept { return descriptor_; }

EspWasmComponent::EspWasmComponent(Runtime& runtime) noexcept : runtime_(&runtime), descriptor_(base_descriptor_) {
    admission_ = xSemaphoreCreateMutexStatic(&admission_storage_);
    snapshot_mutex_ = xSemaphoreCreateMutexStatic(&snapshot_storage_);
    monitor_mutex_ = xSemaphoreCreateMutexStatic(&monitor_storage_);
    ready_ = xSemaphoreCreateBinaryStatic(&ready_storage_);
    queue_ = xQueueCreateStatic(kQueueCapacity, sizeof(Request), queue_bytes_.data(), &queue_storage_);
}
EspWasmComponent::~EspWasmComponent() {
    const auto released = release_reservation();
    configASSERT(released.ok());
}
core::Status EspWasmComponent::release_reservation() noexcept {
    Guard guard(admission_);
    if (!guard.held || started_.load() || worker_created_ || !callbacks_quiesced())
        return failure(ErrorCode::invalid_state, "release", "worker-not-retired");
    heap_caps_free(pool_); pool_ = nullptr;
    heap_caps_free(module_); module_ = nullptr; module_capacity_ = 0;
    heap_caps_free(linear_); linear_ = nullptr;
    buffer_reserved_.store(0);
    return core::Status::success();
}
core::Status EspWasmComponent::bind_capabilities(const core::RegistryView& registry) noexcept {
    if (started_.load() || worker_created_ || !callbacks_quiesced()) return failure(ErrorCode::invalid_state, "bind", "worker-active");
    const auto bound = capabilities_.bind(registry, true);
    if (!bound) return bound;
    std::size_t count = 1;
    required_services_[0] = required[0];
    std::string_view previous;
    for (std::size_t i = 0; i < capabilities_.size(); ++i) {
        const auto module = capabilities_.binding(i).component->wasm.import_module;
        if (module != previous) required_services_[count++] = module;
        previous = module;
    }
    descriptor_.required_services = std::span<const std::string_view>(required_services_).first(count);
    return core::Status::success();
}
core::Status EspWasmComponent::reserve_buffers() noexcept {
    Guard guard(admission_);
    if (!guard.held || started_.load() || worker_created_ || !callbacks_quiesced())
        return failure(ErrorCode::invalid_state, "reserve", "worker-not-retired");
    if (!pool_) {
        pool_ = static_cast<std::byte*>(heap_caps_aligned_alloc(8, kEnginePoolBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
#if defined(CONFIG_IDF_TARGET_ESP32)
        // Guest memory permits unaligned integer accesses. Unlike the engine pool,
        // it needs only malloc's four-byte base alignment; requesting additional
        // alignment makes TLSF skip the available 64 KiB size class on ESP32.
        linear_ = static_cast<std::byte*>(heap_caps_malloc(kLinearBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_IRAM_8BIT));
#endif
    }
    if (!pool_ || (kLinearBytes && !linear_)) {
        ESP_LOGE("blip_wasm", "startup buffers unavailable pool=%u module=%u linear=%u free=%u largest=%u iram_free=%u iram_largest=%u",
            static_cast<unsigned>(pool_ != nullptr), static_cast<unsigned>(module_ != nullptr),
            static_cast<unsigned>(linear_ != nullptr),
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
            static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_IRAM_8BIT)),
            static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_IRAM_8BIT)));
        heap_caps_free(pool_); pool_ = nullptr;
        heap_caps_free(linear_); linear_ = nullptr;
        buffer_reserved_.store(0);
        return failure(ErrorCode::resource_unavailable, "start", "runtime-buffers");
    }
    buffer_reserved_.store(kPoolBytes + module_capacity_);
    return core::Status::success();
}
core::Status EspWasmComponent::start(const core::StartContext&) noexcept {
    if (started_.load() || worker_created_ || !callbacks_quiesced()) return failure(ErrorCode::invalid_state, "start", "worker-state");
    if (epoch_.load() == 0xffffffffU || !admission_ || !snapshot_mutex_ || !monitor_mutex_ || !ready_ || !queue_)
        return failure(ErrorCode::resource_unavailable, "start", "runtime-resources");
    const bool retained = buffer_reserved_.load() != 0;
    const auto reserved = reserve_buffers();
    if (!reserved) return reserved;
    script_admission_ = false;
    xQueueReset(queue_);
    static_cast<void>(xSemaphoreTake(ready_, 0));
    control_ready_.store(false);
    start_error_.store(ErrorCode::none);
    started_.store(true);
    supervisor_quiesced_.store(false);
    if (xTaskCreate(supervisor_entry, "blip_wasm_guard", kSupervisorStackBytes, this, 6, &supervisor_) != pdPASS) {
        supervisor_quiesced_.store(true); static_cast<void>(stop());
        if (!retained) static_cast<void>(release_reservation());
        return failure(ErrorCode::start_failed, "start", "supervisor-task");
    }
    auto config = esp_pthread_get_default_config();
    esp_pthread_cfg_t previous{};
    if (esp_pthread_get_cfg(&previous) != ESP_OK) previous = config;
    config.stack_size = kWorkerStackBytes; config.prio = 2; config.thread_name = "blip_wasm";
    config.pin_to_core = -1; config.inherit_cfg = false;
    worker_quiesced_.store(false);
    const bool created = esp_pthread_set_cfg(&config) == ESP_OK && pthread_create(&worker_, nullptr, worker_entry, this) == 0;
    static_cast<void>(esp_pthread_set_cfg(&previous));
    if (!created) {
        ESP_LOGE("blip_wasm", "worker unavailable free=%u largest=%u stack=%u",
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
            static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
            static_cast<unsigned>(kWorkerStackBytes));
        worker_quiesced_.store(true); static_cast<void>(stop());
        if (!retained) static_cast<void>(release_reservation());
        return failure(ErrorCode::start_failed, "start", "worker-task");
    }
    worker_created_ = true;
    if (xSemaphoreTake(ready_, pdMS_TO_TICKS(1000)) != pdTRUE || start_error_.load() != ErrorCode::none) {
        const auto code = start_error_.load();
        static_cast<void>(stop());
        if (!retained) static_cast<void>(release_reservation());
        return failure(code == ErrorCode::none ? ErrorCode::start_failed : code, "start", "worker-initialization");
    }
    return core::Status::success();
}
core::Status EspWasmComponent::stop() noexcept {
    {
        Guard guard(admission_);
        if (!guard.held) return failure(ErrorCode::stop_failed, "stop", "admission-busy");
        control_ready_.store(false);
        script_admission_ = false;
        controls_.retire();
        started_.store(false);
        if (epoch_.load() != 0xffffffffU) epoch_.fetch_add(1);
    }
    call_cancelled_.store(true);
    runtime_->request_cancel();
    for (unsigned i = 0; i < 100 && !callbacks_quiesced(); ++i) vTaskDelay(pdMS_TO_TICKS(10));
    if (!callbacks_quiesced()) return failure(ErrorCode::stop_failed, "stop", "worker-active");
    if (worker_created_) { pthread_join(worker_, nullptr); worker_created_ = false; }
    supervisor_ = nullptr;
    return core::Status::success();
}
bool EspWasmComponent::callbacks_quiesced() const noexcept {
    return worker_quiesced_.load() && supervisor_quiesced_.load() && controls_.quiescent();
}

core::Result<std::uint32_t> EspWasmComponent::submit(Request& request) noexcept {
    using Result = core::Result<std::uint32_t>;
    Guard guard(admission_);
    if (!guard.held || !started_.load() || !control_ready_.load())
        return Result::failure(failure(ErrorCode::invalid_state, "submit", "not-ready").error());
    if (next_id_ == 0xffffffffU || epoch_.load() == 0xffffffffU)
        return Result::failure(failure(ErrorCode::generation_exhausted, "submit", "request-generation").error());
    request.id = next_id_ + 1;
    request.epoch = epoch_.load();
    if (xQueueSend(queue_, &request, 0) != pdTRUE) {
        rejected_.fetch_add(1);
        return Result::failure(failure(ErrorCode::queue_full, "submit", "worker-queue-full").error());
    }
    ++next_id_;
    if (request.kind == Kind::begin || request.kind == Kind::commit || request.kind == Kind::unload) {
        script_admission_ = false; closed_through_ = request.id;
    }
    return Result::success(request.id);
}
core::Status EspWasmComponent::read_dynamic_parameter(std::uint32_t generation, std::string_view id,
    core::ScalarValue& output, std::span<char> strings) noexcept {
    return controls_.read_id(generation, id, output, strings);
}
core::Status EspWasmComponent::write_dynamic_parameter(std::uint32_t generation, std::string_view id,
    const core::ScalarValue& value) noexcept {
    return controls_.write_id(generation, id, value);
}
core::Status EspWasmComponent::invoke_dynamic_action(std::uint32_t generation, std::string_view id,
    std::span<const core::ScalarValue> input, std::span<core::ScalarValue> output, std::span<char>, std::size_t& count) noexcept {
    count = 0;
    if (output.empty()) return failure(ErrorCode::capacity_exceeded, "action", "request-id-output");
    Guard guard(admission_);
    if (!guard.held || !started_.load() || !control_ready_.load() || !script_admission_)
        return failure(ErrorCode::invalid_state, "action", "script-admission-closed");
    if (next_id_ == UINT32_MAX || epoch_.load() == UINT32_MAX)
        return failure(ErrorCode::generation_exhausted, "action", "request-generation");
    if (!uxQueueSpacesAvailable(queue_)) {
        rejected_.fetch_add(1); return failure(ErrorCode::queue_full, "action", "worker-queue-full");
    }
    Request request{}; request.kind = Kind::script_action; request.id = next_id_ + 1; request.epoch = epoch_.load();
    request.instructions = instruction_budget_.load(); request.deadline_ms = deadline_ms_.load();
    const auto copied = controls_.enqueue_action(generation, id, input, request.id, request.epoch);
    if (!copied) { rejected_.fetch_add(1); return copied; }
    // All producers hold admission_; the consumer only frees slots. Space was
    // checked before the store copy, so this send cannot reject an owned ticket.
    const auto queued = xQueueSend(queue_, &request, 0); configASSERT(queued == pdTRUE);
    ++next_id_; output[0] = core::ScalarValue::from_integer(request.id); count = 1;
    return core::Status::success();
}
core::Result<std::uint32_t> EspWasmComponent::call(std::string_view name, std::span<const Value> arguments) noexcept {
    using Result = core::Result<std::uint32_t>;
    if (name.empty() || name.size() > kMaximumExportNameBytes || name.find('\0') != name.npos || arguments.size() > kMaximumArguments)
        return Result::failure(failure(ErrorCode::invalid_argument, "call", "name-or-arguments").error());
    Request request{}; request.kind = Kind::call;
    {
        Guard guard(snapshot_mutex_);
        if (!guard.held || snapshot_.state != State::loaded)
            return Result::failure(failure(ErrorCode::invalid_state, "call", "module-not-loaded").error());
        request.generation = snapshot_.generation;
    }
    std::memcpy(request.name.data(), name.data(), name.size());
    std::copy(arguments.begin(), arguments.end(), request.arguments.begin());
    request.argument_count = static_cast<std::uint8_t>(arguments.size());
    request.instructions = instruction_budget_.load(); request.deadline_ms = deadline_ms_.load();
    return submit(request);
}
void EspWasmComponent::complete(const Request& request, core::Status status, std::span<const Value> results,
                                std::uint32_t duration, const Snapshot& snapshot, std::uint32_t uploaded) noexcept {
    Guard guard(snapshot_mutex_, portMAX_DELAY);
    auto& slot = completions_[request.id % kCompletionCapacity];
    slot = {}; slot.id = request.id; slot.elapsed_us = duration; slot.error = status.error().code;
    slot.count = static_cast<std::uint8_t>(status ? results.size() : 0);
    if (status) std::copy(results.begin(), results.end(), slot.results.begin());
    snapshot_ = snapshot; upload_received_ = uploaded;
    last_id_ = request.id; last_error_ = status.error().code; last_elapsed_us_ = duration;
    completed_.fetch_add(1); if (!status) failed_.fetch_add(1);
}
void* EspWasmComponent::worker_entry(void* context) noexcept {
    static_cast<EspWasmComponent*>(context)->run_worker(); return nullptr;
}
void EspWasmComponent::supervisor_entry(void* context) noexcept {
    auto* self = static_cast<EspWasmComponent*>(context);
    self->run_supervisor(); self->supervisor_quiesced_.store(true); vTaskDelete(nullptr);
}
void EspWasmComponent::run_worker() noexcept {
    Service service(*runtime_, {pool_, kEnginePoolBytes}, {module_, module_capacity_}, {linear_, kLinearBytes});
    ModuleUpload upload({module_, module_capacity_});
    auto initialized = runtime_->configure_capabilities(capabilities_.size() ? &capabilities_ : nullptr);
    if (initialized) initialized = service.start();
    if (!initialized) {
        const auto& error = initialized.error();
        ESP_LOGE("blip_wasm", "runtime initialization failed code=%u detail=%.*s",
            static_cast<unsigned>(error.code), static_cast<int>(error.detail.size()), error.detail.data());
    }
    start_error_.store(initialized.error().code);
    { Guard guard(snapshot_mutex_, portMAX_DELAY); snapshot_ = service.snapshot(); }
    xSemaphoreGive(ready_);
    auto current_epoch = epoch_.load();
    while (initialized && started_.load()) {
        if (current_epoch != epoch_.load()) {
            current_epoch = epoch_.load(); upload.cancel();
            Guard guard(snapshot_mutex_, portMAX_DELAY); upload_received_ = 0;
        }
        Request request{};
        if (xQueueReceive(queue_, &request, pdMS_TO_TICKS(20)) != pdTRUE) continue;
        const auto began = static_cast<std::uint64_t>(esp_timer_get_time());
        core::Status status = core::Status::success();
        std::array<Value, kMaximumResults> results{}; std::size_t count{};
        // Markers and copied payloads are admitted together under admission_.
        // Take even canceled payloads so every accepted ticket is completed and
        // replacement cannot strand an old action in the store.
        if (request.kind == Kind::script_action) {
            Guard guard(admission_, portMAX_DELAY);
            status = controls_.take_action(action_scratch_);
            if (status && (action_scratch_.ticket != request.id || action_scratch_.epoch != request.epoch))
                status = failure(ErrorCode::verification_failed, "action", "payload-order");
        }
        if (!started_.load() || request.epoch != epoch_.load()) {
            status = failure(ErrorCode::cancelled, "work", "stale-work-epoch"); cancelled_.fetch_add(1);
        } else if (!status) {
            // Preserve the bounded payload admission failure.
        } else if (request.kind == Kind::begin) {
            controls_.retire();
            service.unload(); upload.cancel();
            if (module_capacity_ != request.number) {
                status = service.replace_module_storage({});
                if (status) {
                    heap_caps_free(module_); module_ = nullptr; module_capacity_ = 0;
                    module_ = static_cast<std::byte*>(heap_caps_malloc(request.number, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
                    module_capacity_ = module_ ? request.number : 0;
                    upload = ModuleUpload({module_, module_capacity_});
                    buffer_reserved_.store(kPoolBytes + module_capacity_);
                    status = module_ ? service.replace_module_storage({module_, module_capacity_})
                                     : failure(ErrorCode::resource_unavailable, "upload", "module-buffer-unavailable");
                }
            }
            if (status) status = upload.begin(request.number, request.crc);
        } else if (request.kind == Kind::chunk) {
            status = upload.append(request.number, std::span<const std::byte>(request.bytes).first(request.size));
        } else if (request.kind == Kind::commit) {
            const auto bytes = upload.finish();
            controls_.retire();
            status = bytes ? controls_.prepare(bytes.value(), descriptor_) : core::Status::failure(bytes.error());
            if (status) status = service.load(bytes.value());
            {
                Guard guard(admission_, portMAX_DELAY);
                if (status && (!started_.load() || request.epoch != epoch_.load()))
                    status = failure(ErrorCode::cancelled, "commit", "stale-work-epoch");
                if (status) status = controls_.publish(*runtime_, service.snapshot().generation);
                if (!status) { controls_.retire(); service.unload(); }
                script_admission_ = status.ok() && started_.load() && request.epoch == epoch_.load() && request.id >= closed_through_;
            }
        } else if (request.kind == Kind::unload) {
            controls_.retire();
            upload.cancel(); service.unload();
        } else if (request.kind == Kind::call || request.kind == Kind::script_action) {
            const bool script = request.kind == Kind::script_action;
            if ((script ? action_scratch_.module_generation : request.generation) != service.snapshot().generation ||
                (script && action_scratch_.generation != controls_.generation())) {
                status = failure(ErrorCode::cancelled, "call", "stale-module-generation"); cancelled_.fetch_add(1);
            } else {
                if (script) {
                    std::size_t arguments{};
                    status = copy_script_action_arguments(*runtime_, controls_.action_buffer(), action_scratch_, request.arguments, arguments);
                    if (status) {
                        request.argument_count = static_cast<std::uint8_t>(arguments);
                        request.name = action_scratch_.name;
                    }
                }
                {
                    Guard guard(monitor_mutex_, portMAX_DELAY);
                    active_epoch_ = request.epoch;
                    active_deadline_ = began + static_cast<std::uint64_t>(request.deadline_ms) * 1000;
                    cancellation_ = Cancellation::none;
                    call_cancelled_.store(!started_.load() || request.epoch != epoch_.load()); active_ = true;
                }
                // The platform owns the absolute deadline. Runtime receives fuel
                // and the admission token; the supervisor issues active cancel.
                if (status) status = service.call(request.name.data(), std::span<const Value>(request.arguments).first(request.argument_count),
                    {request.instructions, 0, &call_cancelled_}, results, count);
                controls_.end_invocation();
                Cancellation reason{};
                {
                    Guard guard(monitor_mutex_, portMAX_DELAY);
                    reason = cancellation_;
                    if (request.epoch != epoch_.load()) reason = Cancellation::requested;
                    else if (static_cast<std::uint64_t>(esp_timer_get_time()) >= active_deadline_) reason = Cancellation::deadline;
                    active_ = false;
                }
                if (reason != Cancellation::none) {
                    // Even a late successful return cannot remain runnable.
                    if (service.snapshot().state == State::loaded) service.unload();
                    count = 0;
                    status = failure(reason == Cancellation::deadline ? ErrorCode::budget_exceeded : ErrorCode::cancelled,
                        "call", reason == Cancellation::deadline ? "execution-deadline" : "execution-cancelled");
                    if (reason == Cancellation::deadline) deadlines_.fetch_add(1); else cancelled_.fetch_add(1);
                }
                if (service.snapshot().state != State::loaded) {
                    controls_.retire();
                    Guard guard(admission_, portMAX_DELAY); script_admission_ = false;
                }
            }
        }
        worker_headroom_.store(uxTaskGetStackHighWaterMark(nullptr));
        complete(request, status, std::span<const Value>(results).first(count), elapsed(began), service.snapshot(), upload.received());
    }
    Request pending{};
    while (xQueueReceive(queue_, &pending, 0) == pdTRUE) {
        if (pending.kind == Kind::script_action) {
            Guard guard(admission_, portMAX_DELAY);
            const auto taken = controls_.take_action(action_scratch_);
            configASSERT(taken.ok() && action_scratch_.ticket == pending.id);
        }
        cancelled_.fetch_add(1);
        complete(pending, failure(ErrorCode::cancelled, "stop", "worker-stopped"), {}, 0, service.snapshot(), 0);
    }
    controls_.retire(); service.stop();
    { Guard guard(snapshot_mutex_, portMAX_DELAY); snapshot_ = service.snapshot(); upload_received_ = 0; }
    worker_quiesced_.store(true);
}
void EspWasmComponent::run_supervisor() noexcept {
    while (started_.load() || !worker_quiesced_.load()) {
        {
            Guard guard(monitor_mutex_, portMAX_DELAY);
            if (active_) {
                if (!started_.load() || active_epoch_ != epoch_.load()) cancellation_ = Cancellation::requested;
                else if (static_cast<std::uint64_t>(esp_timer_get_time()) >= active_deadline_) cancellation_ = Cancellation::deadline;
                if (cancellation_ != Cancellation::none) {
                    call_cancelled_.store(true);
                    // Retry while active: cancellation can arrive between the
                    // admission check and the engine publishing its active call.
                    runtime_->request_cancel();
                }
            }
        }
        supervisor_headroom_.store(uxTaskGetStackHighWaterMark(nullptr));
        vTaskDelay(1);
    }
}

core::Status EspWasmComponent::read_parameter(std::string_view id, core::ScalarValue& output) noexcept {
    if (id == "submitted") {
        Guard guard(admission_); if (!guard.held) return failure(ErrorCode::resource_unavailable, "read", "admission-busy");
        output = core::ScalarValue::from_integer(next_id_); return core::Status::success();
    }
    std::uint32_t value{};
    if (id == "instruction_budget") value = instruction_budget_.load();
    else if (id == "deadline_ms") value = deadline_ms_.load();
    else if (id == "epoch") value = epoch_.load();
    else if (id == "buffer_reserved") value = buffer_reserved_.load();
    else if (id == "completed") value = completed_.load();
    else if (id == "failed") value = failed_.load();
    else if (id == "rejected") value = rejected_.load();
    else if (id == "deadlines") value = deadlines_.load();
    else if (id == "cancelled") value = cancelled_.load();
    else if (id == "pending") value = uxQueueMessagesWaiting(queue_);
    else if (id == "worker_stack_headroom") value = worker_headroom_.load();
    else if (id == "supervisor_stack_headroom") value = supervisor_headroom_.load();
    else {
        Guard guard(snapshot_mutex_); if (!guard.held) return failure(ErrorCode::resource_unavailable, "read", "snapshot-busy");
        if (id == "state") {
            std::string_view name = "stopped";
            if (snapshot_.state == State::ready) name = "ready";
            else if (snapshot_.state == State::loaded) name = "loaded";
            else if (snapshot_.state == State::faulted) name = "faulted";
            output = core::ScalarValue::from_string(name); return core::Status::success();
        }
        if (id == "generation") value = snapshot_.generation;
        else if (id == "loaded_bytes") value = snapshot_.loaded_bytes;
        else if (id == "upload_received") value = upload_received_;
        else if (id == "last_request") value = last_id_;
        else if (id == "last_error_code") value = static_cast<std::uint32_t>(last_error_);
        else if (id == "last_elapsed_us") value = last_elapsed_us_;
        else if (id == "pool_reserved") value = snapshot_.runtime.reserved_bytes;
        else if (id == "pool_used") value = snapshot_.runtime.used_bytes;
        else if (id == "pool_peak") value = snapshot_.runtime.peak_bytes;
        else if (id == "native_calls") value = snapshot_.runtime.native_calls;
        else if (id == "native_failures") value = snapshot_.runtime.native_failures;
        else if (id == "native_maximum_us") value = snapshot_.runtime.native_maximum_us;
        else if (id == "native_last_us") value = snapshot_.runtime.native_last_us;
        else if (id == "native_task_last_us") value = snapshot_.runtime.native_task_last_us;
        else if (id == "native_task_maximum_us") value = snapshot_.runtime.native_task_maximum_us;
        else return failure(ErrorCode::not_found, "read", "unknown-parameter");
    }
    output = core::ScalarValue::from_integer(value); return core::Status::success();
}
core::Status EspWasmComponent::write_parameter(std::string_view id, const core::ScalarValue& value) noexcept {
    if (!started_.load() || !control_ready_.load()) return failure(ErrorCode::invalid_state, "write", "not-ready");
    if (value.type != core::ValueType::integer || value.integer < 1) return failure(ErrorCode::invalid_argument, "write", "budget-value");
    if (id == "instruction_budget" && value.integer <= 1000000) instruction_budget_.store(static_cast<std::uint32_t>(value.integer));
    else if (id == "deadline_ms" && value.integer <= 50) deadline_ms_.store(static_cast<std::uint32_t>(value.integer));
    else return failure(ErrorCode::invalid_argument, "write", "budget-value");
    return core::Status::success();
}
core::Status EspWasmComponent::invoke_action(std::string_view id, std::span<const core::ScalarValue> args,
                                           std::span<core::ScalarValue> outputs, std::size_t& count) noexcept {
    count = 0;
    if (id == "completion" || id == "result") {
        const auto expected = id == "result" ? 2U : 1U;
        if (args.size() != expected || args[0].type != core::ValueType::integer || args[0].integer < 1 || args[0].integer > 0xffffffffLL || outputs.size() < (id == "result" ? 3U : 4U))
            return failure(ErrorCode::invalid_argument, "completion", "request-or-output");
        const auto request = static_cast<std::uint32_t>(args[0].integer);
        std::uint32_t submitted{};
        { Guard guard(admission_); if (!guard.held) return failure(ErrorCode::resource_unavailable, "completion", "admission-busy"); submitted = next_id_; }
        if (request > submitted) return failure(ErrorCode::not_found, "completion", "unknown-request");
        Guard guard(snapshot_mutex_); if (!guard.held) return failure(ErrorCode::resource_unavailable, "completion", "snapshot-busy");
        const auto& slot = completions_[request % kCompletionCapacity];
        if (slot.id != request && request <= last_id_) return failure(ErrorCode::not_found, "completion", "completion-expired");
        if (id == "result") {
            if (slot.id != request || slot.error != ErrorCode::none || args[1].type != core::ValueType::integer || args[1].integer < 0 || args[1].integer >= slot.count)
                return failure(ErrorCode::invalid_state, "result", "result-unavailable");
            const auto& value = slot.results[static_cast<std::size_t>(args[1].integer)];
            outputs[0] = core::ScalarValue::from_integer(static_cast<int>(value.type));
            outputs[1] = core::ScalarValue::from_integer(static_cast<std::uint32_t>(value.bits));
            outputs[2] = core::ScalarValue::from_integer(static_cast<std::uint32_t>(value.bits >> 32)); count = 3;
        } else {
            const bool ready = slot.id == request;
            outputs[0] = core::ScalarValue::from_bool(ready);
            outputs[1] = core::ScalarValue::from_integer(ready ? static_cast<int>(slot.error) : 0);
            outputs[2] = core::ScalarValue::from_integer(ready ? slot.count : 0);
            outputs[3] = core::ScalarValue::from_integer(ready ? slot.elapsed_us : 0); count = 4;
        }
        return core::Status::success();
    }
    if (id == "cancel_all" && args.empty()) {
        Guard guard(admission_); if (!guard.held || !started_.load() || !control_ready_.load()) return failure(ErrorCode::invalid_state, "cancel", "not-ready");
        if (epoch_.load() == 0xffffffffU) return failure(ErrorCode::generation_exhausted, "cancel", "work-epoch");
        epoch_.fetch_add(1); return core::Status::success();
    }
    if (outputs.empty()) return failure(ErrorCode::capacity_exceeded, "submit", "request-id-output");
    if (id == "call0" || id == "call_i32") {
        const bool one = id == "call_i32";
        if (args.size() != (one ? 2U : 1U) || args[0].type != core::ValueType::string ||
            (one && (args[1].type != core::ValueType::integer || args[1].integer < -2147483648LL || args[1].integer > 0xffffffffLL)))
            return failure(ErrorCode::invalid_argument, "call", "argument-value");
        const std::array values{Value::i32(one ? static_cast<std::uint32_t>(args[1].integer) : 0)};
        const auto result = call(args[0].string, one ? std::span<const Value>(values) : std::span<const Value>{});
        if (!result) return core::Status::failure(result.error());
        outputs[0] = core::ScalarValue::from_integer(result.value()); count = 1; return core::Status::success();
    }
    Request request{};
    if (id == "upload_begin" && args.size() == 2 && args[0].type == core::ValueType::integer && args[1].type == core::ValueType::integer &&
        args[0].integer >= 8 && args[0].integer <= static_cast<std::int64_t>(kModuleBytes) && args[1].integer >= 0 && args[1].integer <= 0xffffffffLL) {
        request.kind = Kind::begin; request.number = static_cast<std::uint32_t>(args[0].integer); request.crc = static_cast<std::uint32_t>(args[1].integer);
    } else if (id == "upload_chunk" && args.size() == 2 && args[0].type == core::ValueType::integer && args[1].type == core::ValueType::string &&
        args[0].integer >= 0 && args[0].integer < static_cast<std::int64_t>(kModuleBytes)) {
        const auto hex = args[1].string;
        if (hex.empty() || hex.size() % 2 || hex.size() > 2 * kChunkBytes) return failure(ErrorCode::invalid_argument, "upload", "hex-size");
        request.kind = Kind::chunk; request.number = static_cast<std::uint32_t>(args[0].integer); request.size = static_cast<std::uint8_t>(hex.size() / 2);
        for (std::size_t i = 0; i < request.size; ++i) {
            const auto high = nibble(hex[2 * i]), low = nibble(hex[2 * i + 1]);
            if (high < 0 || low < 0) return failure(ErrorCode::invalid_argument, "upload", "hex-character");
            request.bytes[i] = static_cast<std::byte>((high << 4) | low);
        }
    } else if (id == "upload_commit" && args.empty()) request.kind = Kind::commit;
    else if (id == "unload" && args.empty()) request.kind = Kind::unload;
    else return failure(ErrorCode::invalid_argument, "action", "invalid-action");
    const auto result = submit(request);
    if (!result) return core::Status::failure(result.error());
    outputs[0] = core::ScalarValue::from_integer(result.value()); count = 1; return core::Status::success();
}
} // namespace blip::wasm
