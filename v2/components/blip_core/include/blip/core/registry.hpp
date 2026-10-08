#pragma once

#include "blip/core/component.hpp"
#include "blip/core/fixed_vector.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>

namespace blip::core {

class RegistryView {
  public:
    virtual ~RegistryView() = default;
    [[nodiscard]] virtual std::size_t component_count() const noexcept = 0;
    [[nodiscard]] virtual const ComponentDescriptor&
    component_descriptor(std::size_t index) const noexcept = 0;
    [[nodiscard]] virtual std::size_t dynamic_control_count() const noexcept = 0;
    [[nodiscard]] virtual const DynamicControl&
    dynamic_control(std::size_t index) const noexcept = 0;
    [[nodiscard]] virtual wasm::CapabilityProvider* wasm_provider(std::size_t) const noexcept {
        return nullptr;
    }
};

template <std::size_t MaxComponents> struct StartupReport {
    Error primary{};
    FixedVector<Error, MaxComponents> cleanup_errors{};

    [[nodiscard]] constexpr bool ok() const noexcept { return !primary.valid(); }
};

template <std::size_t MaxComponents, std::size_t MaxDynamicControls = 16>
class Registry final : public RegistryView {
  public:
    struct Entry {
        const ComponentDescriptor* descriptor{};
        Component* instance{};
        ComponentState state{ComponentState::absent};
    };

    class ExtensionTransaction {
      public:
        explicit ExtensionTransaction(Registry& registry) noexcept : registry_(&registry) {}

        [[nodiscard]] Status add(DynamicControl control) noexcept {
            if (committed_) {
                return failure(ErrorCode::invalid_state, "extension.add", control.id);
            }
            if (!registry_->has_component(control.component_id) || control.id.empty()) {
                return failure(ErrorCode::invalid_argument, "extension.add", control.id);
            }
            for (const auto& staged : staged_) {
                if (same_key(staged, control)) {
                    return failure(ErrorCode::duplicate_id, "extension.add", control.id);
                }
            }
            if (!staged_.push_back(control)) {
                return failure(ErrorCode::capacity_exceeded, "extension.add", control.id);
            }
            return Status::success();
        }

        [[nodiscard]] Status commit() noexcept {
            if (committed_) {
                return failure(ErrorCode::invalid_state, "extension.commit", {});
            }
            if (registry_->dynamic_controls_.size() + staged_.size() > MaxDynamicControls) {
                return failure(ErrorCode::capacity_exceeded, "extension.commit", {});
            }
            for (const auto& staged : staged_) {
                if (registry_->control_exists(staged)) {
                    return failure(ErrorCode::duplicate_id, "extension.commit", staged.id);
                }
            }
            for (const auto& staged : staged_) {
                static_cast<void>(registry_->dynamic_controls_.push_back(staged));
            }
            committed_ = true;
            return Status::success();
        }

      private:
        [[nodiscard]] Status failure(ErrorCode code, std::string_view operation,
                                     std::string_view detail) const noexcept {
            return Status::failure({ErrorDomain::registry, code, {}, operation, detail});
        }

        [[nodiscard]] static bool same_key(const DynamicControl& left,
                                           const DynamicControl& right) noexcept {
            return left.component_id == right.component_id && left.id == right.id;
        }

        Registry* registry_{};
        FixedVector<DynamicControl, MaxDynamicControls> staged_{};
        bool committed_{};
    };

    [[nodiscard]] Status add(Component& component) noexcept {
        if (closed_) {
            return make_error(ErrorCode::invalid_state, {}, "registry.add", "closed");
        }
        const auto& descriptor = component.descriptor();
        for (const auto& entry : entries_) {
            if (entry.descriptor->id == descriptor.id) {
                return make_error(ErrorCode::duplicate_id, descriptor.id, "registry.add",
                                  descriptor.id);
            }
        }
        if (!entries_.push_back(Entry{&descriptor, &component, ComponentState::constructed})) {
            return make_error(ErrorCode::capacity_exceeded, descriptor.id, "registry.add",
                              descriptor.id);
        }
        return Status::success();
    }

