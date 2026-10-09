#include "blip/ota/update_service.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <memory>

namespace blip::ota {
namespace {

constexpr std::uint32_t kAppDescriptorMagic = 0xabcd5432U;
constexpr std::size_t kAppDescriptorOffset = 32;
constexpr std::size_t kVersionOffset = kAppDescriptorOffset + 16;
constexpr std::size_t kProjectOffset = kVersionOffset + 32;

[[nodiscard]] core::Error update_error(core::ErrorCode code, std::string_view operation,
                                       std::string_view detail) noexcept {
    return {core::ErrorDomain::storage, code, "blip.ota", operation, detail};
}

[[nodiscard]] std::uint32_t read_u32_le(std::span<const std::byte> input,
                                        std::size_t offset) noexcept {
    return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(input[offset])) |
           static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(input[offset + 1U])) << 8U |
           static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(input[offset + 2U])) << 16U |
           static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(input[offset + 3U])) << 24U;
}

[[nodiscard]] bool fixed_text_equals(std::span<const std::byte> input, std::size_t offset,
                                     std::size_t capacity, std::string_view expected) noexcept {
    if (expected.empty() || expected.size() >= capacity) {
        return false;
    }
    const auto* text = reinterpret_cast<const char*>(input.data() + offset);
    const auto end = static_cast<const char*>(std::memchr(text, '\0', capacity));
    return end != nullptr &&
           std::string_view{text, static_cast<std::size_t>(end - text)} == expected;
}

} // namespace


UpdateService::UpdateService(UpdateBackend& backend, std::string_view project,
                             std::string_view target, std::string_view profile) noexcept
    : backend_(&backend), project_(project), target_(target), profile_(profile),
      status_{UpdateState::confirmed, 0, 0, backend.signature_enforced()} {
    static_assert(sizeof(Sha256) <= 112);
    static_assert(alignof(Sha256) <= 8);
}

core::Status UpdateService::begin(const UpdateManifest& manifest) noexcept {
    std::lock_guard guard(mutex_);
    if (status_.state == UpdateState::receiving || status_.state == UpdateState::ready_to_reboot ||
        status_.state == UpdateState::pending_confirmation) {
        return core::Status::failure(
            update_error(core::ErrorCode::invalid_state, "begin", "update-in-progress"));
    }
    if (manifest.image_size < kEspAppDescriptorEnd ||
        manifest.image_size > backend_->maximum_image_size()) {
        return core::Status::failure(
            update_error(core::ErrorCode::capacity_exceeded, "begin", "image-size"));
    }
    if (manifest.project != project_ || manifest.target != target_ ||
        manifest.profile != profile_ || manifest.version.empty() ||
        manifest.version.size() >= 32U) {
        return core::Status::failure(
            update_error(core::ErrorCode::incompatible_version, "begin", "manifest-identity"));
    }
    const auto begun = backend_->begin(manifest.image_size);
    if (!begun) {
        backend_->abort();
        status_.state = UpdateState::failed;
        return begun;
    }
    manifest_ = manifest;
    prefix_size_ = 0;
    status_ = {UpdateState::receiving, 0, manifest.image_size, backend_->signature_enforced()};
    auto* sha = std::construct_at(reinterpret_cast<Sha256*>(sha_storage_.data()));
    sha->reset();
    return core::Status::success();
}

core::Status UpdateService::append(std::span<const std::byte> data) noexcept {
    std::lock_guard guard(mutex_);
    if (status_.state != UpdateState::receiving) {
        return core::Status::failure(
            update_error(core::ErrorCode::invalid_state, "append", "no-update"));
    }
    if (data.empty() || data.size() > status_.expected_bytes - status_.received_bytes) {
        cancel();
        status_.state = UpdateState::failed;
        return core::Status::failure(
            update_error(core::ErrorCode::capacity_exceeded, "append", "image-overflow"));
    }
    const std::size_t prefix_count = std::min(data.size(), prefix_.size() - prefix_size_);
    std::copy_n(data.begin(), prefix_count, prefix_.begin() + prefix_size_);
    prefix_size_ += prefix_count;
    reinterpret_cast<Sha256*>(sha_storage_.data())->update(data);
    const auto written = backend_->write(data);
    if (!written) {
        cancel();
        status_.state = UpdateState::failed;
        return written;
    }
    status_.received_bytes += data.size();
    return core::Status::success();
}

