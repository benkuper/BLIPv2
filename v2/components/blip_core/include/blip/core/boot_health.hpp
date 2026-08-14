#pragma once

#include "blip/core/error.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace blip::core {

inline constexpr std::uint16_t kBootLedgerFormatVersion = 1;
inline constexpr std::size_t kBootLedgerBytes = 32;
inline constexpr std::uint16_t kDefaultBootFailureThreshold = 3;

enum class ResetCause : std::uint8_t {
    unknown,
    power_on,
    external,
    software,
    panic,
    interrupt_watchdog,
    task_watchdog,
    other_watchdog,
    deep_sleep,
    brownout,
    sdio,
    usb,
    jtag,
    efuse,
    power_glitch,
    cpu_lockup,
};

enum class SafeModeReason : std::uint8_t { none, boot_loop };

struct BootState {
    std::uint32_t boot_sequence{};
    std::uint16_t consecutive_failures{};
    std::uint16_t failure_threshold{kDefaultBootFailureThreshold};
    bool previous_boot_confirmed{};
    bool safe_mode{};
    ResetCause reset_cause{ResetCause::unknown};
    SafeModeReason safe_mode_reason{SafeModeReason::none};
};

struct BootDecision {
    BootState state{};
    bool ledger_was_valid{};
};

[[nodiscard]] std::string_view reset_cause_name(ResetCause cause) noexcept;
[[nodiscard]] std::string_view safe_mode_reason_name(SafeModeReason reason) noexcept;

class BootLoopGuard {
  public:
    explicit constexpr BootLoopGuard(
        std::uint16_t failure_threshold = kDefaultBootFailureThreshold) noexcept
        : failure_threshold_(failure_threshold) {}

    [[nodiscard]] core::Result<BootDecision> begin_boot(std::span<std::byte> ledger,
                                                        ResetCause cause) const noexcept;
    [[nodiscard]] core::Status confirm_boot(std::span<std::byte> ledger) const noexcept;
    [[nodiscard]] core::Status clear_safe_mode(std::span<std::byte> ledger) const noexcept;
    [[nodiscard]] core::Result<BootState> inspect(std::span<const std::byte> ledger) const noexcept;

  private:
    std::uint16_t failure_threshold_{};
};

} // namespace blip::core
