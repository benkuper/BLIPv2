#include "blip/ota/update_service.hpp"
#include "test_harness.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace {

using blip::core::ErrorCode;
using blip::core::ErrorDomain;
using blip::core::Status;
using blip::ota::UpdateBackend;
using blip::ota::UpdateManifest;
using blip::ota::UpdateService;
using blip::ota::UpdateState;

class FakeBackend final : public UpdateBackend {
  public:
    [[nodiscard]] std::size_t maximum_image_size() const noexcept override { return 1024; }
    [[nodiscard]] Status begin(std::size_t image_size) noexcept override {
        ++begins;
        expected = image_size;
        bytes.clear();
        open = true;
        return injected("begin");
    }
    [[nodiscard]] Status write(std::span<const std::byte> data) noexcept override {
        ++writes;
        if (fail_operation == "write") {
            return injected("write");
        }
        bytes.insert(bytes.end(), data.begin(), data.end());
        return Status::success();
    }
    [[nodiscard]] Status finish() noexcept override {
        ++finishes;
        open = false;
        return injected("finish");
    }
    void abort() noexcept override {
        ++aborts;
        open = false;
    }
    [[nodiscard]] Status activate() noexcept override {
        ++activations;
        return injected("activate");
    }
    [[nodiscard]] Status confirm_running() noexcept override {
        ++confirmations;
        pending = false;
        return injected("confirm");
    }
    [[nodiscard]] Status rollback_running() noexcept override {
        ++rollbacks;
        pending = false;
        return injected("rollback");
    }
    [[nodiscard]] bool running_image_pending_confirmation() const noexcept override {
        return pending;
    }
    [[nodiscard]] bool signature_enforced() const noexcept override { return signed_images; }

    [[nodiscard]] Status injected(std::string_view operation) const noexcept {
        return fail_operation == operation
                   ? Status::failure({ErrorDomain::storage, ErrorCode::io_failed, "fake", operation,
                                      "injected"})
                   : Status::success();
    }

    std::vector<std::byte> bytes{};
    std::string_view fail_operation{};
    std::size_t expected{};
    std::size_t begins{};
    std::size_t writes{};
    std::size_t finishes{};
    std::size_t aborts{};
    std::size_t activations{};
    std::size_t confirmations{};
    std::size_t rollbacks{};
    bool open{};
    bool pending{};
    bool signed_images{true};
};

[[nodiscard]] std::array<std::byte, blip::ota::kEspAppDescriptorEnd> valid_image() {
    std::array<std::byte, blip::ota::kEspAppDescriptorEnd> image{};
    image[0] = std::byte{0xe9};
    image[32] = std::byte{0x32};
    image[33] = std::byte{0x54};
    image[34] = std::byte{0xcd};
    image[35] = std::byte{0xab};
    constexpr std::string_view version{"0.2.0"};
    constexpr std::string_view project{"blip-v2"};
    std::transform(version.begin(), version.end(), image.begin() + 48,
                   [](char value) { return static_cast<std::byte>(value); });
    std::transform(project.begin(), project.end(), image.begin() + 80,
                   [](char value) { return static_cast<std::byte>(value); });
    return image;
}

[[nodiscard]] UpdateManifest valid_manifest() {
    const auto parsed = blip::ota::parse_sha256_hex(
        "6766882251cc2788543e56b5f67d802b19fc07fb685e951b57da9aba6550f9a4");
    return {
        blip::ota::kEspAppDescriptorEnd, parsed.value(), "blip-v2", "0.2.0", "esp32", "minimal"};
}