core::Status UpdateService::validate_descriptor() const noexcept {
    if (prefix_size_ != prefix_.size() || prefix_[0] != std::byte{0xe9} ||
        read_u32_le(prefix_, kAppDescriptorOffset) != kAppDescriptorMagic) {
        return core::Status::failure(
            update_error(core::ErrorCode::corrupt_data, "finish", "app-descriptor"));
    }
    if (!fixed_text_equals(prefix_, kProjectOffset, 32U, manifest_.project) ||
        !fixed_text_equals(prefix_, kVersionOffset, 32U, manifest_.version)) {
        return core::Status::failure(
            update_error(core::ErrorCode::verification_failed, "finish", "image-identity"));
    }
    return core::Status::success();
}

core::Status UpdateService::finish() noexcept {
    std::lock_guard guard(mutex_);
    if (status_.state != UpdateState::receiving ||
        status_.received_bytes != status_.expected_bytes) {
        cancel();
        status_.state = UpdateState::failed;
        return core::Status::failure(
            update_error(core::ErrorCode::verification_failed, "finish", "incomplete-image"));
    }
    const auto descriptor = validate_descriptor();
    const auto digest = reinterpret_cast<Sha256*>(sha_storage_.data())->finish();
    if (!descriptor || digest != manifest_.sha256) {
        backend_->abort();
        status_.state = UpdateState::failed;
        return descriptor ? core::Status::failure(update_error(core::ErrorCode::verification_failed,
                                                               "finish", "sha256"))
                          : descriptor;
    }
    const auto finished = backend_->finish();
    if (!finished) {
        backend_->abort();
        status_.state = UpdateState::failed;
        return finished;
    }
    const auto activated = backend_->activate();
    if (!activated) {
        status_.state = UpdateState::failed;
        return activated;
    }
    status_.state = UpdateState::ready_to_reboot;
    return core::Status::success();
}

void UpdateService::cancel() noexcept {
    std::lock_guard guard(mutex_);
    if (status_.state != UpdateState::receiving) {
        return;
    }
    backend_->abort();
    status_ = {UpdateState::idle, 0, 0, backend_->signature_enforced()};
}

core::Status UpdateService::confirm_boot() noexcept {
    std::lock_guard guard(mutex_);
    if (status_.state != UpdateState::pending_confirmation) {
        return core::Status::success();
    }
    const auto confirmed = backend_->confirm_running();
    if (confirmed) {
        status_.state = UpdateState::confirmed;
    }
    return confirmed;
}

core::Status UpdateService::reject_boot() noexcept {
    std::lock_guard guard(mutex_);
    if (status_.state != UpdateState::pending_confirmation) {
        return core::Status::success();
    }
    const auto rejected = backend_->rollback_running();
    if (rejected) {
        status_.state = UpdateState::failed;
    }
    return rejected;
}

void UpdateService::refresh_boot_state() noexcept {
    std::lock_guard guard(mutex_);
    status_ = {backend_->running_image_pending_confirmation() ? UpdateState::pending_confirmation
                                                              : UpdateState::confirmed,
               0, 0, backend_->signature_enforced()};
}

const char* update_state_name(UpdateState state) noexcept {
    switch (state) {
    case UpdateState::idle:
        return "idle";
    case UpdateState::receiving:
        return "receiving";
    case UpdateState::ready_to_reboot:
        return "ready-to-reboot";
    case UpdateState::pending_confirmation:
        return "pending-confirmation";
    case UpdateState::confirmed:
        return "confirmed";
    case UpdateState::failed:
        return "failed";
    }
    return "failed";
}

core::Result<std::array<std::byte, kSha256Bytes>> parse_sha256_hex(std::string_view text) noexcept {
    if (text.size() != kSha256Bytes * 2U) {
        return core::Result<std::array<std::byte, kSha256Bytes>>::failure(
            update_error(core::ErrorCode::invalid_argument, "parse-sha256", "length"));
    }
    const auto nibble = [](char value) -> int {
        if (value >= '0' && value <= '9') {
            return value - '0';
        }
        if (value >= 'a' && value <= 'f') {
            return value - 'a' + 10;
        }
        if (value >= 'A' && value <= 'F') {
            return value - 'A' + 10;
        }
        return -1;
    };
    std::array<std::byte, kSha256Bytes> output{};
    for (std::size_t index = 0; index < output.size(); ++index) {
        const int high = nibble(text[index * 2U]);
        const int low = nibble(text[index * 2U + 1U]);
        if (high < 0 || low < 0) {
            return core::Result<std::array<std::byte, kSha256Bytes>>::failure(
                update_error(core::ErrorCode::invalid_argument, "parse-sha256", "character"));
        }
        output[index] = static_cast<std::byte>((high << 4) | low);
    }
    return core::Result<std::array<std::byte, kSha256Bytes>>::success(output);
}

} // namespace blip::ota
