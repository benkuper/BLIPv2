#include "blip/wasm/script_controls.hpp"
#include "test_harness.hpp"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace blip::wasm;
using namespace blip::core;
using Bytes = std::vector<std::byte>;
void byte(Bytes& b, unsigned v) { b.push_back(static_cast<std::byte>(v)); }
void u32(Bytes& b, unsigned v) { do { byte(b, (v & 127U) | (v > 127 ? 128U : 0)); v >>= 7; } while (v); }
void text(Bytes& b, std::string_view v) { u32(b, static_cast<unsigned>(v.size())); for (unsigned char c : v) byte(b, c); }
void bits(Bytes& b, std::uint64_t v) { for (unsigned i = 0; i < 8; ++i) byte(b, static_cast<unsigned>(v >> (8 * i)) & 255U); }
void number(Bytes& b, double v) { bits(b, std::bit_cast<std::uint64_t>(v)); }
Bytes fixture() {
    Bytes data; for (char c : std::string_view("BCM1")) byte(data, c); u32(data, 6);
    const std::array<std::string_view, 4> ids{"armed", "level", "rate", "note"};
    for (unsigned i = 0; i < 4; ++i) {
        byte(data, 0); text(data, ids[i]); text(data, ids[i]); byte(data, i); byte(data, i == 0 ? 0 : i == 2 ? 1 : 2); text(data, "");
        if (i == 0) byte(data, 1); else if (i == 1) bits(data, 5); else if (i == 2) number(data, .5); else text(data, "hello");
        byte(data, i == 1 || i == 2 ? 1 : 0);
        if (i == 1 || i == 2) { number(data, 0); number(data, i == 1 ? 100 : 10); number(data, 1); }
    }
    for (unsigned kind = 1; kind <= 2; ++kind) {
        byte(data, kind); text(data, kind == 1 ? "fire" : "changed"); text(data, "Label");
        if (kind == 1) text(data, "on_fire");
        u32(data, 4);
        for (unsigned i = 0; i < 4; ++i) { text(data, ids[i]); byte(data, i); }
    }
    Bytes body; text(body, kScriptControlsSection); body.insert(body.end(), data.begin(), data.end());
    Bytes result{std::byte{0}, std::byte{0x61}, std::byte{0x73}, std::byte{0x6d}, std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0}};
    byte(result, 0); u32(result, static_cast<unsigned>(body.size())); result.insert(result.end(), body.begin(), body.end()); return result;
}
class Backend final : public Runtime {
  public:
    std::string_view name() const noexcept override { return "store-test"; }
    Status initialize(std::span<std::byte>, Limits, std::span<std::byte>) noexcept override { return Status::success(); }
    Status load(std::span<std::byte>) noexcept override { return Status::success(); }
    void unload() noexcept override {}
    void shutdown() noexcept override {}
    Result<Signature> signature(std::string_view name) noexcept override {
        if (missing || (name != "on_fire" && name != "on_text")) return Result<Signature>::failure(error(ErrorCode::not_found).error());
        Signature s{}; s.argument_count = 5;
        s.arguments = {blip::wasm::ValueType::i32, blip::wasm::ValueType::i64, blip::wasm::ValueType::f64,
                       blip::wasm::ValueType::i32, blip::wasm::ValueType::i32};
        if (name == "on_text") { s.argument_count = 8; s.arguments.fill(blip::wasm::ValueType::i32); }
        if (wrong_signature) s.result_count = 1;
        return Result<Signature>::success(s);
    }
    Result<std::uint32_t> immutable_i32_global(std::string_view name) noexcept override {
        if (missing_buffer || name != kScriptActionBufferGlobal) return Result<std::uint32_t>::failure(error(ErrorCode::not_found).error());
        return Result<std::uint32_t>::success(buffer);
    }
    Status invoke(std::string_view, std::span<const Value>, ExecutionBudget, std::span<Value>, std::size_t&) noexcept override { return error(ErrorCode::invalid_state); }
    void request_cancel() noexcept override {}
    RuntimeSnapshot snapshot() const noexcept override { return {}; }
    Status read_memory(std::uint32_t offset, std::span<std::byte> out) noexcept override {
        if (offset > memory.size() || out.size() > memory.size() - offset) return error(ErrorCode::invalid_argument);
        std::copy_n(memory.begin() + offset, out.size(), out.begin()); return Status::success();
    }
    Status write_memory(std::uint32_t offset, std::span<const std::byte> in) noexcept override {
        if (offset > memory.size() || in.size() > memory.size() - offset) return error(ErrorCode::invalid_argument);
        std::copy(in.begin(), in.end(), memory.begin() + offset); return Status::success();
    }
    static Status error(ErrorCode code) noexcept { return Status::failure({ErrorDomain::control, code, "test", "test", "test"}); }
    bool missing{}, wrong_signature{}, missing_buffer{}; std::uint32_t buffer{512}; std::array<std::byte, 1024> memory{};
};
bool load(ScriptControlStore& store, Backend& backend, unsigned module = 1) {
    store.retire(); return store.prepare(fixture(), {}) && store.publish(backend, module);
}
bool owned_schema_and_retirement() {
    ScriptControlStore store; Backend backend; auto bytes = fixture();
    BLIP_CHECK(store.prepare(bytes, {})); std::fill(bytes.begin(), bytes.end(), std::byte{0xff});
    BLIP_CHECK(store.publish(backend, 1)); const auto token = store.generation();
    DynamicSchemaLease lease; BLIP_CHECK(store.acquire(lease) && lease.size() == 6 && lease.generation() == token);
    BLIP_CHECK(lease.id(3) == "note" && lease.id(4) == "fire");
    store.retire(); BLIP_CHECK(!store.quiescent());
    BLIP_CHECK(!store.prepare(fixture(), {})); BLIP_CHECK(lease.id(4) == "fire");
    ScalarValue out; std::array<char, 128> scratch{};
    BLIP_CHECK(!store.read(token, 1, out, scratch));
    lease.reset(); BLIP_CHECK(store.quiescent() && load(store, backend, 2) && store.generation() > token);
    BLIP_CHECK(!store.write(token, 1, ScalarValue::from_integer(9)));
    store.retire(); return true;
}
bool scalar_access_limits_and_ownership() {
    ScriptControlStore store; Backend backend; BLIP_CHECK(load(store, backend)); const auto token = store.generation();
    ScalarValue out; std::array<char, 128> scratch{};
    BLIP_CHECK(store.read_id(token, "note", out, scratch) && out.string == "hello");
    std::array<char, 128> input{}; input.fill('a'); input[1] = 0;
    BLIP_CHECK(store.write_id(token, "note", ScalarValue::from_string({input.data(), input.size()}))); input.fill('x');
    BLIP_CHECK(store.read(token, 3, out, scratch) && out.string.size() == 128 && out.string[0] == 'a' && out.string[1] == 0);
    BLIP_CHECK(!store.write(token, 3, ScalarValue::from_string(std::string(129, 'b'))));
    BLIP_CHECK(!store.write(token, 3, ScalarValue::from_string("\xc0\x80")));
    BLIP_CHECK(!store.write(token, 1, ScalarValue::from_integer(101)));
    BLIP_CHECK(!store.write(token, 1, ScalarValue::from_bool(true)));
    BLIP_CHECK(store.read(token, 1, out, scratch) && out.integer == 5);
    BLIP_CHECK(!store.write(token, 0, ScalarValue::from_bool(false)));
    BLIP_CHECK(store.write(token, 0, ScalarValue::from_bool(false), true));
    BLIP_CHECK(!store.read(token, 2, out, scratch));
    BLIP_CHECK(store.read(token, 2, out, scratch, true) && out.number == .5);
    BLIP_CHECK(!store.write(token, 2, ScalarValue::from_number(std::numeric_limits<double>::infinity()), true));
    BLIP_CHECK(!store.read(token, 3, out, std::span<char>(scratch).first(127)));
    store.retire(); return true;
}
bool actions_are_owned_bounded_and_tagged() {
    ScriptControlStore store; Backend backend; BLIP_CHECK(load(store, backend, 19)); const auto token = store.generation();
    std::array<char, 6> input{'c','o','p','i','e','d'};
    std::array args{ScalarValue::from_bool(true), ScalarValue::from_integer(-5), ScalarValue::from_number(2.5), ScalarValue::from_string({input.data(), input.size()})};
    for (unsigned i = 0; i < kScriptControlQueueCapacity; ++i) BLIP_CHECK(store.enqueue_action(token, "fire", args, 50 + i, 7));
    BLIP_CHECK(!store.enqueue_action(token, "fire", args, 100, 7)); input.fill('x');
    ScriptControlMessage message;
    for (unsigned i = 0; i < kScriptControlQueueCapacity; ++i) {
        BLIP_CHECK(store.take_action(message)); BLIP_CHECK(message.ticket == 50 + i && message.epoch == 7 && message.module_generation == 19);
        BLIP_CHECK(message.generation == token && std::string_view(message.name.data()) == "on_fire" && message.count == 4);
        BLIP_CHECK(message.values[3].scalar().string == "copied" && message.values[1].scalar().integer == -5);
    }
    BLIP_CHECK(!store.take_action(message));
    args[0] = ScalarValue::from_integer(1); BLIP_CHECK(!store.enqueue_action(token, "fire", args, 200, 7));
    args[0] = ScalarValue::from_bool(true); BLIP_CHECK(store.enqueue_action(token, "fire", args, 201, 7));
    store.retire(); BLIP_CHECK(!store.prepare(fixture(), {})); // IDs must be completed before replacement.
    BLIP_CHECK(store.take_action(message) && message.ticket == 201 && message.generation == token);
    BLIP_CHECK(load(store, backend, 21)); BLIP_CHECK(!store.enqueue_action(token, "fire", args, 202, 7));
    store.retire(); return true;
}
bool publication_validates_callbacks_buffer_and_collisions() {
    ScriptControlStore store; Backend backend;
    std::array<ParameterDescriptor, 1> reserved{{{"level", "Reserved", blip::core::ValueType::integer}}};
    ComponentDescriptor descriptor{}; descriptor.parameters = reserved;
    BLIP_CHECK(!store.prepare(fixture(), descriptor) && store.generation() == 0);
    for (unsigned mode = 0; mode < 4; ++mode) {
        BLIP_CHECK(store.prepare(fixture(), {})); backend.missing = mode == 0; backend.wrong_signature = mode == 1;
        backend.missing_buffer = mode == 2; backend.buffer = mode == 3 ? 1024 : 512;
        BLIP_CHECK(!store.publish(backend, 1) && store.generation() == 0);
    }
    backend.missing = backend.wrong_signature = backend.missing_buffer = false; backend.buffer = 512;
    BLIP_CHECK(load(store, backend)); store.retire(); return true;
}
bool event_builder_and_owned_delivery() {
    ScriptControlStore store; Backend backend; BLIP_CHECK(load(store, backend)); const auto gen = store.generation();
    const auto begun = store.begin_event(gen, 5); BLIP_CHECK(begun); const auto token = begun.value();
    BLIP_CHECK(!store.begin_event(gen, 5)); BLIP_CHECK(!store.commit_event(token));
    BLIP_CHECK(!store.event_bits(token, 0, 2)); BLIP_CHECK(store.event_bits(token, 0, 1));
    BLIP_CHECK(store.event_bits(token, 1, std::bit_cast<std::uint64_t>(std::int64_t{-9})));
    BLIP_CHECK(!store.event_bits(token, 2, std::bit_cast<std::uint64_t>(std::numeric_limits<double>::quiet_NaN())));
    BLIP_CHECK(store.event_bits(token, 2, std::bit_cast<std::uint64_t>(1.25)));
    std::array<char, 4> input{'o','k',0,'!'};
    BLIP_CHECK(!store.event_utf8(token, 3, "\xc0\x80")); BLIP_CHECK(store.event_utf8(token, 3, {input.data(), input.size()}));
    BLIP_CHECK(store.commit_event(token)); input.fill('x');
    BLIP_CHECK(!store.commit_event(token)); ScriptControlMessage message;
    BLIP_CHECK(store.take_event(message) && message.generation == gen && message.count == 4);
    BLIP_CHECK(std::string_view(message.name.data()) == "changed" && message.values[3].scalar().string == std::string_view("ok\0!", 4));
    const auto abandoned = store.begin_event(gen, 5); BLIP_CHECK(abandoned); store.end_invocation();
    BLIP_CHECK(!store.event_bits(abandoned.value(), 0, 1));
    for (unsigned i = 0; i < kScriptControlQueueCapacity; ++i) {
        auto t = store.begin_event(gen, 5); BLIP_CHECK(t);
        BLIP_CHECK(store.event_bits(t.value(), 0, 1) && store.event_bits(t.value(), 1, 9) &&
            store.event_bits(t.value(), 2, std::bit_cast<std::uint64_t>(1.25)) && store.event_utf8(t.value(), 3, "event"));
        BLIP_CHECK(store.commit_event(t.value()));
    }
    const auto overflow = store.begin_event(gen, 5); BLIP_CHECK(overflow);
    BLIP_CHECK(store.event_bits(overflow.value(), 0, 1) && store.event_bits(overflow.value(), 1, 10) &&
        store.event_bits(overflow.value(), 2, std::bit_cast<std::uint64_t>(2.5)) && store.event_utf8(overflow.value(), 3, "retry"));
    const auto rejected = store.commit_event(overflow.value());
    BLIP_CHECK(!rejected && rejected.error().code == ErrorCode::queue_full);
    BLIP_CHECK(store.take_event(message) && message.values[3].scalar().string == "event");
    BLIP_CHECK(store.commit_event(overflow.value())); // Refusal preserves the builder for retry.
    store.retire(); BLIP_CHECK(!store.take_event(message)); // Stale events are discarded.
    BLIP_CHECK(load(store, backend, 3)); BLIP_CHECK(!store.take_event(message)); store.retire(); return true;
}
bool concurrent_reader_retirement_and_replacement() {
    ScriptControlStore store; Backend backend; BLIP_CHECK(load(store, backend));
    std::atomic<bool> pinned{}, finish{}, valid{};
    std::thread reader([&] {
        DynamicSchemaLease lease;
        if (!store.acquire(lease) || !lease.held()) { pinned.store(true); return; }
        pinned.store(true, std::memory_order_release);
        // Sleeping also admits a lower-priority FreeRTOS owner thread. A plain
        // sched_yield only admits peers and can starve the owner on single core.
        while (!finish.load(std::memory_order_acquire)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ParameterDescriptor p; valid.store(lease.parameter(3, p) && p.id == "note" && p.default_value.string == "hello");
    });
    while (!pinned.load(std::memory_order_acquire)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    store.retire(); const bool refused = !store.prepare(fixture(), {}) && !store.quiescent();
    finish.store(true, std::memory_order_release); reader.join();
    BLIP_CHECK(refused && valid.load() && store.quiescent() && load(store, backend, 2)); store.retire(); return true;
}
bool copied_action_arguments_use_checked_memory() {
    ScriptControlStore store; Backend backend; BLIP_CHECK(load(store, backend));
    const std::array args{ScalarValue::from_bool(true), ScalarValue::from_integer(-9007199254740993LL),
        ScalarValue::from_number(2.5), ScalarValue::from_string(std::string_view("ok\0!", 4))};
    BLIP_CHECK(store.enqueue_action(store.generation(), "fire", args, 1, 1));
    ScriptControlMessage message; BLIP_CHECK(store.take_action(message));
    std::array<Value, kMaximumArguments> values{}; std::size_t count = 99;
    BLIP_CHECK(copy_script_action_arguments(backend, store.action_buffer(), message, values, count));
    BLIP_CHECK(count == 5 && values[0].type == blip::wasm::ValueType::i32 && values[0].bits == 1);
    BLIP_CHECK(values[1].type == blip::wasm::ValueType::i64 && std::bit_cast<std::int64_t>(values[1].bits) == -9007199254740993LL);
    BLIP_CHECK(values[2].type == blip::wasm::ValueType::f64 && std::bit_cast<double>(values[2].bits) == 2.5);
    BLIP_CHECK(values[3].bits == 896 && values[4].bits == 4);
    BLIP_CHECK(std::memcmp(backend.memory.data() + 896, "ok\0!", 4) == 0);
    BLIP_CHECK(!copy_script_action_arguments(backend, 0xfffffffeU, message, values, count) && count == 0);
    BLIP_CHECK(!copy_script_action_arguments(backend, 1024, message, values, count) && count == 0);
    BLIP_CHECK(!copy_script_action_arguments(backend, 512, message, std::span<Value>(values).first(4), count) && count == 0);
    message.count = 5;
    BLIP_CHECK(!copy_script_action_arguments(backend, 512, message, values, count) && count == 0);
    message.count = 4; message.values[3].bytes = 129;
    BLIP_CHECK(!copy_script_action_arguments(backend, 512, message, values, count) && count == 0);
    store.retire(); return true;
}
Bytes text_fixture() {
    Bytes data; for (char c : std::string_view("BCM1")) byte(data, c); u32(data, 4);
    byte(data, 0); text(data, "note"); text(data, "Note"); byte(data, 3); byte(data, 2); text(data, ""); text(data, "init"); byte(data, 0);
    for (unsigned kind = 0; kind < 3; ++kind) {
        byte(data, kind == 2 ? 2 : 1); text(data, kind == 0 ? "text_fire" : kind == 1 ? "fire" : "changed"); text(data, "Label");
        if (kind < 2) text(data, kind == 0 ? "on_text" : "on_fire");
        u32(data, 4);
        for (unsigned i = 0; i < 4; ++i) { text(data, "field" + std::to_string(i)); byte(data, kind == 1 ? i : 3); }
    }
    Bytes body; text(body, kScriptControlsSection); body.insert(body.end(), data.begin(), data.end());
    Bytes result{std::byte{0}, std::byte{0x61}, std::byte{0x73}, std::byte{0x6d}, std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0}};
    byte(result, 0); u32(result, static_cast<unsigned>(body.size())); result.insert(result.end(), body.begin(), body.end()); return result;
}
bool compact_text_queues_keep_full_capacity_and_ownership() {
    ScriptControlStore store; Backend backend;
    BLIP_CHECK(store.text_reserved_bytes() == 0);
    BLIP_CHECK(store.prepare(text_fixture(), {}) && store.publish(backend, 1));
    BLIP_CHECK(store.text_reserved_bytes() == 128 + 2 * 4 * 4 * 128);
    std::array<std::string, 4> text;
    ScriptControlMessage action, event;
    std::array<char, 128> readback{}; ScalarValue value;
    for (unsigned cycle = 0; cycle < 3; ++cycle) {
        for (unsigned slot = 0; slot < 4; ++slot) {
            for (unsigned field = 0; field < 4; ++field) text[field].assign(128, static_cast<char>('a' + slot * 4 + field));
            const std::array args{ScalarValue::from_string(text[0]), ScalarValue::from_string(text[1]),
                ScalarValue::from_string(text[2]), ScalarValue::from_string(text[3])};
            BLIP_CHECK(store.enqueue_action(store.generation(), "text_fire", args, slot + 1, cycle + 1));
            const auto token = store.begin_event(store.generation(), 3); BLIP_CHECK(token);
            for (unsigned field = 0; field < 4; ++field) BLIP_CHECK(store.event_utf8(token.value(), field, text[field]));
            BLIP_CHECK(store.commit_event(token.value()));
        }
        const std::array extra{ScalarValue::from_string(""), ScalarValue::from_string(""), ScalarValue::from_string(""), ScalarValue::from_string("")};
        BLIP_CHECK(store.enqueue_action(store.generation(), "text_fire", extra, 9, 1).error().code == ErrorCode::queue_full);
        for (auto& s : text) s.assign(128, 'z');
        BLIP_CHECK(store.write(store.generation(), 0, ScalarValue::from_string(text[0])));
        for (unsigned slot = 0; slot < 4; ++slot) {
            BLIP_CHECK(store.take_action(action) && store.take_event(event));
            BLIP_CHECK(action.ticket == slot + 1 && action.epoch == cycle + 1);
            for (unsigned field = 0; field < 4; ++field) {
                const std::string expected(128, static_cast<char>('a' + slot * 4 + field));
                BLIP_CHECK(action.values[field].scalar().string == expected && event.values[field].scalar().string == expected);
            }
        }
        // Reusing the slot with fewer strings must preserve numeric bits and
        // the last expanded message independently of the packed queue.
        const std::array mixed{ScalarValue::from_bool(true), ScalarValue::from_integer(-9007199254740993LL),
            ScalarValue::from_number(2.5), ScalarValue::from_string("")};
        BLIP_CHECK(store.enqueue_action(store.generation(), "fire", mixed, 77, 1));
        ScriptControlMessage copied = action;
        BLIP_CHECK(store.take_action(action));
        BLIP_CHECK(action.values[0].scalar().boolean && action.values[1].scalar().integer == -9007199254740993LL &&
            action.values[2].scalar().number == 2.5 && action.values[3].scalar().string.empty());
        BLIP_CHECK(copied.values[3].scalar().string == std::string(128, 'p'));
        BLIP_CHECK(store.read(store.generation(), 0, value, readback) && value.string == std::string(128, 'z'));
    }
    store.retire(); BLIP_CHECK(store.prepare(fixture(), {}) && store.publish(backend, 2));
    BLIP_CHECK(store.text_reserved_bytes() == 128 + 2 * 4 * 128);
    BLIP_CHECK(!store.release_retired_text());
    const std::array pending{ScalarValue::from_bool(true), ScalarValue::from_integer(9),
        ScalarValue::from_number(.5), ScalarValue::from_string("pending text")};
    BLIP_CHECK(store.enqueue_action(store.generation(), "fire", pending, 88, 2));
    DynamicSchemaLease lease; BLIP_CHECK(store.acquire(lease));
    store.retire();
    BLIP_CHECK(!store.release_retired_text()); // Old schema readers retain their generation.
    lease.reset(); BLIP_CHECK(!store.release_retired_text()); // Accepted actions still own their text.
    BLIP_CHECK(store.take_action(action) && action.ticket == 88 && action.values[3].scalar().string == "pending text");
    BLIP_CHECK(store.release_retired_text() && store.text_reserved_bytes() == 0);
    BLIP_CHECK(action.values[3].scalar().string == "pending text"); // Expanded copies survive reclamation.
    const std::array empty{std::byte{0}, std::byte{0x61}, std::byte{0x73}, std::byte{0x6d}, std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0}};
    BLIP_CHECK(store.prepare(empty, {}) && store.publish(backend, 3) && store.text_reserved_bytes() == 0);
    store.retire(); return true;
}
} // namespace
int blip_script_controls_run_tests() {
    const TestCase tests[]{
        {"owned declarations survive upload overwrite and leased retirement", owned_schema_and_retirement},
        {"parameter store enforces external access types bounds UTF8 and capacity", scalar_access_limits_and_ownership},
        {"action queues copy fields reject overflow and retain cancellation IDs", actions_are_owned_bounded_and_tagged},
        {"publication checks callback signatures string arena and reserved controls", publication_validates_callbacks_buffer_and_collisions},
        {"event builder requires all fields and delivers owned generation tagged payloads", event_builder_and_owned_delivery},
        {"concurrent reader pins retired metadata until safe replacement", concurrent_reader_retirement_and_replacement},
        {"copied action arguments preserve bits and validate guest memory", copied_action_arguments_use_checked_memory},
        {"compact queues retain full strings all slots wraparound ownership and bits", compact_text_queues_keep_full_capacity_and_ownership}
    }; return run_tests(tests);
}
#ifndef ESP_PLATFORM
int main() { return blip_script_controls_run_tests(); }
#endif
