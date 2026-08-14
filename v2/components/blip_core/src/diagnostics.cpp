#include "blip/core/diagnostics.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>

namespace blip::core {
namespace {

constexpr std::string_view kRedacted = "[redacted]";

[[nodiscard]] core::Error diagnostics_error(core::ErrorCode code, std::string_view operation,
                                            std::string_view detail) noexcept {
    return {core::ErrorDomain::diagnostics, code, "blip.diagnostics", operation, detail};
}

void saturating_increment(std::uint32_t& value) noexcept {
    if (value != std::numeric_limits<std::uint32_t>::max()) {
        ++value;
    }
}

[[nodiscard]] bool valid_name(std::string_view value, std::size_t maximum) noexcept {
    if (value.empty() || value.size() > maximum) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](char character) {
        return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == '_' || character == '-' ||
               character == '.';
    });
}

[[nodiscard]] constexpr bool valid_log_level(LogLevel level) noexcept {
    return static_cast<std::uint8_t>(level) <= static_cast<std::uint8_t>(LogLevel::verbose);
}

[[nodiscard]] constexpr bool valid_log_value_type(LogValueType type) noexcept {
    return static_cast<std::uint8_t>(type) <= static_cast<std::uint8_t>(LogValueType::string);
}

template <std::size_t Capacity>
void copy_text(std::array<char, Capacity>& output, std::uint8_t& output_size,
               std::string_view input) noexcept {
    if (!input.empty()) {
        std::memcpy(output.data(), input.data(), input.size());
    }
    output[input.size()] = '\0';
    output_size = static_cast<std::uint8_t>(input.size());
}

class JsonWriter {
  public:
    explicit JsonWriter(std::span<char> output) noexcept : output_(output) {}

    [[nodiscard]] bool append(std::string_view text) noexcept {
        if (text.size() > remaining()) {
            failed_ = true;
            return false;
        }
        if (!text.empty()) {
            std::memcpy(output_.data() + size_, text.data(), text.size());
        }
        size_ += text.size();
        return true;
    }

    [[nodiscard]] bool quoted(std::string_view text) noexcept {
        if (!append("\"")) {
            return false;
        }
        constexpr char digits[] = "0123456789abcdef";
        for (const char character : text) {
            const auto byte = static_cast<std::uint8_t>(character);
            if (character == '"' || character == '\\') {
                const char escaped[2]{'\\', character};
                if (!append({escaped, 2})) {
                    return false;
                }
            } else if (byte < 0x20U) {
                const char escaped[6]{
                    '\\', 'u', '0', '0', digits[byte >> 4U], digits[byte & 0x0fU]};
                if (!append({escaped, 6})) {
                    return false;
                }
            } else if (!append({&character, 1})) {
                return false;
            }
        }
        return append("\"");
    }

    template <typename T> [[nodiscard]] bool integer(T value) noexcept {
        std::array<char, 32> buffer{};
        const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
        if (converted.ec != std::errc{}) {
            failed_ = true;
            return false;
        }
        return append({buffer.data(), static_cast<std::size_t>(converted.ptr - buffer.data())});
    }

    [[nodiscard]] bool number(double value) noexcept {
        std::array<char, 32> buffer{};
        const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                                             std::chars_format::general, 10);
        if (converted.ec != std::errc{}) {
            failed_ = true;
            return false;
        }
        return append({buffer.data(), static_cast<std::size_t>(converted.ptr - buffer.data())});
    }

    [[nodiscard]] core::Result<std::size_t> finish() noexcept {
        if (failed_ || size_ >= output_.size()) {
            if (!output_.empty()) {
                output_[0] = '\0';
            }
            return core::Result<std::size_t>::failure(diagnostics_error(
                core::ErrorCode::serialization_overflow, "format-log", "output-too-small"));
        }
        output_[size_] = '\0';
        return core::Result<std::size_t>::success(size_);
    }

  private:
    [[nodiscard]] std::size_t remaining() const noexcept {
        return size_ <= output_.size() ? output_.size() - size_ : 0U;
    }

    std::span<char> output_{};
    std::size_t size_{};
    bool failed_{};
};

[[nodiscard]] bool same_component(const LogLevelOverride& override,
                                  std::string_view component) noexcept {
    return override.used && override.component_size == component.size() &&
           std::memcmp(override.component.data(), component.data(), component.size()) == 0;
}

