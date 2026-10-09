#include "blip/storage/sd_spi_card.hpp"
#include <array>
#include <limits>

namespace blip::storage {
namespace {
std::uint16_t crc16(std::uint16_t crc, std::uint8_t byte) noexcept {
    crc ^= std::uint16_t(byte) << 8U;
    for (unsigned bit = 0; bit < 8; ++bit)
        crc = std::uint16_t((crc << 1U) ^ ((crc & 0x8000U) ? 0x1021U : 0U));
    return crc;
}
std::uint8_t crc7(std::span<const std::uint8_t> bytes) noexcept {
    std::uint8_t crc{};
    for (auto byte : bytes) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc <<= 1U;
            if ((byte ^ crc) & 0x80U) crc ^= 0x09U;
            byte <<= 1U;
        }
    }
    return std::uint8_t((crc << 1U) | 1U);
}
} // namespace

void SdSpiCard::release() noexcept {
    bus_->select(false);
    static_cast<void>(bus_->exchange(0xff));
}

bool SdSpiCard::ready(unsigned timeout_ms) noexcept {
    const auto deadline = bus_->milliseconds() + timeout_ms;
    unsigned polls{};
    do {
        if (bus_->exchange(0xff) == 0xff) return true;
        if (++polls % 64 == 0) bus_->yield();
    } while (bus_->milliseconds() < deadline);
    error_ = "card-busy-timeout";
    return false;
}

std::uint8_t SdSpiCard::command(unsigned number, std::uint32_t argument) noexcept {
    release();
    bus_->select(true);
    if (!ready(500)) return 0xff;
    const std::array<std::uint8_t, 5> packet{
        std::uint8_t(0x40U | number), std::uint8_t(argument >> 24U),
        std::uint8_t(argument >> 16U), std::uint8_t(argument >> 8U), std::uint8_t(argument)};
    for (const auto byte : packet) static_cast<void>(bus_->exchange(byte));
    static_cast<void>(bus_->exchange(crc7(packet)));
    for (unsigned poll = 0; poll < 16; ++poll) {
        const auto response = bus_->exchange(0xff);
        if (!(response & 0x80U)) return response;
    }
    error_ = "card-command-timeout";
    return 0xff;
}

bool SdSpiCard::data(std::span<std::byte> bytes) noexcept {
    const auto deadline = bus_->milliseconds() + 500;
    std::uint8_t token{};
    unsigned polls{};
    do {
        token = bus_->exchange(0xff);
        if (token != 0xff) break;
        if (++polls % 64 == 0) bus_->yield();
    } while (bus_->milliseconds() < deadline);
    if (token != 0xfe) { error_ = "card-data-token"; return false; }
    std::uint16_t crc{};
    for (auto& byte : bytes) {
        const auto value = bus_->exchange(0xff);
        byte = std::byte(value);
        crc = crc16(crc, value);
    }
    const auto high = bus_->exchange(0xff);
    const auto low = bus_->exchange(0xff);
    if (crc != std::uint16_t((std::uint16_t(high) << 8U) | low)) {
        error_ = "card-data-crc"; return false;
    }
    return true;
}

