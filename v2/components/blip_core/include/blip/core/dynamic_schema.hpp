#pragma once
#include "blip/core/descriptor.hpp"
#include "blip/core/error.hpp"
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

namespace blip::core {
inline constexpr std::size_t kMaximumDynamicSchemaControls = 16;
class DynamicSchemaSource;
// Pins one immutable schema generation. Owners retain their text and projected
// metadata until every lease is released. A lease never pins guest memory.
class DynamicSchemaLease final {
  public:
    DynamicSchemaLease() noexcept = default;
    ~DynamicSchemaLease() { reset(); }
    DynamicSchemaLease(const DynamicSchemaLease&) = delete;
    DynamicSchemaLease& operator=(const DynamicSchemaLease&) = delete;
    DynamicSchemaLease(DynamicSchemaLease&& other) noexcept { swap(other); }
    DynamicSchemaLease& operator=(DynamicSchemaLease&& other) noexcept {
        if (this != &other) { reset(); swap(other); } return *this;
    }
    void reset() noexcept;
    [[nodiscard]] bool held() const noexcept { return source_ != nullptr; }
    [[nodiscard]] std::uint32_t generation() const noexcept { return generation_; }
    [[nodiscard]] std::size_t size() const noexcept { return count_; }
    [[nodiscard]] std::string_view id(std::size_t) const noexcept;
    [[nodiscard]] DynamicControlKind kind(std::size_t) const noexcept;
    [[nodiscard]] Status parameter(std::size_t, ParameterDescriptor&) const noexcept;
    [[nodiscard]] Status action(std::size_t, std::span<FieldDescriptor>, ActionDescriptor&) const noexcept;
    [[nodiscard]] Status event(std::size_t, std::span<FieldDescriptor>, EventDescriptor&) const noexcept;
    // Called by a source only after it has successfully pinned its metadata.
    // Zero generations and rebinding a held lease are rejected.
    [[nodiscard]] Status bind(const DynamicSchemaSource&, std::uint32_t, std::size_t) noexcept;
  private:
    void swap(DynamicSchemaLease& other) noexcept {
        std::swap(source_, other.source_); std::swap(generation_, other.generation_); std::swap(count_, other.count_);
    }
    const DynamicSchemaSource* source_{};
    std::uint32_t generation_{};
    std::size_t count_{};
};
class DynamicSchemaSource {
  public:
    virtual ~DynamicSchemaSource() = default;
    // Immediate bounded admission. Empty/inactive sources return an empty
    // successful lease. Contention is explicit; never block a transport.
    [[nodiscard]] virtual Status acquire(DynamicSchemaLease&) const noexcept = 0;
    virtual void release(std::uint32_t generation) const noexcept = 0;
    [[nodiscard]] virtual std::string_view id(std::size_t) const noexcept = 0;
    [[nodiscard]] virtual DynamicControlKind kind(std::size_t) const noexcept = 0;
    [[nodiscard]] virtual Status parameter(std::size_t, ParameterDescriptor&) const noexcept = 0;
    [[nodiscard]] virtual Status action(std::size_t, std::span<FieldDescriptor>, ActionDescriptor&) const noexcept = 0;
    [[nodiscard]] virtual Status event(std::size_t, std::span<FieldDescriptor>, EventDescriptor&) const noexcept = 0;
};
inline Status dynamic_schema_error(std::string_view detail) noexcept {
    return Status::failure({ErrorDomain::descriptor, ErrorCode::invalid_state, {}, "dynamic-schema", detail});
}
inline void DynamicSchemaLease::reset() noexcept {
    if (source_) source_->release(generation_);
    source_ = nullptr; generation_ = 0; count_ = 0;
}
inline Status DynamicSchemaLease::bind(const DynamicSchemaSource& source, std::uint32_t generation, std::size_t count) noexcept {
    if (source_ || generation == 0 || count > kMaximumDynamicSchemaControls) return dynamic_schema_error("invalid-lease");
    source_ = &source; generation_ = generation; count_ = count; return Status::success();
}
inline std::string_view DynamicSchemaLease::id(std::size_t index) const noexcept {
    return source_ && index < count_ ? source_->id(index) : std::string_view{};
}
inline DynamicControlKind DynamicSchemaLease::kind(std::size_t index) const noexcept {
    return source_ && index < count_ ? source_->kind(index) : DynamicControlKind::parameter;
}
inline Status DynamicSchemaLease::parameter(std::size_t index, ParameterDescriptor& output) const noexcept {
    return source_ && index < count_ ? source_->parameter(index, output) : dynamic_schema_error("missing-control");
}
inline Status DynamicSchemaLease::action(std::size_t index, std::span<FieldDescriptor> fields, ActionDescriptor& output) const noexcept {
    return source_ && index < count_ ? source_->action(index, fields, output) : dynamic_schema_error("missing-control");
}
inline Status DynamicSchemaLease::event(std::size_t index, std::span<FieldDescriptor> fields, EventDescriptor& output) const noexcept {
    return source_ && index < count_ ? source_->event(index, fields, output) : dynamic_schema_error("missing-control");
}
} // namespace blip::core
