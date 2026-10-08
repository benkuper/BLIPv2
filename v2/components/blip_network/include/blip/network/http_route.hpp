#pragma once
#include <string_view>

namespace blip::network {
// A started app delegate owns the home page on both station and setup AP.
// Keep the independent recovery form reachable even when app services run.
[[nodiscard]] constexpr bool use_setup_portal(bool has_delegate, std::string_view uri,
                                             bool websocket = false) noexcept {
    const auto path = uri.substr(0, uri.find('?'));
    return !has_delegate || (!websocket && path == "/setup");
}
} // namespace blip::network
