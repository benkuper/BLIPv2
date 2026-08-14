#pragma once

#include "blip/core/error.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace blip::core {

inline constexpr std::size_t kMaxLogComponentBytes = 32;
inline constexpr std::size_t kMaxLogEventBytes = 32;
inline constexpr std::size_t kMaxLogFieldKeyBytes = 24;
inline constexpr std::size_t kMaxLogStringBytes = 64;
inline constexpr std::size_t kMaxLogFields = 4;
inline constexpr std::size_t kMaxMetricIdBytes = 40;
inline constexpr std::size_t kMaxMetricUnitBytes = 12;

enum class LogLevel : std::uint8_t { off, error, warning, info, debug, verbose };
enum class LogValueType : std::uint8_t {
    boolean,
    signed_integer,
    unsigned_integer,
    number,
    string
};

struct LogValue {
    LogValueType type{LogValueType::boolean};
    bool boolean{};
    std::int64_t signed_integer{};
    std::uint64_t unsigned_integer{};
    double number{};
    std::string_view string{};

    [[nodiscard]] static constexpr LogValue from_bool(bool value) noexcept {
        LogValue result{};
        result.type = LogValueType::boolean;
        result.boolean = value;
        return result;
    }
    [[nodiscard]] static constexpr LogValue from_signed(std::int64_t value) noexcept {
        LogValue result{};
        result.type = LogValueType::signed_integer;
        result.signed_integer = value;
        return result;
    }
    [[nodiscard]] static constexpr LogValue from_unsigned(std::uint64_t value) noexcept {
        LogValue result{};
        result.type = LogValueType::unsigned_integer;
        result.unsigned_integer = value;
        return result;
    }
    [[nodiscard]] static constexpr LogValue from_number(double value) noexcept {
        LogValue result{};
        result.type = LogValueType::number;
        result.number = value;
        return result;
    }
    [[nodiscard]] static constexpr LogValue from_string(std::string_view value) noexcept {
        LogValue result{};
        result.type = LogValueType::string;
        result.string = value;
        return result;
    }
};

struct LogFieldInput {
    std::string_view key{};
    LogValue value{};
    bool secret{};
};

struct LogInput {
    std::uint64_t timestamp_us{};
    LogLevel level{LogLevel::info};
    std::string_view component{};
    std::string_view event{};
    std::span<const LogFieldInput> fields{};
};

struct StoredLogField {
    std::array<char, kMaxLogFieldKeyBytes + 1> key{};
    std::array<char, kMaxLogStringBytes + 1> string{};
    std::uint8_t key_size{};
    std::uint8_t string_size{};
    LogValueType type{LogValueType::boolean};
    bool boolean{};
    bool redacted{};
    std::int64_t signed_integer{};
    std::uint64_t unsigned_integer{};
    double number{};
};

struct StructuredLogRecord {
    std::uint64_t sequence{};
    std::uint64_t timestamp_us{};
    LogLevel level{LogLevel::info};
    std::array<char, kMaxLogComponentBytes + 1> component{};
    std::array<char, kMaxLogEventBytes + 1> event{};
    std::uint8_t component_size{};
    std::uint8_t event_size{};
    std::uint8_t field_count{};
    std::array<StoredLogField, kMaxLogFields> fields{};
};

struct LogLevelOverride {
    std::array<char, kMaxLogComponentBytes + 1> component{};
    std::uint8_t component_size{};
    LogLevel level{LogLevel::info};
    bool used{};
};

struct LoggerMetrics {
    std::uint32_t accepted{};
    std::uint32_t filtered{};
    std::uint32_t dropped{};
    std::uint32_t redacted_fields{};
    std::size_t high_water_mark{};
};

class StructuredLogger {
  public:
    StructuredLogger(std::span<StructuredLogRecord> records,
                     std::span<LogLevelOverride> overrides) noexcept
        : records_(records), overrides_(overrides) {}

    [[nodiscard]] core::Status set_default_level(LogLevel level) noexcept;
    [[nodiscard]] LogLevel default_level() const noexcept { return default_level_; }
    [[nodiscard]] core::Status set_component_level(std::string_view component,
                                                   LogLevel level) noexcept;
    [[nodiscard]] core::Status clear_component_level(std::string_view component) noexcept;
    [[nodiscard]] LogLevel effective_level(std::string_view component) const noexcept;
    [[nodiscard]] core::Status emit(const LogInput& input) noexcept;
    [[nodiscard]] bool pop(StructuredLogRecord& output) noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] const LoggerMetrics& metrics() const noexcept { return metrics_; }

  private:
    std::span<StructuredLogRecord> records_{};
    std::span<LogLevelOverride> overrides_{};
    LogLevel default_level_{LogLevel::info};
    std::size_t head_{};
    std::size_t size_{};
    std::uint64_t next_sequence_{1};
    LoggerMetrics metrics_{};
};

[[nodiscard]] std::string_view log_level_name(LogLevel level) noexcept;
[[nodiscard]] core::Result<std::size_t>
format_structured_log_json(const StructuredLogRecord& record, std::span<char> output) noexcept;

enum class MetricType : std::uint8_t { counter, gauge };

struct MetricDescriptorInput {
    std::string_view id{};
    MetricType type{MetricType::counter};
    std::string_view unit{};
};

struct MetricRecord {
    std::array<char, kMaxMetricIdBytes + 1> id{};
    std::array<char, kMaxMetricUnitBytes + 1> unit{};
    std::uint8_t id_size{};
    std::uint8_t unit_size{};
    MetricType type{MetricType::counter};
    std::uint64_t counter{};
    std::int64_t gauge{};
    bool used{};
};

struct MetricHandle {
    std::uint16_t index{0xffffU};
};

class MetricRegistry {
  public:
    explicit MetricRegistry(std::span<MetricRecord> records) noexcept : records_(records) {}

    [[nodiscard]] core::Result<MetricHandle>
    register_metric(const MetricDescriptorInput& descriptor) noexcept;
    [[nodiscard]] core::Status increment(MetricHandle handle, std::uint64_t amount = 1) noexcept;
    [[nodiscard]] core::Status set_gauge(MetricHandle handle, std::int64_t value) noexcept;
    [[nodiscard]] core::Result<std::uint64_t> counter(MetricHandle handle) const noexcept;
    [[nodiscard]] core::Result<std::int64_t> gauge(MetricHandle handle) const noexcept;
    [[nodiscard]] std::span<const MetricRecord> records() const noexcept;

  private:
    [[nodiscard]] MetricRecord* resolve(MetricHandle handle, MetricType expected) noexcept;
    [[nodiscard]] const MetricRecord* resolve(MetricHandle handle,
                                              MetricType expected) const noexcept;

    std::span<MetricRecord> records_{};
    std::size_t size_{};
};

} // namespace blip::core
