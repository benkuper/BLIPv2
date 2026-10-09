#pragma once

#include "blip/core/descriptor.hpp"
#include "blip/core/dynamic_schema.hpp"
#include "blip/core/error.hpp"

#include <cstddef>
#include <span>
#include <string_view>

namespace blip::wasm { class CapabilityProvider; }

namespace blip::core {

enum class ComponentState : std::uint8_t {
    absent,
    constructed,
    validated,
    starting,
    running,
    suspended,
    failed,
    stopping,
    stopped,
};

struct ValidationContext final {};
struct StartContext final {};

class Component {
  public:
    Component() = default;
    virtual ~Component() = default;
    Component(const Component&) = delete;
    Component& operator=(const Component&) = delete;
    Component(Component&&) = delete;
    Component& operator=(Component&&) = delete;

    [[nodiscard]] virtual const ComponentDescriptor& descriptor() const noexcept = 0;
    // Optional, component-owned interface. No WASM engine dependency in core.
    [[nodiscard]] virtual wasm::CapabilityProvider* wasm_provider() noexcept { return nullptr; }
    [[nodiscard]] virtual const DynamicSchemaSource* dynamic_schema() const noexcept { return nullptr; }
    // Dynamic operations carry the leased schema generation. String reads copy
    // into caller-owned scratch; they must not return mutable owner/guest views.
    [[nodiscard]] virtual Status read_dynamic_parameter(std::uint32_t, std::string_view,
        ScalarValue&, std::span<char>) noexcept { return dynamic_schema_error("read-unsupported"); }
    [[nodiscard]] virtual Status write_dynamic_parameter(std::uint32_t, std::string_view,
        const ScalarValue&) noexcept { return dynamic_schema_error("write-unsupported"); }
    [[nodiscard]] virtual Status invoke_dynamic_action(std::uint32_t, std::string_view,
        std::span<const ScalarValue>, std::span<ScalarValue>, std::span<char>, std::size_t& count) noexcept {
        count = 0; return dynamic_schema_error("action-unsupported");
    }
    [[nodiscard]] virtual Status validate(const ValidationContext&) noexcept {
        return Status::success();
    }
    [[nodiscard]] virtual Status start(const StartContext&) noexcept = 0;
    [[nodiscard]] virtual Status suspend() noexcept { return Status::success(); }
    [[nodiscard]] virtual Status resume() noexcept { return Status::success(); }
    [[nodiscard]] virtual Status stop() noexcept = 0;
    [[nodiscard]] virtual bool callbacks_quiesced() const noexcept { return true; }
    [[nodiscard]] virtual Status read_parameter(std::string_view id, ScalarValue&) noexcept {
        return Status::failure(
            {ErrorDomain::control, ErrorCode::not_found, descriptor().id, "read-parameter", id});
    }
    // Mutable string owners override this and copy while holding their own
    // guard. The caller's response storage outlives dispatch/serialization.
    [[nodiscard]] virtual Status read_parameter_owned(std::string_view id, ScalarValue& value,
                                                       std::span<char>) noexcept {
        return read_parameter(id, value);
    }
    [[nodiscard]] virtual Status write_parameter(std::string_view id, const ScalarValue&) noexcept {
        return Status::failure(
            {ErrorDomain::control, ErrorCode::not_found, descriptor().id, "write-parameter", id});
    }
    [[nodiscard]] virtual Status invoke_action(std::string_view id, std::span<const ScalarValue>,
                                               std::span<ScalarValue>,
                                               std::size_t& output_count) noexcept {
        output_count = 0;
        return Status::failure(
            {ErrorDomain::control, ErrorCode::not_found, descriptor().id, "invoke-action", id});
    }
};

} // namespace blip::core
