#include "blip/core/control.hpp"
#include "blip/core/registry.hpp"
#include "blip/oscquery/legacy_osc.hpp"
#include "blip/oscquery/osc.hpp"
#include "blip/oscquery/oscquery.hpp"
#include "test_harness.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace blip::core;
using namespace blip::oscquery;

constexpr std::array<MetadataEntry, 1> kMetadata{{{"legacy_path", "/leds/strip1"}}};
constexpr std::array<ParameterDescriptor, 4> kParameters{{
    {"brightness",
     "Brightness",
     ValueType::number,
     Access::read_write,
     true,
     ScalarValue::from_number(0.0),
     {true, 0.0, 1.0, 0.01},
     "ratio"},
    {"level",
     "Level",
     ValueType::integer,
     Access::read_only,
     false,
     ScalarValue::from_integer(7),
     {},
     ""},
    {"mode",
     "Mode",
     ValueType::integer,
     Access::read_write,
     true,
     ScalarValue::from_integer(0),
     {true, 0, 1, 1},
     ""},
    {"token",
     "Token",
     ValueType::string,
     Access::write_only,
     true,
     ScalarValue::from_string(""),
     {},
     ""},
}};
constexpr std::array<LegacyEnumValue, 2> kLegacyModeValues{{
    {ScalarValue::from_integer(0), "Mode A"},
    {ScalarValue::from_integer(1), "Mode B"},
}};
constexpr std::array<LegacyParameterAlias, 1> kLegacyParameters{{
    {"mode", "legacyMode", "Legacy Mode", ValueType::string, kLegacyModeValues},
}};
constexpr std::array<ActionDescriptor, 1> kActions{{{"save", "Save", {}}}};
constexpr std::array<FieldDescriptor, 1> kEventFields{{{"value", ValueType::number, true}}};
constexpr std::array<EventDescriptor, 1> kEvents{{{"changed", kEventFields}}};

class ProbeComponent final : public Component {
  public:
    [[nodiscard]] const ComponentDescriptor& descriptor() const noexcept override {
        static constexpr ComponentDescriptor value = [] {
            ComponentDescriptor result{};
            result.schema_version = 1;
            result.id = "blip.test_strip";
            result.display_name = "Test strip";
            result.description = "Synthetic registry-driven strip";
            result.metadata = kMetadata;
            result.parameters = kParameters;
            result.legacy_parameters = kLegacyParameters;
            result.actions = kActions;
            result.events = kEvents;
            result.settings = {1, 1};
            return result;
        }();
        return value;
    }

    [[nodiscard]] Status start(const StartContext&) noexcept override {
        started_ = true;
        return Status::success();
    }
    [[nodiscard]] Status stop() noexcept override {
        started_ = false;
        return Status::success();
    }
    [[nodiscard]] Status read_parameter(std::string_view id,
                                        ScalarValue& output) noexcept override {
        if (!started_) {
            return Status::failure(
                {ErrorDomain::control, ErrorCode::invalid_state, descriptor().id, "read", id});
        }
        if (id == "brightness") {
            output = ScalarValue::from_number(brightness_);
            return Status::success();
        }
        if (id == "level") {
            output = ScalarValue::from_integer(7);
            return Status::success();
        }
        if (id == "mode") {
            output = ScalarValue::from_integer(mode_);
            return Status::success();
        }
        return Status::failure(
            {ErrorDomain::control, ErrorCode::not_found, descriptor().id, "read", id});
    }
    [[nodiscard]] Status write_parameter(std::string_view id,
                                         const ScalarValue& value) noexcept override {
        if (!started_ || (id != "brightness" && id != "mode")) {
            return Status::failure(
                {ErrorDomain::control, ErrorCode::not_found, descriptor().id, "write", id});
        }
        if (id == "brightness") {
            brightness_ = value.number;
        } else {
            mode_ = value.integer;
        }
        return Status::success();
    }
    [[nodiscard]] Status invoke_action(std::string_view id, std::span<const ScalarValue>,
                                       std::span<ScalarValue>,
                                       std::size_t& output_count) noexcept override {
        if (!started_ || id != "save") {
            return Status::failure(
                {ErrorDomain::control, ErrorCode::not_found, descriptor().id, "invoke", id});
        }
        ++saves_;
        output_count = 0;
        return Status::success();
    }
    [[nodiscard]] double brightness() const noexcept { return brightness_; }
    [[nodiscard]] std::size_t saves() const noexcept { return saves_; }

