#pragma once

#include "blip/core/registry.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <span>
#include <string_view>

namespace blip::core {

inline constexpr std::size_t kMaxControlValues = 8;
inline constexpr std::size_t kControlResponseStringBytes = 512;

enum class ControlOperation : std::uint8_t {
    read_parameter = 1,
    write_parameter = 2,
    invoke_action = 3
};

struct ControlRequest {
    ControlOperation operation{ControlOperation::read_parameter};
    std::string_view component_id{};
    std::string_view control_id{};
    std::span<const ScalarValue> values{};
    // Zero resolves a dynamic generation at dispatch. Explicit tokens reject
    // requests made against a different schema before component admission.
    std::uint32_t generation{};
};

struct ControlResponse {
    std::array<ScalarValue, kMaxControlValues> values{};
    std::size_t value_count{};
    std::array<char, kControlResponseStringBytes> string_storage{};
    ControlResponse() noexcept = default;
    void clear() noexcept {
        values.fill({}); value_count = 0; string_storage.fill(0);
    }
    ControlResponse(const ControlResponse& other) noexcept { *this = other; }
    ControlResponse(ControlResponse&& other) noexcept { *this = other; }
    ControlResponse& operator=(const ControlResponse& other) noexcept {
        if (this == &other) return *this;
        values = other.values; value_count = other.value_count; string_storage = other.string_storage;
        const auto base = reinterpret_cast<std::uintptr_t>(other.string_storage.data());
        for (auto& value : values) {
            const auto address = reinterpret_cast<std::uintptr_t>(value.string.data());
            if (value.type == ValueType::string && address >= base && address - base <= string_storage.size() &&
                value.string.size() <= string_storage.size() - (address - base))
                value.string = {string_storage.data() + (address - base), value.string.size()};
        }
        return *this;
    }
    ControlResponse& operator=(ControlResponse&& other) noexcept { return *this = static_cast<const ControlResponse&>(other); }
    [[nodiscard]] bool owns(std::string_view text) const noexcept {
        if (text.empty()) return true;
        const auto base = reinterpret_cast<std::uintptr_t>(string_storage.data());
        const auto address = reinterpret_cast<std::uintptr_t>(text.data());
        return address >= base && address - base <= string_storage.size() && text.size() <= string_storage.size() - (address - base);
    }
    [[nodiscard]] Status own_strings() noexcept {
        if (value_count > values.size()) return dynamic_schema_error("result-count");
        std::size_t size{};
        for (std::size_t i = 0; i < value_count; ++i) {
            if (values[i].type != ValueType::string) continue;
            if (values[i].string.size() > string_storage.size() - size)
                return Status::failure({ErrorDomain::control, ErrorCode::capacity_exceeded, {}, "response", "string-storage"});
            size += values[i].string.size();
        }
        if (size == 0) return Status::success();
        bool all_owned = true;
        for (std::size_t i = 0; i < value_count; ++i)
            if (values[i].type == ValueType::string && !owns(values[i].string)) all_owned = false;
        if (all_owned) return Status::success();
        // Copy all bytes before rebasing any view, including overlapping slices
        // or a mixture of borrowed and already-owned strings.
        std::array<char, kControlResponseStringBytes> copied{};
        size = 0;
        for (std::size_t i = 0; i < value_count; ++i) {
            auto& value = values[i]; if (value.type != ValueType::string) continue;
            if (!value.string.empty()) std::memcpy(copied.data() + size, value.string.data(), value.string.size());
            value.string = {string_storage.data() + size, value.string.size()}; size += value.string.size();
        }
        string_storage = copied;
        return Status::success();
    }
};

class ControlService {
  public:
    virtual ~ControlService() = default;
    [[nodiscard]] virtual Status execute(const ControlRequest& request,
                                         ControlResponse& response) noexcept = 0;
};

template <std::size_t MaxComponents, std::size_t MaxDynamicControls = 16>
class RegistryControlService final : public Component, public ControlService {
  public:
    explicit RegistryControlService(Registry<MaxComponents, MaxDynamicControls>& registry) noexcept
        : registry_(&registry) {}

    [[nodiscard]] const ComponentDescriptor& descriptor() const noexcept override {
        return descriptor_;
    }

    [[nodiscard]] Status start(const StartContext&) noexcept override {
        started_ = true;
        return Status::success();
    }

    [[nodiscard]] Status stop() noexcept override {
        started_ = false;
        return Status::success();
    }

