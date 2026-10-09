#pragma once
#include "blip/ota/release_image.hpp"

// Public build identity; no MAC/network credentials enter the release query.
[[nodiscard]] blip::ota::ReleaseIdentity firmware_release_identity(std::uint32_t web_code) noexcept;