[[nodiscard]] bool valid_handle(MetricHandle handle, std::size_t size) noexcept {
    return handle.index != 0xffffU && static_cast<std::size_t>(handle.index) < size;
}

} // namespace

std::string_view log_level_name(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::off:
        return "off";
    case LogLevel::error:
        return "error";
    case LogLevel::warning:
        return "warning";
    case LogLevel::info:
        return "info";
    case LogLevel::debug:
        return "debug";
    case LogLevel::verbose:
        return "verbose";
    }
    return "off";
}

core::Status StructuredLogger::set_default_level(LogLevel level) noexcept {
    if (!valid_log_level(level)) {
        return core::Status::failure(diagnostics_error(core::ErrorCode::invalid_argument,
                                                       "set-default-log-level", "invalid-level"));
    }
    default_level_ = level;
    return core::Status::success();
}

core::Status StructuredLogger::set_component_level(std::string_view component,
                                                   LogLevel level) noexcept {
    if (!valid_name(component, kMaxLogComponentBytes) || !valid_log_level(level)) {
        return core::Status::failure(diagnostics_error(core::ErrorCode::invalid_argument,
                                                       "set-log-level", "invalid-component"));
    }
    for (auto& override : overrides_) {
        if (same_component(override, component)) {
            override.level = level;
            return core::Status::success();
        }
    }
    for (auto& override : overrides_) {
        if (!override.used) {
            override = {};
            override.used = true;
            override.level = level;
            copy_text(override.component, override.component_size, component);
            return core::Status::success();
        }
    }
    return core::Status::failure(
        diagnostics_error(core::ErrorCode::capacity_exceeded, "set-log-level", "override-full"));
}

core::Status StructuredLogger::clear_component_level(std::string_view component) noexcept {
    if (!valid_name(component, kMaxLogComponentBytes)) {
        return core::Status::failure(diagnostics_error(core::ErrorCode::invalid_argument,
                                                       "clear-log-level", "invalid-component"));
    }
    for (auto& override : overrides_) {
        if (same_component(override, component)) {
            override = {};
            return core::Status::success();
        }
    }
    return core::Status::failure(
        diagnostics_error(core::ErrorCode::not_found, "clear-log-level", "override-not-found"));
}

LogLevel StructuredLogger::effective_level(std::string_view component) const noexcept {
    for (const auto& override : overrides_) {
        if (same_component(override, component)) {
            return override.level;
        }
    }
    return default_level_;
}

core::Status StructuredLogger::emit(const LogInput& input) noexcept {
    if (!valid_log_level(input.level) || input.level == LogLevel::off ||
        !valid_name(input.component, kMaxLogComponentBytes) ||
        !valid_name(input.event, kMaxLogEventBytes) || input.fields.size() > kMaxLogFields) {
        return core::Status::failure(
            diagnostics_error(core::ErrorCode::invalid_argument, "emit-log", "invalid-record"));
    }
    if (static_cast<std::uint8_t>(input.level) >
        static_cast<std::uint8_t>(effective_level(input.component))) {
        saturating_increment(metrics_.filtered);
        return core::Status::success();
    }
    for (std::size_t index = 0; index < input.fields.size(); ++index) {
        const auto& field = input.fields[index];
        if (!valid_name(field.key, kMaxLogFieldKeyBytes) ||
            !valid_log_value_type(field.value.type) ||
            (!field.secret && field.value.type == LogValueType::string &&
             field.value.string.size() > kMaxLogStringBytes) ||
            (!field.secret && field.value.type == LogValueType::number &&
             !std::isfinite(field.value.number))) {
            return core::Status::failure(
                diagnostics_error(core::ErrorCode::invalid_argument, "emit-log", "invalid-field"));
        }
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (input.fields[prior].key == field.key) {
                return core::Status::failure(diagnostics_error(core::ErrorCode::duplicate_id,
                                                               "emit-log", "duplicate-field"));
            }
        }
    }
    if (size_ == records_.size()) {
        saturating_increment(metrics_.dropped);
        return core::Status::failure(
            diagnostics_error(core::ErrorCode::queue_full, "emit-log", "log-queue-full"));
    }

    StructuredLogRecord record{};
    record.sequence = next_sequence_;
    if (next_sequence_ != std::numeric_limits<std::uint64_t>::max()) {
        ++next_sequence_;
    }
    record.timestamp_us = input.timestamp_us;
    record.level = input.level;
    copy_text(record.component, record.component_size, input.component);
    copy_text(record.event, record.event_size, input.event);
    record.field_count = static_cast<std::uint8_t>(input.fields.size());
    for (std::size_t index = 0; index < input.fields.size(); ++index) {
        const auto& source = input.fields[index];
        auto& target = record.fields[index];
        copy_text(target.key, target.key_size, source.key);
        if (source.secret) {
            target.type = LogValueType::string;
            target.redacted = true;
            copy_text(target.string, target.string_size, kRedacted);
            saturating_increment(metrics_.redacted_fields);
            continue;
        }
        target.type = source.value.type;
        target.boolean = source.value.boolean;
        target.signed_integer = source.value.signed_integer;
        target.unsigned_integer = source.value.unsigned_integer;
        target.number = source.value.number;
        if (source.value.type == LogValueType::string) {
            copy_text(target.string, target.string_size, source.value.string);
        }
    }
    const std::size_t tail = (head_ + size_) % records_.size();
    records_[tail] = record;
    ++size_;
    saturating_increment(metrics_.accepted);
    metrics_.high_water_mark = std::max(metrics_.high_water_mark, size_);
    return core::Status::success();
}

