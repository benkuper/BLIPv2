#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

namespace blip::storage {
class SdSpiBus {
  public:
    virtual ~SdSpiBus() = default;
    virtual std::uint8_t exchange(std::uint8_t value) noexcept = 0;
    virtual void select(bool active) noexcept = 0;
    virtual std::uint64_t milliseconds() noexcept = 0;
    virtual void yield() noexcept = 0;
    virtual void initializing(bool) noexcept {}
};

// Bounded single-sector SPI mode. Used when a board's only hardware SPI host
// is already driving clocked LEDs on a different pin pair (Creators Ball C6).
class SdSpiCard {
  public:
    explicit SdSpiCard(SdSpiBus& bus) noexcept : bus_(&bus) {}
    [[nodiscard]] bool initialize() noexcept;
    [[nodiscard]] bool read(std::uint32_t sector, std::span<std::byte> bytes) noexcept;
    [[nodiscard]] bool write(std::uint32_t sector, std::span<const std::byte> bytes) noexcept;
    [[nodiscard]] std::uint32_t sectors() const noexcept { return sectors_; }
    [[nodiscard]] const char* error() const noexcept { return error_; }
  private:
    std::uint8_t command(unsigned command, std::uint32_t argument) noexcept;
    bool ready(unsigned timeout_ms) noexcept;
    bool data(std::span<std::byte> bytes) noexcept;
    void release() noexcept;
    SdSpiBus* bus_{};
    std::uint32_t sectors_{};
    bool block_addressed_{};
    const char* error_{"not-initialized"};
};
} // namespace blip::storage
