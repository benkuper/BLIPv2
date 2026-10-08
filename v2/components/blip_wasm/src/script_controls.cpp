#include "blip/wasm/script_controls.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

namespace blip::wasm {
namespace {
constexpr std::uint32_t kWriter = 0x80000000U;
core::Status fail(core::ErrorCode code, std::string_view detail) noexcept {
    return core::Status::failure({core::ErrorDomain::control, code, "blip.wasm", "script-controls", detail});
}
bool valid_scalar(const core::ScalarValue& value, core::ValueType expected) noexcept {
    if (value.type != expected) return false;
    if (expected != core::ValueType::boolean && expected != core::ValueType::integer &&
        expected != core::ValueType::number && expected != core::ValueType::string) return false;
    if (expected == core::ValueType::number) return std::isfinite(value.number);
    if (expected == core::ValueType::string)
        return value.string.size() <= kScriptValueStringBytes && valid_utf8(std::as_bytes(std::span(value.string.data(), value.string.size())));
    return true;
}
bool in_bounds(const core::ScalarValue& value, const core::NumericBounds& bounds) noexcept {
    if (!bounds.present) return true;
    const double n = value.type == core::ValueType::integer ? static_cast<double>(value.integer) : value.number;
    return n >= bounds.minimum && n <= bounds.maximum;
}
void name(std::array<char, kMaximumExportNameBytes + 1>& output, std::string_view input) noexcept {
    output.fill(0); std::memcpy(output.data(), input.data(), input.size());
}
Signature callback_signature(const ScriptControl& control) noexcept {
    Signature signature{};
    for (std::size_t i = 0; i < control.field_count; ++i) {
        switch (control.fields[i].type) {
        case core::ValueType::boolean: signature.arguments[signature.argument_count++] = ValueType::i32; break;
        case core::ValueType::integer: signature.arguments[signature.argument_count++] = ValueType::i64; break;
        case core::ValueType::number: signature.arguments[signature.argument_count++] = ValueType::f64; break;
        case core::ValueType::string:
            signature.arguments[signature.argument_count++] = ValueType::i32;
            signature.arguments[signature.argument_count++] = ValueType::i32; break;
        }
    }
    return signature;
}
} // namespace
class ScriptControlStore::WriteGuard {
  public:
    explicit WriteGuard(ScriptControlStore& store) noexcept : store_(&store) {
        std::uint32_t expected{}; held = store.gate_.compare_exchange_strong(expected, kWriter, std::memory_order_acquire);
    }
    ~WriteGuard() { if (held) store_->gate_.store(0, std::memory_order_release); }
    bool held{};
  private:
    ScriptControlStore* store_;
};
class ScriptControlStore::DataGuard {
  public:
    explicit DataGuard(const ScriptControlStore& store) noexcept : held(!store.data_.test_and_set(std::memory_order_acquire)), store_(&store) {}
    ~DataGuard() { if (held) store_->data_.clear(std::memory_order_release); }
    bool held;
  private:
    const ScriptControlStore* store_;
};
class ScriptControlStore::ActionGuard {
  public:
    explicit ActionGuard(const ScriptControlStore& store) noexcept
        : held(!store.action_data_.test_and_set(std::memory_order_acquire)), store_(&store) {}
    ~ActionGuard() { if (held) store_->action_data_.clear(std::memory_order_release); }
    bool held;
  private:
    const ScriptControlStore* store_;
};
core::ScalarValue OwnedScriptValue::scalar() const noexcept {
    switch (type) {
    case core::ValueType::boolean: return core::ScalarValue::from_bool(bits != 0);
    case core::ValueType::integer: return core::ScalarValue::from_integer(std::bit_cast<std::int64_t>(bits));
    case core::ValueType::number: return core::ScalarValue::from_number(std::bit_cast<double>(bits));
    case core::ValueType::string: return core::ScalarValue::from_string({text.data(), bytes});
    }
    return {};
}
core::Status copy_script_action_arguments(GuestMemory& memory, std::uint32_t buffer,
    const ScriptControlMessage& message, std::span<Value> output, std::size_t& count) noexcept {
    count = 0;
    if (message.count > message.values.size()) return fail(core::ErrorCode::invalid_argument, "action-field-count");
    std::size_t needed{};
    for (std::size_t i = 0; i < message.count; ++i) {
        const auto& value = message.values[i];
        if (value.type == core::ValueType::string && value.bytes > value.text.size())
            return fail(core::ErrorCode::invalid_argument, "action-string-size");
        if (!valid_scalar(value.scalar(), value.type) || (value.type == core::ValueType::boolean && value.bits > 1))
            return fail(core::ErrorCode::validation_failed, "action-field-value");
        needed += value.type == core::ValueType::string ? 2 : 1;
    }
    if (needed > output.size()) return fail(core::ErrorCode::capacity_exceeded, "action-argument-storage");
    std::size_t cursor{};
    for (std::size_t i = 0; i < message.count; ++i) {
        const auto& value = message.values[i];
        switch (value.type) {
        case core::ValueType::boolean: output[cursor++] = Value::i32(static_cast<std::uint32_t>(value.bits)); break;
        case core::ValueType::integer: output[cursor++] = Value::i64(value.bits); break;
        case core::ValueType::number: output[cursor++] = {ValueType::f64, value.bits}; break;
        case core::ValueType::string: {
            const auto delta = static_cast<std::uint32_t>(i * kScriptValueStringBytes);
            if (buffer > UINT32_MAX - delta) return fail(core::ErrorCode::invalid_argument, "action-buffer-overflow");
            const auto offset = buffer + delta;
            const auto status = write_utf8(memory, offset, value.scalar().string);
            if (!status) return status;
            output[cursor++] = Value::i32(offset); output[cursor++] = Value::i32(value.bytes); break;
        }
        }
    }
    count = cursor; return core::Status::success();
}
void OwnedScriptValue::assign(const core::ScalarValue& input) noexcept {
    type = input.type; bytes = 0; bits = 0;
    switch (type) {
    case core::ValueType::boolean: bits = input.boolean; break;
    case core::ValueType::integer: bits = std::bit_cast<std::uint64_t>(input.integer); break;
    case core::ValueType::number: bits = std::bit_cast<std::uint64_t>(input.number); break;
    case core::ValueType::string:
        bytes = static_cast<std::uint16_t>(input.string.size());
        if (bytes) std::memmove(text.data(), input.string.data(), bytes);
        break;
    }
}
core::Status ScriptControlStore::acquire(core::DynamicSchemaLease& lease) const noexcept {
    if (lease.held()) return fail(core::ErrorCode::invalid_state, "lease-already-held");
    if (!active_.load(std::memory_order_acquire)) return core::Status::success();
    auto gate = gate_.load(std::memory_order_relaxed);
    // Bounded attempts support simultaneous readers without making a transport
    // spin indefinitely against an active publisher or contending readers.
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        if (gate >= kWriter - 1) return fail(core::ErrorCode::resource_unavailable, "schema-busy");
        if (!gate_.compare_exchange_weak(gate, gate + 1, std::memory_order_acquire)) continue;
        if (!active_.load(std::memory_order_acquire)) { gate_.fetch_sub(1, std::memory_order_release); return core::Status::success(); }
        const auto status = lease.bind(*this, sequence_, schema_.size());
        if (!status) gate_.fetch_sub(1, std::memory_order_release);
        return status;
    }
    return fail(core::ErrorCode::resource_unavailable, "schema-reader-contention");
}
void ScriptControlStore::release(std::uint32_t generation) const noexcept {
    // A valid lease keeps this generation stable until after this decrement.
    if (generation == sequence_) gate_.fetch_sub(1, std::memory_order_release);
}
core::Status ScriptControlStore::pin(std::uint32_t generation, core::DynamicSchemaLease& lease) const noexcept {
    auto status = acquire(lease); if (!status) return status;
    if (!generation || !lease.held() || lease.generation() != generation)
        return fail(core::ErrorCode::cancelled, "stale-schema-generation");
    return core::Status::success();
}
void ScriptControlStore::retire() noexcept {
    active_.store(0, std::memory_order_release); end_invocation();
}
bool ScriptControlStore::quiescent() const noexcept {
    return !active_.load(std::memory_order_acquire) && !gate_.load(std::memory_order_acquire) &&
        !data_.test(std::memory_order_acquire) && !action_data_.test(std::memory_order_acquire);
}
std::string_view ScriptControlStore::id(std::size_t index) const noexcept { return schema_.text(schema_.control(index).id); }
core::DynamicControlKind ScriptControlStore::kind(std::size_t index) const noexcept { return schema_.control(index).kind; }
core::Status ScriptControlStore::parameter(std::size_t index, core::ParameterDescriptor& output) const noexcept { return schema_.parameter(index, output); }
core::Status ScriptControlStore::action(std::size_t index, std::span<core::FieldDescriptor> fields, core::ActionDescriptor& output) const noexcept { return schema_.action(index, fields, output); }
core::Status ScriptControlStore::event(std::size_t index, std::span<core::FieldDescriptor> fields, core::EventDescriptor& output) const noexcept { return schema_.event(index, fields, output); }
std::size_t ScriptControlStore::find(std::string_view name) const noexcept {
    for (std::size_t i = 0; i < schema_.size(); ++i) if (id(i) == name) return i;
    return schema_.size();
}
core::Status ScriptControlStore::prepare(std::span<const std::byte> module, const core::ComponentDescriptor& reserved) noexcept {
    if (generation()) return fail(core::ErrorCode::invalid_state, "retire-before-prepare");
    WriteGuard write(*this); if (!write.held) return fail(core::ErrorCode::resource_unavailable, "schema-leased");
    DataGuard data(*this); if (!data.held) return fail(core::ErrorCode::resource_unavailable, "values-busy");
    ActionGuard actions(*this); if (!actions.held) return fail(core::ErrorCode::resource_unavailable, "actions-busy");
    if (action_count_) return fail(core::ErrorCode::resource_unavailable, "actions-awaiting-completion");
    prepared_ = false;
    auto status = parse_script_manifest(module, schema_); if (!status) return status;
    for (std::size_t i = 0; i < schema_.size(); ++i) {
        const auto control_id = id(i);
        for (const auto& p : reserved.parameters) if (p.id == control_id) return fail(core::ErrorCode::duplicate_id, "reserved-control-id");
        for (const auto& a : reserved.actions) if (a.id == control_id) return fail(core::ErrorCode::duplicate_id, "reserved-control-id");
        for (const auto& e : reserved.events) if (e.id == control_id) return fail(core::ErrorCode::duplicate_id, "reserved-control-id");
        for (const auto& p : reserved.legacy_parameters) if (p.id == control_id) return fail(core::ErrorCode::duplicate_id, "reserved-control-alias");
    }
    prepared_ = true; return core::Status::success();
}
core::Status ScriptControlStore::publish(Runtime& runtime, std::uint32_t module_generation) noexcept {
    WriteGuard write(*this); if (!write.held) return fail(core::ErrorCode::resource_unavailable, "schema-leased");
    DataGuard data(*this); if (!data.held) return fail(core::ErrorCode::resource_unavailable, "values-busy");
    ActionGuard actions(*this); if (!actions.held) return fail(core::ErrorCode::resource_unavailable, "actions-busy");
    if (!prepared_ || generation() || !module_generation || action_count_) return fail(core::ErrorCode::invalid_state, "publication-state");
    if (sequence_ == std::numeric_limits<std::uint32_t>::max()) return fail(core::ErrorCode::generation_exhausted, "schema-generation");
    bool needs_buffer = false;
    for (std::size_t i = 0; i < schema_.size(); ++i) {
        const auto& control = schema_.control(i);
        if (control.kind != core::DynamicControlKind::action) continue;
        const auto wanted = callback_signature(control);
        const auto actual = runtime.signature(schema_.text(control.callback));
        if (!actual) return core::Status::failure(actual.error());
        if (actual.value().argument_count != wanted.argument_count || actual.value().result_count != 0 ||
            !std::equal(wanted.arguments.begin(), wanted.arguments.begin() + wanted.argument_count, actual.value().arguments.begin()))
            return fail(core::ErrorCode::validation_failed, "callback-signature");
        for (std::size_t f = 0; f < control.field_count; ++f) needs_buffer |= control.fields[f].type == core::ValueType::string;
    }
    std::uint32_t buffer{};
    if (needs_buffer) {
        const auto global = runtime.immutable_i32_global(kScriptActionBufferGlobal);
        if (!global) return core::Status::failure(global.error());
        buffer = global.value();
        if (buffer > std::numeric_limits<std::uint32_t>::max() - kScriptActionBufferBytes)
            return fail(core::ErrorCode::invalid_argument, "callback-buffer-overflow");
        // Linear memory is contiguous: a checked last-byte read proves the
        // whole fixed scratch range fits without copying it onto the stack.
        std::array<std::byte, 1> last{};
        const auto checked = runtime.read_memory(buffer + kScriptActionBufferBytes - 1, last);
        if (!checked) return checked;
    }
    for (std::size_t i = 0; i < schema_.size(); ++i)
        if (schema_.control(i).kind == core::DynamicControlKind::parameter) values_[i].assign(schema_.default_value(i));
    action_buffer_ = buffer; module_generation_ = module_generation;
    event_head_ = event_count_ = 0; builder_token_.store(0, std::memory_order_release); prepared_ = false;
    ++sequence_; active_.store(sequence_, std::memory_order_release); return core::Status::success();
}
core::Status ScriptControlStore::read(std::uint32_t generation, std::size_t index, core::ScalarValue& output, std::span<char> strings, bool guest) noexcept {
    core::DynamicSchemaLease lease; auto status = pin(generation, lease); if (!status) return status;
    if (index >= schema_.size() || kind(index) != core::DynamicControlKind::parameter) return fail(core::ErrorCode::not_found, "parameter-index");
    if (!guest && schema_.control(index).access == core::Access::write_only) return fail(core::ErrorCode::invalid_state, "write-only");
    DataGuard data(*this); if (!data.held) return fail(core::ErrorCode::resource_unavailable, "values-busy");
    if (generation != active_.load(std::memory_order_acquire)) return fail(core::ErrorCode::cancelled, "retired-generation");
    const auto value = values_[index].scalar();
    if (value.type == core::ValueType::string) {
        if (strings.size() < value.string.size()) return fail(core::ErrorCode::capacity_exceeded, "read-string-storage");
        if (!value.string.empty()) std::memcpy(strings.data(), value.string.data(), value.string.size());
        output = core::ScalarValue::from_string({strings.data(), value.string.size()});
    } else output = value;
    return core::Status::success();
}
core::Status ScriptControlStore::write(std::uint32_t generation, std::size_t index, const core::ScalarValue& input, bool guest) noexcept {
    core::DynamicSchemaLease lease; auto status = pin(generation, lease); if (!status) return status;
    if (index >= schema_.size() || kind(index) != core::DynamicControlKind::parameter) return fail(core::ErrorCode::not_found, "parameter-index");
    const auto& control = schema_.control(index);
    if (!guest && control.access == core::Access::read_only) return fail(core::ErrorCode::invalid_state, "read-only");
    if (!valid_scalar(input, control.type) || !in_bounds(input, control.bounds)) return fail(core::ErrorCode::validation_failed, "parameter-value");
    DataGuard data(*this); if (!data.held) return fail(core::ErrorCode::resource_unavailable, "values-busy");
    if (generation != active_.load(std::memory_order_acquire)) return fail(core::ErrorCode::cancelled, "retired-generation");
    values_[index].assign(input); return core::Status::success();
}
core::Status ScriptControlStore::read_id(std::uint32_t generation, std::string_view name, core::ScalarValue& output, std::span<char> strings) noexcept {
    core::DynamicSchemaLease lease; auto status = pin(generation, lease); if (!status) return status;
    return read(generation, find(name), output, strings);
}
core::Status ScriptControlStore::write_id(std::uint32_t generation, std::string_view name, const core::ScalarValue& input) noexcept {
    core::DynamicSchemaLease lease; auto status = pin(generation, lease); if (!status) return status;
    return write(generation, find(name), input);
}
core::Status ScriptControlStore::enqueue_action(std::uint32_t generation, std::string_view name_id, std::span<const core::ScalarValue> input, std::uint32_t ticket, std::uint32_t epoch) noexcept {
    core::DynamicSchemaLease lease; auto status = pin(generation, lease); if (!status) return status;
    const auto index = find(name_id);
    if (index == schema_.size() || kind(index) != core::DynamicControlKind::action) return fail(core::ErrorCode::not_found, "action-id");
    const auto& control = schema_.control(index);
    if (!ticket || !epoch || input.size() != control.field_count) return fail(core::ErrorCode::invalid_argument, "action-arguments");
    for (std::size_t i = 0; i < input.size(); ++i)
        if (!valid_scalar(input[i], control.fields[i].type)) return fail(core::ErrorCode::validation_failed, "action-field-value");
    ActionGuard data(*this); if (!data.held) return fail(core::ErrorCode::resource_unavailable, "actions-busy");
    if (generation != active_.load(std::memory_order_acquire)) return fail(core::ErrorCode::cancelled, "retired-generation");
    if (action_count_ == actions_.size()) return fail(core::ErrorCode::queue_full, "action-queue-full");
    auto& item = actions_[(action_head_ + action_count_) % actions_.size()];
    item.generation = generation; item.module_generation = module_generation_; item.ticket = ticket; item.epoch = epoch;
    item.index = static_cast<std::uint8_t>(index); item.count = static_cast<std::uint8_t>(input.size()); name(item.name, schema_.text(control.callback));
    for (std::size_t i = 0; i < input.size(); ++i) item.values[i].assign(input[i]);
    ++action_count_; return core::Status::success();
}
core::Status ScriptControlStore::take_action(ScriptControlMessage& output) noexcept {
    ActionGuard data(*this); if (!data.held) return fail(core::ErrorCode::resource_unavailable, "actions-busy");
    if (!action_count_) return fail(core::ErrorCode::not_found, "no-action");
    output = actions_[action_head_]; action_head_ = static_cast<std::uint8_t>((action_head_ + 1) % actions_.size()); --action_count_;
    return core::Status::success();
}
core::Result<std::uint32_t> ScriptControlStore::begin_event(std::uint32_t generation, std::size_t index) noexcept {
    using Result = core::Result<std::uint32_t>;
    core::DynamicSchemaLease lease; auto status = pin(generation, lease); if (!status) return Result::failure(status.error());
    if (index >= schema_.size() || kind(index) != core::DynamicControlKind::event) return Result::failure(fail(core::ErrorCode::not_found, "event-index").error());
    DataGuard data(*this); if (!data.held) return Result::failure(fail(core::ErrorCode::resource_unavailable, "events-busy").error());
    if (generation != active_.load(std::memory_order_acquire)) return Result::failure(fail(core::ErrorCode::cancelled, "retired-generation").error());
    if (builder_token_.load(std::memory_order_acquire)) return Result::failure(fail(core::ErrorCode::invalid_state, "event-already-open").error());
    if (event_sequence_ == std::numeric_limits<std::uint32_t>::max()) return Result::failure(fail(core::ErrorCode::generation_exhausted, "event-token").error());
    builder_.generation = generation; builder_.module_generation = module_generation_; builder_.index = static_cast<std::uint8_t>(index);
    builder_.count = schema_.control(index).field_count; name(builder_.name, id(index)); field_mask_ = 0;
    ++event_sequence_; builder_token_.store(event_sequence_, std::memory_order_release); return Result::success(event_sequence_);
}
core::Status ScriptControlStore::event_bits(std::uint32_t token, std::size_t field, std::uint64_t bits) noexcept {
    core::DynamicSchemaLease lease; auto status = pin(generation(), lease); if (!status) return status;
    DataGuard data(*this); if (!data.held) return fail(core::ErrorCode::resource_unavailable, "events-busy");
    if (!token || token != builder_token_.load(std::memory_order_acquire) || builder_.generation != generation()) return fail(core::ErrorCode::cancelled, "stale-event-token");
    if (field >= builder_.count) return fail(core::ErrorCode::invalid_argument, "event-field");
    const auto type = schema_.control(builder_.index).fields[field].type;
    if (type == core::ValueType::string || (type == core::ValueType::boolean && bits > 1) ||
        (type == core::ValueType::number && !std::isfinite(std::bit_cast<double>(bits)))) return fail(core::ErrorCode::validation_failed, "event-field-bits");
    builder_.values[field].type = type; builder_.values[field].bits = bits; builder_.values[field].bytes = 0;
    field_mask_ |= static_cast<std::uint8_t>(1U << field); return core::Status::success();
}
core::Status ScriptControlStore::event_utf8(std::uint32_t token, std::size_t field, std::string_view input) noexcept {
    if (!valid_scalar(core::ScalarValue::from_string(input), core::ValueType::string)) return fail(core::ErrorCode::validation_failed, "event-field-string");
    core::DynamicSchemaLease lease; auto status = pin(generation(), lease); if (!status) return status;
    DataGuard data(*this); if (!data.held) return fail(core::ErrorCode::resource_unavailable, "events-busy");
    if (!token || token != builder_token_.load(std::memory_order_acquire) || builder_.generation != generation()) return fail(core::ErrorCode::cancelled, "stale-event-token");
    if (field >= builder_.count || schema_.control(builder_.index).fields[field].type != core::ValueType::string) return fail(core::ErrorCode::invalid_argument, "event-field");
    builder_.values[field].assign(core::ScalarValue::from_string(input)); field_mask_ |= static_cast<std::uint8_t>(1U << field);
    return core::Status::success();
}
core::Status ScriptControlStore::commit_event(std::uint32_t token) noexcept {
    DataGuard data(*this); if (!data.held) return fail(core::ErrorCode::resource_unavailable, "events-busy");
    if (!token || token != builder_token_.load(std::memory_order_acquire) || builder_.generation != generation()) return fail(core::ErrorCode::cancelled, "stale-event-token");
    if (field_mask_ != (1U << builder_.count) - 1) return fail(core::ErrorCode::invalid_state, "event-fields-incomplete");
    if (event_count_ == events_.size()) return fail(core::ErrorCode::queue_full, "event-queue-full");
    builder_.ticket = token; builder_.epoch = 0;
    events_[(event_head_ + event_count_) % events_.size()] = builder_; ++event_count_;
    builder_token_.store(0, std::memory_order_release); return core::Status::success();
}
core::Status ScriptControlStore::take_event(ScriptControlMessage& output) noexcept {
    DataGuard data(*this); if (!data.held) return fail(core::ErrorCode::resource_unavailable, "events-busy");
    // At most the fixed queue capacity can be discarded in one call.
    while (event_count_) {
        const auto& item = events_[event_head_]; const bool current = item.generation == generation();
        if (current) output = item;
        event_head_ = static_cast<std::uint8_t>((event_head_ + 1) % events_.size()); --event_count_;
        if (current) return core::Status::success();
    }
    return fail(core::ErrorCode::not_found, "no-event");
}
} // namespace blip::wasm
