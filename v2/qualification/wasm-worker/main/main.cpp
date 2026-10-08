#include "blip/wasm/esp_wasm_component.hpp"
#include "blip/wasm/wamr_runtime.hpp"
#include "../../wasm-service/main/fixtures.hpp"
#include "faults.hpp"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <algorithm>
#include <atomic>
#include <cstdio>

namespace {
using namespace blip::wasm;
using blip::core::ErrorCode;
using Scalar = blip::core::ScalarValue;
unsigned checks{}, failures{}, cycle_count{}, fault_count{};
std::uint32_t previous_id{}, minimum_worker{UINT32_MAX}, minimum_supervisor{UINT32_MAX};
std::int64_t maximum_stop_us{};
// Observe the actual production invocation, without changing its budget or
// cancellation behavior. Initialization rejection is a separate backend fault.
class ObservedRuntime final : public Runtime {
  public:
    WamrRuntime delegate;
    std::atomic<bool> active{}, reject_initialize{};
    std::string_view name() const noexcept override { return delegate.name(); }
    blip::core::Status initialize(std::span<std::byte> pool, Limits limits, std::span<std::byte> linear) noexcept override {
        if (reject_initialize.exchange(false)) return blip::core::Status::failure(
            {blip::core::ErrorDomain::lifecycle, ErrorCode::resource_unavailable, "qualification", "initialize", "injected-backend-failure"});
        return delegate.initialize(pool, limits, linear);
    }
    blip::core::Status load(std::span<std::byte> bytes) noexcept override { return delegate.load(bytes); }
    void unload() noexcept override { delegate.unload(); }
    void shutdown() noexcept override { delegate.shutdown(); }
    blip::core::Result<Signature> signature(std::string_view name) noexcept override { return delegate.signature(name); }
    blip::core::Status invoke(std::string_view name, std::span<const Value> args, ExecutionBudget budget,
                              std::span<Value> results, std::size_t& count) noexcept override {
        active.store(true);
        auto status = delegate.invoke(name, args, budget, results, count);
        active.store(false);
        return status;
    }
    void request_cancel() noexcept override { delegate.request_cancel(); }
    RuntimeSnapshot snapshot() const noexcept override { return delegate.snapshot(); }
    blip::core::Status read_memory(std::uint32_t offset, std::span<std::byte> output) noexcept override { return delegate.read_memory(offset, output); }
    blip::core::Status write_memory(std::uint32_t offset, std::span<const std::byte> input) noexcept override { return delegate.write_memory(offset, input); }
};
ObservedRuntime runtime;
EspWasmComponent component(runtime);

void check(bool valid, const char* name) {
    ++checks;
    if (!valid) {
        ++failures;
        std::printf("WORKER {\"type\":\"failure\",\"check\":\"%s\"}\n", name);
    }
}
struct Heap {
    std::size_t dram, iram;
    UBaseType_t tasks;
    bool operator==(const Heap&) const = default;
};
Heap heap() {
    // FreeRTOS releases supervisor task storage from idle after self deletion.
    vTaskDelay(pdMS_TO_TICKS(50));
    return {heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
            heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_IRAM_8BIT), uxTaskGetNumberOfTasks()};
}
std::uint32_t read(std::string_view name) {
    Scalar value;
    auto status = component.read_parameter(name, value);
    check(status.ok() && value.type == blip::core::ValueType::integer, "read-diagnostic");
    return static_cast<std::uint32_t>(value.integer);
}
bool state(std::string_view expected) {
    Scalar value;
    return component.read_parameter("state", value) && value.string == expected;
}
std::uint32_t action(std::string_view name, std::span<const Scalar> args = {}) {
    std::array<Scalar, 4> results{};
    std::size_t count{};
    const auto status = component.invoke_action(name, args, results, count);
    check(status.ok() && count == 1, "action-admitted");
    if (!status || count != 1) return 0;
    const auto id = static_cast<std::uint32_t>(results[0].integer);
    check(id > previous_id, "request-id-not-reused");
    previous_id = id;
    return id;
}
struct Completion { bool ready{}; ErrorCode error{}; std::size_t count{}; std::uint32_t elapsed{}; };
Completion completion(std::uint32_t id) {
    const std::array args{Scalar::from_integer(id)};
    const auto until = esp_timer_get_time() + 1000000;
    do {
        std::array<Scalar, 4> output{};
        std::size_t count{};
        const auto status = component.invoke_action("completion", args, output, count);
        if (status && count == 4 && output[0].boolean) return {true, static_cast<ErrorCode>(output[1].integer),
            static_cast<std::size_t>(output[2].integer), static_cast<std::uint32_t>(output[3].integer)};
        vTaskDelay(1);
    } while (esp_timer_get_time() < until);
    return {};
}
bool acknowledge(std::uint32_t id) {
    const auto done = completion(id);
    const bool valid = id && done.ready && done.error == ErrorCode::none && done.count == 0;
    check(valid, "upload-completed");
    return valid;
}
std::uint32_t crc(std::span<const std::byte> bytes) {
    std::uint32_t value = UINT32_MAX;
    for (const auto byte : bytes) {
        value ^= std::to_integer<std::uint8_t>(byte);
        for (unsigned i = 0; i < 8; ++i) value = (value >> 1) ^ (0xedb88320U & (0U - (value & 1U)));
    }
    return ~value;
}
bool begin_upload() {
    const std::array args{Scalar::from_integer(fixtures::workloads.size()), Scalar::from_integer(crc(fixtures::workloads))};
    return acknowledge(action("upload_begin", args));
}
bool chunk(std::size_t offset, std::size_t length) {
    std::array<char, EspWasmComponent::kChunkBytes * 2> hex{};
    constexpr char digits[] = "0123456789abcdef";
    for (std::size_t i = 0; i < length; ++i) {
        const auto byte = std::to_integer<unsigned>(fixtures::workloads[offset + i]);
        hex[2 * i] = digits[byte >> 4]; hex[2 * i + 1] = digits[byte & 15];
    }
    const std::array args{Scalar::from_integer(offset), Scalar::from_string({hex.data(), length * 2})};
    return acknowledge(action("upload_chunk", args));
}
bool load() {
    if (!begin_upload()) return false;
    for (std::size_t at = 0; at < fixtures::workloads.size(); at += EspWasmComponent::kChunkBytes)
        if (!chunk(at, std::min(EspWasmComponent::kChunkBytes, fixtures::workloads.size() - at))) return false;
    return acknowledge(action("upload_commit")) && state("loaded");
}
void sample_stack() {
    const auto worker = read("worker_stack_headroom"), supervisor = read("supervisor_stack_headroom");
    check(worker > 1024 && supervisor > 1024, "native-stack-headroom");
    minimum_worker = std::min(minimum_worker, worker);
    minimum_supervisor = std::min(minimum_supervisor, supervisor);
}
bool echo(std::uint32_t value) {
    const std::array args{Value::i32(value)};
    const auto submitted = component.call("echo", args);
    if (!submitted) return false;
    check(submitted.value() > previous_id, "call-id-not-reused");
    previous_id = submitted.value();
    const auto done = completion(submitted.value());
    const std::array result_args{Scalar::from_integer(submitted.value()), Scalar::from_integer(0)};
    std::array<Scalar, 3> result{};
    std::size_t count{};
    const auto result_status = component.invoke_action("result", result_args, result, count);
    sample_stack();
    return done.ready && done.error == ErrorCode::none && done.count == 1 && result_status && count == 3 &&
        result[0].integer == static_cast<int>(ValueType::i32) && result[1].integer == value && result[2].integer == 0;
}
bool start() {
    const auto status = component.start({});
    check(status.ok() && !component.callbacks_quiesced() && state("ready"), "native-start");
    if (!status) return false;
    component.enable_control();
    check(component.write_parameter("instruction_budget", Scalar::from_integer(10000)).ok() &&
          component.write_parameter("deadline_ms", Scalar::from_integer(10)).ok(), "restart-budget");
    return true;
}
void stop() {
    const auto began = esp_timer_get_time();
    const auto status = component.stop();
    const auto duration = esp_timer_get_time() - began;
    maximum_stop_us = std::max(maximum_stop_us, duration);
    check(status.ok() && component.callbacks_quiesced() && !runtime.active.load() && state("stopped"), "native-stop-quiesced");
    check(duration < 1000000, "stop-bounded");
    check(read("pool_reserved") == 0 && read("pool_used") == 0 && read("upload_received") == 0, "stop-releases-state");
    check(read("buffer_reserved") == EspWasmComponent::kPoolBytes + EspWasmComponent::kModuleBytes, "stop-retains-fixed-reservation");
    check(!component.call("echo", {}), "stop-denies-work");
    check(component.stop().ok(), "stop-idempotent");
}
void injected_start(qualification::Fault fault, const char* name, ErrorCode expected, std::string_view detail) {
    check(component.release_reservation().ok() && read("buffer_reserved") == 0, "cold-injection-releases-reservation");
    const auto before = heap();
    qualification::arm(fault);
    const auto status = component.start({});
    check(!status && status.error().code == expected && status.error().detail == detail, "injected-start-error");
    const auto observed_hits = qualification::hits();
    check(observed_hits == 1 && qualification::consumed(), "fault-injection-consumed-once");
    qualification::arm(qualification::Fault::none);
    check(component.callbacks_quiesced() && component.stop().ok(), "failed-start-quiesced");
    const auto after = heap();
    check(after == before, "failed-start-releases-heap-and-tasks");
    const bool recovered = start() && load() && echo(0xfedcba98);
    check(recovered, "failed-start-recovery");
    stop();
    check(component.release_reservation().ok(), "recovery-reservation-retired");
    const auto recovered_heap = heap();
    check(recovered_heap == before, "recovery-releases-heap-and-tasks");
    ++fault_count;
    std::printf("WORKER {\"type\":\"injection\",\"name\":\"%s\",\"hits\":%u,\"error\":%u,\"dram_before\":%u,\"dram_after\":%u,\"iram_before\":%u,\"iram_after\":%u,\"tasks_before\":%u,\"tasks_after\":%u,\"recovered\":%s}\n",
        name, observed_hits, static_cast<unsigned>(status.error().code), static_cast<unsigned>(before.dram), static_cast<unsigned>(after.dram),
        static_cast<unsigned>(before.iram), static_cast<unsigned>(after.iram), static_cast<unsigned>(before.tasks),
        static_cast<unsigned>(after.tasks), recovered ? "true" : "false");
}
bool await_active() {
    const auto until = esp_timer_get_time() + 100000;
    while (!runtime.active.load() && esp_timer_get_time() < until) vTaskDelay(1);
    return runtime.active.load();
}
}
extern "C" void app_main() {
    // Runner opens the serial port after esptool's reset.
    vTaskDelay(pdMS_TO_TICKS(1500));
    vTaskPrioritySet(nullptr, 7);
    std::printf("WORKER {\"type\":\"start\",\"chip\":\"%s\",\"idf\":\"%s\",\"engine\":\"%s\",\"cpu_mhz\":%d,\"engine_pool_bytes\":%u,\"linear_bytes\":%u,\"worker_stack_bytes\":8192,\"supervisor_stack_bytes\":4096,\"fixture_sha256\":\"%s\"}\n",
        CONFIG_IDF_TARGET, esp_get_idf_version(), runtime.name().data(), CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        static_cast<unsigned>(EspWasmComponent::kEnginePoolBytes), static_cast<unsigned>(EspWasmComponent::kLinearBytes), fixtures::workloads_sha256);
    check(component.callbacks_quiesced(), "cold-quiesced");
    const auto cold = component.start({});
    check(cold.ok(), "cold-native-start");
    if (!cold) { std::printf("WORKER {\"type\":\"fatal\",\"check\":\"cold-start\"}\n"); return; }
    check(!component.write_parameter("deadline_ms", Scalar::from_integer(50)), "control-gated-before-enable");
    component.enable_control();
    check(load() && echo(0x12345678), "cold-upload-call");
    check(!component.start({}), "double-start-denied");
    stop();
    check(start(), "idle-restart"); stop();
    check(start() && begin_upload() && chunk(0, 64) && read("upload_received") == 64, "partial-upload");
    stop();
    check(start() && load() && echo(23), "partial-upload-restart"); stop();

    injected_start(qualification::Fault::engine_pool, "engine-pool", ErrorCode::resource_unavailable, "runtime-buffers");
    injected_start(qualification::Fault::module_buffer, "module-buffer", ErrorCode::resource_unavailable, "runtime-buffers");
#if defined(CONFIG_IDF_TARGET_ESP32)
    injected_start(qualification::Fault::linear_arena, "linear-arena", ErrorCode::resource_unavailable, "runtime-buffers");
#endif
    injected_start(qualification::Fault::supervisor_task, "supervisor-task", ErrorCode::start_failed, "supervisor-task");
    injected_start(qualification::Fault::pthread_argument, "pthread-argument", ErrorCode::start_failed, "worker-task");
    injected_start(qualification::Fault::pthread_record, "pthread-record", ErrorCode::start_failed, "worker-task");
    injected_start(qualification::Fault::worker_task, "worker-task", ErrorCode::start_failed, "worker-task");
    const auto before_backend = heap();
    runtime.reject_initialize.store(true);
    const auto rejected = component.start({});
    check(!rejected && rejected.error().code == ErrorCode::resource_unavailable && rejected.error().detail == "worker-initialization", "backend-init-rejected");
    check(component.callbacks_quiesced() && heap() == before_backend, "backend-init-cleanup");
    check(start() && load() && echo(42), "backend-init-recovery"); stop();
    check(component.release_reservation().ok(), "backend-recovery-reservation-retired");
    check(heap() == before_backend, "backend-recovery-cleanup");

    check(start() && load(), "active-stop-load");
    check(component.write_parameter("instruction_budget", Scalar::from_integer(1000000)).ok() &&
          component.write_parameter("deadline_ms", Scalar::from_integer(50)).ok(), "active-stop-budget");
    std::array<std::uint32_t, 9> cancelled_ids{};
    const auto spinning = component.call("spin", {});
    check(spinning.ok(), "spin-admitted");
    if (spinning) cancelled_ids[0] = spinning.value();
    check(await_active(), "stop-observed-active-invocation");
    const std::array arg{Value::i32(123)};
    for (std::size_t i = 1; i < cancelled_ids.size(); ++i) {
        const auto queued = component.call("echo", arg);
        check(queued.ok(), "stop-queued-admitted");
        if (queued) cancelled_ids[i] = queued.value();
    }
    const auto excess = component.call("echo", arg);
    const auto pending_at_stop = read("pending");
    check(!excess && excess.error().code == ErrorCode::queue_full && pending_at_stop == 8, "native-queue-reject-new");
    const bool active_at_stop = runtime.active.load();
    check(active_at_stop, "active-through-queue-fill");
    previous_id = std::max(previous_id, cancelled_ids.back());
    stop();
    unsigned observed_cancelled{};
    for (auto id : cancelled_ids) {
        const auto done = completion(id);
        const bool cancelled = id && done.ready && done.error == ErrorCode::cancelled && done.count == 0;
        check(cancelled, "stop-cancels-active-and-queued");
        if (cancelled) ++observed_cancelled;
    }
    std::printf("WORKER {\"type\":\"active-stop\",\"observed_active\":%s,\"queued\":%u,\"cancelled\":%u,\"maximum_stop_us\":%lld}\n",
        active_at_stop ? "true" : "false", static_cast<unsigned>(pending_at_stop), observed_cancelled, maximum_stop_us);
    check(start() && load() && echo(47), "active-stop-restart");
    check(component.write_parameter("instruction_budget", Scalar::from_integer(1000000)).ok() &&
          component.write_parameter("deadline_ms", Scalar::from_integer(10)).ok(), "deadline-budget");
    const auto deadlines_before = read("deadlines");
    const auto deadline = component.call("spin", {});
    check(deadline.ok() && await_active(), "deadline-observed-active");
    const auto deadline_done = completion(deadline ? deadline.value() : 0);
    const auto deadlines_after = read("deadlines");
    check(deadline_done.ready && deadline_done.error == ErrorCode::budget_exceeded && deadline_done.count == 0 &&
          deadline_done.elapsed < 100000 && deadlines_after == deadlines_before + 1, "native-supervisor-deadline");
    std::printf("WORKER {\"type\":\"deadline\",\"elapsed_us\":%u,\"counter_before\":%u,\"counter_after\":%u,\"error\":%u}\n",
        static_cast<unsigned>(deadline_done.elapsed), static_cast<unsigned>(deadlines_before), static_cast<unsigned>(deadlines_after),
        static_cast<unsigned>(deadline_done.error));
    previous_id = deadline ? deadline.value() : previous_id;
    stop();

    const auto baseline = heap();
    auto minimum = baseline.dram, maximum = baseline.dram;
    for (unsigned i = 0; i < 20; ++i) {
        check(start() && load() && echo(i), "repeat-native-start-upload-call");
        stop();
        const auto after = heap();
        minimum = std::min(minimum, after.dram); maximum = std::max(maximum, after.dram);
        check(after == baseline, "native-restart-non-growing-heap-and-tasks");
        ++cycle_count;
        std::printf("WORKER {\"type\":\"cycle\",\"cycle\":%u,\"dram\":%u,\"iram\":%u,\"tasks\":%u}\n", cycle_count,
            static_cast<unsigned>(after.dram), static_cast<unsigned>(after.iram), static_cast<unsigned>(after.tasks));
    }
    check(heap_caps_check_integrity_all(true), "sdk-heap-integrity");
    std::printf("WORKER {\"type\":\"complete\",\"checks\":%u,\"failures\":%u,\"start_stop_cycles\":%u,\"allocation_faults\":%u,\"backend_faults\":1,\"heap_baseline\":%u,\"heap_min\":%u,\"heap_max\":%u,\"iram_after\":%u,\"tasks_after\":%u,\"minimum_worker_headroom\":%u,\"minimum_supervisor_headroom\":%u,\"maximum_stop_us\":%lld}\n",
        checks, failures, cycle_count, fault_count, static_cast<unsigned>(baseline.dram), static_cast<unsigned>(minimum),
        static_cast<unsigned>(maximum), static_cast<unsigned>(baseline.iram), static_cast<unsigned>(baseline.tasks),
        static_cast<unsigned>(minimum_worker), static_cast<unsigned>(minimum_supervisor), maximum_stop_us);
}