bool test_sha_parser() {
    const auto parsed = blip::ota::parse_sha256_hex(
        "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
    BLIP_CHECK(parsed.ok());
    BLIP_CHECK(parsed.value()[0] == std::byte{0});
    BLIP_CHECK(parsed.value()[31] == std::byte{0x1f});
    BLIP_CHECK(!blip::ota::parse_sha256_hex("00").ok());
    BLIP_CHECK(!blip::ota::parse_sha256_hex(
                    "z00102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f")
                    .ok());
    return true;
}

bool test_manifest_is_checked_before_erasing() {
    FakeBackend backend{};
    UpdateService service{backend, "blip-v2", "esp32", "minimal"};
    auto manifest = valid_manifest();
    manifest.target = "esp32s3";
    const auto status = service.begin(manifest);
    BLIP_CHECK(!status.ok());
    BLIP_CHECK(status.error().code == ErrorCode::incompatible_version);
    BLIP_CHECK(backend.begins == 0);
    return true;
}

bool test_interrupted_transfer_never_activates() {
    FakeBackend backend{};
    UpdateService service{backend, "blip-v2", "esp32", "minimal"};
    const auto image = valid_image();
    BLIP_CHECK(service.begin(valid_manifest()).ok());
    BLIP_CHECK(service.append(std::span{image}.first(64)).ok());
    service.cancel();
    BLIP_CHECK(backend.aborts == 1);
    BLIP_CHECK(backend.activations == 0);
    BLIP_CHECK(service.status().state == UpdateState::idle);
    return true;
}

bool test_exact_image_and_descriptor_are_required() {
    FakeBackend backend{};
    UpdateService service{backend, "blip-v2", "esp32", "minimal"};
    auto image = valid_image();
    BLIP_CHECK(service.begin(valid_manifest()).ok());
    BLIP_CHECK(service.append(std::span{image}.first(image.size() - 1U)).ok());
    BLIP_CHECK(!service.finish().ok());
    BLIP_CHECK(backend.activations == 0);

    image[80] = std::byte{'x'};
    BLIP_CHECK(service.begin(valid_manifest()).ok());
    BLIP_CHECK(service.append(image).ok());
    const auto status = service.finish();
    BLIP_CHECK(!status.ok());
    BLIP_CHECK(status.error().code == ErrorCode::verification_failed);
    BLIP_CHECK(backend.activations == 0);
    return true;
}

bool test_hash_mismatch_never_finishes_backend() {
    FakeBackend backend{};
    UpdateService service{backend, "blip-v2", "esp32", "minimal"};
    const auto image = valid_image();
    auto manifest = valid_manifest();
    manifest.sha256[0] ^= std::byte{1};
    BLIP_CHECK(service.begin(manifest).ok());
    BLIP_CHECK(service.append(image).ok());
    const auto status = service.finish();
    BLIP_CHECK(!status.ok());
    BLIP_CHECK(backend.finishes == 0);
    BLIP_CHECK(backend.aborts == 1);
    return true;
}

bool test_verified_image_activates_once() {
    FakeBackend backend{};
    UpdateService service{backend, "blip-v2", "esp32", "minimal"};
    const auto image = valid_image();
    BLIP_CHECK(service.begin(valid_manifest()).ok());
    for (std::size_t offset = 0; offset < image.size(); offset += 37U) {
        const std::size_t count = std::min<std::size_t>(37U, image.size() - offset);
        BLIP_CHECK(service.append(std::span{image}.subspan(offset, count)).ok());
    }
    BLIP_CHECK(service.finish().ok());
    BLIP_CHECK(backend.bytes.size() == image.size());
    BLIP_CHECK(backend.finishes == 1);
    BLIP_CHECK(backend.activations == 1);
    BLIP_CHECK(service.status().state == UpdateState::ready_to_reboot);
    BLIP_CHECK(service.status().signature_enforced);
    return true;
}

bool test_pending_image_confirms_only_explicitly() {
    FakeBackend backend{};
    backend.pending = true;
    UpdateService service{backend, "blip-v2", "esp32", "minimal"};
    service.refresh_boot_state();
    BLIP_CHECK(service.status().state == UpdateState::pending_confirmation);
    BLIP_CHECK(backend.confirmations == 0);
    BLIP_CHECK(service.confirm_boot().ok());
    BLIP_CHECK(backend.confirmations == 1);
    BLIP_CHECK(service.status().state == UpdateState::confirmed);
    BLIP_CHECK(service.confirm_boot().ok());
    BLIP_CHECK(backend.confirmations == 1);
    return true;
}

bool test_failed_boot_rejects_pending_image() {
    FakeBackend backend{};
    backend.pending = true;
    UpdateService service{backend, "blip-v2", "esp32", "minimal"};
    service.refresh_boot_state();
    BLIP_CHECK(service.reject_boot().ok());
    BLIP_CHECK(backend.rollbacks == 1);
    BLIP_CHECK(service.status().state == UpdateState::failed);
    BLIP_CHECK(service.reject_boot().ok());
    BLIP_CHECK(backend.rollbacks == 1);
    return true;
}

} // namespace

int main() {
    const TestCase tests[]{
        {"sha parser", test_sha_parser},
        {"manifest checked before erase", test_manifest_is_checked_before_erasing},
        {"interrupted transfer", test_interrupted_transfer_never_activates},
        {"exact image and descriptor", test_exact_image_and_descriptor_are_required},
        {"hash mismatch", test_hash_mismatch_never_finishes_backend},
        {"verified activation", test_verified_image_activates_once},
        {"pending confirmation", test_pending_image_confirms_only_explicitly},
        {"failed boot rollback", test_failed_boot_rejects_pending_image},
    };
    return run_tests(tests);
}
