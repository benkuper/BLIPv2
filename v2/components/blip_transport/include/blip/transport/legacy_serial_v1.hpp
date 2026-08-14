#pragma once

#include "blip/core/error.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace blip::transport {

inline constexpr std::size_t kMaxLegacySerialLineBytes = 256;
inline constexpr std::size_t kMaxLegacySerialValues = 4;

enum class LegacySerialValueType : std::uint8_t { boolean, integer, number, string };

struct LegacySerialValue {
    LegacySerialValueType type{LegacySerialValueType::string};
    bool boolean{};
    std::int64_t integer{};
    double number{};
    std::string_view text{};
};

struct LegacySerialRequest {
    bool discovery{};
    std::string_view component_path{};
    std::string_view command{};
    std::array<LegacySerialValue, kMaxLegacySerialValues> values{};
    std::size_t value_count{};
};

class LegacySerialV1Parser {
  public:
    [[nodiscard]] core::Result<LegacySerialRequest> parse(std::string_view line) const noexcept;
};

[[nodiscard]] core::Result<std::size_t> format_legacy_discovery(std::string_view device_id,
                                                                std::string_view device_type,
                                                                std::string_view device_name,
                                                                std::string_view version,
                                                                std::span<char> output) noexcept;

} // namespace blip::transport
