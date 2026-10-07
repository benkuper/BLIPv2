#include "blip/transport/serial_protocol.hpp"
#include "hal/uart_types.h"
#include <array>
#include <cstdio>
#include <cstring>

namespace {
struct Trace {
    std::uint32_t sequence{}, request{}, frame_crc{};
    std::uint16_t bytes{};
    int written{};
    bool valid{};
};
std::array<Trace, 32> records{};
std::uint32_t sequence{};
std::uint32_t checksum(std::span<const std::byte> bytes) {
    std::uint32_t value = UINT32_MAX;
    for (const auto byte : bytes) {
        value ^= std::to_integer<std::uint8_t>(byte);
        for (unsigned i = 0; i < 8; ++i) value = (value >> 1) ^ (0xedb88320U & (0U - (value & 1U)));
    }
    return ~value;
}
}
extern "C" int __real_uart_write_bytes(uart_port_t, const void*, std::size_t);
extern "C" int __wrap_uart_write_bytes(uart_port_t port, const void* source, std::size_t length) {
    const auto* bytes = static_cast<const std::byte*>(source);
    // Only the serial transport submits a leading/trailing-delimited whole frame.
    // Console fragments continue through the original driver unchanged.
    if (port != UART_NUM_0 || !source || length < 3 || bytes[0] != std::byte{0} ||
        bytes[length - 1] != std::byte{0}) return __real_uart_write_bytes(port, source, length);
    const auto encoded = std::span<const std::byte>{bytes + 1, length - 2};
    std::array<std::byte, blip::transport::kMaxEnvelopeBytes> decoded{};
    const auto decoded_size = blip::transport::cobs_decode_frame(encoded, decoded);
    auto& slot = records[++sequence % records.size()];
    slot = {};
    slot.sequence = sequence;
    slot.bytes = static_cast<std::uint16_t>(encoded.size());
    slot.frame_crc = checksum(encoded);
    bool dump{};
    if (decoded_size) {
        const auto envelope = blip::transport::decode_envelope(std::span<const std::byte>(decoded).first(decoded_size.value()));
        slot.valid = envelope.ok();
        if (envelope) {
            slot.request = envelope.value().request_id;
            const auto control = blip::transport::decode_control_message(envelope.value().payload);
            dump = control && control.value().component_id == "blip.bootstrap" && control.value().control_id == "probe_value";
        }
    }
    const int written = __real_uart_write_bytes(port, source, length);
    slot.written = written;
    // A normal bootstrap probe requests the buffered trace AFTER the burst.
    // Nothing is logged while collecting the earlier replies.
    if (dump) {
        std::array<char, 220> line{};
        const auto first = sequence > records.size() ? sequence - records.size() + 1 : 1;
        for (std::uint32_t index = first; index <= sequence; ++index) {
            const auto& record = records[index % records.size()];
            const int size = std::snprintf(line.data(), line.size(),
                "\nUARTTRACE {\"sequence\":%u,\"request_id\":%u,\"encoded_bytes\":%u,\"frame_crc32\":%u,\"pre_driver_valid\":%s,\"written\":%d}\n",
                static_cast<unsigned>(record.sequence), static_cast<unsigned>(record.request),
                static_cast<unsigned>(record.bytes), static_cast<unsigned>(record.frame_crc), record.valid ? "true" : "false", record.written);
            if (size > 0 && static_cast<std::size_t>(size) < line.size()) __real_uart_write_bytes(port, line.data(), size);
        }
        constexpr char finished[] = "UARTTRACE_END\n";
        __real_uart_write_bytes(port, finished, sizeof(finished) - 1);
    }
    return written;
}
