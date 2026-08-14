#pragma once

#include "blip/core/descriptor.hpp"
#include "blip/core/error.hpp"

#include <cstddef>
#include <span>

namespace blip::core {

[[nodiscard]] Result<std::size_t> write_descriptor_json(const ComponentDescriptor& descriptor,
                                                        std::span<char> output) noexcept;

} // namespace blip::core
