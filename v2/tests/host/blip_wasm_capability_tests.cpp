#include "blip/wasm/capability.hpp"
#include "blip/wasm/module_policy.hpp"
#include "blip/core/descriptor_json.hpp"
#include "test_harness.hpp"
#include <array>
#include <cstring>
#include <limits>

namespace {
using namespace blip::wasm;
using blip::core::ErrorCode;
using blip::core::Status;
using blip::core::WasmArgumentDescriptor;
using blip::core::WasmArgumentRole;
using blip::core::WasmFunctionDescriptor;
Status failure(ErrorCode code) noexcept {
    return Status::failure({blip::core::ErrorDomain::control, code, "test.alpha", "provider", "test-error"});
}
constexpr std::array numeric_args{WasmArgumentDescriptor{"value", ValueType::i32}};
constexpr std::array i32_result{ValueType::i32};
constexpr std::array text_args{
    WasmArgumentDescriptor{"text_offset", ValueType::i32, WasmArgumentRole::utf8_offset},
    WasmArgumentDescriptor{"text_bytes", ValueType::i32, WasmArgumentRole::utf8_length},
    WasmArgumentDescriptor{"destination", ValueType::i32}};
constexpr std::array functions{
    WasmFunctionDescriptor{"echo", "Echo raw bits", numeric_args, i32_result, 100},
    WasmFunctionDescriptor{"text", "Checked UTF-8 copy", text_args, i32_result, 500}};
constexpr blip::core::ComponentDescriptor make_descriptor(std::string_view id, std::string_view module,
    std::span<const WasmFunctionDescriptor> items = functions) noexcept {
    blip::core::ComponentDescriptor out{};
    out.schema_version = 1; out.id = id; out.display_name = id; out.settings = {1, 1};
    out.supports_resume = true; out.supports_restart = true;
    if (!items.empty()) out.wasm = {1, module, items};
    return out;
}
class Memory final : public GuestMemory {
  public:
    std::array<std::byte, 512> bytes{};
    Status read_memory(std::uint32_t offset, std::span<std::byte> out) noexcept override {
        if (offset > bytes.size() || out.size() > bytes.size() - offset) return failure(ErrorCode::invalid_argument);
        if (!out.empty()) std::memmove(out.data(), bytes.data() + offset, out.size());
        return Status::success();
    }
    Status write_memory(std::uint32_t offset, std::span<const std::byte> in) noexcept override {
        if (offset > bytes.size() || in.size() > bytes.size() - offset) return failure(ErrorCode::invalid_argument);
        if (!in.empty()) std::memmove(bytes.data() + offset, in.data(), in.size());
        return Status::success();
    }
};
class TestComponent final : public blip::core::Component, public CapabilityProvider {
  public:
    TestComponent() noexcept : data(make_descriptor("test.alpha", "test.alpha.v1")) {}
    explicit TestComponent(blip::core::ComponentDescriptor d) noexcept : data(d) {}
    const blip::core::ComponentDescriptor& descriptor() const noexcept override { return data; }
    CapabilityProvider* wasm_provider() noexcept override { return expose ? this : nullptr; }
    Status start(const blip::core::StartContext&) noexcept override { enabled.store(true); return Status::success(); }
    Status suspend() noexcept override { enabled.store(false); return Status::success(); }
    Status resume() noexcept override { enabled.store(true); return Status::success(); }
    Status stop() noexcept override { enabled.store(false); return Status::success(); }
    bool available() const noexcept override { return enabled.load(); }
    Status invoke(std::string_view name, CallContext& context, std::span<const Value> args,
        std::span<Value> out, std::size_t& count) noexcept override {
        ++calls;
        count = 0;
        if (reenter) {
            Memory memory; std::array<Value, 1> inner{}; std::size_t n{};
            return reenter->invoke(0, memory, args, inner, n);
        }
        if (fail) { out[0] = Value::i32(0); count = 1; return failure(ErrorCode::queue_full); }
        if (name == "text") {
            std::array<char, kMaximumStringBytes> text{}; std::size_t n{};
            auto s = context.read_utf8({static_cast<std::uint32_t>(args[0].bits), static_cast<std::uint32_t>(args[1].bits)}, text, n);
            if (!s) return s;
            s = context.write_utf8(static_cast<std::uint32_t>(args[2].bits), {text.data(), n});
            if (!s) return s;
            out[0] = Value::i32(static_cast<std::uint32_t>(n));
        } else if (!out.empty()) out[0] = args[0];
        if (wrong_type) out[0] = Value::f64(1.5);
        if (wide_bits) out[0].bits = std::uint64_t{1} << 32;
        count = bad_count ? 99 : out.size();
        if (cancel_after) cancel_after->store(true);
        return Status::success();
    }
    blip::core::ComponentDescriptor data;
    std::atomic<bool> enabled{};
    bool expose{true}, fail{}, wrong_type{}, wide_bits{}, bad_count{};
    std::uint32_t calls{};
    CapabilityRegistry* reenter{};
    std::atomic<bool>* cancel_after{};
};

bool metadata_validation() {
    auto d = make_descriptor("test.alpha", "test.alpha.v1");
    BLIP_CHECK(blip::core::valid_wasm_descriptor(d.id, d.wasm));
    auto c = d.wasm;
    c.import_module = "test.other.v1"; BLIP_CHECK(!blip::core::valid_wasm_descriptor(d.id, c));
    c.import_module = "test.alpha.v01"; BLIP_CHECK(!blip::core::valid_wasm_descriptor(d.id, c));
    c.import_module = "test.alpha.v2"; BLIP_CHECK(!blip::core::valid_wasm_descriptor(d.id, c));
    c = d.wasm; c.abi_version = 0; BLIP_CHECK(!blip::core::valid_wasm_descriptor(d.id, c));
    c.abi_version = std::numeric_limits<std::uint32_t>::max(); c.import_module = "test.alpha.v4294967295";
    BLIP_CHECK(blip::core::valid_wasm_descriptor(d.id, c));
    c = {}; BLIP_CHECK(blip::core::valid_wasm_descriptor(d.id, c));
    c.import_module = d.wasm.import_module; BLIP_CHECK(!blip::core::valid_wasm_descriptor(d.id, c));
    c = d.wasm; c.functions = {}; BLIP_CHECK(!blip::core::valid_wasm_descriptor(d.id, c));
    std::array<WasmFunctionDescriptor, 9> f{};
    f.fill(functions[0]); c = d.wasm; c.functions = f;
    BLIP_CHECK(!blip::core::valid_wasm_descriptor(d.id, c));
    c.functions = std::span(f).first(2); BLIP_CHECK(!blip::core::valid_wasm_descriptor(d.id, c));
    c.functions = std::span(f).first(1);
    f[0].id = "bad/name"; BLIP_CHECK(!blip::core::valid_wasm_descriptor(d.id, c));
    f[0] = functions[0]; f[0].maximum_call_us = 0; BLIP_CHECK(!blip::core::valid_wasm_descriptor(d.id, c));
    f[0].maximum_call_us = blip::core::kMaximumWasmNativeCallUs + 1; BLIP_CHECK(!blip::core::valid_wasm_descriptor(d.id, c));
    f[0] = functions[0]; constexpr std::array many_results{ValueType::i32, ValueType::i64};
    f[0].results = many_results; BLIP_CHECK(!blip::core::valid_wasm_descriptor(d.id, c));
    f[0] = functions[0]; const std::array invalid_results{static_cast<ValueType>(255)};
    f[0].results = invalid_results; BLIP_CHECK(!blip::core::valid_wasm_descriptor(d.id, c));
    std::array<char, 257> long_description{}; f[0] = functions[0];
    f[0].description = {long_description.data(), long_description.size()};
    BLIP_CHECK(!blip::core::valid_wasm_descriptor(d.id, c));
    const auto corrupt = make_descriptor("test.alpha bad", "test.alpha bad.v1");
    BLIP_CHECK(!blip::core::valid_wasm_descriptor(corrupt.id, corrupt.wasm));
    return true;
}
bool argument_validation() {
    auto c = make_descriptor("test.alpha", "test.alpha.v1").wasm;
    std::array<WasmArgumentDescriptor, 9> args{}; args.fill(numeric_args[0]);
    auto f = functions[0]; f.arguments = args; c.functions = {&f, 1};
    BLIP_CHECK(!blip::core::valid_wasm_descriptor("test.alpha", c));
    f.arguments = std::span(args).first(2);
    BLIP_CHECK(!blip::core::valid_wasm_descriptor("test.alpha", c)); // Duplicate IDs.
    args[1].id = "other"; BLIP_CHECK(blip::core::valid_wasm_descriptor("test.alpha", c));
    args[0].role = WasmArgumentRole::utf8_offset;
    BLIP_CHECK(!blip::core::valid_wasm_descriptor("test.alpha", c));
    args[1].role = WasmArgumentRole::utf8_length;
    BLIP_CHECK(blip::core::valid_wasm_descriptor("test.alpha", c));
    args[1].type = ValueType::i64; BLIP_CHECK(!blip::core::valid_wasm_descriptor("test.alpha", c));
    args[1].type = ValueType::i32; args[0].type = ValueType::f32;
    BLIP_CHECK(!blip::core::valid_wasm_descriptor("test.alpha", c));
    args[0] = numeric_args[0]; args[1] = {"other", ValueType::i32, WasmArgumentRole::utf8_length};
    BLIP_CHECK(!blip::core::valid_wasm_descriptor("test.alpha", c));
    args[0].type = static_cast<ValueType>(255); args[1].role = WasmArgumentRole::scalar;
    BLIP_CHECK(!blip::core::valid_wasm_descriptor("test.alpha", c));
    args[0].type = ValueType::i32; args[0].role = static_cast<WasmArgumentRole>(255);
    BLIP_CHECK(!blip::core::valid_wasm_descriptor("test.alpha", c));
    return true;
}
bool owner_validation() {
    {
        blip::core::Registry<1> r; TestComponent c; c.expose = false;
        BLIP_CHECK(r.add(c)); BLIP_CHECK(!r.validate()); BLIP_CHECK(c.calls == 0);
    }
    {
        blip::core::Registry<1> r; TestComponent c{make_descriptor("test.alpha", "", {})};
        BLIP_CHECK(r.add(c)); BLIP_CHECK(!r.validate());
    }
    {
        blip::core::Registry<1> r; TestComponent c; c.data.wasm.import_module = "test.other.v1";
        BLIP_CHECK(r.add(c)); BLIP_CHECK(!r.validate());
    }
    return true;
}
bool bind_and_dispatch() {
    blip::core::Registry<2> r;
    TestComponent beta{make_descriptor("test.beta", "test.beta.v1")}, alpha;
    BLIP_CHECK(r.add(beta)); BLIP_CHECK(r.add(alpha)); BLIP_CHECK(r.validate());
    CapabilityRegistry c; BLIP_CHECK(c.bind(r)); BLIP_CHECK(c.size() == 4);
    BLIP_CHECK(c.binding(0).component == &alpha.data); BLIP_CHECK(c.binding(0).function == &functions[0]);
    BLIP_CHECK(c.binding(2).provider == &beta); BLIP_CHECK(!c.bind(r));
    BLIP_CHECK(c.resolve("test.beta.v1", "echo").value() == 2);
    BLIP_CHECK(!c.resolve("test.beta.v2", "echo")); BLIP_CHECK(!c.resolve("env", "echo"));
    BLIP_CHECK(!c.resolve("test.beta.v1", "missing"));
    Memory memory; constexpr std::array args{Value::i32(0xffffffffU)};
    std::array<Value, 1> out{Value::i32(42)}; std::size_t n = 9;
    BLIP_CHECK(c.invoke(0, memory, args, out, n).error().code == ErrorCode::resource_unavailable);
    BLIP_CHECK(n == 0 && alpha.calls == 0 && out[0].bits == 42);
    BLIP_CHECK(r.start_all().ok()); BLIP_CHECK(c.invoke(0, memory, args, out, n));
    BLIP_CHECK(n == 1 && out[0].bits == 0xffffffffU && alpha.calls == 1 && beta.calls == 0);
    BLIP_CHECK(c.invoke(2, memory, args, out, n)); BLIP_CHECK(beta.calls == 1);
    BLIP_CHECK(r.suspend_all()); BLIP_CHECK(!c.invoke(0, memory, args, out, n));
    BLIP_CHECK(r.resume_all()); BLIP_CHECK(c.invoke(0, memory, args, out, n));
    BLIP_CHECK(r.stop_all()); BLIP_CHECK(!c.invoke(0, memory, args, out, n));
    return true;
}
bool invocation_guards() {
    blip::core::Registry<1> r; TestComponent owner; BLIP_CHECK(r.add(owner)); BLIP_CHECK(r.validate());
    CapabilityRegistry c; Memory memory; std::array<Value, 1> out{Value::i32(42)}; std::size_t n = 9;
    constexpr std::array args{Value::i32(7)};
    BLIP_CHECK(!c.invoke(0, memory, args, out, n)); BLIP_CHECK(n == 0);
    BLIP_CHECK(c.bind(r)); BLIP_CHECK(r.start_all().ok());
    BLIP_CHECK(!c.invoke(99, memory, args, out, n));
    BLIP_CHECK(!c.invoke(0, memory, {}, out, n));
    BLIP_CHECK(!c.invoke(0, memory, args, {}, n));
    constexpr std::array wrong{Value::i64(7)};
    BLIP_CHECK(!c.invoke(0, memory, wrong, out, n));
    constexpr std::array wide{Value{ValueType::i32, std::uint64_t{1} << 32}};
    BLIP_CHECK(!c.invoke(0, memory, wide, out, n)); BLIP_CHECK(owner.calls == 0);
    std::atomic<bool> cancelled{true};
    BLIP_CHECK(c.invoke(0, memory, args, out, n, &cancelled).error().code == ErrorCode::cancelled);
    BLIP_CHECK(owner.calls == 0 && n == 0 && out[0].bits == 42);
    cancelled.store(false); owner.cancel_after = &cancelled;
    BLIP_CHECK(c.invoke(0, memory, args, out, n, &cancelled).error().code == ErrorCode::cancelled);
    BLIP_CHECK(owner.calls == 1 && n == 0 && out[0].bits == 42);
    owner.cancel_after = nullptr; owner.reenter = &c;
    BLIP_CHECK(c.invoke(0, memory, args, out, n).error().code == ErrorCode::recursive_dispatch);
    BLIP_CHECK(owner.calls == 2 && n == 0 && out[0].bits == 42);
    owner.reenter = nullptr; BLIP_CHECK(c.invoke(0, memory, args, out, n));
    BLIP_CHECK(r.stop_all()); return true;
}
bool failure_atomicity() {
    blip::core::Registry<1> r; TestComponent owner; BLIP_CHECK(r.add(owner)); BLIP_CHECK(r.validate());
    CapabilityRegistry c; BLIP_CHECK(c.bind(r)); BLIP_CHECK(r.start_all().ok());
    Memory memory; std::array<Value, 1> out{Value::i32(42)}; std::size_t n{};
    constexpr std::array args{Value::i32(7)};
    owner.fail = true; BLIP_CHECK(c.invoke(0, memory, args, out, n).error().code == ErrorCode::queue_full);
    BLIP_CHECK(n == 0 && out[0].bits == 42); owner.fail = false;
    owner.wrong_type = true; BLIP_CHECK(!c.invoke(0, memory, args, out, n)); owner.wrong_type = false;
    BLIP_CHECK(n == 0 && out[0].bits == 42);
    owner.wide_bits = true; BLIP_CHECK(!c.invoke(0, memory, args, out, n)); owner.wide_bits = false;
    owner.bad_count = true; BLIP_CHECK(!c.invoke(0, memory, args, out, n)); owner.bad_count = false;
    BLIP_CHECK(n == 0 && out[0].bits == 42); BLIP_CHECK(c.invoke(0, memory, args, out, n));
    BLIP_CHECK(n == 1 && out[0].bits == 7); BLIP_CHECK(r.stop_all()); return true;
}
bool checked_text() {
    blip::core::Registry<1> r; TestComponent owner; BLIP_CHECK(r.add(owner)); BLIP_CHECK(r.validate());
    CapabilityRegistry c; BLIP_CHECK(c.bind(r)); BLIP_CHECK(r.start_all().ok());
    Memory memory; memory.bytes.fill(std::byte{0x5a});
    constexpr std::array input{std::byte{0xc3}, std::byte{0xa9}, std::byte{0}, std::byte{'A'}};
    std::memcpy(memory.bytes.data(), input.data(), input.size());
    constexpr std::array args{Value::i32(0), Value::i32(4), Value::i32(256)};
    std::array<Value, 1> out{}; std::size_t n{};
    BLIP_CHECK(c.invoke(1, memory, args, out, n)); BLIP_CHECK(n == 1 && out[0].bits == 4);
    BLIP_CHECK(std::memcmp(memory.bytes.data() + 256, input.data(), input.size()) == 0);
    BLIP_CHECK(memory.bytes[260] == std::byte{0x5a});
    memory.bytes[0] = std::byte{0xff}; const auto previous = memory.bytes;
    BLIP_CHECK(!c.invoke(1, memory, args, out, n)); BLIP_CHECK(n == 0 && previous == memory.bytes);
    constexpr std::array bad{Value::i32(0xffffffffU), Value::i32(4), Value::i32(256)};
    BLIP_CHECK(!c.invoke(1, memory, bad, out, n)); BLIP_CHECK(previous == memory.bytes);
    std::atomic<bool> cancelled{true}; CallContext context{memory, &cancelled};
    std::array<char, 8> text{}; text.fill('Q'); n = 9;
    BLIP_CHECK(context.cancelled()); BLIP_CHECK(!context.read_utf8({0, 4}, text, n));
    BLIP_CHECK(n == 0 && text[0] == 'Q'); BLIP_CHECK(!context.write_utf8(0, "ok"));
    BLIP_CHECK(previous == memory.bytes); BLIP_CHECK(r.stop_all()); return true;
}
bool numeric_types_and_void() {
    for (const auto v : {Value::i32(0xffffffffU), Value::i64(0xffffffffffffffffULL),
                        Value{ValueType::f32, 0x7fc01234U}, Value{ValueType::f64, 0x7ff8000012345678ULL}}) {
        const std::array args{WasmArgumentDescriptor{"value", v.type}};
        const std::array result_types{v.type};
        const std::array f{WasmFunctionDescriptor{"echo", "Typed echo", args, result_types, 100}};
        TestComponent owner{make_descriptor("test.alpha", "test.alpha.v1", f)};
        blip::core::Registry<1> r; BLIP_CHECK(r.add(owner)); BLIP_CHECK(r.validate());
        CapabilityRegistry c; BLIP_CHECK(c.bind(r)); BLIP_CHECK(r.start_all().ok());
        Memory memory; const std::array input{v}; std::array<Value, 1> out{}; std::size_t n{};
        BLIP_CHECK(c.invoke(0, memory, input, out, n));
        BLIP_CHECK(n == 1 && out[0].type == v.type && out[0].bits == v.bits); BLIP_CHECK(r.stop_all());
    }
    const std::array f{WasmFunctionDescriptor{"noop", "No result", {}, {}, 100}};
    TestComponent owner{make_descriptor("test.alpha", "test.alpha.v1", f)};
    blip::core::Registry<1> r; BLIP_CHECK(r.add(owner)); BLIP_CHECK(r.validate());
    CapabilityRegistry c; BLIP_CHECK(c.bind(r)); BLIP_CHECK(r.start_all().ok());
    Memory memory; std::size_t n = 9; BLIP_CHECK(c.invoke(0, memory, {}, {}, n)); BLIP_CHECK(n == 0);
    BLIP_CHECK(r.stop_all()); return true;
}
class View final : public blip::core::RegistryView {
  public:
    std::array<blip::core::ComponentDescriptor, 9> descriptors{};
    std::array<TestComponent, 9> owners{};
    std::size_t count{};
    bool same_provider{}, missing_provider{};
    std::size_t component_count() const noexcept override { return count; }
    const blip::core::ComponentDescriptor& component_descriptor(std::size_t i) const noexcept override { return descriptors[i]; }
    CapabilityProvider* wasm_provider(std::size_t i) const noexcept override {
        if (missing_provider) return nullptr;
        // Lifetime/ownership test view; provider methods themselves are mutable.
        return const_cast<TestComponent*>(&owners[same_provider ? 0 : i]);
    }
    std::size_t dynamic_control_count() const noexcept override { return 0; }
    const blip::core::DynamicControl& dynamic_control(std::size_t) const noexcept override { return dummy; }
    blip::core::DynamicControl dummy{};
};
bool catalog_bounds_and_atomic_binding() {
    constexpr std::array ids{"test.a", "test.b", "test.c", "test.d", "test.e", "test.f", "test.g", "test.h", "test.i"};
    constexpr std::array modules{"test.a.v1", "test.b.v1", "test.c.v1", "test.d.v1", "test.e.v1", "test.f.v1", "test.g.v1", "test.h.v1", "test.i.v1"};
    constexpr std::array names{"a", "b", "c", "d", "e", "f", "g", "h"};
    std::array<WasmFunctionDescriptor, 8> fs{};
    for (std::size_t i = 0; i < fs.size(); ++i) fs[i] = {names[i], "Count test", {}, {}, 100};
    View view; for (std::size_t i = 0; i < ids.size(); ++i) view.descriptors[i] = make_descriptor(ids[i], modules[i], fs);
    CapabilityRegistry c; view.count = 5; BLIP_CHECK(!c.bind(view)); BLIP_CHECK(!c.bound() && c.size() == 0);
    view.count = 4; BLIP_CHECK(c.bind(view)); BLIP_CHECK(c.size() == kMaximumCapabilityFunctions);
    CapabilityRegistry many; view.count = 9;
    for (auto& d : view.descriptors) d.wasm.functions = std::span(fs).first(1);
    BLIP_CHECK(!many.bind(view)); BLIP_CHECK(!many.bound() && many.size() == 0);
    view.count = 8; BLIP_CHECK(many.bind(view)); BLIP_CHECK(many.size() == kMaximumCapabilityProviders);
    CapabilityRegistry repeated; view.count = 2; view.same_provider = true;
    BLIP_CHECK(!repeated.bind(view)); BLIP_CHECK(!repeated.bound() && repeated.size() == 0);
    view.same_provider = false; view.descriptors[1] = view.descriptors[0];
    BLIP_CHECK(!repeated.bind(view)); BLIP_CHECK(!repeated.bound() && repeated.size() == 0);
    view.descriptors[1] = make_descriptor(ids[1], modules[1], std::span(fs).first(1));
    view.missing_provider = true; BLIP_CHECK(!repeated.bind(view));
    view.missing_provider = false; BLIP_CHECK(repeated.bind(view));
    CapabilityRegistry huge; view.count = kMaximumCapabilityComponents + 1; BLIP_CHECK(!huge.bind(view));
    return true;
}
bool manifest_from_registry() {
    TestComponent owner; std::array<char, 4096> output{};
    const auto result = blip::core::write_descriptor_json(owner.descriptor(), output);
    BLIP_CHECK(result); const std::string_view json{output.data(), result.value()};
    BLIP_CHECK(json.find("\"wasm\":{\"abi_version\":1,\"import_module\":\"test.alpha.v1\"") != json.npos);
    BLIP_CHECK(json.find("\"role\":\"utf8_offset\"") != json.npos);
    BLIP_CHECK(json.find("\"maximum_call_us\":500") != json.npos);
    BLIP_CHECK(json.find("\"results\":[\"i32\"]") != json.npos);
    auto f = functions[0]; f.description = "Quote \" and slash \\ and newline\n";
    owner.data.wasm.functions = {&f, 1};
    const auto escaped = blip::core::write_descriptor_json(owner.descriptor(), output);
    BLIP_CHECK(escaped);
    BLIP_CHECK(std::string_view(output.data(), escaped.value()).find("newline\\u000a") != json.npos);
    std::array<char, 128> short_output{};
    BLIP_CHECK(!blip::core::write_descriptor_json(owner.descriptor(), short_output));
    std::array<char, 256> controls{}; controls.fill('\n'); f.description = {controls.data(), controls.size()};
    std::array<char, 1025> guarded{}; guarded.fill('Q');
    BLIP_CHECK(!blip::core::write_descriptor_json(owner.descriptor(), std::span(guarded).first(1024)));
    BLIP_CHECK(guarded.back() == 'Q');
    return true;
}
bool exact_import_policy() {
    blip::core::Registry<1> r; TestComponent owner; BLIP_CHECK(r.add(owner)); BLIP_CHECK(r.validate());
    CapabilityRegistry catalog; Signature signature{}; signature.argument_count = 1; signature.result_count = 1;
    signature.arguments[0] = ValueType::i32; signature.results[0] = ValueType::i32;
    BLIP_CHECK(!catalog.check_import("test.alpha.v1", "echo", signature));
    BLIP_CHECK(catalog.bind(r)); BLIP_CHECK(catalog.check_import("test.alpha.v1", "echo", signature));
    BLIP_CHECK(!catalog.check_import("test.alpha.v2", "echo", signature));
    BLIP_CHECK(!catalog.check_import("test.alpha.v1", "_echo", signature));
    BLIP_CHECK(!catalog.check_import("env", "echo", signature));
    signature.arguments[0] = ValueType::i64; BLIP_CHECK(!catalog.check_import("test.alpha.v1", "echo", signature));
    signature.arguments[0] = ValueType::i32; signature.result_count = 2;
    BLIP_CHECK(!catalog.check_import("test.alpha.v1", "echo", signature));
    signature.result_count = 1; signature.results[0] = ValueType::f32;
    BLIP_CHECK(!catalog.check_import("test.alpha.v1", "echo", signature));
    signature.results[0] = ValueType::i32; signature.argument_count = 255;
    BLIP_CHECK(!catalog.check_import("test.alpha.v1", "echo", signature));
    return true;
}
bool production_lifecycle_service_binding() {
    blip::core::Registry<1> r; TestComponent owner; BLIP_CHECK(r.add(owner));
    CapabilityRegistry catalog; const auto missing = catalog.bind(r, true);
    BLIP_CHECK(!missing && missing.error().code == ErrorCode::missing_dependency && !catalog.bound() && catalog.size() == 0);
    const std::array<std::string_view, 1> services{"test.alpha.v1"};
    owner.data.provided_services = services;
    BLIP_CHECK(catalog.bind(r, true)); BLIP_CHECK(!catalog.bind(r, true));
    BLIP_CHECK(r.validate());
    return true;
}
bool original_module_name_policy() {
    blip::core::Registry<1> r; TestComponent owner; BLIP_CHECK(r.add(owner)); BLIP_CHECK(r.validate());
    CapabilityRegistry catalog; BLIP_CHECK(catalog.bind(r));
    // This tests the pre-engine section policy, not whole-module validity.
    auto bytes = [](std::string_view module, std::string_view field, unsigned kind = 0) {
        std::array<std::byte, 128> out{};
        const std::array head{std::byte{0}, std::byte{0x61}, std::byte{0x73}, std::byte{0x6d},
            std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0}};
        std::copy(head.begin(), head.end(), out.begin()); std::size_t at = 8;
        out[at++] = std::byte{2}; const auto length_position = at++;
        out[at++] = std::byte{1}; out[at++] = static_cast<std::byte>(module.size());
        for (const char c : module) out[at++] = static_cast<std::byte>(c);
        out[at++] = static_cast<std::byte>(field.size());
        for (const char c : field) out[at++] = static_cast<std::byte>(c);
        out[at++] = static_cast<std::byte>(kind); out[at++] = std::byte{0};
        out[length_position] = static_cast<std::byte>(at - length_position - 1);
        return std::pair{out, at};
    };
    const auto good = bytes("test.alpha.v1", "echo");
    BLIP_CHECK(passive_module(std::span(good.first).first(good.second), &catalog));
    BLIP_CHECK(!passive_module(std::span(good.first).first(good.second)));
    for (const auto name : {std::string_view("test.alpha.v1\0evil", 18), std::string_view("test.alpha.v2")}) {
        const auto bad = bytes(name, "echo"); BLIP_CHECK(!passive_module(std::span(bad.first).first(bad.second), &catalog));
    }
    for (const auto name : {std::string_view("echo\0evil", 9), std::string_view("_echo"), std::string_view("\xff", 1)}) {
        const auto bad = bytes("test.alpha.v1", name); BLIP_CHECK(!passive_module(std::span(bad.first).first(bad.second), &catalog));
    }
    const auto memory = bytes("test.alpha.v1", "echo", 2);
    BLIP_CHECK(!passive_module(std::span(memory.first).first(memory.second), &catalog));
    for (std::size_t length = 0; length < good.second; ++length)
        BLIP_CHECK(!passive_module(std::span(good.first).first(length), &catalog) || length == 8);
    auto malformed = good; malformed.first[9] = std::byte{0xff};
    BLIP_CHECK(!passive_module(std::span(malformed.first).first(malformed.second), &catalog));
    const auto empty = std::span(good.first).first(8);
    std::array<std::byte, 32> exported{}; std::copy(empty.begin(), empty.end(), exported.begin());
    const std::array payload{std::byte{7},std::byte{11},std::byte{1},std::byte{7},std::byte{'e'},std::byte{'c'},std::byte{'h'},
        std::byte{'o'},std::byte{0},std::byte{'e'},std::byte{'x'},std::byte{0},std::byte{0}};
    std::copy(payload.begin(), payload.end(), exported.begin() + 8);
    BLIP_CHECK(!passive_module(std::span(exported).first(8 + payload.size()), &catalog));
    return true;
}
} // namespace
int main() {
    const TestCase tests[]{
        {"provider metadata ownership and ABI versions", metadata_validation},
        {"typed arguments and UTF-8 role pairs", argument_validation},
        {"registry rejects missing or mismatched owners", owner_validation},
        {"independent providers and suspend/resume/stop", bind_and_dispatch},
        {"call admission, cancellation and recursive dispatch", invocation_guards},
        {"failure and malformed results preserve output", failure_atomicity},
        {"provider checked UTF-8 context", checked_text},
        {"raw numeric bits and void results", numeric_types_and_void},
        {"catalog bounds and atomic registration", catalog_bounds_and_atomic_binding},
        {"registry-generated capability manifest", manifest_from_registry},
        {"exact version/name/typed import policy", exact_import_policy},
        {"production providers declare lifecycle services before binding", production_lifecycle_service_binding},
        {"original module names, truncation and C-string aliases", original_module_name_policy}};
    return run_tests(tests);
}
