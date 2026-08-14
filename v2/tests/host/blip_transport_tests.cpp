#include "blip/core/control.hpp"
#include "blip/core/registry.hpp"
#include "blip/transport/envelope.hpp"
#include "blip/transport/legacy_serial_v1.hpp"
#include "blip/transport/serial_protocol.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <string_view>

namespace {

using namespace blip::core;
using namespace blip::transport;

#define BLIP_CHECK(expression)                                                                     \
    do {                                                                                           \
        if (!(expression)) {                                                                       \
            std::cerr << __func__ << ':' << __LINE__ << ": check failed: " #expression << '\n';    \
            return false;                                                                          \
        }                                                                                          \
    } while (false)

constexpr std::array<ParameterDescriptor, 2> kProbeParameters{{
    {"value",
     "Value",
     ValueType::integer,
     Access::read_write,
     false,
     ScalarValue::from_integer(0),
     {true, -100, 100, 1},
     ""},
    {"secret",
     "Secret",
     ValueType::string,
     Access::write_only,
     true,
     ScalarValue::from_string(""),
     {},
     ""},
}};
constexpr std::array<ActionDescriptor, 1> kProbeActions{{
    {"reset", "Reset", {}},
}};

class ProbeComponent final : public Component {
  public:
    [[nodiscard]] const ComponentDescriptor& descriptor() const noexcept override {
        static constexpr ComponentDescriptor descriptor = [] {
            ComponentDescriptor result{};
            result.schema_version = 1;
            result.id = "blip.probe";
            result.display_name = "Probe";
            result.description = "Transport probe";
            result.parameters = kProbeParameters;
            result.actions = kProbeActions;
            result.settings = {1, 1};
            return result;
        }();
        return descriptor;
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
        if (!started_ || id != "value") {
            return Status::failure(
                {ErrorDomain::control, ErrorCode::not_found, descriptor().id, "read", id});
        }
        output = ScalarValue::from_integer(value_);
        return Status::success();
    }
    [[nodiscard]] Status write_parameter(std::string_view id,
                                         const ScalarValue& value) noexcept override {
        if (started_ && id == "secret" && value.type == ValueType::string) {
            secret_writes_++;
            return Status::success();
        }
        if (!started_ || id != "value") {
            return Status::failure(
                {ErrorDomain::control, ErrorCode::not_found, descriptor().id, "write", id});
        }
        value_ = value.integer;
        return Status::success();
    }
    [[nodiscard]] std::size_t secret_writes() const noexcept { return secret_writes_; }
    [[nodiscard]] Status invoke_action(std::string_view id, std::span<const ScalarValue>,
                                       std::span<ScalarValue>,
                                       std::size_t& output_count) noexcept override {
        if (!started_ || id != "reset") {
            return Status::failure(
                {ErrorDomain::control, ErrorCode::not_found, descriptor().id, "invoke", id});
        }
        value_ = 0;
        output_count = 0;
        return Status::success();
    }

