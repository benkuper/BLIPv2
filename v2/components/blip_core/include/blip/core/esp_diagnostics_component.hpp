#pragma once

#include "blip/core/boot_health.hpp"
#include "blip/core/component.hpp"
#include "blip/core/diagnostics.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace blip::core {

enum class CoredumpStatus : std::uint8_t { absent, valid, corrupt, io_error };

struct CoredumpInfo {
    CoredumpStatus status{CoredumpStatus::absent};
    std::uint32_t flash_address{};
    std::uint32_t size_bytes{};
    std::uint32_t identity_crc32{};
};

struct RuntimeDiagnosticsSnapshot {
    BootState boot{};
    CoredumpInfo coredump{};
    std::uint32_t free_internal_heap{};
    std::uint32_t minimum_free_internal_heap{};
    std::uint32_t largest_free_internal_block{};
    std::uint32_t main_stack_high_water_bytes{};
};

[[nodiscard]] const char* coredump_status_name(CoredumpStatus status) noexcept;

class EspDiagnosticsComponent final : public Component {
  public:
    EspDiagnosticsComponent() noexcept;

    [[nodiscard]] const ComponentDescriptor& descriptor() const noexcept override;
    [[nodiscard]] Status start(const StartContext&) noexcept override;
    [[nodiscard]] Status stop() noexcept override;
    [[nodiscard]] Status read_parameter(std::string_view id, ScalarValue& output) noexcept override;
    [[nodiscard]] Status write_parameter(std::string_view id,
                                         const ScalarValue& value) noexcept override;
    [[nodiscard]] Status invoke_action(std::string_view id, std::span<const ScalarValue> arguments,
                                       std::span<ScalarValue> output,
                                       std::size_t& output_count) noexcept override;

    [[nodiscard]] Status prepare_boot() noexcept;
    [[nodiscard]] Status confirm_boot() noexcept;
    [[nodiscard]] Status clear_safe_mode() noexcept;
    [[nodiscard]] Status erase_coredump() noexcept;
    [[nodiscard]] Status refresh_metrics() noexcept;
    [[nodiscard]] Status flush_logs() noexcept;
    [[nodiscard]] Status set_log_level(std::string_view component, LogLevel level) noexcept {
        return logger_.set_component_level(component, level);
    }
    [[nodiscard]] Status set_default_log_level(LogLevel level) noexcept {
        return logger_.set_default_level(level);
    }

    [[nodiscard]] bool safe_mode() const noexcept { return snapshot_.boot.safe_mode; }
    [[nodiscard]] bool prepared() const noexcept { return prepared_; }
    [[nodiscard]] const RuntimeDiagnosticsSnapshot& snapshot() const noexcept { return snapshot_; }
    [[nodiscard]] StructuredLogger& logger() noexcept { return logger_; }
    [[nodiscard]] MetricRegistry& metrics() noexcept { return metrics_; }

  private:
    static constexpr std::size_t kLogRecordCapacity = 8;
    static constexpr std::size_t kLogOverrideCapacity = 8;
    static constexpr std::size_t kMetricCapacity = 16;

    struct MetricHandles {
        MetricHandle reset_cause{};
        MetricHandle boot_sequence{};
        MetricHandle boot_failures{};
        MetricHandle safe_mode{};
        MetricHandle heap_free{};
        MetricHandle heap_minimum{};
        MetricHandle heap_largest{};
        MetricHandle stack_high_water{};
        MetricHandle coredump_status{};
        MetricHandle coredump_size{};
        MetricHandle coredump_identity{};
        MetricHandle log_filtered{};
        MetricHandle log_dropped{};
        MetricHandle log_redacted{};
    };

    [[nodiscard]] bool register_metrics() noexcept;
    [[nodiscard]] Status probe_coredump() noexcept;
    [[nodiscard]] Status emit_boot_log() noexcept;

    static const ComponentDescriptor descriptor_;

    std::array<StructuredLogRecord, kLogRecordCapacity> log_records_{};
    std::array<LogLevelOverride, kLogOverrideCapacity> log_overrides_{};
    StructuredLogger logger_{log_records_, log_overrides_};
    std::array<MetricRecord, kMetricCapacity> metric_records_{};
    MetricRegistry metrics_{metric_records_};
    MetricHandles metric_handles_{};
    BootLoopGuard boot_guard_{};
    RuntimeDiagnosticsSnapshot snapshot_{};
    bool metrics_ready_{};
    bool prepared_{};
    bool started_{};
};

static_assert(sizeof(EspDiagnosticsComponent) <= 8192,
              "diagnostics component exceeds its declared static RAM budget");

} // namespace blip::core
