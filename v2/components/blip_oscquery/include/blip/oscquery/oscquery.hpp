#pragma once

#include "blip/core/control.hpp"
#include "blip/core/registry.hpp"
#include "blip/oscquery/legacy_osc.hpp"

#include <cstdint>
#include <string_view>

namespace blip::oscquery {

enum class HttpGetSurface : std::uint8_t { oscquery, web_asset, asset_status, not_found };

[[nodiscard]] constexpr HttpGetSurface route_http_get(std::string_view path, bool has_query,
                                                      bool accepts_html) noexcept {
    if (has_query) {
        return path == "/" ? HttpGetSurface::oscquery : HttpGetSurface::not_found;
    }
    if (path == "/api/web-assets") {
        return HttpGetSurface::asset_status;
    }
    if (path == "/" && !accepts_html) {
        return HttpGetSurface::oscquery;
    }
    return path.starts_with('/') ? HttpGetSurface::web_asset : HttpGetSurface::not_found;
}

class TextSink {
  public:
    virtual ~TextSink() = default;
    [[nodiscard]] virtual bool write(std::string_view text) noexcept = 0;
};

[[nodiscard]] core::Status write_oscquery_host_info(const DeviceIdentity& identity,
                                                    TextSink& sink) noexcept;

[[nodiscard]] core::Status write_oscquery_tree(const core::RegistryView& registry,
                                               core::ControlService& controls, bool include_config,
                                               TextSink& sink) noexcept;

} // namespace blip::oscquery
