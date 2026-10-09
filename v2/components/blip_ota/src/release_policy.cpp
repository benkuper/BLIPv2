#include "blip/ota/release_policy.hpp"
#include <algorithm>

namespace blip::ota {
ReleasePolicy default_release_policy() noexcept {
    ReleasePolicy output;
    static_cast<void>(output.endpoint.assign(kDefaultReleaseEndpoint));
    static_cast<void>(output.channel.assign("stable"));
    return output;
}
bool valid_release_policy(const ReleasePolicy& policy) noexcept {
    const auto channel = policy.channel.view();
    return policy.endpoint.length <= 319 && valid_release_url(policy.endpoint.view()) &&
        !channel.empty() && channel.size() <= 15 && policy.interval_hours <= 168 &&
        std::all_of(channel.begin(), channel.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        });
}
bool encode_release_policy(const ReleasePolicy& policy, std::span<std::byte> bytes) noexcept {
    if (bytes.size() != kReleasePolicyBytes || !valid_release_policy(policy)) return false;
    std::fill(bytes.begin(), bytes.end(), std::byte{});
    bytes[0] = std::byte{'B'}; bytes[1] = std::byte{'U'}; bytes[2] = std::byte{'P'}; bytes[3] = std::byte{'1'};
    bytes[4] = static_cast<std::byte>(policy.interval_hours);
    bytes[5] = static_cast<std::byte>((policy.automatic_firmware ? 1 : 0) | (policy.automatic_web ? 2 : 0));
    for (std::size_t index = 0; index < policy.endpoint.length; ++index) bytes[12 + index] = static_cast<std::byte>(policy.endpoint.bytes[index]);
    for (std::size_t index = 0; index < policy.channel.length; ++index) bytes[332 + index] = static_cast<std::byte>(policy.channel.bytes[index]);
    return true;
}
bool decode_release_policy(std::span<const std::byte> bytes, ReleasePolicy& output) noexcept {
    output = {};
    if (bytes.size() != kReleasePolicyBytes || bytes[0] != std::byte{'B'} || bytes[1] != std::byte{'U'} ||
        bytes[2] != std::byte{'P'} || bytes[3] != std::byte{'1'} || std::to_integer<unsigned>(bytes[5]) > 3 ||
        !std::all_of(bytes.begin() + 6, bytes.begin() + 12, [](std::byte b) { return b == std::byte{}; })) return false;
    ReleasePolicy policy;
    const auto field = [&](std::size_t offset, std::size_t capacity, auto& target) {
        const auto view = bytes.subspan(offset, capacity);
        const auto zero = std::find(view.begin(), view.end(), std::byte{});
        if (zero == view.end() || !std::all_of(zero, view.end(), [](std::byte b) { return b == std::byte{}; })) return false;
        return target.assign({reinterpret_cast<const char*>(view.data()), static_cast<std::size_t>(zero - view.begin())});
    };
    if (!field(12, 320, policy.endpoint) || !field(332, 16, policy.channel)) return false;
    policy.interval_hours = std::to_integer<unsigned>(bytes[4]);
    const auto flags = std::to_integer<unsigned>(bytes[5]);
    policy.automatic_firmware = flags & 1; policy.automatic_web = flags & 2;
    if (!valid_release_policy(policy)) return false;
    output = policy; return true;
}
} // namespace blip::ota