  private:
    bool started_{};
    double brightness_{};
    std::int64_t mode_{};
    std::size_t saves_{};
};

class StringSink final : public TextSink {
  public:
    [[nodiscard]] bool write(std::string_view text) noexcept override {
        value.append(text);
        return true;
    }
    std::string value{};
};

[[nodiscard]] std::vector<std::byte> fixture(std::string_view relative) {
    const std::string path = std::string{BLIP_V1_FIXTURES} + "/" + std::string{relative};
    std::ifstream input{path, std::ios::binary};
    const std::vector<char> bytes{std::istreambuf_iterator<char>{input},
                                  std::istreambuf_iterator<char>{}};
    std::vector<std::byte> result(bytes.size());
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        result[index] = static_cast<std::byte>(static_cast<unsigned char>(bytes[index]));
    }
    return result;
}

bool v1_datagrams_decode_and_reencode_exactly() {
    struct Expected {
        std::string_view path;
        std::string_view address;
        std::size_t arguments;
    };
    constexpr std::array<Expected, 4> expected{{
        {"osc/yo.osc", "/yo", 1},
        {"osc/brightness.osc", "/leds/strip1/brightness", 1},
        {"osc/save.osc", "/settings/save", 0},
        {"osc/ping.osc", "/ping", 1},
    }};
    for (const auto& item : expected) {
        const auto bytes = fixture(item.path);
        BLIP_CHECK(!bytes.empty());
        const auto decoded = decode_osc_message(bytes);
        BLIP_CHECK(decoded);
        BLIP_CHECK(decoded.value().address == item.address);
        BLIP_CHECK(decoded.value().argument_count == item.arguments);
        std::array<std::byte, kMaxOscPacketBytes> encoded{};
        const auto size = encode_osc_message(decoded.value(), encoded);
        BLIP_CHECK(size && size.value() == bytes.size());
        BLIP_CHECK(std::equal(bytes.begin(), bytes.end(), encoded.begin()));
        for (std::size_t prefix = 0; prefix < bytes.size(); ++prefix) {
            BLIP_CHECK(!decode_osc_message({bytes.data(), prefix}));
        }
    }
    return true;
}

bool all_supported_types_round_trip_and_bad_padding_fails() {
    OscMessage source{};
    source.address = "/types";
    source.arguments[0] = OscValue::from_bool(true);
    source.arguments[1] = OscValue::from_bool(false);
    source.arguments[2] = OscValue::from_integer(-42);
    source.arguments[3] = OscValue::from_number(1.25F);
    source.arguments[4] = OscValue::from_string("demo");
    source.arguments[5].type = OscValueType::rgba;
    source.arguments[5].word = 0x11223344U;
    source.arguments[6].type = OscValueType::midi;
    source.arguments[6].word = 0x01903c7fU;
    source.arguments[7].type = OscValueType::timetag;
    source.arguments[7].timetag = 0x0102030405060708ULL;
    source.argument_count = 8;
    std::array<std::byte, kMaxOscPacketBytes> encoded{};
    const auto size = encode_osc_message(source, encoded);
    BLIP_CHECK(size);
    const auto decoded = decode_osc_message({encoded.data(), size.value()});
    BLIP_CHECK(decoded && decoded.value().argument_count == 8U);
    BLIP_CHECK(decoded.value().arguments[0].boolean);
    BLIP_CHECK(!decoded.value().arguments[1].boolean);
    BLIP_CHECK(decoded.value().arguments[2].integer == -42);
    BLIP_CHECK(decoded.value().arguments[3].number == 1.25F);
    BLIP_CHECK(decoded.value().arguments[4].string == "demo");
    BLIP_CHECK(decoded.value().arguments[5].word == 0x11223344U);
    BLIP_CHECK(decoded.value().arguments[6].word == 0x01903c7fU);
    BLIP_CHECK(decoded.value().arguments[7].timetag == 0x0102030405060708ULL);
    encoded[7] = std::byte{1};
    BLIP_CHECK(!decode_osc_message({encoded.data(), size.value()}));
    return true;
}

