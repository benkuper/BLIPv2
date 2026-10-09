#pragma once
#include "blip/ota/release_catalog.hpp"
#include <array>
#include <cstddef>
#include <cstdint>

namespace blip::ota {
inline constexpr std::size_t kReleaseImageDescriptorBytes = 204;
inline constexpr std::size_t kReleaseImagePrefixBytes = kEspAppDescriptorEnd + kReleaseImageDescriptorBytes;

// Fixed little-endian on ESP32/S3/C6. Parse untrusted bytes explicitly, never
// cast a downloaded buffer to this native structure.
struct FirmwareReleaseDescriptor {
    std::array<char, 8> magic{'B', 'L', 'I', 'P', 'R', 'E', 'L', '1'};
    std::uint32_t schema{1}, code{}, flash_bytes{}, features{}, api{};
    std::array<char, 32> project{};
    std::array<char, 64> board{};
    std::array<char, 16> target{};
    std::array<char, 32> layout{}, profile{};
};
static_assert(sizeof(FirmwareReleaseDescriptor) == kReleaseImageDescriptorBytes);
static_assert(offsetof(FirmwareReleaseDescriptor, project) == 28);
static_assert(offsetof(FirmwareReleaseDescriptor, profile) == 172);

[[nodiscard]] constexpr FirmwareReleaseDescriptor make_firmware_release_descriptor(const ReleaseIdentity& identity) noexcept {
    FirmwareReleaseDescriptor output{};
    output.code = identity.firmware_code; output.flash_bytes = identity.flash_bytes;
    output.features = identity.features; output.api = identity.api;
    const auto copy = [](auto& destination, std::string_view source) {
        if (source.size() >= destination.size()) return;
        for (std::size_t index = 0; index < source.size(); ++index) destination[index] = source[index];
    };
    copy(output.project, identity.project); copy(output.board, identity.board);
    copy(output.target, identity.target); copy(output.layout, identity.layout); copy(output.profile, identity.profile);
    return output;
}
[[nodiscard]] core::Status validate_firmware_release_prefix(std::span<const std::byte> prefix,
    const ReleaseIdentity& identity, const ReleaseArtifact& artifact) noexcept;
} // namespace blip::ota
