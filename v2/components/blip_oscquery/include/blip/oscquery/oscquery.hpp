#pragma once

#include "blip/core/control.hpp"
#include "blip/core/registry.hpp"
#include "blip/oscquery/legacy_osc.hpp"

#include <string_view>

namespace blip::oscquery {

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
