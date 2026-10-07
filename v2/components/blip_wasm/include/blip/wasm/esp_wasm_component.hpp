#pragma once
#include "blip/core/component.hpp"
#include "blip/wasm/service.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <array>
#include <atomic>
#include <pthread.h>

namespace blip::wasm {
class EspWasmComponent final : public core::Component {
  public:
    static constexpr std::size_t kPoolBytes = 81920, kModuleBytes = 16384;
    static constexpr std::size_t kWorkerStackBytes = 8192, kSupervisorStackBytes = 4096;
    static constexpr std::size_t kQueueCapacity = 8, kCompletionCapacity = 16, kChunkBytes = 64;
    explicit EspWasmComponent(Runtime& runtime) noexcept;
    [[nodiscard]] const core::ComponentDescriptor& descriptor() const noexcept override;
    [[nodiscard]] core::Status start(const core::StartContext&) noexcept override;
    [[nodiscard]] core::Status stop() noexcept override;
    [[nodiscard]] bool callbacks_quiesced() const noexcept override;
    [[nodiscard]] core::Status read_parameter(std::string_view, core::ScalarValue&) noexcept override;
    [[nodiscard]] core::Status write_parameter(std::string_view, const core::ScalarValue&) noexcept override;
    [[nodiscard]] core::Status invoke_action(std::string_view, std::span<const core::ScalarValue>,
        std::span<core::ScalarValue>, std::size_t&) noexcept override;
    void enable_control() noexcept { control_ready_.store(true); }
    // Full typed entry point for future component-owned providers/SDK bindings.
    [[nodiscard]] core::Result<std::uint32_t> call(std::string_view, std::span<const Value>) noexcept;
  private:
    enum class Kind : std::uint8_t { begin, chunk, commit, call, unload };
    enum class Cancellation : std::uint8_t { none, requested, deadline };
    struct Request {
        std::uint32_t id{}, epoch{}, generation{}, number{}, crc{}, instructions{}, deadline_ms{};
        Kind kind{};
        std::uint8_t argument_count{}, size{};
        std::array<Value, kMaximumArguments> arguments{};
        std::array<char, kMaximumExportNameBytes + 1> name{};
        std::array<std::byte, kChunkBytes> bytes{};
    };
    struct Completion {
        std::uint32_t id{}, elapsed_us{};
        core::ErrorCode error{core::ErrorCode::none};
        std::uint8_t count{};
        std::array<Value, kMaximumResults> results{};
    };
    [[nodiscard]] core::Result<std::uint32_t> submit(Request&) noexcept;
    void complete(const Request&, core::Status, std::span<const Value>, std::uint32_t, const Snapshot&, std::uint32_t) noexcept;
    static void* worker_entry(void*) noexcept;
    static void supervisor_entry(void*) noexcept;
    void run_worker() noexcept;
    void run_supervisor() noexcept;
    Runtime* runtime_;
    std::byte* pool_{};
    std::byte* module_{};
    StaticSemaphore_t admission_storage_{}, snapshot_storage_{}, monitor_storage_{}, ready_storage_{};
    SemaphoreHandle_t admission_{}, snapshot_mutex_{}, monitor_mutex_{}, ready_{};
    StaticQueue_t queue_storage_{};
    alignas(8) std::array<std::uint8_t, kQueueCapacity * sizeof(Request)> queue_bytes_{};
    QueueHandle_t queue_{};
    TaskHandle_t supervisor_{};
    pthread_t worker_{};
    bool worker_created_{};
    std::atomic<bool> started_{}, control_ready_{}, worker_quiesced_{true}, supervisor_quiesced_{true};
    std::atomic<std::uint32_t> epoch_{1}, instruction_budget_{10000}, deadline_ms_{10};
    std::atomic<core::ErrorCode> start_error_{core::ErrorCode::none};
    std::atomic<std::uint32_t> rejected_{}, completed_{}, failed_{}, deadlines_{}, cancelled_{}, worker_headroom_{}, supervisor_headroom_{};
    std::uint32_t next_id_{}; // Admission mutex; never reset or reuse IDs.
    Snapshot snapshot_{}; // Snapshot mutex, together with completion ring.
    std::array<Completion, kCompletionCapacity> completions_{};
    std::uint32_t upload_received_{}, last_id_{}, last_elapsed_us_{};
    core::ErrorCode last_error_{core::ErrorCode::none};
    // Monitor mutex protects publication/retirement and cancellation issuance.
    bool active_{};
    std::uint32_t active_epoch_{};
    std::uint64_t active_deadline_{};
    Cancellation cancellation_{Cancellation::none};
    std::atomic<bool> call_cancelled_{};
    static const core::ComponentDescriptor descriptor_;
};
} // namespace blip::wasm
