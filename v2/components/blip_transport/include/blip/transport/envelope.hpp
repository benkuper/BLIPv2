#pragma once

#include "blip/core/control.hpp"
#include "blip/core/error.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace blip::transport {

inline constexpr std::uint8_t kEnvelopeFormatVersion = 1;
inline constexpr std::size_t kEnvelopeHeaderBytes = 24;
inline constexpr std::size_t kMaxEnvelopePayloadBytes = 512;
inline constexpr std::size_t kMaxEnvelopeBytes = kEnvelopeHeaderBytes + kMaxEnvelopePayloadBytes;
inline constexpr std::uint8_t kControlPayloadFormatVersion = 1;
inline constexpr std::size_t kControlHeaderBytes = 12;
inline constexpr std::size_t kMaxControlComponentBytes = 63;
inline constexpr std::size_t kMaxControlIdBytes = 31;
inline constexpr std::size_t kMaxControlDetailBytes = 63;
inline constexpr std::size_t kMaxControlStringBytes = 128;
inline constexpr std::size_t kMaxControlPayloadBytes = kMaxEnvelopePayloadBytes;

enum class EnvelopeKind : std::uint8_t { request = 1, response = 2, event = 3, error = 4 };
enum class PayloadType : std::uint16_t {
    control = 1,
    descriptor = 2,
    diagnostics = 3,
    provisioning = 4
};

struct EnvelopeInput {
    EnvelopeKind kind{EnvelopeKind::request};
    PayloadType payload_type{PayloadType::control};
    std::uint32_t request_id{};
    std::uint8_t flags{};
    std::span<const std::byte> payload{};
};

struct EnvelopeView {
    EnvelopeKind kind{EnvelopeKind::request};
    PayloadType payload_type{PayloadType::control};
    std::uint32_t request_id{};
    std::uint8_t flags{};
    std::span<const std::byte> payload{};
};

[[nodiscard]] core::Result<std::size_t> encode_envelope(const EnvelopeInput& input,
                                                        std::span<std::byte> output) noexcept;
[[nodiscard]] core::Result<EnvelopeView> decode_envelope(std::span<const std::byte> input) noexcept;

struct ControlMessage {
    core::ControlOperation operation{core::ControlOperation::read_parameter};
    core::ErrorDomain error_domain{core::ErrorDomain::none};
    core::ErrorCode error_code{core::ErrorCode::none};
    std::string_view component_id{};
    std::string_view control_id{};
    std::string_view detail{};
    std::span<const core::ScalarValue> values{};
};

struct DecodedControlMessage {
    core::ControlOperation operation{core::ControlOperation::read_parameter};
    core::ErrorDomain error_domain{core::ErrorDomain::none};
    core::ErrorCode error_code{core::ErrorCode::none};
    std::string_view component_id{};
    std::string_view control_id{};
    std::string_view detail{};
    std::array<core::ScalarValue, core::kMaxControlValues> values{};
    std::size_t value_count{};
};

[[nodiscard]] core::Result<std::size_t>
encode_control_message(const ControlMessage& message, std::span<std::byte> output) noexcept;
[[nodiscard]] core::Result<DecodedControlMessage>
decode_control_message(std::span<const std::byte> input) noexcept;

} // namespace blip::transport