bool legacy_endpoint_routes_registry_controls_and_discovery() {
    Registry<2> registry{};
    ProbeComponent probe{};
    RegistryControlService<2> controls{registry};
    BLIP_CHECK(registry.add(probe));
    BLIP_CHECK(registry.add(controls));
    BLIP_CHECK(registry.validate());
    BLIP_CHECK(registry.start_all().ok());
    const DeviceIdentity identity{"02:00:00:00:00:07", "Creators Ball V2", "Fixture BLIP", "1.2.0",
                                  9000};
    LegacyOscEndpoint endpoint{registry, controls, identity};

    const auto brightness_bytes = fixture("osc/brightness.osc");
    const auto brightness = decode_osc_message(brightness_bytes);
    BLIP_CHECK(brightness);
    OscMessage response{};
    bool reply{};
    BLIP_CHECK(endpoint.handle(brightness.value(), "192.0.2.7", true, response, reply));
    BLIP_CHECK(reply && probe.brightness() == 0.5);
    BLIP_CHECK(response.address == "/leds/strip1/brightness");
    BLIP_CHECK(response.argument_count == 2U);
    BLIP_CHECK(response.arguments[0].string == identity.id);
    BLIP_CHECK(response.arguments[1].number == 0.5F);

    OscMessage legacy_mode{};
    legacy_mode.address = "/leds/strip1/legacyMode";
    legacy_mode.arguments[0] = OscValue::from_string("Mode B");
    legacy_mode.argument_count = 1U;
    BLIP_CHECK(endpoint.handle(legacy_mode, "192.0.2.7", false, response, reply));
    BLIP_CHECK(reply && response.arguments[0].string == "Mode B");
    legacy_mode.argument_count = 0U;
    BLIP_CHECK(endpoint.handle(legacy_mode, "192.0.2.7", false, response, reply));
    BLIP_CHECK(response.argument_count == 1U && response.arguments[0].string == "Mode B");

    const auto yo_bytes = fixture("osc/yo.osc");
    const auto yo = decode_osc_message(yo_bytes);
    BLIP_CHECK(yo);
    BLIP_CHECK(endpoint.handle(yo.value(), "192.0.2.7", true, response, reply));
    BLIP_CHECK(reply && response.address == "/wassup" && response.argument_count == 5U);
    BLIP_CHECK(response.arguments[0].string == "192.0.2.7");
    BLIP_CHECK(response.arguments[1].string == identity.id);

    OscMessage save{};
    save.address = "/leds/strip1/save";
    BLIP_CHECK(endpoint.handle(save, "192.0.2.7", false, response, reply));
    BLIP_CHECK(reply && probe.saves() == 1U && response.argument_count == 0U);
    BLIP_CHECK(registry.stop_all());
    return true;
}

bool oscquery_is_registry_derived_and_filters_config() {
    Registry<2> registry{};
    ProbeComponent probe{};
    RegistryControlService<2> controls{registry};
    BLIP_CHECK(registry.add(probe));
    BLIP_CHECK(registry.add(controls));
    BLIP_CHECK(registry.validate());
    BLIP_CHECK(registry.start_all().ok());
    ControlResponse ignored{};
    const std::array<ScalarValue, 1> value{{ScalarValue::from_number(0.75)}};
    BLIP_CHECK(controls.execute(
        {ControlOperation::write_parameter, "blip.test_strip", "brightness", value}, ignored));

    StringSink complete{};
    BLIP_CHECK(write_oscquery_tree(registry, controls, true, complete));
    BLIP_CHECK(complete.value.starts_with("{\"DESCRIPTION\":\"Root\""));
    BLIP_CHECK(complete.value.find("\"strip1\"") != std::string::npos);
    BLIP_CHECK(complete.value.find("/leds/strip1/brightness") != std::string::npos);
    BLIP_CHECK(complete.value.find("\"VALUE\":[0.75]") != std::string::npos);
    BLIP_CHECK(complete.value.find("\"RANGE\":[{\"MIN\":0,\"MAX\":1}]") != std::string::npos);
    BLIP_CHECK(complete.value.find("\"BLIP_KIND\":\"component\"") != std::string::npos);
    BLIP_CHECK(complete.value.find("\"BLIP_COMPONENT_ID\":\"blip.test_strip\"") !=
               std::string::npos);
    BLIP_CHECK(complete.value.find("\"BLIP_KIND\":\"parameter\"") != std::string::npos);
    BLIP_CHECK(complete.value.find("\"BLIP_PERSISTED\":true") != std::string::npos);
    BLIP_CHECK(complete.value.find("\"BLIP_STEP\":0.01") != std::string::npos);
    BLIP_CHECK(complete.value.find("\"BLIP_UNIT\":\"ratio\"") != std::string::npos);
    BLIP_CHECK(complete.value.find("\"token\":{\"DESCRIPTION\":\"Token\",\"ACCESS\":3,"
                                   "\"TYPE\":\"s\",\"FULL_PATH\":\"/leds/strip1/token\","
                                   "\"BLIP_KIND\":\"parameter\",\"BLIP_PERSISTED\":true,"
                                   "\"BLIP_READABLE\":false,\"BLIP_WRITABLE\":true}") !=
               std::string::npos);
    BLIP_CHECK(complete.value.find("\"BLIP_KIND\":\"action\",\"BLIP_FIELDS\":[]") !=
               std::string::npos);
    BLIP_CHECK(complete.value.find("\"BLIP_KIND\":\"event\",\"BLIP_FIELDS\":[{\"ID\":\"value\","
                                   "\"TYPE\":\"f\",\"REQUIRED\":true}]") != std::string::npos);
    BLIP_CHECK(complete.value.find("/leds/strip1/changed") != std::string::npos);
    BLIP_CHECK(complete.value.find("/leds/strip1/legacyMode") != std::string::npos);
    BLIP_CHECK(complete.value.find("\"VALUE\":[\"Mode A\"]") != std::string::npos);
    BLIP_CHECK(complete.value.find("\"VALS\":[\"Mode A\",\"Mode B\"]") != std::string::npos);

    StringSink filtered{};
    BLIP_CHECK(write_oscquery_tree(registry, controls, false, filtered));
    BLIP_CHECK(filtered.value.find("brightness") == std::string::npos);
    BLIP_CHECK(filtered.value.find("legacyMode") == std::string::npos);
    BLIP_CHECK(filtered.value.find("token") == std::string::npos);
    BLIP_CHECK(filtered.value.find("/leds/strip1/level") != std::string::npos);
    BLIP_CHECK(registry.stop_all());
    return true;
}