    [[nodiscard]] Status execute(const ControlRequest& request,
                                 ControlResponse& response) noexcept override {
        response.clear();
        if (!started_ || request.component_id.empty() || request.control_id.empty() ||
            request.values.size() > kMaxControlValues) {
            return failure(ErrorCode::invalid_state, request.component_id, "dispatch",
                           started_ ? "invalid-request" : "not-started");
        }
        const auto* entry = registry_->find(request.component_id);
        if (entry == nullptr) {
            return failure(ErrorCode::not_found, request.component_id, "dispatch",
                           "component-not-found");
        }
        if (entry->state != ComponentState::running) {
            return failure(ErrorCode::invalid_state, request.component_id, "dispatch",
                           "component-not-running");
        }
        Status status = failure(ErrorCode::invalid_argument, request.component_id, "dispatch", "invalid-operation");
        switch (request.operation) {
        case ControlOperation::read_parameter:
            status = read(*entry, request, response); break;
        case ControlOperation::write_parameter:
            status = write(*entry, request, response); break;
        case ControlOperation::invoke_action:
            status = invoke(*entry, request, response); break;
        }
        if (status) status = response.own_strings();
        if (!status) response.clear();
        return status;
    }

  private:
    using RegistryType = Registry<MaxComponents, MaxDynamicControls>;
    using Entry = typename RegistryType::Entry;

    [[nodiscard]] static constexpr ComponentDescriptor make_descriptor() noexcept {
        ComponentDescriptor descriptor{};
        descriptor.schema_version = 1;
        descriptor.id = "blip.control";
        descriptor.display_name = "Control dispatcher";
        descriptor.description = "Registry-validated parameter and action dispatch";
        descriptor.provided_services = provided_services_;
        descriptor.settings = {1, 1};
        descriptor.disable_policy = DisablePolicy::reboot_required;
        descriptor.cost = {8192, 512, 0};
        return descriptor;
    }

    [[nodiscard]] static Status failure(ErrorCode code, std::string_view component,
                                        std::string_view operation,
                                        std::string_view detail) noexcept {
        return Status::failure({ErrorDomain::control, code, component, operation, detail});
    }

    [[nodiscard]] static const ParameterDescriptor*
    find_parameter(const ComponentDescriptor& descriptor, std::string_view id) noexcept {
        const auto found =
            std::find_if(descriptor.parameters.begin(), descriptor.parameters.end(),
                         [id](const ParameterDescriptor& parameter) { return parameter.id == id; });
        return found == descriptor.parameters.end() ? nullptr : &*found;
    }

    [[nodiscard]] static const ActionDescriptor* find_action(const ComponentDescriptor& descriptor,
                                                             std::string_view id) noexcept {
        const auto found =
            std::find_if(descriptor.actions.begin(), descriptor.actions.end(),
                         [id](const ActionDescriptor& action) { return action.id == id; });
        return found == descriptor.actions.end() ? nullptr : &*found;
    }

    [[nodiscard]] static bool value_in_bounds(const ScalarValue& value,
                                              const ParameterDescriptor& parameter) noexcept {
        if (!parameter.bounds.present) {
            return true;
        }
        const double numeric =
            value.type == ValueType::integer ? static_cast<double>(value.integer) : value.number;
        return std::isfinite(numeric) && numeric >= parameter.bounds.minimum &&
               numeric <= parameter.bounds.maximum;
    }

    [[nodiscard]] static Status read(const Entry& entry, const ControlRequest& request,
                                     ControlResponse& response) noexcept {
        const auto* parameter = find_parameter(*entry.descriptor, request.control_id);
        if (parameter == nullptr) {
            return dynamic(entry, request, response);
        }
        if (parameter->access == Access::write_only) {
            return failure(ErrorCode::invalid_state, request.component_id, "read-parameter",
                           "write-only");
        }
        if (!request.values.empty()) {
            return failure(ErrorCode::invalid_argument, request.component_id, "read-parameter",
                           "values-not-allowed");
        }
        auto status = entry.instance->read_parameter_owned(request.control_id, response.values[0], response.string_storage);
        if (!status) {
            return status;
        }
        if (response.values[0].type != parameter->type) {
            return failure(ErrorCode::validation_failed, request.component_id, "read-parameter",
                           "component-returned-wrong-type");
        }
        response.value_count = 1;
        return Status::success();
    }

    [[nodiscard]] static Status write(const Entry& entry, const ControlRequest& request, ControlResponse& response) noexcept {
        const auto* parameter = find_parameter(*entry.descriptor, request.control_id);
        if (parameter == nullptr) {
            return dynamic(entry, request, response);
        }
        if (parameter->access == Access::read_only) {
            return failure(ErrorCode::invalid_state, request.component_id, "write-parameter",
                           "read-only");
        }
        if (request.values.size() != 1U || request.values[0].type != parameter->type ||
            !value_in_bounds(request.values[0], *parameter)) {
            return failure(ErrorCode::validation_failed, request.component_id, "write-parameter",
                           "value-does-not-match-schema");
        }
        return entry.instance->write_parameter(request.control_id, request.values[0]);
    }

