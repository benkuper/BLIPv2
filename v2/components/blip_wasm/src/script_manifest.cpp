#include "blip/wasm/script_manifest.hpp"
#include "blip/wasm/utf8.hpp"
#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstring>

namespace blip::wasm {
namespace {
using core::ErrorCode;
using core::Status;
static_assert(static_cast<unsigned>(core::ValueType::boolean) == 0 &&
    static_cast<unsigned>(core::ValueType::integer) == 1 &&
    static_cast<unsigned>(core::ValueType::number) == 2 &&
    static_cast<unsigned>(core::ValueType::string) == 3);
static_assert(static_cast<unsigned>(core::Access::read_only) == 0 &&
    static_cast<unsigned>(core::Access::write_only) == 1 &&
    static_cast<unsigned>(core::Access::read_write) == 2);
static_assert(static_cast<unsigned>(core::DynamicControlKind::parameter) == 0 &&
    static_cast<unsigned>(core::DynamicControlKind::action) == 1 &&
    static_cast<unsigned>(core::DynamicControlKind::event) == 2);
Status failure(ErrorCode code, std::string_view detail) noexcept {
    return Status::failure({core::ErrorDomain::descriptor, code, "blip.wasm", "script-manifest", detail});
}
class Reader final {
  public:
    explicit Reader(std::span<const std::byte> bytes) noexcept : bytes_(bytes) {}
    bool byte(unsigned& value) noexcept {
        if (empty()) return false;
        value = std::to_integer<unsigned>(bytes_[position_++]); return true;
    }
    bool u32(std::uint32_t& value) noexcept {
        value = 0;
        for (unsigned i = 0; i < 5; ++i) {
            unsigned b{};
            if (!byte(b) || (i == 4 && (b & 0xf0U))) return false;
            value |= (b & 0x7fU) << (i * 7);
            if (!(b & 0x80U)) return true;
        }
        return false;
    }
    bool take(std::uint32_t size, std::span<const std::byte>& value) noexcept {
        if (size > bytes_.size() - position_) return false;
        value = bytes_.subspan(position_, size); position_ += size; return true;
    }
    bool text(std::string_view& value) noexcept {
        std::uint32_t size{}; std::span<const std::byte> data;
        if (!u32(size) || !take(size, data) || !valid_utf8(data)) return false;
        value = {reinterpret_cast<const char*>(data.data()), data.size()}; return true;
    }
    bool u64(std::uint64_t& value) noexcept {
        value = 0;
        for (unsigned i = 0; i < 8; ++i) {
            unsigned b{}; if (!byte(b)) return false;
            value |= static_cast<std::uint64_t>(b) << (i * 8);
        }
        return true;
    }
    bool number(double& value) noexcept {
        std::uint64_t bits{}; if (!u64(bits)) return false;
        value = std::bit_cast<double>(bits); return std::isfinite(value);
    }
    bool empty() const noexcept { return position_ == bytes_.size(); }
  private:
    std::span<const std::byte> bytes_;
    std::size_t position_{};
};
bool public_id(std::string_view id) noexcept {
    if (id.empty() || id.front() < 'a' || id.front() > 'z') return false;
    return std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}
bool export_id(std::string_view id) noexcept {
    const auto letter = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
    if (id.empty() || !letter(id.front())) return false;
    return std::all_of(id.begin(), id.end(), [letter](char c) { return letter(c) || (c >= '0' && c <= '9'); });
}
bool display_text(std::string_view text) noexcept {
    return std::all_of(text.begin(), text.end(), [](unsigned char c) { return c >= 32 && c != 127; });
}
bool integer_bound(double value) noexcept {
    constexpr double maximum_exact = 0x1fffffffffffff;
    return std::abs(value) <= maximum_exact && std::trunc(value) == value;
}
} // namespace

class ScriptManifestDecoder final {
  public:
    Status decode(std::span<const std::byte> bytes, ScriptManifest* output) noexcept {
        constexpr std::array header{std::byte{0}, std::byte{0x61}, std::byte{0x73}, std::byte{0x6d},
            std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0}};
        if (bytes.size() > 16384) return failure(ErrorCode::capacity_exceeded, "module-size");
        if (bytes.size() < header.size() || !std::equal(header.begin(), header.end(), bytes.begin()))
            return failure(ErrorCode::corrupt_data, "module-header");
        output_ = output;
        if (output_) {
            output_->controls_.fill({}); output_->text_.fill(0);
            output_->count_ = output_->text_bytes_ = 0;
        }
        Reader module(bytes.subspan(header.size()));
        bool found = false;
        while (!module.empty()) {
            unsigned id{}; std::uint32_t size{}; std::span<const std::byte> payload;
            if (!module.byte(id) || !module.u32(size) || !module.take(size, payload))
                return failure(ErrorCode::corrupt_data, "section-framing");
            if (id != 0) continue;
            Reader section(payload); std::string_view name;
            if (!section.text(name)) return failure(ErrorCode::corrupt_data, "section-name");
            if (name != kScriptControlsSection) {
                if (name.starts_with("blip.controls.")) return failure(ErrorCode::incompatible_version, "declaration-version");
                continue;
            }
            if (found) return failure(ErrorCode::duplicate_id, "declaration-section");
            found = true;
            if (payload.size() > kMaximumScriptManifestBytes) return failure(ErrorCode::capacity_exceeded, "declaration-size");
            std::span<const std::byte> magic;
            constexpr std::array expected{std::byte{'B'}, std::byte{'C'}, std::byte{'M'}, std::byte{'1'}};
            if (!section.take(4, magic)) return failure(ErrorCode::corrupt_data, "declaration-header");
            if (!std::equal(magic.begin(), magic.end(), expected.begin())) return failure(ErrorCode::incompatible_version, "declaration-header");
            std::uint32_t count{};
            if (!section.u32(count)) return failure(ErrorCode::corrupt_data, "control-count");
            if (count > kMaximumScriptControls) return failure(ErrorCode::capacity_exceeded, "control-count");
            std::array<std::string_view, kMaximumScriptControls> ids{};
            for (std::uint32_t i = 0; i < count; ++i) {
                ScriptControl control; unsigned kind{};
                if (!section.byte(kind) || kind > 2) return failure(ErrorCode::corrupt_data, "control-kind");
                control.kind = static_cast<core::DynamicControlKind>(kind);
                if (!read_text(section, ids[i], control.id, 32, false) || !public_id(ids[i])) return error("control-id");
                for (std::uint32_t prior = 0; prior < i; ++prior)
                    if (ids[prior] == ids[i]) return failure(ErrorCode::duplicate_id, "control-id");
                std::string_view label;
                if (!read_text(section, label, control.label, 64, false) || !display_text(label)) return error("control-label");
                if (kind == 0) {
                    if (!parameter(section, control)) return error("parameter");
                } else {
                    if (kind == 1) {
                        std::string_view callback;
                        if (!read_text(section, callback, control.callback, 64, false) || !export_id(callback)) return error("action-export");
                    }
                    if (!fields(section, control)) return error("control-fields");
                }
                if (output_) output_->controls_[i] = control;
            }
            if (!section.empty()) return failure(ErrorCode::corrupt_data, "trailing-declaration-bytes");
            if (output_) output_->count_ = static_cast<std::uint16_t>(count);
        }
        if (output_) output_->text_bytes_ = static_cast<std::uint16_t>(text_bytes_);
        return Status::success();
    }
  private:
    Status error(std::string_view detail) const noexcept { return failure(code_, detail); }
    bool read_text(Reader& reader, std::string_view& value, ScriptTextRef& ref,
                   std::size_t maximum, bool allow_empty) noexcept {
        if (!reader.text(value) || (!allow_empty && value.empty())) return false;
        if (value.size() > maximum || value.size() > kScriptManifestTextBytes - text_bytes_) {
            code_ = ErrorCode::capacity_exceeded; return false;
        }
        ref = {static_cast<std::uint16_t>(text_bytes_), static_cast<std::uint16_t>(value.size())};
        if (output_ && !value.empty()) std::memcpy(output_->text_.data() + text_bytes_, value.data(), value.size());
        text_bytes_ += value.size(); return true;
    }
    bool type(Reader& reader, core::ValueType& value) noexcept {
        unsigned type{}; if (!reader.byte(type) || type > 3) return false;
        value = static_cast<core::ValueType>(type); return true;
    }
    bool parameter(Reader& reader, ScriptControl& control) noexcept {
        unsigned access{};
        if (!type(reader, control.type) || !reader.byte(access) || access > 2) return false;
        control.access = static_cast<core::Access>(access);
        std::string_view unit;
        if (!read_text(reader, unit, control.unit, 16, true) || !display_text(unit)) return false;
        switch (control.type) {
        case core::ValueType::boolean: {
            unsigned value{}; if (!reader.byte(value) || value > 1) return false;
            control.default_bits = value; break;
        }
        case core::ValueType::integer:
            if (!reader.u64(control.default_bits)) return false;
            break;
        case core::ValueType::number: {
            double value{}; if (!reader.number(value)) return false;
            control.default_bits = std::bit_cast<std::uint64_t>(value); break;
        }
        case core::ValueType::string: {
            std::string_view value;
            if (!read_text(reader, value, control.default_text, 128, true)) return false;
            break;
        }
        }
        unsigned bounds{}; if (!reader.byte(bounds) || bounds > 1) return false;
        if (!bounds) return true;
        auto& b = control.bounds; b.present = true;
        if ((control.type != core::ValueType::integer && control.type != core::ValueType::number) ||
            !reader.number(b.minimum) || !reader.number(b.maximum) || !reader.number(b.step) ||
            b.minimum > b.maximum || b.step <= 0) return false;
        if (control.type == core::ValueType::integer) {
            if (!integer_bound(b.minimum) || !integer_bound(b.maximum) || !integer_bound(b.step)) return false;
            const auto value = std::bit_cast<std::int64_t>(control.default_bits);
            return value >= static_cast<std::int64_t>(b.minimum) && value <= static_cast<std::int64_t>(b.maximum);
        }
        const auto value = std::bit_cast<double>(control.default_bits);
        return value >= b.minimum && value <= b.maximum;
    }
    bool fields(Reader& reader, ScriptControl& control) noexcept {
        std::uint32_t count{}; if (!reader.u32(count)) return false;
        if (count > kMaximumScriptFields) { code_ = ErrorCode::capacity_exceeded; return false; }
        control.field_count = static_cast<std::uint8_t>(count);
        std::array<std::string_view, kMaximumScriptFields> ids{};
        for (std::uint32_t i = 0; i < count; ++i) {
            if (!read_text(reader, ids[i], control.fields[i].id, 32, false) || !public_id(ids[i]) ||
                !type(reader, control.fields[i].type)) return false;
            for (std::uint32_t prior = 0; prior < i; ++prior)
                if (ids[prior] == ids[i]) { code_ = ErrorCode::duplicate_id; return false; }
        }
        return true;
    }
    ScriptManifest* output_{};
    std::size_t text_bytes_{};
    ErrorCode code_{ErrorCode::corrupt_data};
};

