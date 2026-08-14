#pragma once

#include "blip/core/error.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace blip::ota {

constexpr std::size_t kSha256Bytes = 32;
constexpr std::size_t kEspAppDescriptorEnd = 288;

enum class UpdateState : std::uint8_t {
    idle,
    receiving,
    ready_to_reboot,
    pending_confirmation,
    confirmed,
    failed,
};

struct UpdateManifest {
    std::size_t image_size{};
    std::array<std::byte, kSha256Bytes> sha256{};
    std::string_view project{};
    std::string_view version{};
    std::string_view target{};
    std::string_view profile{};
};

struct UpdateStatus {
    UpdateState state{UpdateState::idle};
    std::size_t received_bytes{};
    std::size_t expected_bytes{};
    bool signature_enforced{};
};

class UpdateBackend {
  public:
    virtual ~UpdateBackend() = default;
    [[nodiscard]] virtual std::size_t maximum_image_size() const noexcept = 0;
    [[nodiscard]] virtual core::Status begin(std::size_t image_size) noexcept = 0;
    [[nodiscard]] virtual core::Status write(std::span<const std::byte> data) noexcept = 0;
    // The platform must perform its native image/chip/signature validation here.
    [[nodiscard]] virtual core::Status finish() noexcept = 0;
    virtual void abort() noexcept = 0;
    [[nodiscard]] virtual core::Status activate() noexcept = 0;
    [[nodiscard]] virtual core::Status confirm_running() noexcept = 0;
    [[nodiscard]] virtual core::Status rollback_running() noexcept = 0;
    [[nodiscard]] virtual bool running_image_pending_confirmation() const noexcept = 0;
    [[nodiscard]] virtual bool signature_enforced() const noexcept = 0;
};

class UpdateService {
  public:
    UpdateService(UpdateBackend& backend, std::string_view project, std::string_view target,
                  std::string_view profile) noexcept;

    [[nodiscard]] core::Status begin(const UpdateManifest& manifest) noexcept;
    [[nodiscard]] core::Status append(std::span<const std::byte> data) noexcept;
    [[nodiscard]] core::Status finish() noexcept;
    void cancel() noexcept;
    [[nodiscard]] core::Status confirm_boot() noexcept;
    [[nodiscard]] core::Status reject_boot() noexcept;
    void refresh_boot_state() noexcept;

    [[nodiscard]] const UpdateStatus& status() const noexcept { return status_; }

  private:
    [[nodiscard]] core::Status validate_descriptor() const noexcept;

    class Sha256;
    UpdateBackend* backend_{};
    std::string_view project_{};
    std::string_view target_{};
    std::string_view profile_{};
    UpdateManifest manifest_{};
    std::array<std::byte, kEspAppDescriptorEnd> prefix_{};
    std::size_t prefix_size_{};
    UpdateStatus status_{};
    alignas(8) std::array<std::byte, 112> sha_storage_{};
};

[[nodiscard]] const char* update_state_name(UpdateState state) noexcept;
[[nodiscard]] core::Result<std::array<std::byte, kSha256Bytes>>
parse_sha256_hex(std::string_view text) noexcept;

} // namespace blip::ota