bool SdSpiCard::initialize() noexcept {
    sectors_ = 0;
    block_addressed_ = false;
    error_ = "card-not-found";
    bus_->initializing(true);
    bus_->select(false);
    for (unsigned i = 0; i < 10; ++i) static_cast<void>(bus_->exchange(0xff));
    if (command(0, 0) != 1) { release(); return false; }
    const auto response = command(8, 0x1aa);
    const bool version2 = response == 1;
    if (version2) {
        std::array<std::uint8_t, 4> echo{};
        for (auto& byte : echo) byte = bus_->exchange(0xff);
        if (echo[2] != 1 || echo[3] != 0xaa) { release(); return false; }
    } else if (response != 5) { release(); return false; }
    const auto deadline = bus_->milliseconds() + 2000;
    bool initialized{};
    do {
        if (command(55, 0) > 1) break;
        const auto status = command(41, version2 ? 0x40000000U : 0U);
        if (status == 0) { initialized = true; break; }
        if (status != 1) break;
        bus_->yield();
    } while (bus_->milliseconds() < deadline);
    if (!initialized) { error_ = "card-initialize-timeout"; release(); return false; }
    if (command(58, 0) != 0) { release(); return false; }
    const auto ocr = bus_->exchange(0xff);
    for (unsigned i = 0; i < 3; ++i) static_cast<void>(bus_->exchange(0xff));
    if (!(ocr & 0x80U)) { release(); return false; }
    block_addressed_ = version2 && (ocr & 0x40U);
    if (!block_addressed_ && command(16, 512) != 0) { release(); return false; }
    if (command(9, 0) != 0) { release(); return false; }
    std::array<std::byte, 16> csd{};
    if (!data(csd)) { release(); return false; }
    release();
    const auto byte = [&](std::size_t i) { return std::to_integer<std::uint32_t>(csd[i]); };
    std::uint64_t capacity{};
    if ((byte(0) >> 6U) == 1) {
        const auto size = ((byte(7) & 0x3fU) << 16U) | (byte(8) << 8U) | byte(9);
        capacity = (std::uint64_t(size) + 1U) * 1024U;
    } else if ((byte(0) >> 6U) == 0) {
        const auto size = ((byte(6) & 3U) << 10U) | (byte(7) << 2U) | (byte(8) >> 6U);
        const auto multiplier = ((byte(9) & 3U) << 1U) | (byte(10) >> 7U);
        const auto block_length = byte(5) & 15U;
        if (block_length < 9 || block_length > 11) return false;
        capacity = (std::uint64_t(size) + 1U) << (multiplier + 2U + block_length - 9U);
    }
    if (!capacity || capacity > std::numeric_limits<std::uint32_t>::max()) {
        error_ = "card-capacity"; return false;
    }
    sectors_ = static_cast<std::uint32_t>(capacity);
    bus_->initializing(false);
    error_ = "";
    return true;
}

bool SdSpiCard::read(std::uint32_t sector, std::span<std::byte> bytes) noexcept {
    if (!sectors_ || bytes.empty() || bytes.size() % 512 || sector >= sectors_ ||
        bytes.size() / 512 > sectors_ - sector) { error_ = "card-read-range"; return false; }
    while (!bytes.empty()) {
        if (command(17, block_addressed_ ? sector : sector * 512U) != 0 || !data(bytes.first(512))) {
            release(); return false;
        }
        release();
        bytes = bytes.subspan(512);
        ++sector;
        bus_->yield();
    }
    return true;
}

bool SdSpiCard::write(std::uint32_t sector, std::span<const std::byte> bytes) noexcept {
    if (!sectors_ || bytes.empty() || bytes.size() % 512 || sector >= sectors_ ||
        bytes.size() / 512 > sectors_ - sector) { error_ = "card-write-range"; return false; }
    while (!bytes.empty()) {
        if (command(24, block_addressed_ ? sector : sector * 512U) != 0) { release(); return false; }
        static_cast<void>(bus_->exchange(0xff));
        static_cast<void>(bus_->exchange(0xfe));
        std::uint16_t crc{};
        for (const auto byte : bytes.first(512)) {
            const auto value = std::to_integer<std::uint8_t>(byte);
            static_cast<void>(bus_->exchange(value));
            crc = crc16(crc, value);
        }
        static_cast<void>(bus_->exchange(std::uint8_t(crc >> 8U)));
        static_cast<void>(bus_->exchange(std::uint8_t(crc)));
        const auto response = bus_->exchange(0xff);
        if ((response & 0x1fU) != 5 || !ready(2000)) {
            error_ = "card-write-rejected-or-timeout"; release(); return false;
        }
        if (command(13, 0) != 0 || bus_->exchange(0xff) != 0) {
            error_ = "card-write-status"; release(); return false;
        }
        release();
        bytes = bytes.subspan(512);
        ++sector;
        bus_->yield();
    }
    return true;
}
} // namespace blip::storage
