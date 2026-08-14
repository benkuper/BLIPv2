#pragma once

#include <cstdint>
#include <string_view>
#include <utility>

namespace blip::core {

enum class ErrorDomain : std::uint8_t {
    none,
    registry,
    lifecycle,
    descriptor,
    resource,
    scheduler,
    event,
    storage,
};

enum class ErrorCode : std::uint16_t {
    none,
    invalid_argument,
    invalid_state,
    capacity_exceeded,
    duplicate_id,
    missing_dependency,
    dependency_cycle,
    validation_failed,
    start_failed,
    suspend_failed,
    resume_failed,
    stop_failed,
    serialization_overflow,
    resource_unavailable,
    resource_conflict,
    resource_reserved,
    queue_full,
    queue_faulted,
    budget_exceeded,
    cancelled,
    wrong_task_context,
    recursive_dispatch,
    not_found,
    io_failed,
    storage_full,
    corrupt_data,
    incompatible_version,
    verification_failed,
    generation_exhausted,
};

struct Error {
    ErrorDomain domain{ErrorDomain::none};
    ErrorCode code{ErrorCode::none};
    std::string_view component{};
    std::string_view operation{};
    std::string_view detail{};

    [[nodiscard]] constexpr bool valid() const noexcept { return code != ErrorCode::none; }
};

class Status {
  public:
    [[nodiscard]] static constexpr Status success() noexcept { return Status{}; }

    [[nodiscard]] static constexpr Status failure(Error error) noexcept { return Status{error}; }

    [[nodiscard]] constexpr bool ok() const noexcept { return !error_.valid(); }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return ok(); }
    [[nodiscard]] constexpr const Error& error() const noexcept { return error_; }

  private:
    constexpr Status() noexcept = default;
    constexpr explicit Status(Error error) noexcept : error_(error) {}

    Error error_{};
};

template <typename T> class Result {
  public:
    [[nodiscard]] static constexpr Result success(T value) noexcept {
        return Result{std::move(value), {}};
    }

    [[nodiscard]] static constexpr Result failure(Error error) noexcept {
        return Result{{}, error};
    }

    [[nodiscard]] constexpr bool ok() const noexcept { return !error_.valid(); }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return ok(); }
    [[nodiscard]] constexpr const T& value() const noexcept { return value_; }
    [[nodiscard]] constexpr T& value() noexcept { return value_; }
    [[nodiscard]] constexpr const Error& error() const noexcept { return error_; }

  private:
    constexpr Result(T value, Error error) noexcept : value_(std::move(value)), error_(error) {}

    T value_{};
    Error error_{};
};

} // namespace blip::core
