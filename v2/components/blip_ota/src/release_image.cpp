#include "blip/ota/release_image.hpp"
#include <algorithm>

namespace blip::ota {
namespace {
std::uint32_t word(std::span<const std::byte> bytes, std::size_t offset) noexcept {
    std::uint32_t output{};
    for (std::size_t index = 0; index < 4; ++index)
        output |= static_cast<std::uint32_t>(bytes[offset + index]) << (index * 8);
    return output;
}
bool text(std::span<const std::byte> bytes, std::size_t offset, std::size_t capacity, std::string_view expected) noexcept {
    if (expected.empty() || expected.size() >= capacity) return false;
    for (std::size_t index = 0; index < capacity; ++index) {
        const auto actual = std::to_integer<unsigned char>(bytes[offset + index]);
        if (index >= expected.size()) { if (actual != 0) return false; }
        else if (actual < 32 || actual >= 127 || actual != static_cast<unsigned char>(expected[index])) return false;
    }
    return true;
}
core::Status invalid(std::string_view detail) noexcept {
    return core::Status::failure({core::ErrorDomain::storage, core::ErrorCode::incompatible_version,
        "blip.updates", "image-prefix", detail});
}
core::Status validate_prefix(std::span<const std::byte> prefix,
    const ReleaseIdentity& identity, std::string_view version, std::uint32_t code) noexcept {
    const auto chip = identity.target == "esp32" ? 0U : identity.target == "esp32s3" ? 9U :
        identity.target == "esp32c6" ? 13U : 0xffffU;
    if (prefix.size() < kReleaseImagePrefixBytes || !code || chip == 0xffffU ||
        prefix[0] != std::byte{0xe9} || word(prefix, 32) != 0xabcd5432U ||
        (word(prefix, 12) & 0xffffU) != chip ||
        !text(prefix, 48, 32, version) || !text(prefix, 80, 32, identity.project))
        return invalid("app-descriptor");
    const auto descriptor = prefix.subspan(kEspAppDescriptorEnd, kReleaseImageDescriptorBytes);
    constexpr std::string_view magic = "BLIPREL1";
    for (std::size_t index = 0; index < magic.size(); ++index)
        if (descriptor[index] != static_cast<std::byte>(magic[index])) return invalid("release-marker");
    if (word(descriptor, 8) != 1 || word(descriptor, 12) != code ||
        word(descriptor, 16) != identity.flash_bytes || word(descriptor, 20) != identity.features ||
        word(descriptor, 24) != identity.api || !text(descriptor, 28, 32, identity.project) ||
        !text(descriptor, 60, 64, identity.board) || !text(descriptor, 124, 16, identity.target) ||
        !text(descriptor, 140, 32, identity.layout) || !text(descriptor, 172, 32, identity.profile))
        return invalid("embedded-release-identity");
    return core::Status::success();
}
} // namespace
core::Status validate_firmware_release_prefix(std::span<const std::byte> prefix,
    const ReleaseIdentity& identity, const ReleaseArtifact& artifact) noexcept {
    if (!artifact.present) return invalid("absent-artifact");
    return validate_prefix(prefix, identity, artifact.version.view(), artifact.code);
}
core::Status validate_manual_firmware_prefix(std::span<const std::byte> prefix,
    const ReleaseIdentity& identity, std::string_view version) noexcept {
    if (prefix.size() < kReleaseImagePrefixBytes) return invalid("short-release-prefix");
    return validate_prefix(prefix, identity, version, word(prefix, kEspAppDescriptorEnd + 12));
}
} // namespace blip::ota
