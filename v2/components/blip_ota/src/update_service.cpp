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

[[nodiscard]] constexpr std::uint32_t rotate_right(std::uint32_t value, unsigned count) noexcept {
    return std::rotr(value, static_cast<int>(count));
}

} // namespace

class UpdateService::Sha256 {
  public:
    void reset() noexcept {
        state_ = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                  0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
        block_size_ = 0;
        total_size_ = 0;
    }

    void update(std::span<const std::byte> input) noexcept {
        total_size_ += input.size();
        while (!input.empty()) {
            const std::size_t count = std::min(input.size(), block_.size() - block_size_);
            std::copy_n(input.begin(), count, block_.begin() + block_size_);
            block_size_ += count;
            input = input.subspan(count);
            if (block_size_ == block_.size()) {
                transform();
                block_size_ = 0;
            }
        }
    }

    [[nodiscard]] std::array<std::byte, kSha256Bytes> finish() noexcept {
        const std::uint64_t bits = static_cast<std::uint64_t>(total_size_) * 8U;
        block_[block_size_++] = std::byte{0x80};
        if (block_size_ > 56U) {
            std::fill(block_.begin() + block_size_, block_.end(), std::byte{0});
            transform();
            block_size_ = 0;
        }
        std::fill(block_.begin() + block_size_, block_.begin() + 56, std::byte{0});
        for (std::size_t index = 0; index < 8U; ++index) {
            block_[56U + index] = static_cast<std::byte>(bits >> ((7U - index) * 8U));
        }
        transform();
        std::array<std::byte, kSha256Bytes> digest{};
        for (std::size_t word = 0; word < state_.size(); ++word) {
            for (std::size_t byte = 0; byte < 4U; ++byte) {
                digest[word * 4U + byte] =
                    static_cast<std::byte>(state_[word] >> ((3U - byte) * 8U));
            }
        }
        return digest;
    }

  private:
    void transform() noexcept {
        static constexpr std::array<std::uint32_t, 64> constants{
            0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U,
            0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
            0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U,
            0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
            0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
            0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
            0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
            0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
            0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU,
            0x5b9cca4fU, 0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
            0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};
        std::array<std::uint32_t, 64> words{};
        for (std::size_t index = 0; index < 16U; ++index) {
            const std::size_t offset = index * 4U;
            words[index] =
                static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(block_[offset])) << 24U |
                static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(block_[offset + 1U]))
                    << 16U |
                static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(block_[offset + 2U]))
                    << 8U |
                static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(block_[offset + 3U]));
        }
        for (std::size_t index = 16; index < words.size(); ++index) {
            const std::uint32_t s0 = rotate_right(words[index - 15U], 7U) ^
                                     rotate_right(words[index - 15U], 18U) ^
                                     (words[index - 15U] >> 3U);
            const std::uint32_t s1 = rotate_right(words[index - 2U], 17U) ^
                                     rotate_right(words[index - 2U], 19U) ^
                                     (words[index - 2U] >> 10U);
            words[index] = words[index - 16U] + s0 + words[index - 7U] + s1;
        }
        auto [a, b, c, d, e, f, g, h] = state_;
        for (std::size_t index = 0; index < words.size(); ++index) {
            const std::uint32_t s1 =
                rotate_right(e, 6U) ^ rotate_right(e, 11U) ^ rotate_right(e, 25U);
            const std::uint32_t choice = (e & f) ^ (~e & g);
            const std::uint32_t temp1 = h + s1 + choice + constants[index] + words[index];
            const std::uint32_t s0 =
                rotate_right(a, 2U) ^ rotate_right(a, 13U) ^ rotate_right(a, 22U);
            const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = s0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::array<std::uint32_t, 8> state_{};
    std::array<std::byte, 64> block_{};
    std::size_t block_size_{};
    std::size_t total_size_{};
};

UpdateService::UpdateService(UpdateBackend& backend, std::string_view project,
                             std::string_view target, std::string_view profile) noexcept
    : backend_(&backend), project_(project), target_(target), profile_(profile),
      status_{UpdateState::confirmed, 0, 0, backend.signature_enforced()} {
    static_assert(sizeof(Sha256) <= 112);
    static_assert(alignof(Sha256) <= 8);
}

core::Status UpdateService::begin(const UpdateManifest& manifest) noexcept {
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
    if (status_.state != UpdateState::receiving) {
        return;
    }
    backend_->abort();
    status_ = {UpdateState::idle, 0, 0, backend_->signature_enforced()};
}

core::Status UpdateService::confirm_boot() noexcept {
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