bool host_info_matches_v1_contract() {
    StringSink sink{};
    const DeviceIdentity identity{"02:00:00:00:00:07", "Creators Ball V2", "Fixture BLIP", "1.2.0",
                                  9000};
    BLIP_CHECK(write_oscquery_host_info(identity, sink));
    BLIP_CHECK(sink.value.find("\"NAME\":\"Fixture BLIP\"") != std::string::npos);
    BLIP_CHECK(sink.value.find("\"VERSION\":\"1.2.0\"") != std::string::npos);
    BLIP_CHECK(sink.value.find("\"DEVICE_TYPE\":\"Creators Ball V2\"") != std::string::npos);
    BLIP_CHECK(sink.value.find("\"DEVICE_ID\":\"02:00:00:00:00:07\"") != std::string::npos);
    BLIP_CHECK(sink.value.find("\"OSC_PORT\":9000") != std::string::npos);
    BLIP_CHECK(sink.value.find("\"OSC_TRANSPORT\":\"UDP\"") != std::string::npos);
    BLIP_CHECK(sink.value.find("\"BLIP_KIND\":true") != std::string::npos);
    BLIP_CHECK(sink.value.find("\"BLIP_FIELDS\":true") != std::string::npos);
    return true;
}

bool http_content_negotiation_preserves_oscquery_root() {
    BLIP_CHECK(route_http_get("/", false, false) == HttpGetSurface::oscquery);
    BLIP_CHECK(route_http_get("/", true, true) == HttpGetSurface::oscquery);
    BLIP_CHECK(route_http_get("/", false, true) == HttpGetSurface::web_asset);
    BLIP_CHECK(route_http_get("/src/app.js", false, false) == HttpGetSurface::web_asset);
    BLIP_CHECK(route_http_get("/api/web-assets", false, false) == HttpGetSurface::asset_status);
    BLIP_CHECK(route_http_get("/src/app.js", true, false) == HttpGetSurface::not_found);
    return true;
}

constexpr TestCase kTests[]{
    {"v1 datagrams", v1_datagrams_decode_and_reencode_exactly},
    {"types and rejection", all_supported_types_round_trip_and_bad_padding_fails},
    {"registry OSC routing", legacy_endpoint_routes_registry_controls_and_discovery},
    {"registry OSCQuery", oscquery_is_registry_derived_and_filters_config},
    {"host info", host_info_matches_v1_contract},
    {"HTTP content negotiation", http_content_negotiation_preserves_oscquery_root}};

} // namespace

int main() { return run_tests(kTests); }
