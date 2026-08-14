#pragma once

#include "blip/core/registry.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <span>
#include <string_view>

namespace blip::core {

inline constexpr std::size_t kMaxControlValues = 8;

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
};

struct ControlResponse {
    std::array<ScalarValue, kMaxControlValues> values{};
    std::size_t value_count{};
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
        response = {};
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
        switch (request.operation) {
        case ControlOperation::read_parameter:
            return read(*entry, request, response);
        case ControlOperation::write_parameter:
            return write(*entry, request);
        case ControlOperation::invoke_action:
            return invoke(*entry, request, response);
        }
        return failure(ErrorCode::invalid_argument, request.component_id, "dispatch",
                       "invalid-operation");
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
            return failure(ErrorCode::not_found, request.component_id, "read-parameter",
                           "parameter-not-found");
        }
        if (parameter->access == Access::write_only) {
            return failure(ErrorCode::invalid_state, request.component_id, "read-parameter",
                           "write-only");
        }
        if (!request.values.empty()) {
            return failure(ErrorCode::invalid_argument, request.component_id, "read-parameter",
                           "values-not-allowed");
        }
        auto status = entry.instance->read_parameter(request.control_id, response.values[0]);
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

    [[nodiscard]] static Status write(const Entry& entry, const ControlRequest& request) noexcept {
        const auto* parameter = find_parameter(*entry.descriptor, request.control_id);
        if (parameter == nullptr) {
            return failure(ErrorCode::not_found, request.component_id, "write-parameter",
                           "parameter-not-found");
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
            return failure(ErrorCode::not_found, request.component_id, "invoke-action",
                           "action-not-found");
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
            response = {};
            return failure(ErrorCode::capacity_exceeded, request.component_id, "invoke-action",
                           "too-many-results");
        }
        return Status::success();
    }

    inline static constexpr std::array<std::string_view, 1> provided_services_{"control.dispatch"};
    inline static constexpr ComponentDescriptor descriptor_{make_descriptor()};

    RegistryType* registry_{};
    bool started_{};
};

} // namespace blip::core
