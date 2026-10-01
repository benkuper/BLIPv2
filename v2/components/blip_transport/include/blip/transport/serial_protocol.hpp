#pragma once

#include "blip/core/control.hpp"
#include "blip/transport/envelope.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace blip::transport {

inline constexpr std::size_t kMaxSerialFrameBytes = kMaxEnvelopeBytes + 4U;

[[nodiscard]] core::Result<std::size_t> cobs_encode_frame(std::span<const std::byte> input,
                                                          std::span<std::byte> output) noexcept;
[[nodiscard]] core::Result<std::size_t> cobs_decode_frame(std::span<const std::byte> input,
                                                          std::span<std::byte> output) noexcept;

struct SerialEndpointMetrics {
    std::uint32_t accepted{};
    std::uint32_t rejected{};
    std::uint32_t error_responses{};
};

class ControlEnvelopeEndpoint {
  public:
    ControlEnvelopeEndpoint(core::ControlService& controls,
                            std::span<std::byte> payload_buffer) noexcept
        : controls_(&controls), payload_buffer_(payload_buffer) {}

    [[nodiscard]] core::Result<std::size_t>
    handle_request(std::span<const std::byte> request,
                   std::span<std::byte> response) noexcept;

  private:
    core::ControlService* controls_{};
    std::span<std::byte> payload_buffer_{};
};

class SerialControlEndpoint {
  public:
    SerialControlEndpoint(core::ControlService& controls, std::span<std::byte> decode_buffer,
                          std::span<std::byte> envelope_buffer,
                          std::span<std::byte> payload_buffer) noexcept
        : endpoint_(controls, payload_buffer), decode_buffer_(decode_buffer),
          envelope_buffer_(envelope_buffer) {}

    [[nodiscard]] core::Result<std::size_t>
    handle_frame(std::span<const std::byte> encoded_frame,
                 std::span<std::byte> output_frame) noexcept;
    [[nodiscard]] const SerialEndpointMetrics& metrics() const noexcept { return metrics_; }

  private:
    ControlEnvelopeEndpoint endpoint_;
    std::span<std::byte> decode_buffer_{};
    std::span<std::byte> envelope_buffer_{};
    SerialEndpointMetrics metrics_{};
};

} // namespace blip::transport