Status parse_script_manifest(std::span<const std::byte> module, ScriptManifest& output) noexcept {
    ScriptManifestDecoder validator;
    const auto valid = validator.decode(module, nullptr);
    if (!valid) return valid;
    ScriptManifestDecoder writer;
    const auto written = writer.decode(module, &output);
    assert(written.ok()); // identical bytes were fully validated before mutation
    return written;
}
std::string_view ScriptManifest::text(ScriptTextRef ref) const noexcept {
    if (ref.offset > text_bytes_ || ref.bytes > text_bytes_ - ref.offset) return {};
    return {text_.data() + ref.offset, ref.bytes};
}
core::ScalarValue ScriptManifest::default_value(std::size_t index) const noexcept {
    if (index >= count_) return {};
    const auto& c = controls_[index];
    switch (c.type) {
    case core::ValueType::boolean: return core::ScalarValue::from_bool(c.default_bits != 0);
    case core::ValueType::integer: return core::ScalarValue::from_integer(std::bit_cast<std::int64_t>(c.default_bits));
    case core::ValueType::number: return core::ScalarValue::from_number(std::bit_cast<double>(c.default_bits));
    case core::ValueType::string: return core::ScalarValue::from_string(text(c.default_text));
    }
    return {};
}
Status ScriptManifest::parameter(std::size_t index, core::ParameterDescriptor& output) const noexcept {
    if (index >= count_ || controls_[index].kind != core::DynamicControlKind::parameter)
        return failure(ErrorCode::invalid_argument, "not-parameter");
    const auto& c = controls_[index]; core::ParameterDescriptor p{};
    p.id = text(c.id); p.label = text(c.label); p.type = c.type; p.access = c.access;
    p.default_value = default_value(index); p.bounds = c.bounds; p.unit = text(c.unit);
    output = p; return Status::success();
}
Status ScriptManifest::action(std::size_t index, std::span<core::FieldDescriptor> fields,
                              core::ActionDescriptor& output) const noexcept {
    if (index >= count_ || controls_[index].kind != core::DynamicControlKind::action)
        return failure(ErrorCode::invalid_argument, "not-action");
    const auto& c = controls_[index];
    if (fields.size() < c.field_count) return failure(ErrorCode::capacity_exceeded, "field-output");
    for (std::size_t i = 0; i < c.field_count; ++i) fields[i] = {text(c.fields[i].id), c.fields[i].type, true};
    output = {text(c.id), text(c.label), fields.first(c.field_count)}; return Status::success();
}
Status ScriptManifest::event(std::size_t index, std::span<core::FieldDescriptor> fields,
                             core::EventDescriptor& output) const noexcept {
    if (index >= count_ || controls_[index].kind != core::DynamicControlKind::event)
        return failure(ErrorCode::invalid_argument, "not-event");
    const auto& c = controls_[index];
    if (fields.size() < c.field_count) return failure(ErrorCode::capacity_exceeded, "field-output");
    for (std::size_t i = 0; i < c.field_count; ++i) fields[i] = {text(c.fields[i].id), c.fields[i].type, true};
    output = {text(c.id), fields.first(c.field_count)}; return Status::success();
}
} // namespace blip::wasm
