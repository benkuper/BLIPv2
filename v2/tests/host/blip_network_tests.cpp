#include "blip/network/provisioning_form.hpp"
#include "blip/network/wifi_config.hpp"
#include "blip/network/wifi_state_machine.hpp"
#include "blip/storage/imported_settings.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string_view>

namespace {

using namespace blip::core;
using namespace blip::network;
using namespace blip::storage;

#define BLIP_CHECK(expression)                                                                     \
    do {                                                                                           \
        if (!(expression)) {                                                                       \
            std::cerr << __func__ << ':' << __LINE__ << ": check failed: " #expression << '\n';    \
            return false;                                                                          \
        }                                                                                          \
    } while (false)

bool native_config_round_trip_and_corruption() {
    WifiConfig config{};
    config.mode = WifiMode::station_and_ap;
    BLIP_CHECK(config.ssid.assign("Studio WiFi"));
    BLIP_CHECK(config.password.assign("correct horse battery staple"));
    BLIP_CHECK(config.manual_ip.assign("192.168.10.42"));
    BLIP_CHECK(config.manual_gateway.assign("192.168.10.1"));
    config.channel_scan = false;
    config.tx_power_index = 3;
    config.protocol = WifiProtocol::ax;
    config.channel = 11;

    std::array<std::byte, kMaxWifiSettingsBytes> encoded{};
    const auto encoded_size = encode_wifi_config(config, encoded);
    BLIP_CHECK(encoded_size);
    ImportedSettingsDecodeWorkspace workspace{};
    const auto decoded = decode_wifi_config(
        std::span<const std::byte>{encoded.data(), encoded_size.value()}, workspace);
    BLIP_CHECK(decoded && decoded.value().source == WifiSettingsSource::native);
    BLIP_CHECK(decoded.value().config == config);
    for (std::size_t prefix = 0; prefix < encoded_size.value(); ++prefix) {
        BLIP_CHECK(
            !decode_wifi_config(std::span<const std::byte>{encoded.data(), prefix}, workspace));
    }
    encoded[encoded_size.value() - 1U] ^= std::byte{1};
    BLIP_CHECK(!decode_wifi_config(std::span<const std::byte>{encoded.data(), encoded_size.value()},
                                   workspace));
    return true;
}

bool config_validation_rejects_unsafe_credentials_and_addresses() {
    WifiConfig config{};
    BLIP_CHECK(config.ssid.assign("network"));
    BLIP_CHECK(config.password.assign("short"));
    BLIP_CHECK(!validate_wifi_config(config));
    BLIP_CHECK(config.password.assign("valid-passphrase"));
    BLIP_CHECK(config.manual_ip.assign("192.168.1.22"));
    BLIP_CHECK(!validate_wifi_config(config));
    BLIP_CHECK(config.manual_gateway.assign("999.168.1.1"));
    BLIP_CHECK(!validate_wifi_config(config));
    BLIP_CHECK(config.manual_gateway.assign("192.168.1.1"));
    BLIP_CHECK(validate_wifi_config(config, true));
    BLIP_CHECK(valid_ipv4("0.0.0.0"));
    BLIP_CHECK(valid_ipv4("255.255.255.255"));
    BLIP_CHECK(!valid_ipv4("1.2.3"));
    BLIP_CHECK(!valid_ipv4("1.2.3.4.5"));
    BLIP_CHECK(!valid_utf8(std::string_view{"\xc0\x80", 2}));
    return true;
}

bool legacy_wifi_payload_migrates_without_exposing_secret() {
    std::array<std::byte, 512> payload{};
    ImportedSettingsBuilder builder{payload};
    const auto append = [&builder](std::string_view field, LegacyValue value) {
        return builder.append({"wifi", "blip.transport.wifi", field, value});
    };
    LegacyValue enabled{};
    enabled.type = LegacyValueType::boolean;
    enabled.boolean = true;
    BLIP_CHECK(append("enabled", enabled));
    LegacyValue mode{};
    mode.type = LegacyValueType::signed_integer;
    mode.signed_integer = 0;
    BLIP_CHECK(append("mode", mode));
    LegacyValue ssid{};
    ssid.type = LegacyValueType::string;
    ssid.string = "fixture-network";
    BLIP_CHECK(append("ssid", ssid));
    LegacyValue password{};
    password.type = LegacyValueType::string;
    password.string = "not-a-real-secret";
    BLIP_CHECK(append("pass", password));
    const auto size = builder.finish();
    BLIP_CHECK(size);

    ImportedSettingsDecodeWorkspace workspace{};
    const auto decoded =
        decode_wifi_config(std::span<const std::byte>{payload.data(), size.value()}, workspace);
    BLIP_CHECK(decoded && decoded.value().source == WifiSettingsSource::legacy_import);
    BLIP_CHECK(decoded.value().config.ssid.view() == "fixture-network");
    BLIP_CHECK(decoded.value().config.password.view() == "not-a-real-secret");

    std::array<std::byte, kMaxWifiSettingsBytes> native{};
    const auto native_size = encode_wifi_config(decoded.value().config, native);
    BLIP_CHECK(native_size);
    const std::string_view native_bytes{reinterpret_cast<const char*>(native.data()),
                                        native_size.value()};
    BLIP_CHECK(native_bytes.find("not-a-real-secret") != std::string_view::npos);
    return true;
}

bool provisioning_form_is_bounded_and_strict() {
    const auto parsed =
        parse_provisioning_form("ssid=Studio+WiFi&password=correct%20horse%20battery%20staple");
    BLIP_CHECK(parsed);
    BLIP_CHECK(parsed.value().ssid.view() == "Studio WiFi");
    BLIP_CHECK(parsed.value().password.view() == "correct horse battery staple");
    BLIP_CHECK(parse_provisioning_form("ssid=OpenNetwork&password="));
    BLIP_CHECK(!parse_provisioning_form("ssid=network"));
    BLIP_CHECK(!parse_provisioning_form("ssid=a&ssid=b&password=12345678"));
    BLIP_CHECK(!parse_provisioning_form("ssid=%GG&password=12345678"));
    BLIP_CHECK(!parse_provisioning_form("ssid=x&password=short"));
    std::array<char, kMaxProvisioningFormBytes + 1U> oversized{};
    oversized.fill('x');
    BLIP_CHECK(!parse_provisioning_form({oversized.data(), oversized.size()}));
    return true;
}

bool state_machine_provisions_retries_and_recovers() {
    WifiStateMachine machine{};
    WifiConfig blank{};
    auto transition = machine.apply(blank, 0);
    BLIP_CHECK(transition.start_ap && !transition.start_station);
    BLIP_CHECK(machine.state() == WifiConnectionState::hotspot && machine.ap_active());

    WifiConfig station{};
    BLIP_CHECK(station.ssid.assign("network"));
    BLIP_CHECK(station.password.assign("valid-password"));
    transition = machine.apply(station, 100);
    BLIP_CHECK(transition.start_station && transition.connect_station && !transition.stop_ap);
    BLIP_CHECK(machine.state() == WifiConnectionState::connecting);
    BLIP_CHECK(machine.ap_active());
    BLIP_CHECK(!machine.tick(599).connect_station);
    BLIP_CHECK(machine.tick(600).connect_station && machine.retry_count() == 1U);
    transition = machine.tick(20100);
    BLIP_CHECK(!transition.start_ap && machine.ap_active());
    BLIP_CHECK(machine.state() == WifiConnectionState::connection_error);
    transition = machine.station_connected(20200);
    BLIP_CHECK(transition.stop_ap && machine.state() == WifiConnectionState::connected);
    transition = machine.station_disconnected(21000);
    BLIP_CHECK(machine.state() == WifiConnectionState::connecting);
    BLIP_CHECK(machine.tick(21000).connect_station);

    station.enabled = false;
    transition = machine.apply(station, 22000);
    BLIP_CHECK(transition.stop_radio && machine.state() == WifiConnectionState::disabled);
    return true;
}

bool station_and_ap_disconnect_retries_without_dropping_hotspot() {
    WifiStateMachine machine{};
    WifiConfig config{};
    config.mode = WifiMode::station_and_ap;
    BLIP_CHECK(config.ssid.assign("network"));
    BLIP_CHECK(config.password.assign("valid-password"));

    auto transition = machine.apply(config, 0);
    BLIP_CHECK(transition.start_station && transition.start_ap && transition.connect_station);
    BLIP_CHECK(machine.ap_active() && machine.station_active());
    transition = machine.station_connected(100);
    BLIP_CHECK(machine.state() == WifiConnectionState::connected && !transition.stop_ap);
    transition = machine.station_disconnected(200);
    BLIP_CHECK(transition.state_changed);
    BLIP_CHECK(machine.state() == WifiConnectionState::connection_error);
    BLIP_CHECK(machine.ap_active());
    BLIP_CHECK(machine.tick(200).connect_station);
    return true;
}

} // namespace

int main() {
    const std::array tests{
        native_config_round_trip_and_corruption,
        config_validation_rejects_unsafe_credentials_and_addresses,
        legacy_wifi_payload_migrates_without_exposing_secret,
        provisioning_form_is_bounded_and_strict,
        state_machine_provisions_retries_and_recovers,
        station_and_ap_disconnect_retries_without_dropping_hotspot,
    };
    for (const auto test : tests) {
        if (!test()) {
            return 1;
        }
    }
    std::cout << "blip network tests passed: " << tests.size() << '\n';
    return 0;
}