bool StructuredLogger::pop(StructuredLogRecord& output) noexcept {
    if (size_ == 0U) {
        return false;
    }
    output = records_[head_];
    head_ = (head_ + 1U) % records_.size();
    --size_;
    return true;
}

core::Result<std::size_t> format_structured_log_json(const StructuredLogRecord& record,
                                                     std::span<char> output) noexcept {
    if (!valid_log_level(record.level) || record.component_size > kMaxLogComponentBytes ||
        record.event_size > kMaxLogEventBytes || record.field_count > kMaxLogFields ||
        !valid_name({record.component.data(), record.component_size}, kMaxLogComponentBytes) ||
        !valid_name({record.event.data(), record.event_size}, kMaxLogEventBytes)) {
        return core::Result<std::size_t>::failure(
            diagnostics_error(core::ErrorCode::invalid_argument, "format-log", "invalid-record"));
    }
    for (std::size_t index = 0; index < record.field_count; ++index) {
        const auto& field = record.fields[index];
        if (field.key_size > kMaxLogFieldKeyBytes || field.string_size > kMaxLogStringBytes ||
            !valid_name({field.key.data(), field.key_size}, kMaxLogFieldKeyBytes) ||
            !valid_log_value_type(field.type) ||
            (field.type == LogValueType::number && !std::isfinite(field.number))) {
            return core::Result<std::size_t>::failure(diagnostics_error(
                core::ErrorCode::invalid_argument, "format-log", "invalid-field"));
        }
    }
    JsonWriter writer{output};
    if (!writer.append("{\"seq\":") || !writer.integer(record.sequence) ||
        !writer.append(",\"ts_us\":") || !writer.integer(record.timestamp_us) ||
        !writer.append(",\"level\":") || !writer.quoted(log_level_name(record.level)) ||
        !writer.append(",\"component\":") ||
        !writer.quoted({record.component.data(), record.component_size}) ||
        !writer.append(",\"event\":") || !writer.quoted({record.event.data(), record.event_size}) ||
        !writer.append(",\"fields\":{")) {
        return writer.finish();
    }
    for (std::size_t index = 0; index < record.field_count; ++index) {
        const auto& field = record.fields[index];
        if ((index != 0U && !writer.append(",")) ||
            !writer.quoted({field.key.data(), field.key_size}) || !writer.append(":")) {
            return writer.finish();
        }
        bool written{};
        switch (field.type) {
        case LogValueType::boolean:
            written = writer.append(field.boolean ? "true" : "false");
            break;
        case LogValueType::signed_integer:
            written = writer.integer(field.signed_integer);
            break;
        case LogValueType::unsigned_integer:
            written = writer.integer(field.unsigned_integer);
            break;
        case LogValueType::number:
            written = writer.number(field.number);
            break;
        case LogValueType::string:
            written = writer.quoted({field.string.data(), field.string_size});
            break;
        }
        if (!written) {
            return writer.finish();
        }
    }
    static_cast<void>(writer.append("}}"));
    return writer.finish();
}