    [[nodiscard]] static Status invoke(const Entry& entry, const ControlRequest& request,
                                       ControlResponse& response) noexcept {
        const auto* action = find_action(*entry.descriptor, request.control_id);
        if (action == nullptr) {
            return dynamic(entry, request, response);
        }
        if (request.values.size() != action->arguments.size()) {
            return failure(ErrorCode::validation_failed, request.component_id, "invoke-action",
                           "argument-count");
        }
        for (std::size_t index = 0; index < request.values.size(); ++index) {
            if (request.values[index].type != action->arguments[index].type) {
                return failure(ErrorCode::validation_failed, request.component_id, "invoke-action",
                               "argument-type");
            }
        }
        auto status = entry.instance->invoke_action(request.control_id, request.values,
                                                    response.values, response.value_count);
        if (!status) {
            return status;
        }
        if (response.value_count > response.values.size()) {
            response.clear();
            return failure(ErrorCode::capacity_exceeded, request.component_id, "invoke-action",
                           "too-many-results");
        }
        return Status::success();
    }

    [[nodiscard]] static Status dynamic(const Entry& entry, const ControlRequest& request, ControlResponse& response) noexcept {
        const auto* source = entry.instance->dynamic_schema();
        if (!source) return failure(ErrorCode::not_found, request.component_id, "dynamic", "control-not-found");
        DynamicSchemaLease lease;
        auto status = source->acquire(lease); if (!status) return status;
        if (!lease.held()) return failure(ErrorCode::not_found, request.component_id, "dynamic", "inactive-schema");
        if (request.generation && request.generation != lease.generation())
            return failure(ErrorCode::cancelled, request.component_id, "dynamic", "stale-schema-generation");
        for (std::size_t i = 0; i < lease.size(); ++i) {
            if (lease.id(i) != request.control_id) continue;
            if (request.operation == ControlOperation::invoke_action) {
                std::array<FieldDescriptor, kMaxControlValues> fields{}; ActionDescriptor action{};
                status = lease.action(i, fields, action); if (!status) return status;
                if (request.values.size() != action.arguments.size())
                    return failure(ErrorCode::validation_failed, request.component_id, "dynamic-action", "argument-count");
                for (std::size_t v = 0; v < request.values.size(); ++v)
                    if (request.values[v].type != action.arguments[v].type ||
                        (request.values[v].type == ValueType::number && !std::isfinite(request.values[v].number)))
                        return failure(ErrorCode::validation_failed, request.component_id, "dynamic-action", "argument-type");
                status = entry.instance->invoke_dynamic_action(lease.generation(), request.control_id, request.values,
                    response.values, response.string_storage, response.value_count);
                if (status && response.value_count > response.values.size())
                    return failure(ErrorCode::capacity_exceeded, request.component_id, "dynamic-action", "result-count");
                if (status)
                    for (std::size_t v = 0; v < response.value_count; ++v)
                        if (response.values[v].type == ValueType::string && !response.owns(response.values[v].string))
                            return failure(ErrorCode::validation_failed, request.component_id, "dynamic-action", "result-storage");
                return status;
            }
            ParameterDescriptor parameter{};
            status = lease.parameter(i, parameter); if (!status) return status;
            if (request.operation == ControlOperation::read_parameter) {
                if (parameter.access == Access::write_only || !request.values.empty())
                    return failure(ErrorCode::invalid_argument, request.component_id, "dynamic-read", "access-or-arguments");
                status = entry.instance->read_dynamic_parameter(lease.generation(), request.control_id, response.values[0], response.string_storage);
                if (!status) return status;
                if (response.values[0].type != parameter.type ||
                    (parameter.type == ValueType::string && !response.owns(response.values[0].string)))
                    return failure(ErrorCode::validation_failed, request.component_id, "dynamic-read", "result-type-or-storage");
                response.value_count = 1; return Status::success();
            }
            if (parameter.access == Access::read_only || request.values.size() != 1 ||
                request.values[0].type != parameter.type || !value_in_bounds(request.values[0], parameter) ||
                (parameter.type == ValueType::number && !std::isfinite(request.values[0].number)))
                return failure(ErrorCode::validation_failed, request.component_id, "dynamic-write", "access-type-or-bounds");
            return entry.instance->write_dynamic_parameter(lease.generation(), request.control_id, request.values[0]);
        }
        return failure(ErrorCode::not_found, request.component_id, "dynamic", "control-not-found");
    }

    inline static constexpr std::array<std::string_view, 1> provided_services_{"control.dispatch"};
    inline static constexpr ComponentDescriptor descriptor_{make_descriptor()};

    RegistryType* registry_{};
    bool started_{};
};

} // namespace blip::core
