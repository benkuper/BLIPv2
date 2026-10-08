#pragma once
#include "blip/ota/update_service.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace blip::ota {
constexpr std::size_t kMaximumReleaseCatalogBytes = 4096;
constexpr std::string_view kDefaultReleaseEndpoint = "https://www.goldengeek.org/blip/update";

template <std::size_t Capacity> struct ReleaseText {
    std::array<char, Capacity + 1> bytes{};
    std::uint16_t length{};
    [[nodiscard]] std::string_view view() const noexcept { return {bytes.data(), length}; }
    [[nodiscard]] bool assign(std::string_view text) noexcept {
        if (text.size() > Capacity || text.find('\0') != text.npos) return false;
        std::copy(text.begin(), text.end(), bytes.begin());
        length = static_cast<std::uint16_t>(text.size()); bytes[length] = '\0'; return true;
    }
};

struct ReleaseIdentity {
    std::string_view project{}, board{}, target{}, layout{}, profile{}, channel{};
    std::uint32_t flash_bytes{}, features{}, api{};
    std::uint32_t firmware_code{}, web_code{};
    std::string_view firmware_version{};
};
struct ReleaseArtifact {
    bool present{};
    std::uint32_t code{}, bytes{}, minimum_other_code{};
    ReleaseText<31> version{};
    ReleaseText<319> url{};
    std::array<std::byte, kSha256Bytes> sha256{};
};
struct ReleaseCatalog {
    ReleaseText<31> project{}, profile{}, layout{};
    ReleaseText<63> board{};
    ReleaseText<15> target{}, channel{};
    std::uint32_t flash_bytes{}, features{}, api{};
    ReleaseArtifact firmware{}, web{};
};

[[nodiscard]] core::Status decode_release_catalog(std::string_view json, ReleaseCatalog& output) noexcept;
[[nodiscard]] core::Status validate_release_catalog(const ReleaseCatalog&, const ReleaseIdentity&,
                                                    std::uint32_t maximum_firmware_bytes,
                                                    std::uint32_t maximum_web_bytes) noexcept;
[[nodiscard]] core::Status build_release_query(std::string_view endpoint, const ReleaseIdentity&,
                                               std::span<char> output, std::size_t& written) noexcept;
[[nodiscard]] bool valid_release_https_url(std::string_view url) noexcept;
[[nodiscard]] bool firmware_update_available(const ReleaseCatalog& catalog, const ReleaseIdentity& identity) noexcept;
[[nodiscard]] bool web_update_available(const ReleaseCatalog& catalog, const ReleaseIdentity& identity) noexcept;
} // namespace blip::ota