core::Result<MetricHandle>
MetricRegistry::register_metric(const MetricDescriptorInput& descriptor) noexcept {
    if (!valid_name(descriptor.id, kMaxMetricIdBytes) ||
        descriptor.unit.size() > kMaxMetricUnitBytes) {
        return core::Result<MetricHandle>::failure(diagnostics_error(
            core::ErrorCode::invalid_argument, "register-metric", "invalid-descriptor"));
    }
    for (std::size_t index = 0; index < size_; ++index) {
        const auto& record = records_[index];
        if (record.id_size == descriptor.id.size() &&
            std::memcmp(record.id.data(), descriptor.id.data(), descriptor.id.size()) == 0) {
            return core::Result<MetricHandle>::failure(diagnostics_error(
                core::ErrorCode::duplicate_id, "register-metric", "duplicate-metric"));
        }
    }
    if (size_ == records_.size() || size_ >= std::numeric_limits<std::uint16_t>::max()) {
        return core::Result<MetricHandle>::failure(diagnostics_error(
            core::ErrorCode::capacity_exceeded, "register-metric", "metric-registry-full"));
    }
    auto& record = records_[size_];
    record = {};
    record.used = true;
    record.type = descriptor.type;
    copy_text(record.id, record.id_size, descriptor.id);
    copy_text(record.unit, record.unit_size, descriptor.unit);
    const MetricHandle handle{static_cast<std::uint16_t>(size_)};
    ++size_;
    return core::Result<MetricHandle>::success(handle);
}

MetricRecord* MetricRegistry::resolve(MetricHandle handle, MetricType expected) noexcept {
    if (!valid_handle(handle, size_) || records_[handle.index].type != expected) {
        return nullptr;
    }
    return &records_[handle.index];
}

const MetricRecord* MetricRegistry::resolve(MetricHandle handle,
                                            MetricType expected) const noexcept {
    if (!valid_handle(handle, size_) || records_[handle.index].type != expected) {
        return nullptr;
    }
    return &records_[handle.index];
}

core::Status MetricRegistry::increment(MetricHandle handle, std::uint64_t amount) noexcept {
    MetricRecord* record = resolve(handle, MetricType::counter);
    if (record == nullptr) {
        return core::Status::failure(diagnostics_error(
            core::ErrorCode::invalid_argument, "increment-metric", "invalid-handle-or-type"));
    }
    record->counter = amount > std::numeric_limits<std::uint64_t>::max() - record->counter
                          ? std::numeric_limits<std::uint64_t>::max()
                          : record->counter + amount;
    return core::Status::success();
}

core::Status MetricRegistry::set_gauge(MetricHandle handle, std::int64_t value) noexcept {
    MetricRecord* record = resolve(handle, MetricType::gauge);
    if (record == nullptr) {
        return core::Status::failure(diagnostics_error(core::ErrorCode::invalid_argument,
                                                       "set-gauge", "invalid-handle-or-type"));
    }
    record->gauge = value;
    return core::Status::success();
}

core::Result<std::uint64_t> MetricRegistry::counter(MetricHandle handle) const noexcept {
    const MetricRecord* record = resolve(handle, MetricType::counter);
    if (record == nullptr) {
        return core::Result<std::uint64_t>::failure(diagnostics_error(
            core::ErrorCode::invalid_argument, "read-counter", "invalid-handle-or-type"));
    }
    return core::Result<std::uint64_t>::success(record->counter);
}

core::Result<std::int64_t> MetricRegistry::gauge(MetricHandle handle) const noexcept {
    const MetricRecord* record = resolve(handle, MetricType::gauge);
    if (record == nullptr) {
        return core::Result<std::int64_t>::failure(diagnostics_error(
            core::ErrorCode::invalid_argument, "read-gauge", "invalid-handle-or-type"));
    }
    return core::Result<std::int64_t>::success(record->gauge);
}

std::span<const MetricRecord> MetricRegistry::records() const noexcept {
    return records_.first(size_);
}

} // namespace blip::core