    [[nodiscard]] Status validate() noexcept {
        if (closed_) {
            return validated_
                       ? Status::success()
                       : make_error(ErrorCode::invalid_state, {}, "registry.validate", "closed");
        }
        closed_ = true;

        for (const auto& entry : entries_) {
            if (!valid_descriptor(*entry.descriptor) ||
                !valid_wasm_descriptor(entry.descriptor->id, entry.descriptor->wasm) ||
                (entry.descriptor->wasm.functions.empty() != (entry.instance->wasm_provider() == nullptr))) {
                return make_error(ErrorCode::validation_failed, entry.descriptor->id,
                                  "registry.validate", "descriptor");
            }
        }

        for (std::size_t left = 0; left < entries_.size(); ++left) {
            for (const auto service : entries_[left].descriptor->provided_services) {
                for (std::size_t right = left + 1; right < entries_.size(); ++right) {
                    for (const auto other : entries_[right].descriptor->provided_services) {
                        if (service == other) {
                            return make_error(ErrorCode::duplicate_id,
                                              entries_[right].descriptor->id, "registry.validate",
                                              service);
                        }
                    }
                }
            }
        }

        const auto order_status = build_start_order();
        if (!order_status) {
            return order_status;
        }

        for (std::size_t order_index = 0; order_index < entries_.size(); ++order_index) {
            auto& entry = entries_[start_order_[order_index]];
            const auto status = entry.instance->validate(ValidationContext{});
            if (!status) {
                return status;
            }
            entry.state = ComponentState::validated;
        }
        validated_ = true;
        return Status::success();
    }

    [[nodiscard]] StartupReport<MaxComponents> start_all() noexcept {
        StartupReport<MaxComponents> report{};
        if (!validated_ || started_once_) {
            report.primary = {ErrorDomain::lifecycle,
                              ErrorCode::invalid_state,
                              {},
                              "registry.start_all",
                              started_once_ ? "already-started" : "not-validated"};
            return report;
        }
        started_once_ = true;

        std::size_t started = 0;
        for (; started < entries_.size(); ++started) {
            auto& entry = entries_[start_order_[started]];
            entry.state = ComponentState::starting;
            const auto status = entry.instance->start(StartContext{});
            if (!status) {
                entry.state = ComponentState::failed;
                report.primary = status.error();
                // A failed start can still own partial resources/callbacks.
                cleanup_started(started + 1, report.cleanup_errors);
                return report;
            }
            entry.state = ComponentState::running;
        }
        return report;
    }

    [[nodiscard]] Status suspend_all() noexcept {
        for (std::size_t reverse = entries_.size(); reverse > 0; --reverse) {
            auto& entry = entries_[start_order_[reverse - 1]];
            if (entry.state == ComponentState::suspended) {
                continue;
            }
            if (entry.state != ComponentState::running) {
                return make_error(ErrorCode::invalid_state, entry.descriptor->id,
                                  "registry.suspend_all", "not-running");
            }
            const auto status = entry.instance->suspend();
            if (!status) {
                return status;
            }
            entry.state = ComponentState::suspended;
        }
        return Status::success();
    }

    [[nodiscard]] Status resume_all() noexcept {
        for (std::size_t index = 0; index < entries_.size(); ++index) {
            auto& entry = entries_[start_order_[index]];
            if (entry.state == ComponentState::running) {
                continue;
            }
            if (entry.state != ComponentState::suspended || !entry.descriptor->supports_resume) {
                return make_error(ErrorCode::invalid_state, entry.descriptor->id,
                                  "registry.resume_all", "resume-unsupported");
            }
            const auto status = entry.instance->resume();
            if (!status) {
                return status;
            }
            entry.state = ComponentState::running;
        }
        return Status::success();
    }

    [[nodiscard]] Status stop_all() noexcept {
        Error first_error{};
        std::array<bool, MaxComponents> retained{};
        for (std::size_t reverse = entries_.size(); reverse > 0; --reverse) {
            const auto index = start_order_[reverse - 1];
            auto& entry = entries_[index];
            if (retained[index]) continue;
            if (entry.state == ComponentState::stopped ||
                entry.state == ComponentState::constructed ||
                entry.state == ComponentState::validated) {
                continue;
            }
            entry.state = ComponentState::stopping;
            const auto status = entry.instance->stop();
            if (!status && !first_error.valid()) {
                first_error = status.error();
            }
            const bool quiesced = entry.instance->callbacks_quiesced();
            if (!quiesced && !first_error.valid()) {
                first_error = {ErrorDomain::lifecycle, ErrorCode::stop_failed, entry.descriptor->id,
                               "registry.stop_all", "callbacks-active"};
            }
            if (!status || !quiesced) {
                retain_dependencies(index, retained);
                continue; // Keep stopping state so shutdown can be retried.
            }
            entry.state = ComponentState::stopped;
        }
        return first_error.valid() ? Status::failure(first_error) : Status::success();
    }

