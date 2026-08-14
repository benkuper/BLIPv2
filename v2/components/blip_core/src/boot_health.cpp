#include "blip/core/boot_health.hpp"

#include <algorithm>
#include <limits>

namespace blip::core {
namespace {

constexpr std::uint32_t kLedgerMagic = 0x48424c42U; // "BLBH".
constexpr std::size_t kCrcOffset = 28;

[[nodiscard]] core::Error boot_error(core::ErrorCode code, std::string_view operation,
                                     std::string_view detail) noexcept {
    return {core::ErrorDomain::diagnostics, code, "blip.diagnostics", operation, detail};
}

void write_u16(std::span<std::byte> output, std::size_t offset, std::uint16_t value) noexcept {
    output[offset] = static_cast<std::byte>(value & 0xffU);
    output[offset + 1U] = static_cast<std::byte>(value >> 8U);
}

void write_u32(std::span<std::byte> output, std::size_t offset, std::uint32_t value) noexcept {
    for (std::size_t index = 0; index < 4U; ++index) {
        output[offset + index] = static_cast<std::byte>(value >> (index * 8U));
    }
}

[[nodiscard]] std::uint16_t read_u16(std::span<const std::byte> input,
                                     std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(input[offset])) |
           static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(input[offset + 1U])) << 8U;
}

[[nodiscard]] std::uint32_t read_u32(std::span<const std::byte> input,
                                     std::size_t offset) noexcept {
    std::uint32_t value{};
    for (std::size_t index = 0; index < 4U; ++index) {
        value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(input[offset + index]))
                 << (index * 8U);
    }
    return value;
}

