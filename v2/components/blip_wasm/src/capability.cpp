#include "blip/wasm/capability.hpp"
#include <algorithm>

namespace blip::wasm {
namespace {
core::Status failure(core::ErrorCode code, std::string_view detail,
                     std::string_view component = "blip.wasm") noexcept {
    return core::Status::failure({core::ErrorDomain::control, code, component, "capability", detail});
}
bool valid_value(const Value& value, ValueType expected) noexcept {
    return value.type == expected &&
        ((expected != ValueType::i32 && expected != ValueType::f32) || value.bits <= 0xffffffffU);
}
}
bool CallContext::cancelled() const noexcept { return cancellation_ && cancellation_->load(); }
core::Status CallContext::read_utf8(StringRef ref, std::span<char> output, std::size_t& count) noexcept {
    count = 0;
    if (cancelled()) return failure(core::ErrorCode::cancelled, "cancelled-copy");
    return blip::wasm::read_utf8(*memory_, ref, output, count);
}
core::Status CallContext::write_utf8(std::uint32_t offset, std::string_view input) noexcept {
    if (cancelled()) return failure(core::ErrorCode::cancelled, "cancelled-copy");
    return blip::wasm::write_utf8(*memory_, offset, input);
}
core::Status CapabilityRegistry::bind(const core::RegistryView& registry) noexcept {
    if (bound_) return failure(core::ErrorCode::invalid_state, "already-bound");
    if (registry.component_count() > kMaximumCapabilityComponents)
        return failure(core::ErrorCode::capacity_exceeded, "component-limit");
    std::size_t providers{}, functions{};
    // Preflight completely before publishing any borrowed binding. A failed
    // bind leaves an empty catalog and permits retry with a corrected registry.
    for (std::size_t i = 0; i < registry.component_count(); ++i) {
        const auto& component = registry.component_descriptor(i);
        const auto* provider = registry.wasm_provider(i);
        if (!core::valid_wasm_descriptor(component.id, component.wasm) ||
            (component.wasm.functions.empty() != (provider == nullptr)))
            return failure(core::ErrorCode::validation_failed, "provider-descriptor", component.id);
        if (!provider) continue;
        if (++providers > kMaximumCapabilityProviders ||
            component.wasm.functions.size() > kMaximumCapabilityFunctions - functions)
            return failure(core::ErrorCode::capacity_exceeded, "provider-or-function-limit", component.id);
        functions += component.wasm.functions.size();
        for (std::size_t prior = 0; prior < i; ++prior) {
            if (provider == registry.wasm_provider(prior))
                return failure(core::ErrorCode::resource_conflict, "provider-shared-by-components", component.id);
            if (component.wasm.import_module == registry.component_descriptor(prior).wasm.import_module)
                return failure(core::ErrorCode::duplicate_id, "import-module", component.id);
        }
    }
    for (std::size_t i = 0; i < registry.component_count(); ++i) {
        const auto& component = registry.component_descriptor(i);
        auto* provider = registry.wasm_provider(i);
        for (const auto& function : component.wasm.functions)
            static_cast<void>(bindings_.push_back({&component, &function, provider}));
    }
    std::sort(bindings_.begin(), bindings_.end(), [](const Binding& a, const Binding& b) noexcept {
        return a.component->wasm.import_module == b.component->wasm.import_module
            ? a.function->id < b.function->id : a.component->wasm.import_module < b.component->wasm.import_module;
    });
    bound_ = true;
    return core::Status::success();
}
core::Result<std::size_t> CapabilityRegistry::resolve(std::string_view module, std::string_view function) const noexcept {
    using Result = core::Result<std::size_t>;
    if (!bound_) return Result::failure(failure(core::ErrorCode::invalid_state, "catalog-not-bound").error());
    for (std::size_t i = 0; i < bindings_.size(); ++i)
        if (bindings_[i].component->wasm.import_module == module && bindings_[i].function->id == function)
            return Result::success(i);
    return Result::failure(failure(core::ErrorCode::not_found, "import-unavailable").error());
}
core::Status CapabilityRegistry::invoke(std::size_t index, GuestMemory& memory, std::span<const Value> arguments,
    std::span<Value> results, std::size_t& count, const std::atomic<bool>* cancellation) noexcept {
    count = 0;
    if (!bound_ || index >= bindings_.size()) return failure(core::ErrorCode::invalid_state, "binding-unavailable");
    if (invoking_) return failure(core::ErrorCode::recursive_dispatch, "provider-reentry");
    const auto& binding = bindings_[index];
    const auto& function = *binding.function;
    const auto component = binding.component->id;
    if (arguments.size() != function.arguments.size() || results.size() < function.results.size())
        return failure(core::ErrorCode::invalid_argument, "function-arity", component);
    for (std::size_t i = 0; i < arguments.size(); ++i)
        if (!valid_value(arguments[i], function.arguments[i].type))
            return failure(core::ErrorCode::invalid_argument, "argument-type-or-bits", component);
    CallContext context{memory, cancellation};
    if (context.cancelled()) return failure(core::ErrorCode::cancelled, "cancelled-admission", component);
    if (!binding.provider->available()) return failure(core::ErrorCode::resource_unavailable, "provider-unavailable", component);
    // Trusted providers write into bounded scratch. Failed/malformed callbacks
    // cannot expose partial result values to a guest or another runtime client.
    std::array<Value, 1> output{};
    std::size_t produced{};
    invoking_ = true;
    const auto status = binding.provider->invoke(function.id, context, arguments,
        std::span(output).first(function.results.size()), produced);
    invoking_ = false;
    if (context.cancelled()) return failure(core::ErrorCode::cancelled, "cancelled-return", component);
    if (!status) return status;
    if (produced != function.results.size()) return failure(core::ErrorCode::verification_failed, "provider-result-count", component);
    for (std::size_t i = 0; i < produced; ++i) {
        if (!valid_value(output[i], function.results[i]))
            return failure(core::ErrorCode::verification_failed, "provider-result-type-or-bits", component);
    }
    std::copy_n(output.begin(), produced, results.begin());
    count = produced;
    return core::Status::success();
}
} // namespace blip::wasm
