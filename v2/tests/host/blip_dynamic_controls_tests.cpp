#include "blip/core/control.hpp"
#include "blip/oscquery/oscquery.hpp"
#include "blip/oscquery/legacy_osc.hpp"
#include "test_harness.hpp"
#include <array>
#include <cstring>
#include <string>
#include <utility>

namespace {
using namespace blip::core;
class Script final : public Component, public DynamicSchemaSource {
  public:
    Script() {
        descriptor_.id = "test.script"; descriptor_.display_name = "Script";
        descriptor_.schema_version = 1; descriptor_.settings = {1, 1};
        std::memcpy(message.data(), "hello", 5);
    }
    const ComponentDescriptor& descriptor() const noexcept override { return descriptor_; }
    const DynamicSchemaSource* dynamic_schema() const noexcept override { return this; }
    Status start(const StartContext&) noexcept override { return Status::success(); }
    Status stop() noexcept override { active = false; return Status::success(); }
    Status acquire(DynamicSchemaLease& lease) const noexcept override {
        if (busy) return fail(ErrorCode::resource_unavailable);
        if (lease.held()) return fail(ErrorCode::invalid_state);
        if (!active) return Status::success();
        auto status = lease.bind(*this, generation, 4); if (status) ++pins; return status;
    }
    void release(std::uint32_t token) const noexcept override { if (token == generation && pins) --pins; else ++bad_releases; }
    std::string_view id(std::size_t i) const noexcept override { return ids[i]; }
    DynamicControlKind kind(std::size_t i) const noexcept override {
        return i < 2 ? DynamicControlKind::parameter : i == 2 ? DynamicControlKind::action : DynamicControlKind::event;
    }
    Status parameter(std::size_t i, ParameterDescriptor& out) const noexcept override {
        if (i >= 2) return fail(ErrorCode::invalid_argument);
        if (i == 0) out = {"level", "Intensity", ValueType::integer, Access::read_write, false,
                          ScalarValue::from_integer(5), {true, 0, 100, 1}, "%"};
        else out = {"note", "Note", ValueType::string, access, false, ScalarValue::from_string("hello")};
        return Status::success();
    }
    Status action(std::size_t i, std::span<FieldDescriptor> scratch, ActionDescriptor& out) const noexcept override {
        if (i != 2 || scratch.size() < 2) return fail(ErrorCode::invalid_argument);
        scratch[0] = {"armed", ValueType::boolean, true}; scratch[1] = {"text", ValueType::string, true};
        out = {"fire", "Fire script", scratch.first(2)}; return Status::success();
    }
    Status event(std::size_t i, std::span<FieldDescriptor> scratch, EventDescriptor& out) const noexcept override {
        if (i != 3 || scratch.size() < 2) return fail(ErrorCode::invalid_argument);
        scratch[0] = {"value", ValueType::integer, true}; scratch[1] = {"text", ValueType::string, true};
        out = {"changed", scratch.first(2)}; return Status::success();
    }
    Status read_dynamic_parameter(std::uint32_t token, std::string_view id, ScalarValue& out, std::span<char> scratch) noexcept override {
        if (!active || token != generation) return fail(ErrorCode::cancelled);
        ++reads;
        if (id == "level") out = ScalarValue::from_integer(level);
        else if (id == "note") {
            if (scratch.size() < message_size) return fail(ErrorCode::capacity_exceeded);
            std::memcpy(scratch.data(), message.data(), message_size);
            out = ScalarValue::from_string(borrow_wrongly ? "unowned" : std::string_view(scratch.data(), message_size));
        } else return fail(ErrorCode::not_found);
        if (wrong_type) out = ScalarValue::from_bool(false);
        return Status::success();
    }
    Status write_dynamic_parameter(std::uint32_t token, std::string_view id, const ScalarValue& value) noexcept override {
        if (!active || token != generation) return fail(ErrorCode::cancelled);
        ++writes;
        if (id == "level") level = value.integer;
        else if (id == "note") {
            if (value.string.size() > message.size()) return fail(ErrorCode::capacity_exceeded);
            std::memcpy(message.data(), value.string.data(), value.string.size()); message_size = value.string.size();
        } else return fail(ErrorCode::not_found);
        return Status::success();
    }
    Status invoke_dynamic_action(std::uint32_t token, std::string_view id, std::span<const ScalarValue> values,
        std::span<ScalarValue> out, std::span<char>, std::size_t& count) noexcept override {
        count = 0;
        if (!active || token != generation) return fail(ErrorCode::cancelled);
        if (id != "fire" || values.size() != 2 || out.empty()) return fail(ErrorCode::invalid_argument);
        ++actions; observed_generation = token; observed_text.assign(values[1].string);
        out[0] = ScalarValue::from_integer(123); count = bad_count ? 99 : 1;
        return Status::success();
    }
    bool replace() noexcept { if (pins) return false; ++generation; return true; }
    static Status fail(ErrorCode code) noexcept { return Status::failure({ErrorDomain::control, code, "test.script", "test", "test-failure"}); }
    ComponentDescriptor descriptor_{};
    inline static constexpr std::array<std::string_view, 4> ids{"level", "note", "fire", "changed"};
    std::array<char, 128> message{}; std::size_t message_size{5}; std::int64_t level{5};
    mutable unsigned pins{}, bad_releases{};
    std::uint32_t generation{1}, observed_generation{};
    unsigned reads{}, writes{}, actions{};
    bool active{true}, busy{}, wrong_type{}, borrow_wrongly{}, bad_count{};
    Access access{Access::read_write}; std::string observed_text;
};
struct Setup {
    Script script;
    Registry<2> registry;
    RegistryControlService<2> controls{registry};
    bool start() { return registry.add(script) && registry.add(controls) && registry.validate() && registry.start_all().ok(); }
};
bool lease_lifetime() {
    Setup s; BLIP_CHECK(s.start());
    DynamicSchemaLease lease; BLIP_CHECK(s.registry.acquire_dynamic_schema("test.script", lease));
    BLIP_CHECK(lease.size() == 4 && lease.generation() == 1 && s.script.pins == 1);
    BLIP_CHECK(!s.script.replace());
    auto moved = std::move(lease); BLIP_CHECK(!lease.held() && moved.held() && s.script.pins == 1);
    BLIP_CHECK(!s.registry.acquire_dynamic_schema("test.script", moved));
    moved.reset(); BLIP_CHECK(s.script.pins == 0 && s.script.bad_releases == 0 && s.script.replace());
    BLIP_CHECK(s.registry.acquire_dynamic_schema("test.script", lease) && lease.generation() == 2);
    lease.reset(); s.script.active = false;
    BLIP_CHECK(s.registry.acquire_dynamic_schema("test.script", lease) && !lease.held());
    s.script.busy = true; BLIP_CHECK(!s.registry.acquire_dynamic_schema("test.script", lease));
    BLIP_CHECK(!s.registry.acquire_dynamic_schema("missing.component", lease));
    BLIP_CHECK(!lease.bind(s.script, 0, 1) && !lease.bind(s.script, 1, 17));
    return true;
}
bool typed_dispatch_and_generation() {
    Setup s; BLIP_CHECK(s.start()); ControlResponse response;
    BLIP_CHECK(s.controls.execute({ControlOperation::read_parameter, "test.script", "level", {}}, response));
    BLIP_CHECK(response.value_count == 1 && response.values[0].integer == 5 && s.script.pins == 0);
    std::array values{ScalarValue::from_integer(73)};
    BLIP_CHECK(s.controls.execute({ControlOperation::write_parameter, "test.script", "level", values}, response));
    BLIP_CHECK(s.script.level == 73 && s.script.writes == 1);
    values[0] = ScalarValue::from_integer(101);
    BLIP_CHECK(!s.controls.execute({ControlOperation::write_parameter, "test.script", "level", values}, response));
    BLIP_CHECK(s.script.writes == 1 && response.value_count == 0);
    values[0] = ScalarValue::from_bool(true);
    BLIP_CHECK(!s.controls.execute({ControlOperation::write_parameter, "test.script", "level", values}, response));
    BLIP_CHECK(s.script.writes == 1);
    std::array args{ScalarValue::from_bool(true), ScalarValue::from_string("owned action")};
    BLIP_CHECK(s.controls.execute({ControlOperation::invoke_action, "test.script", "fire", args}, response));
    BLIP_CHECK(s.script.actions == 1 && s.script.observed_generation == 1 && response.values[0].integer == 123);
    args[0] = ScalarValue::from_integer(1);
    BLIP_CHECK(!s.controls.execute({ControlOperation::invoke_action, "test.script", "fire", args}, response));
    BLIP_CHECK(s.script.actions == 1);
    BLIP_CHECK(s.script.replace());
    args[0] = ScalarValue::from_bool(true);
    const auto stale = s.controls.execute({ControlOperation::invoke_action, "test.script", "fire", args, 1}, response);
    BLIP_CHECK(!stale && stale.error().code == ErrorCode::cancelled && s.script.actions == 1);
    BLIP_CHECK(s.controls.execute({ControlOperation::invoke_action, "test.script", "fire", args, 2}, response));
    BLIP_CHECK(s.script.actions == 2 && s.script.observed_generation == 2);
    s.script.bad_count = true;
    BLIP_CHECK(!s.controls.execute({ControlOperation::invoke_action, "test.script", "fire", args}, response) && response.value_count == 0);
    s.script.wrong_type = true;
    BLIP_CHECK(!s.controls.execute({ControlOperation::read_parameter, "test.script", "level", {}}, response) && response.value_count == 0);
    return true;
}
bool owned_string_response() {
    Setup s; BLIP_CHECK(s.start()); ControlResponse response;
    BLIP_CHECK(s.controls.execute({ControlOperation::read_parameter, "test.script", "note", {}}, response));
    BLIP_CHECK(response.values[0].string == "hello");
    auto copy = response; auto moved = std::move(response); copy = moved;
    s.script.message.fill('x');
    BLIP_CHECK(copy.values[0].string == "hello" && moved.values[0].string == "hello");
    s.script.borrow_wrongly = true;
    BLIP_CHECK(!s.controls.execute({ControlOperation::read_parameter, "test.script", "note", {}}, response));
    BLIP_CHECK(response.value_count == 0);
    s.script.borrow_wrongly = false; s.script.access = Access::write_only;
    const auto reads = s.script.reads;
    BLIP_CHECK(!s.controls.execute({ControlOperation::read_parameter, "test.script", "note", {}}, response));
    BLIP_CHECK(s.script.reads == reads);
    s.script.access = Access::read_only;
    std::array input{ScalarValue::from_string("new")};
    BLIP_CHECK(!s.controls.execute({ControlOperation::write_parameter, "test.script", "note", input}, response));
    BLIP_CHECK(s.script.writes == 0);
    return true;
}
class Sink final : public blip::oscquery::TextSink {
  public:
    bool write(std::string_view value) noexcept override {
        if (script && script->pins) { attempted = true; if (script->replace()) return false; }
        bytes.append(value); return true;
    }
    Script* script{}; bool attempted{}; std::string bytes;
};
bool oscquery_uses_leased_schema() {
    Setup s; BLIP_CHECK(s.start()); Sink sink; sink.script = &s.script;
    BLIP_CHECK(blip::oscquery::write_oscquery_tree(s.registry, s.controls, false, sink));
    BLIP_CHECK(sink.attempted && s.script.pins == 0 && s.script.bad_releases == 0);
    BLIP_CHECK(sink.bytes.find("\"DESCRIPTION\":\"Intensity\"") != std::string::npos);
    BLIP_CHECK(sink.bytes.find("\"FULL_PATH\":\"/test/script/level\"") != std::string::npos);
    BLIP_CHECK(sink.bytes.find("\"VALUE\":[\"hello\"]") != std::string::npos);
    BLIP_CHECK(sink.bytes.find("\"TYPE\":\"bs\"") != std::string::npos && sink.bytes.find("\"TYPE\":\"is\"") != std::string::npos);
    BLIP_CHECK(sink.bytes.find("\"BLIP_SCHEMA_GENERATION\":1") != std::string::npos);
    s.script.active = false; Sink empty;
    BLIP_CHECK(blip::oscquery::write_oscquery_tree(s.registry, s.controls, false, empty));
    BLIP_CHECK(empty.bytes.find("/test/script/level") == std::string::npos);
    return true;
}
bool osc_routes_dynamic_and_retains_feedback() {
    Setup s; BLIP_CHECK(s.start());
    blip::oscquery::LegacyOscEndpoint endpoint(s.registry, s.controls, {"id", "type", "name", "version"});
    blip::oscquery::OscMessage request{}, reply{}; bool should_reply{};
    request.address = "/test/script/note";
    BLIP_CHECK(endpoint.handle(request, "127.0.0.1", false, reply, should_reply));
    BLIP_CHECK(should_reply && reply.argument_count == 1 && reply.arguments[0].string == "hello");
    s.script.message.fill('x'); BLIP_CHECK(reply.arguments[0].string == "hello");
    request.address = "/test/script/fire"; request.argument_count = 2;
    request.arguments[0] = blip::oscquery::OscValue::from_bool(true);
    request.arguments[1] = blip::oscquery::OscValue::from_string("OSC action");
    BLIP_CHECK(endpoint.handle(request, "127.0.0.1", false, reply, should_reply));
    BLIP_CHECK(s.script.actions == 1 && s.script.observed_text == "OSC action");
    // The endpoint resolves kind/generation; the dispatcher still validates
    // the leased action fields before the script can observe the request.
    request.arguments[0] = blip::oscquery::OscValue::from_integer(1);
    const auto invalid = endpoint.handle(request, "127.0.0.1", true, reply, should_reply);
    BLIP_CHECK(!invalid && invalid.error().code == ErrorCode::validation_failed);
    BLIP_CHECK(s.script.actions == 1);
    request.address = "/test/script/changed"; request.argument_count = 0;
    BLIP_CHECK(!endpoint.handle(request, "127.0.0.1", false, reply, should_reply));
    return true;
}
bool response_capacity_copy_and_aliasing() {
    ControlResponse response;
    std::memcpy(response.string_storage.data(), "abcdef", 6);
    response.values[0] = ScalarValue::from_string({response.string_storage.data() + 3, 3});
    response.values[1] = ScalarValue::from_string("external");
    response.values[2] = ScalarValue::from_string({response.string_storage.data(), 3});
    response.value_count = 3;
    BLIP_CHECK(response.own_strings());
    BLIP_CHECK(response.values[0].string == "def" && response.values[1].string == "external" && response.values[2].string == "abc");
    auto copy = response; response = {};
    BLIP_CHECK(copy.values[0].string == "def" && copy.values[1].string == "external" && copy.values[2].string == "abc");
    std::array<char, kControlResponseStringBytes + 1> large{}; large.fill('a');
    response.values[0] = ScalarValue::from_string({large.data(), kControlResponseStringBytes}); response.value_count = 1;
    BLIP_CHECK(response.own_strings() && response.values[0].string.size() == kControlResponseStringBytes);
    response.values[1] = ScalarValue::from_string("x"); response.value_count = 2;
    BLIP_CHECK(!response.own_strings());
    response.value_count = 99; BLIP_CHECK(!response.own_strings());
    return true;
}
} // namespace
int blip_dynamic_controls_run_tests() {
    const TestCase tests[]{
        {"schema lease pins generation and releases exactly once", lease_lifetime},
        {"dynamic typed dispatch validates before admission", typed_dispatch_and_generation},
        {"dynamic string responses survive copies owner mutation and retirement", owned_string_response},
        {"OSCQuery emits one leased typed schema generation", oscquery_uses_leased_schema},
        {"OSC routes dynamic controls and owns returned feedback", osc_routes_dynamic_and_retains_feedback},
        {"response arena capacity deep copies and overlapping source slices", response_capacity_copy_and_aliasing}
    };
    return run_tests(tests);
}
#ifndef ESP_PLATFORM
int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--dump-tree") {
        Setup setup; if (!setup.start()) return 1; Sink sink;
        if (!blip::oscquery::write_oscquery_tree(setup.registry, setup.controls, false, sink)) return 1;
        std::fwrite(sink.bytes.data(), 1, sink.bytes.size(), stdout); return 0;
    }
    return blip_dynamic_controls_run_tests();
}
#endif
