#include "blip/wasm/script_manifest.hpp"
#include "test_harness.hpp"
#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace {
using namespace blip::wasm;
using namespace std::string_view_literals;
using blip::core::ErrorCode;
using blip::core::ScalarValue;
using Bytes = std::vector<std::byte>;
void byte(Bytes& out, unsigned value) { out.push_back(static_cast<std::byte>(value)); }
void u32(Bytes& out, std::uint32_t value) {
    do { unsigned b = value & 127U; value >>= 7; byte(out, b | (value ? 128U : 0U)); } while (value);
}
void text(Bytes& out, std::string_view value) {
    u32(out, static_cast<std::uint32_t>(value.size()));
    for (unsigned char c : value) byte(out, c);
}
void bits(Bytes& out, std::uint64_t value) { for (unsigned i = 0; i < 8; ++i) byte(out, static_cast<unsigned>(value >> (i * 8)) & 255U); }
void number(Bytes& out, double value) { bits(out, std::bit_cast<std::uint64_t>(value)); }
Bytes payload(unsigned count) { Bytes out; for (char c : std::string_view("BCM1")) byte(out, c); u32(out, count); return out; }
Bytes module() { return {std::byte{0}, std::byte{0x61}, std::byte{0x73}, std::byte{0x6d}, std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0}}; }
void section(Bytes& out, std::string_view name, const Bytes& data) {
    Bytes body; text(body, name); body.insert(body.end(), data.begin(), data.end());
    byte(out, 0); u32(out, static_cast<std::uint32_t>(body.size())); out.insert(out.end(), body.begin(), body.end());
}
Bytes wrapped(const Bytes& data) { auto out = module(); section(out, kScriptControlsSection, data); return out; }
void parameter(Bytes& out, std::string_view id, ScalarValue value, unsigned access = 2,
               bool bounded = false, double low = 0, double high = 100, double step = 1,
               std::string_view label = "Label", std::string_view unit = "") {
    byte(out, 0); text(out, id); text(out, label); byte(out, static_cast<unsigned>(value.type)); byte(out, access); text(out, unit);
    switch (value.type) {
    case blip::core::ValueType::boolean: byte(out, value.boolean ? 1 : 0); break;
    case blip::core::ValueType::integer: bits(out, std::bit_cast<std::uint64_t>(value.integer)); break;
    case blip::core::ValueType::number: number(out, value.number); break;
    case blip::core::ValueType::string: text(out, value.string); break;
    }
    byte(out, bounded ? 1 : 0);
    if (bounded) { number(out, low); number(out, high); number(out, step); }
}
void action(Bytes& out, std::string_view id, unsigned fields = 0, std::string_view callback = "on_fire") {
    byte(out, 1); text(out, id); text(out, "Fire"); text(out, callback); u32(out, fields);
    for (unsigned i = 0; i < fields; ++i) { text(out, "arg" + std::to_string(i)); byte(out, i % 4); }
}
void event(Bytes& out, std::string_view id, unsigned fields = 0) {
    byte(out, 2); text(out, id); text(out, "Event"); u32(out, fields);
    for (unsigned i = 0; i < fields; ++i) { text(out, "field" + std::to_string(i)); byte(out, i % 4); }
}
Bytes example() {
    auto out = payload(6);
    parameter(out, "armed", ScalarValue::from_bool(true), 0);
    parameter(out, "level", ScalarValue::from_integer(-42), 2, true, -100, 100, 1, "Niveau \xc3\xa9", "%");
    parameter(out, "rate", ScalarValue::from_number(1.25), 1, true, 0, 10, .25, "Rate", "Hz");
    parameter(out, "name", ScalarValue::from_string(std::string_view("A\0\xf0\x9f\x8e\xaf", 6)));
    action(out, "fire", 4); event(out, "changed", 4);
    return wrapped(out);
}
bool unchanged_failure(const Bytes& bytes, ErrorCode expected = ErrorCode::none) {
    ScriptManifest out; BLIP_CHECK(parse_script_manifest(example(), out));
    std::array<std::byte, sizeof(out)> before{}; std::memcpy(before.data(), &out, sizeof(out));
    const auto status = parse_script_manifest(bytes, out);
    BLIP_CHECK(!status); BLIP_CHECK(expected == ErrorCode::none || status.error().code == expected);
    BLIP_CHECK(std::memcmp(before.data(), &out, sizeof(out)) == 0);
    return true;
}
bool all_types_and_projection() {
    ScriptManifest out; BLIP_CHECK(parse_script_manifest(example(), out)); BLIP_CHECK(out.size() == 6);
    BLIP_CHECK(out.default_value(0).boolean && out.default_value(1).integer == -42);
    BLIP_CHECK(out.default_value(2).number == 1.25 && out.default_value(3).string.size() == 6);
    blip::core::ParameterDescriptor p; BLIP_CHECK(out.parameter(1, p));
    BLIP_CHECK(p.id == "level" && p.label == "Niveau \xc3\xa9" && p.unit == "%");
    BLIP_CHECK(p.bounds.present && p.bounds.minimum == -100 && p.bounds.maximum == 100 && !p.persisted);
    BLIP_CHECK(out.parameter(0, p) && p.access == blip::core::Access::read_only);
    BLIP_CHECK(out.parameter(2, p) && p.access == blip::core::Access::write_only);
    std::array<blip::core::FieldDescriptor, 4> fields{};
    blip::core::ActionDescriptor a; BLIP_CHECK(out.action(4, fields, a));
    BLIP_CHECK(a.id == "fire" && a.arguments.size() == 4 && a.arguments[3].type == blip::core::ValueType::string);
    BLIP_CHECK(a.arguments[0].required && out.text(out.control(4).callback) == "on_fire");
    blip::core::EventDescriptor e; BLIP_CHECK(out.event(5, fields, e));
    BLIP_CHECK(e.id == "changed" && e.fields[2].id == "field2");
    return true;
}
bool ownership_and_copy() {
    auto bytes = example(); ScriptManifest original; BLIP_CHECK(parse_script_manifest(bytes, original));
    auto copy = original; std::fill(bytes.begin(), bytes.end(), std::byte{0xff});
    BLIP_CHECK(parse_script_manifest(module(), original) && original.size() == 0);
    blip::core::ParameterDescriptor p; BLIP_CHECK(copy.parameter(1, p));
    BLIP_CHECK(p.id == "level" && p.default_value.integer == -42);
    BLIP_CHECK(copy.default_value(3).string == std::string_view("A\0\xf0\x9f\x8e\xaf", 6));
    return true;
}
bool absent_and_unknown() {
    ScriptManifest out; BLIP_CHECK(parse_script_manifest(example(), out));
    auto bytes = module(); section(bytes, "debug", {std::byte{0xff}, std::byte{0}});
    BLIP_CHECK(parse_script_manifest(bytes, out) && out.size() == 0);
    section(bytes, kScriptControlsSection, payload(0)); section(bytes, "other", {});
    BLIP_CHECK(parse_script_manifest(bytes, out) && out.size() == 0);
    BLIP_CHECK(unchanged_failure({}, ErrorCode::corrupt_data));
    bytes = module(); bytes[4] = std::byte{2}; BLIP_CHECK(unchanged_failure(bytes));
    bytes.resize(16385); BLIP_CHECK(unchanged_failure(bytes, ErrorCode::capacity_exceeded));
    return true;
}
bool every_truncation_is_atomic() {
    const auto bytes = example();
    for (std::size_t size = 9; size < bytes.size(); ++size) {
        BLIP_CHECK(unchanged_failure(Bytes(bytes.begin(), bytes.begin() + size)));
    }
    auto trailing = bytes; byte(trailing, 0); BLIP_CHECK(unchanged_failure(trailing));
    auto data = payload(0); byte(data, 0); BLIP_CHECK(unchanged_failure(wrapped(data)));
    return true;
}
bool names_duplicates_versions() {
    auto data = payload(2); parameter(data, "same", ScalarValue::from_integer(0)); action(data, "same");
    BLIP_CHECK(unchanged_failure(wrapped(data), ErrorCode::duplicate_id));
    for (std::string_view id : std::initializer_list<std::string_view>{"", "Upper", "a/b", "a.b", "a b", "_first", "\xc3\xa9", "a\0b"sv}) {
        data = payload(1); event(data, id); BLIP_CHECK(unchanged_failure(wrapped(data)));
    }
    auto bytes = example(); section(bytes, kScriptControlsSection, payload(0));
    BLIP_CHECK(unchanged_failure(bytes, ErrorCode::duplicate_id));
    for (std::string_view name : std::initializer_list<std::string_view>{"blip.controls.v2", "blip.controls.v01", "blip.controls.v1\0suffix"sv}) {
        bytes = module(); section(bytes, name, payload(0)); BLIP_CHECK(unchanged_failure(bytes, ErrorCode::incompatible_version));
    }
    data = payload(1); byte(data, 1); text(data, "fire"); text(data, "Fire"); text(data, "on_fire"); u32(data, 2);
    text(data, "same"); byte(data, 0); text(data, "same"); byte(data, 1);
    BLIP_CHECK(unchanged_failure(wrapped(data), ErrorCode::duplicate_id));
    return true;
}
bool bounded_capacity() {
    auto data = payload(16); for (unsigned i = 0; i < 16; ++i) action(data, "action" + std::to_string(i));
    ScriptManifest out; BLIP_CHECK(parse_script_manifest(wrapped(data), out) && out.size() == 16);
    data = payload(17); BLIP_CHECK(unchanged_failure(wrapped(data), ErrorCode::capacity_exceeded));
    data = payload(1); action(data, "fire", 5); BLIP_CHECK(unchanged_failure(wrapped(data), ErrorCode::capacity_exceeded));
    data = payload(16);
    for (unsigned i = 0; i < 16; ++i) parameter(data, "param" + std::to_string(i), ScalarValue::from_string(std::string(128, 'x')), 2, false, 0, 0, 1, std::string(64, 'L'));
    BLIP_CHECK(unchanged_failure(wrapped(data), ErrorCode::capacity_exceeded));
    data = payload(0); data.resize(4097); BLIP_CHECK(unchanged_failure(wrapped(data), ErrorCode::capacity_exceeded));
    data = payload(1); event(data, std::string(33, 'a')); BLIP_CHECK(unchanged_failure(wrapped(data), ErrorCode::capacity_exceeded));
    data = payload(1); action(data, "fire", 0, std::string(65, 'a')); BLIP_CHECK(unchanged_failure(wrapped(data), ErrorCode::capacity_exceeded));
    return true;
}
bool invalid_scalar_and_metadata() {
    for (double value : {std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        auto data = payload(1); parameter(data, "rate", ScalarValue::from_number(value)); BLIP_CHECK(unchanged_failure(wrapped(data)));
    }
    for (const auto bounds : {std::array{2., 1., 1.}, {0., 1., 0.}, {0., 1., -1.}, {0., 1., std::numeric_limits<double>::infinity()}, {0., std::numeric_limits<double>::infinity(), 1.}}) {
        auto data = payload(1); parameter(data, "rate", ScalarValue::from_number(.5), 2, true, bounds[0], bounds[1], bounds[2]);
        BLIP_CHECK(unchanged_failure(wrapped(data)));
    }
    auto data = payload(1); parameter(data, "level", ScalarValue::from_integer(2), 2, true, 0, 1, 1); BLIP_CHECK(unchanged_failure(wrapped(data)));
    data = payload(1); parameter(data, "level", ScalarValue::from_integer(0), 2, true, 0, 1.5, 1); BLIP_CHECK(unchanged_failure(wrapped(data)));
    data = payload(1); parameter(data, "level", ScalarValue::from_integer(INT64_MAX), 2, true, 0, 0x1p63, 1); BLIP_CHECK(unchanged_failure(wrapped(data)));
    data = payload(1); parameter(data, "enabled", ScalarValue::from_bool(true), 2, true); BLIP_CHECK(unchanged_failure(wrapped(data)));
    data = payload(1); parameter(data, "label", ScalarValue::from_string("\xc0\x80")); BLIP_CHECK(unchanged_failure(wrapped(data)));
    data = payload(1); parameter(data, "level", ScalarValue::from_integer(0), 3); BLIP_CHECK(unchanged_failure(wrapped(data)));
    data = payload(1); parameter(data, "level", ScalarValue::from_integer(0), 2, false, 0, 0, 1, "line\nfeed"); BLIP_CHECK(unchanged_failure(wrapped(data)));
    data = payload(1); action(data, "fire", 0, "bad/name"); BLIP_CHECK(unchanged_failure(wrapped(data)));
    return true;
}
bool malformed_wire() {
    auto bytes = module(); byte(bytes, 0); for (unsigned i = 0; i < 5; ++i) byte(bytes, 0x80); BLIP_CHECK(unchanged_failure(bytes));
    bytes = module(); byte(bytes, 0); for (unsigned i = 0; i < 4; ++i) byte(bytes, 0xff); byte(bytes, 0x10); BLIP_CHECK(unchanged_failure(bytes));
    auto data = payload(0); data[3] = std::byte{2}; BLIP_CHECK(unchanged_failure(wrapped(data), ErrorCode::incompatible_version));
    data = payload(1); byte(data, 3); BLIP_CHECK(unchanged_failure(wrapped(data)));
    bytes = module(); section(bytes, "\xc0\x80", {}); BLIP_CHECK(unchanged_failure(bytes));
    data = payload(1); byte(data, 0); text(data, "enabled"); text(data, "Enabled"); byte(data, 0); byte(data, 2); text(data, ""); byte(data, 2); byte(data, 0);
    BLIP_CHECK(unchanged_failure(wrapped(data)));
    return true;
}
bool projection_failure_atomicity() {
    ScriptManifest out; BLIP_CHECK(parse_script_manifest(example(), out));
    blip::core::ParameterDescriptor p; p.id = "sentinel"; BLIP_CHECK(!out.parameter(4, p) && p.id == "sentinel");
    BLIP_CHECK(!out.parameter(999, p) && p.id == "sentinel");
    std::array<blip::core::FieldDescriptor, 4> fields{}; fields[0].id = "sentinel";
    blip::core::ActionDescriptor a; a.id = "sentinel"; BLIP_CHECK(!out.action(4, std::span(fields).first(3), a));
    BLIP_CHECK(a.id == "sentinel" && fields[0].id == "sentinel");
    blip::core::EventDescriptor e; e.id = "sentinel"; BLIP_CHECK(!out.event(4, fields, e));
    BLIP_CHECK(e.id == "sentinel" && fields[0].id == "sentinel");
    BLIP_CHECK(out.text({2048, 1}).empty());
    return true;
}
bool numeric_precision_and_signed_zero() {
    auto data = payload(4);
    parameter(data, "low", ScalarValue::from_integer(INT64_MIN));
    parameter(data, "high", ScalarValue::from_integer(INT64_MAX));
    parameter(data, "zero", ScalarValue::from_number(-0.));
    parameter(data, "exact", ScalarValue::from_integer(-9007199254740991LL), 2, true, -9007199254740991., 9007199254740991., 1);
    ScriptManifest out; BLIP_CHECK(parse_script_manifest(wrapped(data), out));
    BLIP_CHECK(out.default_value(0).integer == INT64_MIN && out.default_value(1).integer == INT64_MAX);
    BLIP_CHECK(std::bit_cast<std::uint64_t>(out.default_value(2).number) == (std::uint64_t{1} << 63));
    BLIP_CHECK(out.default_value(3).integer == -9007199254740991LL);
    data = payload(1); parameter(data, "exact", ScalarValue::from_integer(0), 2, true, 0, 9007199254740992., 1);
    BLIP_CHECK(unchanged_failure(wrapped(data)));
    return true;
}
bool exact_text_arena_boundary() {
    const auto make = [](unsigned default_bytes) {
        auto data = payload(10);
        for (unsigned i = 0; i < 9; ++i) {
            byte(data, 2); text(data, std::string(31, 'e') + static_cast<char>('a' + i)); text(data, std::string(64, 'L')); u32(data, 4);
            for (unsigned f = 0; f < 4; ++f) { text(data, std::string(31, 'f') + static_cast<char>('a' + f)); byte(data, 1); }
        }
        parameter(data, "z", ScalarValue::from_string(std::string(default_bytes, 'x')), 2, false, 0, 0, 1, "Z");
        return wrapped(data);
    };
    ScriptManifest out; BLIP_CHECK(parse_script_manifest(make(30), out) && out.size() == 10);
    BLIP_CHECK(out.default_value(9).string == std::string(30, 'x'));
    BLIP_CHECK(unchanged_failure(make(31), ErrorCode::capacity_exceeded));
    auto data = payload(1);
    std::string unicode; for (unsigned i = 0; i < 64; ++i) unicode += "\xc3\xa9";
    parameter(data, std::string(32, 'a'), ScalarValue::from_string(unicode), 2, false, 0, 0, 1, std::string(64, 'L'), std::string(16, 'u'));
    BLIP_CHECK(parse_script_manifest(wrapped(data), out));
    BLIP_CHECK(out.default_value(0).string == unicode && out.default_value(0).string.size() == 128);
    data = payload(1); parameter(data, "name", ScalarValue::from_string(std::string(129, 'x')));
    BLIP_CHECK(unchanged_failure(wrapped(data), ErrorCode::capacity_exceeded));
    return true;
}
bool all_types_and_field_tags_are_checked() {
    auto data = payload(1); byte(data, 0); text(data, "level"); text(data, "Level"); byte(data, 4);
    BLIP_CHECK(unchanged_failure(wrapped(data)));
    data = payload(1); byte(data, 2); text(data, "changed"); text(data, "Changed"); u32(data, 1); text(data, "value"); byte(data, 4);
    BLIP_CHECK(unchanged_failure(wrapped(data)));
    auto bytes = module(); byte(bytes, 0); byte(bytes, 1); byte(bytes, 0x80);
    BLIP_CHECK(unchanged_failure(bytes));
    return true;
}
} // namespace
int blip_manifest_run_tests() {
    const TestCase tests[]{
        {"all scalar types and descriptor projection", all_types_and_projection},
        {"owned text survives module overwrite and schema copy", ownership_and_copy},
        {"absent and unknown sections", absent_and_unknown},
        {"all truncations leave schema unchanged", every_truncation_is_atomic},
        {"names duplicate controls fields and versions", names_duplicates_versions},
        {"bounded schema text and wire capacity", bounded_capacity},
        {"invalid scalar bounds UTF-8 and metadata", invalid_scalar_and_metadata},
        {"malformed module and declaration framing", malformed_wire},
        {"projection failure preserves descriptors and fields", projection_failure_atomicity},
        {"full int64 precision exact bounds and signed zero", numeric_precision_and_signed_zero},
        {"exact text arena and UTF-8 byte limits", exact_text_arena_boundary},
        {"all scalar and field tags are checked", all_types_and_field_tags_are_checked}
    };
    return run_tests(tests);
}
#ifndef ESP_PLATFORM
int main() { return blip_manifest_run_tests(); }
#endif