  private:
    std::int64_t value_{};
    bool started_{};
    std::size_t secret_writes_{};
};

bool envelope_round_trip_and_corruption() {
    constexpr std::array<std::byte, 7> payload{std::byte{0},    std::byte{1}, std::byte{2},
                                               std::byte{0xff}, std::byte{4}, std::byte{0},
                                               std::byte{6}};
    std::array<std::byte, kMaxEnvelopeBytes> encoded{};
    const EnvelopeInput input{EnvelopeKind::request, PayloadType::control, 0x12345678U, 0, payload};
    const auto size = encode_envelope(input, encoded);
    BLIP_CHECK(size);
    const auto decoded = decode_envelope(std::span<const std::byte>{encoded.data(), size.value()});
    BLIP_CHECK(decoded);
    BLIP_CHECK(decoded.value().kind == EnvelopeKind::request);
    BLIP_CHECK(decoded.value().request_id == 0x12345678U);
    BLIP_CHECK(decoded.value().payload.size() == payload.size());
    for (std::size_t index = 0; index < payload.size(); ++index) {
        BLIP_CHECK(decoded.value().payload[index] == payload[index]);
    }
    for (std::size_t prefix = 0; prefix < size.value(); ++prefix) {
        BLIP_CHECK(!decode_envelope(std::span<const std::byte>{encoded.data(), prefix}));
    }
    encoded[kEnvelopeHeaderBytes + 2U] ^= std::byte{0x40};
    BLIP_CHECK(!decode_envelope(std::span<const std::byte>{encoded.data(), size.value()}));
    return true;
}

bool control_payload_round_trip_and_validation() {
    const std::array<ScalarValue, 4> values{
        {ScalarValue::from_bool(true), ScalarValue::from_integer(-42),
         ScalarValue::from_number(1.25), ScalarValue::from_string("demo")}};
    const ControlMessage message{ControlOperation::invoke_action,
                                 ErrorDomain::none,
                                 ErrorCode::none,
                                 "blip.probe",
                                 "run",
                                 {},
                                 values};
    std::array<std::byte, kMaxControlPayloadBytes> encoded{};
    const auto size = encode_control_message(message, encoded);
    BLIP_CHECK(size);
    const auto decoded =
        decode_control_message(std::span<const std::byte>{encoded.data(), size.value()});
    BLIP_CHECK(decoded);
    BLIP_CHECK(decoded.value().operation == ControlOperation::invoke_action);
    BLIP_CHECK(decoded.value().component_id == "blip.probe");
    BLIP_CHECK(decoded.value().control_id == "run");
    BLIP_CHECK(decoded.value().value_count == values.size());
    BLIP_CHECK(decoded.value().values[0].boolean);
    BLIP_CHECK(decoded.value().values[1].integer == -42);
    BLIP_CHECK(decoded.value().values[2].number == 1.25);
    BLIP_CHECK(decoded.value().values[3].string == "demo");
    for (std::size_t prefix = 0; prefix < size.value(); ++prefix) {
        BLIP_CHECK(!decode_control_message(std::span<const std::byte>{encoded.data(), prefix}));
    }
    encoded[2] = std::byte{5};
    BLIP_CHECK(!decode_control_message(std::span<const std::byte>{encoded.data(), size.value()}));
    const std::array<char, 2> invalid_utf8{{static_cast<char>(0xc0), static_cast<char>(0x80)}};
    const std::array<ScalarValue, 1> invalid_string{{
        ScalarValue::from_string({invalid_utf8.data(), invalid_utf8.size()}),
    }};
    BLIP_CHECK(!encode_control_message({ControlOperation::write_parameter,
                                        ErrorDomain::none,
                                        ErrorCode::none,
                                        "blip.probe",
                                        "text",
                                        {},
                                        invalid_string},
                                       encoded));
    return true;
}

bool cobs_round_trip_and_errors() {
    constexpr std::array<std::byte, 8> raw{std::byte{0},    std::byte{1}, std::byte{0},
                                           std::byte{2},    std::byte{3}, std::byte{0},
                                           std::byte{0xff}, std::byte{0}};
    std::array<std::byte, 32> framed{};
    const auto encoded = cobs_encode_frame(raw, framed);
    BLIP_CHECK(encoded);
    BLIP_CHECK(framed[encoded.value() - 1U] == std::byte{0});
    std::array<std::byte, 32> decoded{};
    const auto decoded_size =
        cobs_decode_frame(std::span<const std::byte>{framed.data(), encoded.value() - 1U}, decoded);
    BLIP_CHECK(decoded_size);
    BLIP_CHECK(decoded_size.value() == raw.size());
    for (std::size_t index = 0; index < raw.size(); ++index) {
        BLIP_CHECK(decoded[index] == raw[index]);
    }
    constexpr std::array<std::byte, 1> invalid{std::byte{0}};
    BLIP_CHECK(!cobs_decode_frame(invalid, decoded));

    std::array<std::byte, kMaxEnvelopeBytes> maximum{};
    maximum.fill(std::byte{0xa5});
    std::array<std::byte, kMaxSerialFrameBytes> maximum_frame{};
    const auto maximum_size = cobs_encode_frame(maximum, maximum_frame);
    BLIP_CHECK(maximum_size && maximum_size.value() == maximum_frame.size());
    std::array<std::byte, kMaxEnvelopeBytes> maximum_decoded{};
    const auto maximum_decoded_size =
        cobs_decode_frame({maximum_frame.data(), maximum_size.value() - 1U}, maximum_decoded);
    BLIP_CHECK(maximum_decoded_size && maximum_decoded_size.value() == maximum.size());
    BLIP_CHECK(maximum == maximum_decoded);
    return true;
}

bool registry_control_dispatch_validates_schema() {
    Registry<2> registry{};
    ProbeComponent probe{};
    RegistryControlService<2> controls{registry};
    BLIP_CHECK(registry.add(probe));
    BLIP_CHECK(registry.add(controls));
    BLIP_CHECK(registry.validate());
    BLIP_CHECK(registry.start_all().ok());

    ControlResponse response{};
    const std::array<ScalarValue, 1> value{{ScalarValue::from_integer(27)}};
    BLIP_CHECK(controls.execute({ControlOperation::write_parameter, "blip.probe", "value", value},
                                response));
    BLIP_CHECK(
        controls.execute({ControlOperation::read_parameter, "blip.probe", "value", {}}, response));
    BLIP_CHECK(response.value_count == 1U && response.values[0].integer == 27);
    const std::array<ScalarValue, 1> wrong_type{{ScalarValue::from_number(1.0)}};
    BLIP_CHECK(!controls.execute(
        {ControlOperation::write_parameter, "blip.probe", "value", wrong_type}, response));
    const std::array<ScalarValue, 1> out_of_range{{ScalarValue::from_integer(101)}};
    BLIP_CHECK(!controls.execute(
        {ControlOperation::write_parameter, "blip.probe", "value", out_of_range}, response));
    const std::array<ScalarValue, 1> secret{{ScalarValue::from_string("hidden")}};
    BLIP_CHECK(controls.execute({ControlOperation::write_parameter, "blip.probe", "secret", secret},
                                response));
    BLIP_CHECK(probe.secret_writes() == 1U);
    BLIP_CHECK(!controls.execute({ControlOperation::read_parameter, "blip.probe", "secret", {}},
                                 response));
    BLIP_CHECK(
        controls.execute({ControlOperation::invoke_action, "blip.probe", "reset", {}}, response));
    BLIP_CHECK(
        controls.execute({ControlOperation::read_parameter, "blip.probe", "value", {}}, response));
    BLIP_CHECK(response.values[0].integer == 0);
    BLIP_CHECK(!controls.execute({ControlOperation::read_parameter, "blip.unknown", "value", {}},
                                 response));
    BLIP_CHECK(registry.stop_all());
    return true;
}

bool framed_endpoint_controls_registry() {
    Registry<2> registry{};
    ProbeComponent probe{};
    RegistryControlService<2> controls{registry};
    BLIP_CHECK(registry.add(probe));
    BLIP_CHECK(registry.add(controls));
    BLIP_CHECK(registry.validate());
    BLIP_CHECK(registry.start_all().ok());

    std::array<std::byte, kMaxSerialFrameBytes> decode_buffer{};
    std::array<std::byte, kMaxSerialFrameBytes> envelope_buffer{};
    std::array<std::byte, kMaxControlPayloadBytes> payload_buffer{};
    SerialControlEndpoint endpoint{controls, decode_buffer, envelope_buffer, payload_buffer};
    const std::array<ScalarValue, 1> value{{ScalarValue::from_integer(-9)}};
    const ControlMessage request{ControlOperation::write_parameter,
                                 ErrorDomain::none,
                                 ErrorCode::none,
                                 "blip.probe",
                                 "value",
                                 {},
                                 value};
    std::array<std::byte, kMaxControlPayloadBytes> request_payload{};
    const auto payload_size = encode_control_message(request, request_payload);
    BLIP_CHECK(payload_size);
    std::array<std::byte, kMaxEnvelopeBytes> request_envelope{};
    const auto envelope_size = encode_envelope({EnvelopeKind::request,
                                                PayloadType::control,
                                                77,
                                                0,
                                                {request_payload.data(), payload_size.value()}},
                                               request_envelope);
    BLIP_CHECK(envelope_size);
    std::array<std::byte, kMaxSerialFrameBytes> request_frame{};
    const auto frame_size =
        cobs_encode_frame({request_envelope.data(), envelope_size.value()}, request_frame);
    BLIP_CHECK(frame_size);
    std::array<std::byte, kMaxSerialFrameBytes> response_frame{};
    const auto response_size =
        endpoint.handle_frame({request_frame.data(), frame_size.value() - 1U}, response_frame);
    BLIP_CHECK(response_size);
    BLIP_CHECK(response_frame[response_size.value() - 1U] == std::byte{0});

    std::array<std::byte, kMaxEnvelopeBytes> response_envelope{};
    const auto raw_size =
        cobs_decode_frame({response_frame.data(), response_size.value() - 1U}, response_envelope);
    BLIP_CHECK(raw_size);
    const auto envelope = decode_envelope({response_envelope.data(), raw_size.value()});
    BLIP_CHECK(envelope && envelope.value().kind == EnvelopeKind::response);
    BLIP_CHECK(envelope.value().request_id == 77U);
    const auto response = decode_control_message(envelope.value().payload);
    BLIP_CHECK(response && response.value().error_code == ErrorCode::none);
    BLIP_CHECK(endpoint.metrics().accepted == 1U);

    request_frame[3] ^= std::byte{0x80};
    BLIP_CHECK(
        !endpoint.handle_frame({request_frame.data(), frame_size.value() - 1U}, response_frame));
    BLIP_CHECK(endpoint.metrics().rejected == 1U);
    BLIP_CHECK(registry.stop_all());
    return true;
}

bool legacy_serial_fixture_maps_without_unsafe_guessing() {
    LegacySerialV1Parser parser{};
    const auto discovery = parser.parse("yo\n");
    BLIP_CHECK(discovery && discovery.value().discovery);
    const auto brightness = parser.parse("leds.strip1.brightness 0.5\n");
    BLIP_CHECK(brightness && !brightness.value().discovery);
    BLIP_CHECK(brightness.value().component_path == "leds.strip1");
    BLIP_CHECK(brightness.value().command == "brightness");
    BLIP_CHECK(brightness.value().value_count == 1U);
    BLIP_CHECK(brightness.value().values[0].type == LegacySerialValueType::number);
    const auto script = parser.parse("script.setParam gain,-2\n");
    BLIP_CHECK(script && script.value().value_count == 2U);
    BLIP_CHECK(script.value().values[1].type == LegacySerialValueType::string);
    BLIP_CHECK(script.value().values[1].text == "-2");
    BLIP_CHECK(!parser.parse("bad command extra\n"));
    constexpr std::string_view expected =
        "wassup 02:00:00:00:00:07 \"Creators Ball V2\" \"Fixture BLIP\" \"1.2.0\"\n";
    std::array<char, expected.size() + 1U> output{};
    const auto size = format_legacy_discovery("02:00:00:00:00:07", "Creators Ball V2",
                                              "Fixture BLIP", "1.2.0", output);
    BLIP_CHECK(size);
    const std::string_view discovery_text{output.data(), size.value()};
    BLIP_CHECK(discovery_text == expected);
    BLIP_CHECK(!format_legacy_discovery("02:00:00:00:00:07", "Creators Ball V2", "Fixture BLIP",
                                        "1.2.0",
                                        std::span<char>{output.data(), output.size() - 1U}));
    return true;
}

} // namespace

int main() {
    const std::array tests{
        envelope_round_trip_and_corruption, control_payload_round_trip_and_validation,
        cobs_round_trip_and_errors,         registry_control_dispatch_validates_schema,
        framed_endpoint_controls_registry,  legacy_serial_fixture_maps_without_unsafe_guessing,
    };
    for (const auto test : tests) {
        if (!test()) {
            return 1;
        }
    }
    std::cout << "blip transport tests passed: " << tests.size() << '\n';
    return 0;
}
