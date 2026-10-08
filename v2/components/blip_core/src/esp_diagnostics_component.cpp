#include "blip/core/esp_diagnostics_component.hpp"

#include "esp_attr.h"
#include "esp_core_dump.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <algorithm>
#include <array>
#include <limits>
#include <string_view>

namespace blip::core {
namespace {

constexpr char kTag[] = "blip_diagnostics";
constexpr std::array<std::string_view, 1> kProvidedServices{"diagnostics.runtime"};
constexpr std::array<MetadataEntry, 2> kMetadata{{
    {"ui_topic", "System"}, {"ui_primary", "heap_free_internal"}}};
constexpr std::array<ParameterDescriptor, 6> kParameters{{
    {"log_level",
     "Default log level",
     ValueType::integer,
     Access::read_write,
     false,
     ScalarValue::from_integer(static_cast<std::int64_t>(LogLevel::info)),
     {true, 0, 5, 1},
     ""},
    {"heap_free_internal", "Free internal heap", ValueType::integer, Access::read_only,
     false, ScalarValue::from_integer(0), {}, "bytes"},
    {"heap_minimum_internal", "Minimum free internal heap", ValueType::integer,
     Access::read_only, false, ScalarValue::from_integer(0), {}, "bytes"},
    {"heap_largest_internal", "Largest free internal block", ValueType::integer,
     Access::read_only, false, ScalarValue::from_integer(0), {}, "bytes"},
    {"boot_sequence", "Boot sequence", ValueType::integer, Access::read_only,
     false, ScalarValue::from_integer(0), {}, "boots"},
    {"reset_cause", "Reset cause", ValueType::integer, Access::read_only,
     false, ScalarValue::from_integer(0), {}, "enum"},
}};
constexpr std::array<ActionDescriptor, 2> kActions{{
    {"clear_safe_mode", "Clear safe mode on next reboot", {}},
    {"erase_coredump", "Erase retained coredump", {}},
}};
constexpr std::array<DiagnosticDescriptor, 14> kDiagnostics{{
    {"reset_cause", ValueType::integer, "enum"},
    {"boot_sequence", ValueType::integer, "boots"},
    {"boot_failures", ValueType::integer, "boots"},
    {"safe_mode", ValueType::boolean, ""},
    {"heap_free_internal", ValueType::integer, "bytes"},
    {"heap_minimum_internal", ValueType::integer, "bytes"},
    {"heap_largest_internal", ValueType::integer, "bytes"},
    {"main_stack_high_water", ValueType::integer, "bytes"},
    {"coredump_status", ValueType::integer, "enum"},
    {"coredump_size", ValueType::integer, "bytes"},
    {"coredump_identity", ValueType::integer, "crc32"},
    {"log_filtered", ValueType::integer, "records"},
    {"log_dropped", ValueType::integer, "records"},
    {"log_redacted", ValueType::integer, "fields"},
}};

RTC_NOINIT_ATTR alignas(4) std::array<std::byte, kBootLedgerBytes> rtc_boot_ledger;

[[nodiscard]] constexpr ComponentDescriptor diagnostics_descriptor() noexcept {
    ComponentDescriptor descriptor{};
    descriptor.schema_version = 1;
    descriptor.id = "blip.diagnostics";
    descriptor.display_name = "Runtime diagnostics";
    descriptor.description = "Bounded logs, metrics, reset/coredump reporting, and safe mode";
    descriptor.provided_services = kProvidedServices;
    descriptor.metadata = kMetadata;
    descriptor.parameters = kParameters;
    descriptor.actions = kActions;
    descriptor.diagnostics = kDiagnostics;
    descriptor.settings = {1, 1};
    descriptor.disable_policy = DisablePolicy::reboot_required;
    descriptor.cost = {32768, 8192, 0};
    return descriptor;
}

[[nodiscard]] Error diagnostics_error(ErrorCode code, std::string_view operation,
                                      std::string_view detail) noexcept {
    return {ErrorDomain::diagnostics, code, "blip.diagnostics", operation, detail};
}

[[nodiscard]] ResetCause map_reset_cause(esp_reset_reason_t reason) noexcept {
    switch (reason) {
    case ESP_RST_POWERON:
        return ResetCause::power_on;
    case ESP_RST_EXT:
        return ResetCause::external;
    case ESP_RST_SW:
        return ResetCause::software;
    case ESP_RST_PANIC:
        return ResetCause::panic;
    case ESP_RST_INT_WDT:
        return ResetCause::interrupt_watchdog;
    case ESP_RST_TASK_WDT:
        return ResetCause::task_watchdog;
    case ESP_RST_WDT:
        return ResetCause::other_watchdog;
    case ESP_RST_DEEPSLEEP:
        return ResetCause::deep_sleep;
    case ESP_RST_BROWNOUT:
        return ResetCause::brownout;
    case ESP_RST_SDIO:
        return ResetCause::sdio;
    case ESP_RST_USB:
        return ResetCause::usb;
    case ESP_RST_JTAG:
        return ResetCause::jtag;
    case ESP_RST_EFUSE:
        return ResetCause::efuse;
    case ESP_RST_PWR_GLITCH:
        return ResetCause::power_glitch;
    case ESP_RST_CPU_LOCKUP:
        return ResetCause::cpu_lockup;
    case ESP_RST_UNKNOWN:
    default:
        return ResetCause::unknown;
    }
}

[[nodiscard]] std::uint32_t crc32_update(std::uint32_t crc,
                                         std::span<const std::byte> input) noexcept {
    for (const std::byte value : input) {
        crc ^= std::to_integer<std::uint8_t>(value);
        for (std::uint8_t bit = 0; bit < 8U; ++bit) {
            const std::uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return crc;
}

[[nodiscard]] std::int64_t bounded_size(std::size_t value) noexcept {
    return value > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())
               ? std::numeric_limits<std::int64_t>::max()
               : static_cast<std::int64_t>(value);
}

} // namespace

const ComponentDescriptor EspDiagnosticsComponent::descriptor_{diagnostics_descriptor()};

const char* coredump_status_name(CoredumpStatus status) noexcept {
    switch (status) {
    case CoredumpStatus::absent:
        return "absent";
    case CoredumpStatus::valid:
        return "valid";
    case CoredumpStatus::corrupt:
        return "corrupt";
    case CoredumpStatus::io_error:
        return "io-error";
    }
    return "io-error";
}

EspDiagnosticsComponent::EspDiagnosticsComponent() noexcept { metrics_ready_ = register_metrics(); }

const ComponentDescriptor& EspDiagnosticsComponent::descriptor() const noexcept {
    return descriptor_;
}

bool EspDiagnosticsComponent::register_metrics() noexcept {
    const auto add = [this](MetricHandle& handle, std::string_view id, MetricType type,
                            std::string_view unit) noexcept {
        const auto registered = metrics_.register_metric({id, type, unit});
        if (!registered) {
            return false;
        }
        handle = registered.value();
        return true;
    };
    return add(metric_handles_.reset_cause, "reset_cause", MetricType::gauge, "enum") &&
           add(metric_handles_.boot_sequence, "boot_sequence", MetricType::gauge, "boots") &&
           add(metric_handles_.boot_failures, "boot_failures", MetricType::gauge, "boots") &&
           add(metric_handles_.safe_mode, "safe_mode", MetricType::gauge, "bool") &&
           add(metric_handles_.heap_free, "heap_free_internal", MetricType::gauge, "bytes") &&
           add(metric_handles_.heap_minimum, "heap_minimum_internal", MetricType::gauge, "bytes") &&
           add(metric_handles_.heap_largest, "heap_largest_internal", MetricType::gauge, "bytes") &&
           add(metric_handles_.stack_high_water, "main_stack_high_water", MetricType::gauge,
               "bytes") &&
           add(metric_handles_.coredump_status, "coredump_status", MetricType::gauge, "enum") &&
           add(metric_handles_.coredump_size, "coredump_size", MetricType::gauge, "bytes") &&
           add(metric_handles_.coredump_identity, "coredump_identity", MetricType::gauge,
               "crc32") &&
           add(metric_handles_.log_filtered, "log_filtered", MetricType::gauge, "records") &&
           add(metric_handles_.log_dropped, "log_dropped", MetricType::gauge, "records") &&
           add(metric_handles_.log_redacted, "log_redacted", MetricType::gauge, "fields");
}

Status EspDiagnosticsComponent::prepare_boot() noexcept {
    if (prepared_) {
        return Status::success();
    }
    if (!metrics_ready_) {
        return Status::failure(
            diagnostics_error(ErrorCode::invalid_state, "prepare-boot", "metrics-not-ready"));
    }
    const auto decision =
        boot_guard_.begin_boot(rtc_boot_ledger, map_reset_cause(esp_reset_reason()));
    if (!decision) {
        return Status::failure(decision.error());
    }
    snapshot_.boot = decision.value().state;
    prepared_ = true;
    const auto coredump = probe_coredump();
    if (!coredump) {
        return coredump;
    }
    return refresh_metrics();
}

Status EspDiagnosticsComponent::probe_coredump() noexcept {
    snapshot_.coredump = {};
    std::size_t address{};
    std::size_t size{};
    const esp_err_t located = esp_core_dump_image_get(&address, &size);
    if (located == ESP_ERR_NOT_FOUND) {
        return Status::success();
    }
    if (located != ESP_OK || address > std::numeric_limits<std::uint32_t>::max() ||
        size > std::numeric_limits<std::uint32_t>::max()) {
        snapshot_.coredump.status = CoredumpStatus::io_error;
        return Status::success();
    }
    snapshot_.coredump.flash_address = static_cast<std::uint32_t>(address);
    snapshot_.coredump.size_bytes = static_cast<std::uint32_t>(size);
    if (esp_core_dump_image_check() != ESP_OK) {
        snapshot_.coredump.status = CoredumpStatus::corrupt;
        return Status::success();
    }
    const esp_partition_t* partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, nullptr);
    if (partition == nullptr || address < partition->address ||
        size > partition->size - (address - partition->address)) {
        snapshot_.coredump.status = CoredumpStatus::io_error;
        return Status::success();
    }
    std::array<std::byte, 256> buffer{};
    std::uint32_t crc = 0xffffffffU;
    std::size_t consumed{};
    while (consumed < size) {
        const std::size_t chunk = std::min(buffer.size(), size - consumed);
        if (esp_partition_read(partition, address - partition->address + consumed, buffer.data(),
                               chunk) != ESP_OK) {
            snapshot_.coredump.status = CoredumpStatus::io_error;
            return Status::success();
        }
        crc = crc32_update(crc, std::span<const std::byte>{buffer.data(), chunk});
        consumed += chunk;
    }
    snapshot_.coredump.identity_crc32 = ~crc;
    snapshot_.coredump.status = CoredumpStatus::valid;
    return Status::success();
}

Status EspDiagnosticsComponent::refresh_metrics() noexcept {
    if (!prepared_) {
        return Status::failure(
            diagnostics_error(ErrorCode::invalid_state, "refresh-metrics", "not-prepared"));
    }
    snapshot_.free_internal_heap =
        static_cast<std::uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    snapshot_.minimum_free_internal_heap = static_cast<std::uint32_t>(
        heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    snapshot_.largest_free_internal_block = static_cast<std::uint32_t>(
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    snapshot_.main_stack_high_water_bytes =
        static_cast<std::uint32_t>(uxTaskGetStackHighWaterMark(nullptr));

    const auto set = [this](MetricHandle handle, std::int64_t value) noexcept {
        return metrics_.set_gauge(handle, value).ok();
    };
    const auto& logger_metrics = logger_.metrics();
    if (!set(metric_handles_.reset_cause, static_cast<std::int64_t>(snapshot_.boot.reset_cause)) ||
        !set(metric_handles_.boot_sequence, snapshot_.boot.boot_sequence) ||
        !set(metric_handles_.boot_failures, snapshot_.boot.consecutive_failures) ||
        !set(metric_handles_.safe_mode, snapshot_.boot.safe_mode ? 1 : 0) ||
        !set(metric_handles_.heap_free, snapshot_.free_internal_heap) ||
        !set(metric_handles_.heap_minimum, snapshot_.minimum_free_internal_heap) ||
        !set(metric_handles_.heap_largest, snapshot_.largest_free_internal_block) ||
        !set(metric_handles_.stack_high_water, snapshot_.main_stack_high_water_bytes) ||
        !set(metric_handles_.coredump_status,
             static_cast<std::int64_t>(snapshot_.coredump.status)) ||
        !set(metric_handles_.coredump_size, snapshot_.coredump.size_bytes) ||
        !set(metric_handles_.coredump_identity, snapshot_.coredump.identity_crc32) ||
        !set(metric_handles_.log_filtered, bounded_size(logger_metrics.filtered)) ||
        !set(metric_handles_.log_dropped, bounded_size(logger_metrics.dropped)) ||
        !set(metric_handles_.log_redacted, bounded_size(logger_metrics.redacted_fields))) {
        return Status::failure(
            diagnostics_error(ErrorCode::invalid_state, "refresh-metrics", "metric-update-failed"));
    }
    return Status::success();
}

Status EspDiagnosticsComponent::emit_boot_log() noexcept {
    const std::array<LogFieldInput, 4> fields{{
        {"reset", LogValue::from_string(reset_cause_name(snapshot_.boot.reset_cause)), false},
        {"safe_mode", LogValue::from_bool(snapshot_.boot.safe_mode), false},
        {"safe_reason",
         LogValue::from_string(safe_mode_reason_name(snapshot_.boot.safe_mode_reason)), false},
        {"coredump", LogValue::from_string(coredump_status_name(snapshot_.coredump.status)), false},
    }};
    return logger_.emit({static_cast<std::uint64_t>(esp_timer_get_time()),
                         snapshot_.boot.safe_mode ? LogLevel::warning : LogLevel::info,
                         descriptor_.id, "boot", fields});
}

Status EspDiagnosticsComponent::flush_logs() noexcept {
    StructuredLogRecord record{};
    std::array<char, 512> encoded{};
    while (logger_.pop(record)) {
        const auto formatted = format_structured_log_json(record, encoded);
        if (!formatted) {
            return Status::failure(formatted.error());
        }
        ESP_LOGI(kTag, "%s", encoded.data());
    }
    return Status::success();
}

Status EspDiagnosticsComponent::start(const StartContext&) noexcept {
    if (started_) {
        return Status::success();
    }
    auto status = prepare_boot();
    if (!status) {
        return status;
    }
    status = emit_boot_log();
    if (!status) {
        return status;
    }
    status = flush_logs();
    if (!status) {
        return status;
    }
    started_ = true;
    return refresh_metrics();
}

Status EspDiagnosticsComponent::read_parameter(std::string_view id, ScalarValue& output) noexcept {
    if (id == "log_level") {
        output = ScalarValue::from_integer(static_cast<std::int64_t>(logger_.default_level()));
    } else if (id == "heap_free_internal") {
        output = ScalarValue::from_integer(
            heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    } else if (id == "heap_minimum_internal") {
        output = ScalarValue::from_integer(
            heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    } else if (id == "heap_largest_internal") {
        output = ScalarValue::from_integer(
            heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    } else if (id == "boot_sequence") {
        output = ScalarValue::from_integer(snapshot_.boot.boot_sequence);
    } else if (id == "reset_cause") {
        output = ScalarValue::from_integer(static_cast<std::int64_t>(snapshot_.boot.reset_cause));
    } else {
        return Status::failure(
            diagnostics_error(ErrorCode::not_found, "read-parameter", "parameter-not-found"));
    }
    return Status::success();
}

Status EspDiagnosticsComponent::write_parameter(std::string_view id,
                                                const ScalarValue& value) noexcept {
    if (id != "log_level") {
        return Status::failure(
            diagnostics_error(ErrorCode::not_found, "write-parameter", "parameter-not-found"));
    }
    if (value.type != ValueType::integer || value.integer < 0 || value.integer > 5) {
        return Status::failure(
            diagnostics_error(ErrorCode::validation_failed, "write-parameter", "invalid-level"));
    }
    return logger_.set_default_level(static_cast<LogLevel>(value.integer));
}

Status EspDiagnosticsComponent::invoke_action(std::string_view id,
                                              std::span<const ScalarValue> arguments,
                                              std::span<ScalarValue>,
                                              std::size_t& output_count) noexcept {
    output_count = 0;
    if (!arguments.empty()) {
        return Status::failure(diagnostics_error(ErrorCode::validation_failed, "invoke-action",
                                                 "arguments-not-allowed"));
    }
    if (id == "clear_safe_mode") {
        return clear_safe_mode();
    }
    if (id == "erase_coredump") {
        return erase_coredump();
    }
    return Status::failure(
        diagnostics_error(ErrorCode::not_found, "invoke-action", "action-not-found"));
}

Status EspDiagnosticsComponent::confirm_boot() noexcept {
    if (!started_) {
        return Status::failure(
            diagnostics_error(ErrorCode::invalid_state, "confirm-boot", "not-started"));
    }
    const auto confirmed = boot_guard_.confirm_boot(rtc_boot_ledger);
    if (!confirmed) {
        return confirmed;
    }
    const auto inspected = boot_guard_.inspect(rtc_boot_ledger);
    if (!inspected) {
        return Status::failure(inspected.error());
    }
    snapshot_.boot = inspected.value();
    return refresh_metrics();
}

Status EspDiagnosticsComponent::clear_safe_mode() noexcept {
    if (!prepared_) {
        return Status::failure(
            diagnostics_error(ErrorCode::invalid_state, "clear-safe-mode", "not-prepared"));
    }
    const auto cleared = boot_guard_.clear_safe_mode(rtc_boot_ledger);
    if (!cleared) {
        return cleared;
    }
    const auto inspected = boot_guard_.inspect(rtc_boot_ledger);
    if (!inspected) {
        return Status::failure(inspected.error());
    }
    snapshot_.boot = inspected.value();
    return refresh_metrics();
}

Status EspDiagnosticsComponent::erase_coredump() noexcept {
    const esp_err_t erased = esp_core_dump_image_erase();
    if (erased != ESP_OK && erased != ESP_ERR_NOT_FOUND) {
        return Status::failure(
            diagnostics_error(ErrorCode::io_failed, "erase-coredump", "flash-erase-failed"));
    }
    const auto probed = probe_coredump();
    if (!probed) {
        return probed;
    }
    return prepared_ ? refresh_metrics() : Status::success();
}

Status EspDiagnosticsComponent::stop() noexcept {
    started_ = false;
    return Status::success();
}

} // namespace blip::core
