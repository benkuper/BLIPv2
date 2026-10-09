#include "blip/ota/release_image.hpp"
#include "test_harness.hpp"
#include <cstring>
#include <charconv>
#include <fstream>
#include <iterator>
#include <string>

namespace {
using namespace blip::ota;
constexpr ReleaseIdentity identity{"blip-v2", "creators-ball-v2", "esp32c6", "ota-8mb-v1", "minimal", "stable",
    8388608, 123, 1, 2, 2000, "0.2.0-alpha"};
std::array<std::byte, kReleaseImagePrefixBytes> image() {
    std::array<std::byte, kReleaseImagePrefixBytes> bytes{};
    bytes[0] = std::byte{0xe9};
    const std::uint32_t magic = 0xabcd5432U;
    std::memcpy(bytes.data() + 32, &magic, sizeof(magic));
    std::memcpy(bytes.data() + 48, identity.firmware_version.data(), identity.firmware_version.size());
    std::memcpy(bytes.data() + 80, identity.project.data(), identity.project.size());
    const auto descriptor = make_firmware_release_descriptor(identity);
    std::memcpy(bytes.data() + kEspAppDescriptorEnd, &descriptor, sizeof(descriptor));
    return bytes;
}
ReleaseArtifact artifact() {
    ReleaseArtifact output; output.present = true; output.code = 2;
    if (!output.version.assign(identity.firmware_version)) std::abort();
    return output;
}
bool compatible_binary_and_all_identity_fields() {
    const auto bytes = image(); const auto release = artifact();
    BLIP_CHECK(validate_firmware_release_prefix(bytes, identity, release));
    for (std::size_t offset : {0U, 32U, 48U, 80U, 288U, 296U, 300U, 304U, 308U, 312U,
                              316U, 348U, 412U, 428U, 460U}) {
        auto corrupt = bytes; corrupt[offset] ^= std::byte{1};
        BLIP_CHECK(!validate_firmware_release_prefix(corrupt, identity, release));
    }
    auto wrong = identity; wrong.board = "another-c6";
    BLIP_CHECK(!validate_firmware_release_prefix(bytes, wrong, release));
    auto old = release; old.code = 1;
    BLIP_CHECK(!validate_firmware_release_prefix(bytes, identity, old));
    return true;
}
bool bounded_prefix_and_canonical_text() {
    const auto bytes = image(); const auto release = artifact();
    BLIP_CHECK(!validate_firmware_release_prefix(std::span<const std::byte>(bytes).first(bytes.size() - 1), identity, release));
    auto corrupt = bytes; corrupt[347] = std::byte{'x'};
    BLIP_CHECK(!validate_firmware_release_prefix(corrupt, identity, release));
    auto absent = release; absent.present = false;
    BLIP_CHECK(!validate_firmware_release_prefix(bytes, identity, absent));
    auto wrong = identity; wrong.target = "";
    BLIP_CHECK(!validate_firmware_release_prefix(bytes, wrong, release));
    return true;
}
} // namespace
int main(int argc, char** argv) {
    if (argc == 10) {
        std::ifstream stream(argv[1], std::ios::binary);
        if (!stream) return 2;
        std::array<std::byte, kReleaseImagePrefixBytes> prefix{};
        stream.read(reinterpret_cast<char*>(prefix.data()), prefix.size());
        if (stream.gcount() != static_cast<std::streamsize>(prefix.size())) return 2;
        const auto number = [](std::string_view source, std::uint32_t& output) {
            const auto parsed = std::from_chars(source.data(), source.data() + source.size(), output);
            return parsed.ec == std::errc{} && parsed.ptr == source.data() + source.size();
        };
        auto expected = identity;
        expected.board = argv[2]; expected.target = argv[3]; expected.layout = argv[4]; expected.profile = argv[5];
        if (!number(argv[6], expected.flash_bytes) || !number(argv[7], expected.features) || !number(argv[8], expected.firmware_code)) return 2;
        auto candidate = artifact(); candidate.code = expected.firmware_code;
        if (!candidate.version.assign(argv[9])) return 2;
        return validate_firmware_release_prefix(prefix, expected, candidate) ? 0 : 1;
    }
    const TestCase cases[]{{"embedded binary identity", compatible_binary_and_all_identity_fields},
                           {"bounded prefix and canonical strings", bounded_prefix_and_canonical_text}};
    return run_tests(cases);
}