    [[nodiscard]] constexpr std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] std::size_t component_count() const noexcept override { return entries_.size(); }
    [[nodiscard]] const ComponentDescriptor&
    component_descriptor(std::size_t index) const noexcept override {
        return *entries_[index].descriptor;
    }
    [[nodiscard]] wasm::CapabilityProvider* wasm_provider(std::size_t index) const noexcept override {
        return entries_[index].instance->wasm_provider();
    }
    [[nodiscard]] constexpr bool closed() const noexcept { return closed_; }
    [[nodiscard]] std::size_t dynamic_control_count() const noexcept override {
        return dynamic_controls_.size();
    }
    [[nodiscard]] const DynamicControl& dynamic_control(std::size_t index) const noexcept override {
        return dynamic_controls_[index];
    }

    [[nodiscard]] const Entry* find(std::string_view id) const noexcept {
        for (const auto& entry : entries_) {
            if (entry.descriptor->id == id) {
                return &entry;
            }
        }
        return nullptr;
    }

    [[nodiscard]] const Entry& ordered_entry(std::size_t index) const noexcept {
        return entries_[start_order_[index]];
    }

    [[nodiscard]] ExtensionTransaction begin_extension() noexcept {
        return ExtensionTransaction{*this};
    }

  private:
    [[nodiscard]] static bool valid_component_id(std::string_view id) noexcept {
        if (id.size() < 3 || id.find('.') == std::string_view::npos || id.front() < 'a' ||
            id.front() > 'z') {
            return false;
        }
        for (const char character : id) {
            const bool valid = (character >= 'a' && character <= 'z') ||
                               (character >= '0' && character <= '9') || character == '_' ||
                               character == '-' || character == '.';
            if (!valid) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] static bool valid_public_id(std::string_view id) noexcept {
        if (id.empty() || id.front() < 'a' || id.front() > 'z') {
            return false;
        }
        for (const char character : id) {
            const bool valid = (character >= 'a' && character <= 'z') ||
                               (character >= '0' && character <= '9') || character == '_' ||
                               character == '-';
            if (!valid) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] static bool valid_legacy_id(std::string_view id) noexcept {
        if (id.empty() || !((id.front() >= 'a' && id.front() <= 'z') ||
                            (id.front() >= 'A' && id.front() <= 'Z'))) {
            return false;
        }
        for (const char character : id) {
            const bool valid =
                (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                (character >= '0' && character <= '9') || character == '_' || character == '-';
            if (!valid) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] static bool same_scalar(const ScalarValue& left,
                                          const ScalarValue& right) noexcept {
        if (left.type != right.type) {
            return false;
        }
        switch (left.type) {
        case ValueType::boolean:
            return left.boolean == right.boolean;
        case ValueType::integer:
            return left.integer == right.integer;
        case ValueType::number:
            return left.number == right.number;
        case ValueType::string:
            return left.string == right.string;
        }
        return false;
    }

    [[nodiscard]] static bool valid_fields(std::span<const FieldDescriptor> fields) noexcept {
        for (std::size_t index = 0; index < fields.size(); ++index) {
            if (!valid_public_id(fields[index].id)) {
                return false;
            }
            for (std::size_t prior = 0; prior < index; ++prior) {
                if (fields[prior].id == fields[index].id) {
                    return false;
                }
            }
        }
        return true;
    }

    [[nodiscard]] static bool valid_descriptor(const ComponentDescriptor& descriptor) noexcept {
        if (!valid_component_id(descriptor.id) || descriptor.schema_version == 0 ||
            descriptor.settings.schema_version == 0) {
            return false;
        }
        for (const auto service : descriptor.provided_services) {
            if (!valid_component_id(service)) {
                return false;
            }
        }
        for (const auto service : descriptor.required_services) {
            if (!valid_component_id(service)) {
                return false;
            }
        }
        for (const auto& metadata : descriptor.metadata) {
            if (!valid_public_id(metadata.key)) {
                return false;
            }
        }
        for (std::size_t index = 0; index < descriptor.parameters.size(); ++index) {
            const auto& parameter = descriptor.parameters[index];
            if (!valid_public_id(parameter.id) || parameter.type != parameter.default_value.type ||
                (parameter.bounds.present &&
                 ((parameter.type != ValueType::integer && parameter.type != ValueType::number) ||
                  parameter.bounds.minimum > parameter.bounds.maximum ||
                  parameter.bounds.step <= 0.0))) {
                return false;
            }
            for (std::size_t prior = 0; prior < index; ++prior) {
                if (descriptor.parameters[prior].id == parameter.id) {
                    return false;
                }
            }
        }
        for (std::size_t index = 0; index < descriptor.legacy_parameters.size(); ++index) {
            const auto& alias = descriptor.legacy_parameters[index];
            const auto parameter =
                std::find_if(descriptor.parameters.begin(), descriptor.parameters.end(),
                             [&alias](const ParameterDescriptor& candidate) {
                                 return candidate.id == alias.parameter_id;
                             });
            if (parameter == descriptor.parameters.end() || !valid_legacy_id(alias.id) ||
                alias.label.empty() ||
                (alias.enum_values.empty() && alias.type != parameter->type) ||
                (!alias.enum_values.empty() && alias.type != ValueType::string)) {
                return false;
            }
            for (std::size_t prior = 0; prior < index; ++prior) {
                if (descriptor.legacy_parameters[prior].parameter_id == alias.parameter_id ||
                    descriptor.legacy_parameters[prior].id == alias.id) {
                    return false;
                }
            }
            for (const auto& other : descriptor.parameters) {
                if (other.id != alias.parameter_id && other.id == alias.id) {
                    return false;
                }
            }
            for (const auto& action : descriptor.actions) {
                if (action.id == alias.id) {
                    return false;
                }
            }
            for (const auto& event : descriptor.events) {
                if (event.id == alias.id) {
                    return false;
                }
            }
            for (std::size_t value_index = 0; value_index < alias.enum_values.size();
                 ++value_index) {
                const auto& value = alias.enum_values[value_index];
                if (value.canonical_value.type != parameter->type || value.label.empty()) {
                    return false;
                }
                for (std::size_t prior = 0; prior < value_index; ++prior) {
                    if (alias.enum_values[prior].label == value.label ||
                        same_scalar(alias.enum_values[prior].canonical_value,
                                    value.canonical_value)) {
                        return false;
                    }
                }
            }
        }
        for (std::size_t index = 0; index < descriptor.actions.size(); ++index) {
            const auto& action = descriptor.actions[index];
            if (!valid_public_id(action.id) || !valid_fields(action.arguments)) {
                return false;
            }
            for (const auto& parameter : descriptor.parameters) {
                if (parameter.id == action.id) {
                    return false;
                }
            }
            for (std::size_t prior = 0; prior < index; ++prior) {
                if (descriptor.actions[prior].id == action.id) {
                    return false;
                }
            }
        }
        for (std::size_t index = 0; index < descriptor.events.size(); ++index) {
            const auto& event = descriptor.events[index];
            if (!valid_public_id(event.id) || !valid_fields(event.fields)) {
                return false;
            }
            for (const auto& parameter : descriptor.parameters) {
                if (parameter.id == event.id) {
                    return false;
                }
            }
            for (const auto& action : descriptor.actions) {
                if (action.id == event.id) {
                    return false;
                }
            }
            for (std::size_t prior = 0; prior < index; ++prior) {
                if (descriptor.events[prior].id == event.id) {
                    return false;
                }
            }
        }
        for (const auto& diagnostic : descriptor.diagnostics) {
            if (!valid_public_id(diagnostic.id)) {
                return false;
            }
        }
        for (const auto& resource : descriptor.resources) {
            if (!valid_public_id(resource.logical_name) || resource.amount == 0 ||
                resource.alternatives.empty()) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool has_component(std::string_view id) const noexcept {
        return find(id) != nullptr;
    }

    [[nodiscard]] bool control_exists(const DynamicControl& control) const noexcept {
        for (const auto& existing : dynamic_controls_) {
            if (existing.component_id == control.component_id && existing.id == control.id) {
                return true;
            }
        }
        const auto* entry = find(control.component_id);
        if (entry == nullptr) {
            return false;
        }
        for (const auto& parameter : entry->descriptor->parameters) {
            if (parameter.id == control.id) {
                return true;
            }
        }
        for (const auto& alias : entry->descriptor->legacy_parameters) {
            if (alias.id == control.id) {
                return true;
            }
        }
        for (const auto& action : entry->descriptor->actions) {
            if (action.id == control.id) {
                return true;
            }
        }
        for (const auto& event : entry->descriptor->events) {
            if (event.id == control.id) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] int provider_index(std::string_view service) const noexcept {
        int result = -1;
        for (std::size_t index = 0; index < entries_.size(); ++index) {
            for (const auto provided : entries_[index].descriptor->provided_services) {
                if (provided == service) {
                    if (result >= 0) {
                        return -2;
                    }
                    result = static_cast<int>(index);
                }
            }
        }
        return result;
    }

    [[nodiscard]] Status build_start_order() noexcept {
        std::array<std::size_t, MaxComponents> indegree{};
        std::array<bool, MaxComponents> emitted{};

        for (std::size_t consumer = 0; consumer < entries_.size(); ++consumer) {
            for (const auto dependency : entries_[consumer].descriptor->required_services) {
                const int provider = provider_index(dependency);
                if (provider == -2) {
                    return make_error(ErrorCode::duplicate_id, entries_[consumer].descriptor->id,
                                      "registry.validate", dependency);
                }
                if (provider < 0) {
                    return make_error(ErrorCode::missing_dependency,
                                      entries_[consumer].descriptor->id, "registry.validate",
                                      dependency);
                }
                ++indegree[consumer];
            }
        }

        for (std::size_t order = 0; order < entries_.size(); ++order) {
            std::size_t selected = MaxComponents;
            for (std::size_t candidate = 0; candidate < entries_.size(); ++candidate) {
                if (!emitted[candidate] && indegree[candidate] == 0 &&
                    (selected == MaxComponents ||
                     entries_[candidate].descriptor->id < entries_[selected].descriptor->id)) {
                    selected = candidate;
                }
            }
            if (selected == MaxComponents) {
                return make_error(ErrorCode::dependency_cycle, {}, "registry.validate",
                                  "dependency-cycle");
            }
            start_order_[order] = selected;
            emitted[selected] = true;

            for (std::size_t consumer = 0; consumer < entries_.size(); ++consumer) {
                if (emitted[consumer]) {
                    continue;
                }
                for (const auto dependency : entries_[consumer].descriptor->required_services) {
                    if (provider_index(dependency) == static_cast<int>(selected)) {
                        --indegree[consumer];
                    }
                }
            }
        }
        return Status::success();
    }

    void retain_dependencies(std::size_t consumer, std::array<bool, MaxComponents>& retained) const noexcept {
        retained[consumer] = true;
        // Providers precede consumers. One reverse pass also marks their providers.
        for (std::size_t reverse = entries_.size(); reverse > 0; --reverse) {
            const auto index = start_order_[reverse - 1];
            if (!retained[index]) continue;
            for (const auto service : entries_[index].descriptor->required_services) {
                const auto provider = provider_index(service);
                if (provider >= 0) retained[static_cast<std::size_t>(provider)] = true;
            }
        }
    }

    void cleanup_started(std::size_t started,
                         FixedVector<Error, MaxComponents>& cleanup_errors) noexcept {
        std::array<bool, MaxComponents> retained{};
        while (started > 0) {
            --started;
            const auto index = start_order_[started];
            auto& entry = entries_[index];
            if (retained[index]) continue;
            entry.state = ComponentState::stopping;
            const auto status = entry.instance->stop();
            if (!status) {
                static_cast<void>(cleanup_errors.push_back(status.error()));
            }
            const bool quiesced = entry.instance->callbacks_quiesced();
            if (!quiesced) {
                static_cast<void>(cleanup_errors.push_back(
                    {ErrorDomain::lifecycle, ErrorCode::stop_failed, entry.descriptor->id,
                     "registry.cleanup", "callbacks-active"}));
            }
            if (!status || !quiesced) {
                retain_dependencies(index, retained);
                continue;
            }
            entry.state = ComponentState::stopped;
        }
    }

    [[nodiscard]] static Status make_error(ErrorCode code, std::string_view component,
                                           std::string_view operation,
                                           std::string_view detail) noexcept {
        return Status::failure({ErrorDomain::registry, code, component, operation, detail});
    }

    FixedVector<Entry, MaxComponents> entries_{};
    FixedVector<DynamicControl, MaxDynamicControls> dynamic_controls_{};
    std::array<std::size_t, MaxComponents> start_order_{};
    bool closed_{};
    bool validated_{};
    bool started_once_{};
};

} // namespace blip::core
