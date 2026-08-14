#include "blip/core/boot_health.hpp"
#include "blip/core/diagnostics.hpp"
#include "test_harness.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>

namespace {

using namespace blip::core;

bool boot_loop_enters_and_exits_safe_mode() {
    std::array<std::byte, kBootLedgerBytes> ledger{};
    BootLoopGuard guard{};

    auto boot = guard.begin_boot(ledger, ResetCause::power_on);
    BLIP_CHECK(boot);
    BLIP_CHECK(!boot.value().ledger_was_valid);
    BLIP_CHECK(boot.value().state.boot_sequence == 1U);
    BLIP_CHECK(boot.value().state.consecutive_failures == 0U);
    BLIP_CHECK(!boot.value().state.safe_mode);

    for (std::uint16_t failure = 1; failure <= kDefaultBootFailureThreshold; ++failure) {
        boot = guard.begin_boot(ledger, ResetCause::panic);
        BLIP_CHECK(boot);
        BLIP_CHECK(boot.value().ledger_was_valid);
        BLIP_CHECK(boot.value().state.consecutive_failures == failure);
        BLIP_CHECK(boot.value().state.safe_mode == (failure == kDefaultBootFailureThreshold));
    }
    BLIP_CHECK(boot.value().state.safe_mode_reason == SafeModeReason::boot_loop);

    BLIP_CHECK(guard.confirm_boot(ledger));
    boot = guard.begin_boot(ledger, ResetCause::software);
    BLIP_CHECK(boot);
    BLIP_CHECK(boot.value().state.safe_mode);
    BLIP_CHECK(boot.value().state.consecutive_failures == kDefaultBootFailureThreshold);

    BLIP_CHECK(guard.clear_safe_mode(ledger));
    boot = guard.begin_boot(ledger, ResetCause::software);
    BLIP_CHECK(boot);
    BLIP_CHECK(!boot.value().state.safe_mode);
    BLIP_CHECK(boot.value().state.consecutive_failures == 0U);
    return true;
}

bool confirmation_and_cold_reset_break_failure_streak() {
    std::array<std::byte, kBootLedgerBytes> ledger{};
    BootLoopGuard guard{};
    BLIP_CHECK(guard.begin_boot(ledger, ResetCause::power_on));
    auto boot = guard.begin_boot(ledger, ResetCause::task_watchdog);
    BLIP_CHECK(boot);
    BLIP_CHECK(boot.value().state.consecutive_failures == 1U);
    BLIP_CHECK(guard.confirm_boot(ledger));

    boot = guard.begin_boot(ledger, ResetCause::software);
    BLIP_CHECK(boot);
    BLIP_CHECK(boot.value().state.consecutive_failures == 0U);
    boot = guard.begin_boot(ledger, ResetCause::panic);
    BLIP_CHECK(boot);
    BLIP_CHECK(boot.value().state.consecutive_failures == 1U);
    boot = guard.begin_boot(ledger, ResetCause::brownout);
    BLIP_CHECK(boot);
    BLIP_CHECK(boot.value().state.consecutive_failures == 0U);
    BLIP_CHECK(!boot.value().state.safe_mode);
    return true;
}

bool corrupt_ledger_fails_safe() {
    std::array<std::byte, kBootLedgerBytes> ledger{};
    BootLoopGuard guard{};
    BLIP_CHECK(guard.begin_boot(ledger, ResetCause::power_on));
    ledger[10] ^= std::byte{0x40};
    BLIP_CHECK(!guard.inspect(ledger));
    BLIP_CHECK(!guard.confirm_boot(ledger));
    const auto recovered = guard.begin_boot(ledger, ResetCause::panic);
    BLIP_CHECK(recovered);
    BLIP_CHECK(!recovered.value().ledger_was_valid);
    BLIP_CHECK(recovered.value().state.consecutive_failures == 0U);
    BLIP_CHECK(!recovered.value().state.safe_mode);
    return true;
}

bool structured_logging_is_bounded_and_redacted() {
    std::array<StructuredLogRecord, 2> records{};
    std::array<LogLevelOverride, 1> overrides{};
    StructuredLogger logger{records, overrides};
    BLIP_CHECK(logger.set_default_level(LogLevel::info));
    BLIP_CHECK(!logger.set_default_level(static_cast<LogLevel>(99)));

    BLIP_CHECK(logger.emit({1, LogLevel::debug, "blip.test", "filtered", {}}));
    BLIP_CHECK(logger.size() == 0U);
    BLIP_CHECK(logger.metrics().filtered == 1U);
    BLIP_CHECK(logger.set_component_level("blip.test", LogLevel::debug));

    constexpr std::string_view secret = "fixture-password";
    const std::array<LogFieldInput, 3> fields{{
        {"password", LogValue::from_string(secret), true},
        {"attempt", LogValue::from_unsigned(7), false},
        {"ready", LogValue::from_bool(true), false},
    }};
    BLIP_CHECK(logger.emit({2, LogLevel::debug, "blip.test", "connected", fields}));
    BLIP_CHECK(logger.metrics().redacted_fields == 1U);

    StructuredLogRecord record{};
    BLIP_CHECK(logger.pop(record));
    BLIP_CHECK(record.field_count == fields.size());
    BLIP_CHECK(record.fields[0].redacted);
    const std::string_view redacted{record.fields[0].string.data(), record.fields[0].string_size};
    BLIP_CHECK(redacted == "[redacted]");

    std::array<char, 512> json{};
    const auto formatted = format_structured_log_json(record, json);
    BLIP_CHECK(formatted);
    const std::string_view text{json.data(), formatted.value()};
    BLIP_CHECK(text.find("\"password\":\"[redacted]\"") != std::string_view::npos);
    BLIP_CHECK(text.find(secret) == std::string_view::npos);
    BLIP_CHECK(text.find("\"attempt\":7") != std::string_view::npos);
    BLIP_CHECK(text.find("\"ready\":true") != std::string_view::npos);

    std::array<char, 8> too_small{};
    BLIP_CHECK(!format_structured_log_json(record, too_small));
    BLIP_CHECK(too_small[0] == '\0');

    auto malformed = record;
    malformed.field_count = static_cast<std::uint8_t>(kMaxLogFields + 1U);
    BLIP_CHECK(!format_structured_log_json(malformed, json));
    malformed = record;
    malformed.fields[0].type = static_cast<LogValueType>(99);
    BLIP_CHECK(!format_structured_log_json(malformed, json));

    BLIP_CHECK(logger.emit({3, LogLevel::info, "blip.test", "one", {}}));
    BLIP_CHECK(logger.emit({4, LogLevel::info, "blip.test", "two", {}}));
    BLIP_CHECK(!logger.emit({5, LogLevel::error, "blip.test", "overflow", {}}));
    BLIP_CHECK(logger.metrics().dropped == 1U);
    BLIP_CHECK(logger.metrics().high_water_mark == 2U);
    return true;
}

bool structured_logger_validates_levels_and_fields() {
    std::array<StructuredLogRecord, 1> records{};
    std::array<LogLevelOverride, 1> overrides{};
    StructuredLogger logger{records, overrides};
    BLIP_CHECK(!logger.set_component_level("bad/component", LogLevel::debug));
    BLIP_CHECK(!logger.set_component_level("blip.test", static_cast<LogLevel>(99)));
    BLIP_CHECK(!logger.emit({1, static_cast<LogLevel>(99), "blip.test", "invalid_level", {}}));
    BLIP_CHECK(logger.set_component_level("blip.test", LogLevel::off));
    BLIP_CHECK(logger.emit({1, LogLevel::error, "blip.test", "hidden", {}}));
    BLIP_CHECK(logger.size() == 0U);

    const std::array<LogFieldInput, 2> duplicate{{
        {"value", LogValue::from_signed(1), false},
        {"value", LogValue::from_signed(2), false},
    }};
    BLIP_CHECK(!logger.emit({2, LogLevel::info, "blip.other", "duplicate", duplicate}));
    const std::array<LogFieldInput, 1> non_finite{{
        {"value", LogValue::from_number(std::numeric_limits<double>::infinity()), false},
    }};
    BLIP_CHECK(!logger.emit({2, LogLevel::info, "blip.other", "number", non_finite}));
    BLIP_CHECK(logger.clear_component_level("blip.test"));
    BLIP_CHECK(!logger.clear_component_level("blip.test"));
    return true;
}

bool metric_registry_is_typed_bounded_and_saturating() {
    std::array<MetricRecord, 2> storage{};
    MetricRegistry metrics{storage};
    const auto counter = metrics.register_metric({"events", MetricType::counter, "records"});
    const auto gauge = metrics.register_metric({"heap.free", MetricType::gauge, "bytes"});
    BLIP_CHECK(counter);
    BLIP_CHECK(gauge);
    BLIP_CHECK(!metrics.register_metric({"events", MetricType::counter, "records"}));
    BLIP_CHECK(!metrics.register_metric({"third", MetricType::gauge, ""}));

    BLIP_CHECK(metrics.increment(counter.value(), std::numeric_limits<std::uint64_t>::max() - 1U));
    BLIP_CHECK(metrics.increment(counter.value(), 10U));
    BLIP_CHECK(metrics.counter(counter.value()));
    BLIP_CHECK(metrics.counter(counter.value()).value() ==
               std::numeric_limits<std::uint64_t>::max());
    BLIP_CHECK(metrics.set_gauge(gauge.value(), -42));
    BLIP_CHECK(metrics.gauge(gauge.value()));
    BLIP_CHECK(metrics.gauge(gauge.value()).value() == -42);
    BLIP_CHECK(!metrics.increment(gauge.value()));
    BLIP_CHECK(!metrics.set_gauge(counter.value(), 0));
    BLIP_CHECK(metrics.records().size() == 2U);
    return true;
}

} // namespace

int main() {
    const TestCase tests[]{
        {"boot loop safe mode", boot_loop_enters_and_exits_safe_mode},
        {"boot confirmation streak", confirmation_and_cold_reset_break_failure_streak},
        {"corrupt boot ledger", corrupt_ledger_fails_safe},
        {"bounded redacted logs", structured_logging_is_bounded_and_redacted},
        {"log validation and levels", structured_logger_validates_levels_and_fields},
        {"typed saturating metrics", metric_registry_is_typed_bounded_and_saturating},
    };
    return run_tests(tests);
}
