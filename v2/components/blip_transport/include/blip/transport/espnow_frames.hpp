#pragma once

#include "blip/transport/envelope.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace blip::transport {

// V2-only application framing. Every radio packet fits an ESP-NOW v1 payload,
// including when the peer's radio supports larger ESP-NOW v2 packets.
inline constexpr std::size_t kEspNowPacketBytes = 250;
inline constexpr std::size_t kEspNowHeaderBytes = 16;
inline constexpr std::size_t kEspNowFragmentBytes = kEspNowPacketBytes - kEspNowHeaderBytes;
inline constexpr std::size_t kEspNowMaxFragments =
    (kMaxEnvelopeBytes + kEspNowFragmentBytes - 1U) / kEspNowFragmentBytes;

enum class EspNowPacketKind : std::uint8_t { data = 1, acknowledgement = 2 };

struct EspNowPacketView {
    EspNowPacketKind kind{};
    std::uint32_t session{};
    std::uint32_t sequence{};
    std::uint16_t total_size{};
    std::uint8_t fragment_index{};
    std::uint8_t fragment_count{};
    std::span<const std::byte> payload{};
};

[[nodiscard]] bool decode_espnow_packet(std::span<const std::byte> packet,
                                       EspNowPacketView& view) noexcept;
[[nodiscard]] std::size_t encode_espnow_ack(std::uint32_t session, std::uint32_t sequence,
                                            std::span<std::byte> output) noexcept;

class EspNowTransmit {
  public:
    [[nodiscard]] bool begin(std::uint32_t session, std::uint32_t sequence,
                             std::span<const std::byte> envelope) noexcept;
    [[nodiscard]] std::size_t fragment(std::size_t index,
                                       std::span<std::byte> output) const noexcept;
    [[nodiscard]] bool acknowledge(const EspNowPacketView& packet) noexcept;
    [[nodiscard]] bool pending() const noexcept { return pending_; }
    [[nodiscard]] std::size_t fragment_count() const noexcept { return fragment_count_; }

  private:
    std::span<const std::byte> envelope_{};
    std::uint32_t session_{};
    std::uint32_t sequence_{};
    std::size_t fragment_count_{};
    bool pending_{};
};

enum class EspNowReceiveResult : std::uint8_t {
    rejected,
    partial,
    complete,
    duplicate,
};

class EspNowReassemble {
  public:
    [[nodiscard]] EspNowReceiveResult accept(const EspNowPacketView& packet) noexcept;
    [[nodiscard]] std::span<const std::byte> envelope() const noexcept {
        return {buffer_.data(), completed_ ? total_size_ : 0U};
    }
    void reset() noexcept;

  private:
    std::array<std::byte, kMaxEnvelopeBytes> buffer_{};
    std::uint32_t session_{};
    std::uint32_t sequence_{};
    std::uint16_t total_size_{};
    std::uint8_t fragment_count_{};
    std::uint8_t received_mask_{};
    bool active_{};
    bool completed_{};
};

} // namespace blip::transport
