#pragma once

#include "blip/core/error.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace blip::oscquery {

inline constexpr std::size_t kMaxOscPacketBytes = 1024;
inline constexpr std::size_t kMaxOscAddressBytes = 128;
inline constexpr std::size_t kMaxOscStringBytes = 256;
inline constexpr std::size_t kMaxOscControlArguments = 8;
inline constexpr std::size_t kMaxOscArguments = kMaxOscControlArguments + 1U;

enum class OscValueType : std::uint8_t {
    boolean,
    int32,
    float32,
    string,
    rgba,
    midi,
    timetag,
};

struct OscValue {
    OscValueType type{OscValueType::int32};
    bool boolean{};
    std::int32_t integer{};
    float number{};
    std::string_view string{};
    std::uint32_t word{};
    std::uint64_t timetag{};

    [[nodiscard]] static constexpr OscValue from_bool(bool value) noexcept {
        OscValue result{};
        result.type = OscValueType::boolean;
        result.boolean = value;
        return result;
    }

    [[nodiscard]] static constexpr OscValue from_integer(std::int32_t value) noexcept {
        OscValue result{};
        result.type = OscValueType::int32;
        result.integer = value;
        return result;
    }

    [[nodiscard]] static constexpr OscValue from_number(float value) noexcept {
        OscValue result{};
        result.type = OscValueType::float32;
        result.number = value;
        return result;
    }

    [[nodiscard]] static constexpr OscValue from_string(std::string_view value) noexcept {
        OscValue result{};
        result.type = OscValueType::string;
        result.string = value;
        return result;
    }
};

struct OscMessage {
    std::string_view address{};
    std::array<OscValue, kMaxOscArguments> arguments{};
    std::size_t argument_count{};
};

[[nodiscard]] core::Result<OscMessage>
decode_osc_message(std::span<const std::byte> packet) noexcept;

[[nodiscard]] core::Result<std::size_t> encode_osc_message(const OscMessage& message,
                                                           std::span<std::byte> output) noexcept;

} // namespace blip::oscquery
