#include "blip/transport/serial_protocol.hpp"

#include <algorithm>
#include <limits>

namespace blip::transport {
namespace {

[[nodiscard]] core::Error serial_error(core::ErrorCode code, std::string_view operation,
                                       std::string_view detail) noexcept {
    return {core::ErrorDomain::transport, code, "blip.transport.serial", operation, detail};
}

void saturating_increment(std::uint32_t& value) noexcept {
    if (value != std::numeric_limits<std::uint32_t>::max()) {
        ++value;
    }
}

[[nodiscard]] std::string_view wire_safe_detail(std::string_view detail) noexcept {
    if (detail.size() <= kMaxControlDetailBytes &&
        std::all_of(detail.begin(), detail.end(), [](char character) {
            const auto byte = static_cast<std::uint8_t>(character);
            return byte >= 0x20U && byte <= 0x7eU;
        })) {
        return detail;
    }
    return "control-error";
}

} // namespace

core::Result<std::size_t> cobs_encode_frame(std::span<const std::byte> input,
                                            std::span<std::byte> output) noexcept {
    if (input.empty() || input.size() > kMaxEnvelopeBytes || output.size() < input.size() + 2U) {
        return core::Result<std::size_t>::failure(
            serial_error(output.size() < input.size() + 2U ? core::ErrorCode::serialization_overflow
                                                           : core::ErrorCode::invalid_argument,
                         "cobs-encode", "invalid-input-or-output"));
    }
    std::size_t read_index{};
    std::size_t write_index{1};
    std::size_t code_index{};
    std::uint8_t code{1};
    while (read_index < input.size()) {
        if (input[read_index] == std::byte{0}) {
            output[code_index] = static_cast<std::byte>(code);
            code_index = write_index++;
            code = 1;
            ++read_index;
        } else {
            if (write_index >= output.size() - 1U) {
                return core::Result<std::size_t>::failure(serial_error(
                    core::ErrorCode::serialization_overflow, "cobs-encode", "output-too-small"));
            }
            output[write_index++] = input[read_index++];
            ++code;
            if (code == 0xffU) {
                output[code_index] = static_cast<std::byte>(code);
                code_index = write_index++;
                code = 1;
            }
        }
    }
    output[code_index] = static_cast<std::byte>(code);
    output[write_index++] = std::byte{0};
    return core::Result<std::size_t>::success(write_index);
}

core::Result<std::size_t> cobs_decode_frame(std::span<const std::byte> input,
                                            std::span<std::byte> output) noexcept {
    if (input.empty() || input.size() > kMaxSerialFrameBytes) {
        return core::Result<std::size_t>::failure(
            serial_error(core::ErrorCode::invalid_argument, "cobs-decode", "invalid-input"));
    }
    std::size_t read_index{};
    std::size_t write_index{};
    while (read_index < input.size()) {
        const std::uint8_t code = std::to_integer<std::uint8_t>(input[read_index++]);
        if (code == 0U || static_cast<std::size_t>(code - 1U) > input.size() - read_index) {
            return core::Result<std::size_t>::failure(
                serial_error(core::ErrorCode::corrupt_data, "cobs-decode", "invalid-code"));
        }
        for (std::uint8_t index = 1; index < code; ++index) {
            if (write_index == output.size()) {
                return core::Result<std::size_t>::failure(serial_error(
                    core::ErrorCode::serialization_overflow, "cobs-decode", "output-too-small"));
            }
            output[write_index++] = input[read_index++];
        }
        if (code != 0xffU && read_index < input.size()) {
            if (write_index == output.size()) {
                return core::Result<std::size_t>::failure(serial_error(
                    core::ErrorCode::serialization_overflow, "cobs-decode", "output-too-small"));
            }
            output[write_index++] = std::byte{0};
        }
    }
    return core::Result<std::size_t>::success(write_index);
}

core::Result<std::size_t>
SerialControlEndpoint::handle_frame(std::span<const std::byte> encoded_frame,
                                    std::span<std::byte> output_frame) noexcept {
    const auto decoded_size = cobs_decode_frame(encoded_frame, decode_buffer_);
    if (!decoded_size) {
        saturating_increment(metrics_.rejected);
        return core::Result<std::size_t>::failure(decoded_size.error());
    }
    const auto envelope =
        decode_envelope(decode_buffer_.first(static_cast<std::size_t>(decoded_size.value())));
    if (!envelope || envelope.value().kind != EnvelopeKind::request ||
        envelope.value().payload_type != PayloadType::control) {
        saturating_increment(metrics_.rejected);
        return core::Result<std::size_t>::failure(
            envelope ? serial_error(core::ErrorCode::invalid_argument, "handle-frame",
                                    "request-control-required")
                     : envelope.error());
    }
    const auto message = decode_control_message(envelope.value().payload);
    if (!message || message.value().error_code != core::ErrorCode::none ||
        !message.value().detail.empty()) {
        saturating_increment(metrics_.rejected);
        return core::Result<std::size_t>::failure(
            message ? serial_error(core::ErrorCode::invalid_argument, "handle-frame",
                                   "request-status-must-be-empty")
                    : message.error());
    }

    const core::ControlRequest request{
        message.value().operation,
        message.value().component_id,
        message.value().control_id,
        {message.value().values.data(), message.value().value_count}};
    core::ControlResponse response{};
    const auto status = controls_->execute(request, response);
    const core::Error error = status ? core::Error{} : status.error();
    const ControlMessage response_message{
        request.operation,
        error.domain,
        error.code,
        request.component_id,
        request.control_id,
        wire_safe_detail(error.detail),
        {response.values.data(), status ? response.value_count : 0U},
    };
    const auto payload_size = encode_control_message(response_message, payload_buffer_);
    if (!payload_size) {
        saturating_increment(metrics_.rejected);
        return core::Result<std::size_t>::failure(payload_size.error());
    }
    const auto response_kind = status ? EnvelopeKind::response : EnvelopeKind::error;
    const auto envelope_size =
        encode_envelope({response_kind, PayloadType::control, envelope.value().request_id, 0,
                         payload_buffer_.first(payload_size.value())},
                        envelope_buffer_);
    if (!envelope_size) {
        saturating_increment(metrics_.rejected);
        return core::Result<std::size_t>::failure(envelope_size.error());
    }
    const auto frame_size =
        cobs_encode_frame(envelope_buffer_.first(envelope_size.value()), output_frame);
    if (!frame_size) {
        saturating_increment(metrics_.rejected);
        return frame_size;
    }
    saturating_increment(metrics_.accepted);
    if (!status) {
        saturating_increment(metrics_.error_responses);
    }
    return frame_size;
}

} // namespace blip::transport
