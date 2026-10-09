#include "blip/storage/sd_spi_card.hpp"
#include "test_harness.hpp"
#include <array>
#include <deque>
#include <vector>
#include <string_view>

namespace {
using blip::storage::SdSpiBus;
using blip::storage::SdSpiCard;

std::uint16_t checksum(std::span<const std::byte> bytes) {
    unsigned crc{};
    for (const auto byte : bytes) {
        crc ^= std::to_integer<unsigned>(byte) << 8U;
        for (unsigned bit = 0; bit < 8; ++bit) crc = (crc << 1U) ^ ((crc & 0x8000U) ? 0x1021U : 0U);
        crc &= 0xffffU;
    }
    return static_cast<std::uint16_t>(crc);
}

class CardModel final : public SdSpiBus {
  public:
    bool present{true}, version2{true}, corrupt_read{}, never_initialize{}, write_busy{};
    bool selected{}, idle{true}, initializing_mode{}, crc_commands_correct{true};
    std::uint64_t time{};
    std::array<std::byte, 512> sector{};
    std::vector<unsigned> commands;
    std::vector<std::uint32_t> addresses;
    unsigned written{};
    std::array<std::uint8_t, 6> packet{};
    unsigned packet_size{}, write_count{};
    bool accepting_write{}, write_token{}, busy{};
    std::deque<std::uint8_t> response;
    void block(std::span<const std::byte> bytes) {
        response.push_back(0xfe);
        for (const auto byte : bytes) response.push_back(std::to_integer<std::uint8_t>(byte));
        const auto crc = checksum(bytes);
        response.push_back(static_cast<std::uint8_t>(crc >> 8U));
        response.push_back(static_cast<std::uint8_t>(crc ^ (corrupt_read ? 1 : 0)));
    }
    std::uint8_t exchange(std::uint8_t value) noexcept override {
        ++time;
        if (!present || !selected) return 0xff;
        if (!response.empty()) {
            const auto result = response.front(); response.pop_front(); return result;
        }
        if (busy) return 0;
        if (accepting_write) {
            if (!write_token) { if (value == 0xfe) write_token = true; return 0xff; }
            if (write_count < 512) sector[write_count] = std::byte(value);
            if (++write_count == 514) {
                ++written; accepting_write = false; response.push_back(5);
                if (write_busy) busy = true;
            }
            return 0xff;
        }
        if (!packet_size && (value & 0xc0U) != 0x40U) return 0xff;
        packet[packet_size++] = value;
        if (packet_size != packet.size()) return 0xff;
        packet_size = 0;
        const auto number = packet[0] & 0x3fU;
        const auto argument = (std::uint32_t(packet[1]) << 24U) | (std::uint32_t(packet[2]) << 16U) |
                              (std::uint32_t(packet[3]) << 8U) | packet[4];
        commands.push_back(number);
        if (number == 0 && packet[5] != 0x95) crc_commands_correct = false;
        if (number == 8 && packet[5] != 0x87) crc_commands_correct = false;
        switch (number) {
        case 0: response = {1}; idle = true; break;
        case 8: response = version2 ? std::deque<std::uint8_t>{1, 0, 0, 1, 0xaa}
                                     : std::deque<std::uint8_t>{5}; break;
        case 55: response = {std::uint8_t(idle ? 1 : 0)}; break;
        case 41: if (!never_initialize) idle = false; response = {std::uint8_t(idle ? 1 : 0)}; break;
        case 58: response = {0, std::uint8_t(version2 ? 0xc0 : 0x80), 0xff, 0x80, 0}; break;
        case 9: {
            response = {0};
            std::array<std::byte, 16> csd{};
            if (version2) { csd[0] = std::byte{0x40}; csd[8] = std::byte{3}; csd[9] = std::byte{0xff}; }
            else { csd[5] = std::byte{9}; csd[7] = std::byte{0xff}; csd[8] = std::byte{0xc0}; }
            block(csd); break;
        }
        case 16: response = {std::uint8_t(argument == 512 ? 0 : 4)}; break;
        case 17: addresses.push_back(argument); response = {0}; block(sector); break;
        case 24: addresses.push_back(argument); response = {0}; accepting_write = true; write_token = false; write_count = 0; break;
        case 13: response = {0, 0}; break;
        default: response = {4}; break;
        }
        return 0xff;
    }
    void select(bool active) noexcept override {
        selected = active;
        if (!active) { response.clear(); packet_size = 0; }
    }
    std::uint64_t milliseconds() noexcept override { return time / 1000; }
    void yield() noexcept override { time += 10000; }
    void initializing(bool value) noexcept override { initializing_mode = value; }
};

bool initializes_sdhc_and_sdsc_and_transfers_sectors() {
    for (const bool version2 : {false, true}) {
        CardModel bus;
        bus.version2 = version2;
        SdSpiCard card(bus);
        BLIP_CHECK(card.initialize());
        BLIP_CHECK(bus.crc_commands_correct && !bus.initializing_mode);
        BLIP_CHECK(card.sectors() == (version2 ? 1048576U : 4096U));
        std::array<std::byte, 512> input{}, output{};
        for (std::size_t i = 0; i < input.size(); ++i) input[i] = std::byte(i % 251);
        BLIP_CHECK(card.write(7, input));
        BLIP_CHECK(card.read(7, output));
        BLIP_CHECK(input == output && bus.written == 1);
        BLIP_CHECK(bus.addresses.back() == (version2 ? 7U : 3584U));
        BLIP_CHECK(!card.read(card.sectors(), output));
        BLIP_CHECK(!card.write(card.sectors() - 1, std::span(input).first(511)));
        BLIP_CHECK(!bus.selected);
    }
    return true;
}
bool errors_are_bounded_and_crc_is_checked() {
    CardModel missing;
    missing.present = false;
    SdSpiCard absent(missing);
    BLIP_CHECK(!absent.initialize() && absent.sectors() == 0 && !missing.selected);
    CardModel waiting;
    waiting.never_initialize = true;
    SdSpiCard never_ready(waiting);
    BLIP_CHECK(!never_ready.initialize() && waiting.milliseconds() < 2600 && !waiting.selected);
    CardModel bad;
    SdSpiCard card(bad);
    BLIP_CHECK(card.initialize());
    bad.corrupt_read = true;
    std::array<std::byte, 512> output{};
    BLIP_CHECK(!card.read(0, output));
    BLIP_CHECK(std::string_view(card.error()) == "card-data-crc" && !bad.selected);
    bad.corrupt_read = false;
    bad.write_busy = true;
    const auto start = bad.milliseconds();
    BLIP_CHECK(!card.write(0, output));
    BLIP_CHECK(bad.milliseconds() - start < 2600 && !bad.selected);
    return true;
}
} // namespace

int main() {
    const TestCase tests[]{
        {"SDHC SDSC initialization and sector transfers", initializes_sdhc_and_sdsc_and_transfers_sectors},
        {"missing timeout and bad CRC are bounded", errors_are_bounded_and_crc_is_checked},
    };
    return run_tests(tests);
}
