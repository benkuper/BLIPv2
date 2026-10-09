#pragma once
#include "blip/ota/release_catalog.hpp"

namespace blip::ota {
constexpr std::size_t kReleasePolicyBytes = 348;
struct ReleasePolicy {
    ReleaseText<319> endpoint{};
    ReleaseText<15> channel{};
    std::uint32_t interval_hours{24};
    bool automatic_firmware{}, automatic_web{};
};
[[nodiscard]] ReleasePolicy default_release_policy() noexcept;
[[nodiscard]] bool valid_release_policy(const ReleasePolicy&) noexcept;
[[nodiscard]] bool encode_release_policy(const ReleasePolicy&, std::span<std::byte>) noexcept;
[[nodiscard]] bool decode_release_policy(std::span<const std::byte>, ReleasePolicy&) noexcept;
} // namespace blip::ota