[[nodiscard]] std::uint32_t ledger_crc(std::span<const std::byte> input) noexcept {
    std::uint32_t crc = 0xffffffffU;
    for (std::size_t index = 0; index < input.size(); ++index) {
        const std::uint8_t byte = index >= kCrcOffset && index < kCrcOffset + 4U
                                      ? 0U
                                      : std::to_integer<std::uint8_t>(input[index]);
        crc ^= byte;
        for (std::uint8_t bit = 0; bit < 8U; ++bit) {
            const std::uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return ~crc;
}

[[nodiscard]] bool valid_reset_cause(std::uint8_t value) noexcept {
    return value <= static_cast<std::uint8_t>(ResetCause::cpu_lockup);
}

[[nodiscard]] bool valid_safe_reason(std::uint8_t value) noexcept {
    return value <= static_cast<std::uint8_t>(SafeModeReason::boot_loop);
}

[[nodiscard]] bool starts_new_streak(ResetCause cause) noexcept {
    switch (cause) {
    case ResetCause::unknown:
    case ResetCause::power_on:
    case ResetCause::external:
    case ResetCause::deep_sleep:
    case ResetCause::brownout:
    case ResetCause::sdio:
    case ResetCause::usb:
    case ResetCause::jtag:
    case ResetCause::efuse:
    case ResetCause::power_glitch:
        return true;
    case ResetCause::software:
    case ResetCause::panic:
    case ResetCause::interrupt_watchdog:
    case ResetCause::task_watchdog:
    case ResetCause::other_watchdog:
    case ResetCause::cpu_lockup:
        return false;
    }
    return true;
}

[[nodiscard]] core::Status encode(const BootState& state, std::span<std::byte> ledger) noexcept {
    if (ledger.size() != kBootLedgerBytes || state.failure_threshold == 0U) {
        return core::Status::failure(
            boot_error(core::ErrorCode::invalid_argument, "encode-boot-ledger", "invalid-state"));
    }
    std::fill(ledger.begin(), ledger.end(), std::byte{0});
    write_u32(ledger, 0, kLedgerMagic);
    write_u16(ledger, 4, kBootLedgerFormatVersion);
    write_u16(ledger, 6, static_cast<std::uint16_t>(kBootLedgerBytes));
    write_u32(ledger, 8, state.boot_sequence);
    write_u16(ledger, 12, state.consecutive_failures);
    write_u16(ledger, 14, state.failure_threshold);
    ledger[16] = state.previous_boot_confirmed ? std::byte{1} : std::byte{0};
    ledger[17] = state.safe_mode ? std::byte{1} : std::byte{0};
    ledger[18] = static_cast<std::byte>(state.reset_cause);
    ledger[19] = static_cast<std::byte>(state.safe_mode_reason);
    write_u32(ledger, kCrcOffset, ledger_crc(ledger));
    return core::Status::success();
}

[[nodiscard]] core::Result<BootState> decode(std::span<const std::byte> ledger) noexcept {
    if (ledger.size() != kBootLedgerBytes || read_u32(ledger, 0) != kLedgerMagic ||
        read_u16(ledger, 4) != kBootLedgerFormatVersion ||
        read_u16(ledger, 6) != kBootLedgerBytes || read_u16(ledger, 14) == 0U ||
        std::to_integer<std::uint8_t>(ledger[16]) > 1U ||
        std::to_integer<std::uint8_t>(ledger[17]) > 1U ||
        !valid_reset_cause(std::to_integer<std::uint8_t>(ledger[18])) ||
        !valid_safe_reason(std::to_integer<std::uint8_t>(ledger[19])) ||
        !std::all_of(ledger.begin() + 20, ledger.begin() + 28,
                     [](std::byte value) { return value == std::byte{0}; }) ||
        read_u32(ledger, kCrcOffset) != ledger_crc(ledger)) {
        return core::Result<BootState>::failure(
            boot_error(core::ErrorCode::corrupt_data, "decode-boot-ledger", "invalid-record"));
    }
    BootState state{};
    state.boot_sequence = read_u32(ledger, 8);
    state.consecutive_failures = read_u16(ledger, 12);
    state.failure_threshold = read_u16(ledger, 14);
    state.previous_boot_confirmed = ledger[16] == std::byte{1};
    state.safe_mode = ledger[17] == std::byte{1};
    state.reset_cause = static_cast<ResetCause>(std::to_integer<std::uint8_t>(ledger[18]));
    state.safe_mode_reason = static_cast<SafeModeReason>(std::to_integer<std::uint8_t>(ledger[19]));
    if (state.safe_mode != (state.safe_mode_reason != SafeModeReason::none)) {
        return core::Result<BootState>::failure(boot_error(
            core::ErrorCode::corrupt_data, "decode-boot-ledger", "inconsistent-safe-mode"));
    }
    return core::Result<BootState>::success(state);
}

} // namespace

std::string_view reset_cause_name(ResetCause cause) noexcept {
    switch (cause) {
    case ResetCause::unknown:
        return "unknown";
    case ResetCause::power_on:
        return "power-on";
    case ResetCause::external:
        return "external";
    case ResetCause::software:
        return "software";
    case ResetCause::panic:
        return "panic";
    case ResetCause::interrupt_watchdog:
        return "interrupt-watchdog";
    case ResetCause::task_watchdog:
        return "task-watchdog";
    case ResetCause::other_watchdog:
        return "watchdog";
    case ResetCause::deep_sleep:
        return "deep-sleep";
    case ResetCause::brownout:
        return "brownout";
    case ResetCause::sdio:
        return "sdio";
    case ResetCause::usb:
        return "usb";
    case ResetCause::jtag:
        return "jtag";
    case ResetCause::efuse:
        return "efuse";
    case ResetCause::power_glitch:
        return "power-glitch";
    case ResetCause::cpu_lockup:
        return "cpu-lockup";
    }
    return "unknown";
}

std::string_view safe_mode_reason_name(SafeModeReason reason) noexcept {
    switch (reason) {
    case SafeModeReason::none:
        return "none";
    case SafeModeReason::boot_loop:
        return "boot-loop";
    }
    return "none";
}

core::Result<BootDecision> BootLoopGuard::begin_boot(std::span<std::byte> ledger,
                                                     ResetCause cause) const noexcept {
    if (ledger.size() != kBootLedgerBytes || failure_threshold_ == 0U) {
        return core::Result<BootDecision>::failure(boot_error(
            core::ErrorCode::invalid_argument, "begin-boot", "invalid-ledger-or-threshold"));
    }
    const auto prior = decode(ledger);
    const bool prior_valid = prior.ok();
    BootState state = prior_valid ? prior.value() : BootState{};
    state.failure_threshold = failure_threshold_;

    if (!prior_valid || starts_new_streak(cause)) {
        state.consecutive_failures = 0;
        state.safe_mode = false;
        state.safe_mode_reason = SafeModeReason::none;
        if (!prior_valid) {
            state.boot_sequence = 0;
        }
    } else if (!state.previous_boot_confirmed &&
               state.consecutive_failures != std::numeric_limits<std::uint16_t>::max()) {
        ++state.consecutive_failures;
    } else if (state.previous_boot_confirmed && !state.safe_mode) {
        state.consecutive_failures = 0;
    }

    if (state.consecutive_failures >= state.failure_threshold) {
        state.safe_mode = true;
        state.safe_mode_reason = SafeModeReason::boot_loop;
    }
    if (state.boot_sequence != std::numeric_limits<std::uint32_t>::max()) {
        ++state.boot_sequence;
    }
    state.previous_boot_confirmed = false;
    state.reset_cause = cause;
    const auto encoded = encode(state, ledger);
    if (!encoded) {
        return core::Result<BootDecision>::failure(encoded.error());
    }
    return core::Result<BootDecision>::success({state, prior_valid});
}

core::Status BootLoopGuard::confirm_boot(std::span<std::byte> ledger) const noexcept {
    const auto decoded = decode(ledger);
    if (!decoded) {
        return core::Status::failure(decoded.error());
    }
    BootState state = decoded.value();
    state.previous_boot_confirmed = true;
    if (!state.safe_mode) {
        state.consecutive_failures = 0;
    }
    return encode(state, ledger);
}

core::Status BootLoopGuard::clear_safe_mode(std::span<std::byte> ledger) const noexcept {
    const auto decoded = decode(ledger);
    if (!decoded) {
        return core::Status::failure(decoded.error());
    }
    BootState state = decoded.value();
    state.consecutive_failures = 0;
    state.previous_boot_confirmed = true;
    state.safe_mode = false;
    state.safe_mode_reason = SafeModeReason::none;
    return encode(state, ledger);
}

core::Result<BootState> BootLoopGuard::inspect(std::span<const std::byte> ledger) const noexcept {
    return decode(ledger);
}

} // namespace blip::core
