#include "blip/core/descriptor_json.hpp"
#include "blip/core/fixed_vector.hpp"
#include "blip/core/registry.hpp"
#include "test_harness.hpp"

#include <array>
#include <string_view>

namespace {

using blip::core::ActionDescriptor;
using blip::core::Component;
using blip::core::ComponentDescriptor;
using blip::core::DisablePolicy;
using blip::core::DynamicControl;
using blip::core::DynamicControlKind;
using blip::core::Error;
using blip::core::ErrorCode;
using blip::core::ErrorDomain;
using blip::core::EventDescriptor;
using blip::core::FieldDescriptor;
using blip::core::FixedVector;
using blip::core::ParameterDescriptor;
using blip::core::Registry;
using blip::core::ScalarValue;
using blip::core::StartContext;
using blip::core::Status;
using blip::core::ValidationContext;
using blip::core::ValueType;

struct LogEntry {
    std::string_view component;
    std::string_view operation;
};

using CallLog = FixedVector<LogEntry, 64>;

class TestComponent final : public Component {
  public:
    TestComponent(ComponentDescriptor descriptor, CallLog& log) noexcept
        : descriptor_(descriptor), log_(&log) {}

    [[nodiscard]] const ComponentDescriptor& descriptor() const noexcept override {
        return descriptor_;
    }

    [[nodiscard]] Status validate(const ValidationContext&) noexcept override {
        ++validate_calls;
        static_cast<void>(log_->push_back({descriptor_.id, "validate"}));
        return validate_status;
    }

    [[nodiscard]] Status start(const StartContext&) noexcept override {
        ++start_calls;
        static_cast<void>(log_->push_back({descriptor_.id, "start"}));
        return start_status;
    }

    [[nodiscard]] Status suspend() noexcept override {
        ++suspend_calls;
        static_cast<void>(log_->push_back({descriptor_.id, "suspend"}));
        return suspend_status;
    }

    [[nodiscard]] Status resume() noexcept override {
        ++resume_calls;
        static_cast<void>(log_->push_back({descriptor_.id, "resume"}));
        return resume_status;
    }

    [[nodiscard]] Status stop() noexcept override {
        ++stop_calls;
        static_cast<void>(log_->push_back({descriptor_.id, "stop"}));
        return stop_status;
    }

    [[nodiscard]] bool callbacks_quiesced() const noexcept override { return quiesced; }

    Status validate_status{Status::success()};
    Status start_status{Status::success()};
    Status suspend_status{Status::success()};
    Status resume_status{Status::success()};
    Status stop_status{Status::success()};
    bool quiesced{true};
    int validate_calls{};
    int start_calls{};
    int suspend_calls{};
    int resume_calls{};
    int stop_calls{};

  private:
    ComponentDescriptor descriptor_{};
    CallLog* log_{};
};

[[nodiscard]] constexpr ComponentDescriptor
descriptor(std::string_view id, std::span<const std::string_view> provided = {},
           std::span<const std::string_view> required = {}) noexcept {
    ComponentDescriptor value{};
    value.schema_version = 1;
    value.id = id;
    value.display_name = id;
    value.description = "test component";
    value.provided_services = provided;
    value.required_services = required;
    value.settings = {1, 1};
    value.disable_policy = DisablePolicy::live;
    value.supports_resume = true;
    return value;
}

[[nodiscard]] constexpr Status component_error(std::string_view component, ErrorCode code,
                                               std::string_view operation) noexcept {
    return Status::failure({ErrorDomain::lifecycle, code, component, operation, "synthetic"});
}

bool ordering_and_stable_ties() {
    constexpr std::array<std::string_view, 1> z_services{"service.z"};
    constexpr std::array<std::string_view, 1> b_dependencies{"service.z"};
    CallLog log{};
    TestComponent b{descriptor("test.b", {}, b_dependencies), log};
    TestComponent z{descriptor("test.z", z_services), log};
    TestComponent a{descriptor("test.a"), log};
    Registry<3> registry{};

    BLIP_CHECK(registry.add(b));
    BLIP_CHECK(registry.add(z));
    BLIP_CHECK(registry.add(a));
    BLIP_CHECK(registry.validate());
    BLIP_CHECK(registry.ordered_entry(0).descriptor->id == "test.a");
    BLIP_CHECK(registry.ordered_entry(1).descriptor->id == "test.z");
    BLIP_CHECK(registry.ordered_entry(2).descriptor->id == "test.b");
    const auto report = registry.start_all();
    BLIP_CHECK(report.ok());
    BLIP_CHECK(log[3].component == "test.a" && log[3].operation == "start");
    BLIP_CHECK(log[4].component == "test.z" && log[4].operation == "start");
    BLIP_CHECK(log[5].component == "test.b" && log[5].operation == "start");
    return true;
}

bool duplicate_and_capacity() {
    CallLog log{};
    TestComponent first{descriptor("test.same"), log};
    TestComponent duplicate{descriptor("test.same"), log};
    TestComponent overflow{descriptor("test.overflow"), log};
    Registry<1> registry{};

    BLIP_CHECK(registry.add(first));
    const auto duplicate_status = registry.add(duplicate);
    BLIP_CHECK(!duplicate_status);
    BLIP_CHECK(duplicate_status.error().code == ErrorCode::duplicate_id);
    const auto capacity_status = registry.add(overflow);
    BLIP_CHECK(!capacity_status);
    BLIP_CHECK(capacity_status.error().code == ErrorCode::capacity_exceeded);
    return true;
}

bool missing_dependency_has_no_callbacks() {
    constexpr std::array<std::string_view, 1> dependencies{"service.missing"};
    CallLog log{};
    TestComponent component{descriptor("test.consumer", {}, dependencies), log};
    Registry<1> registry{};
    BLIP_CHECK(registry.add(component));
    const auto status = registry.validate();
    BLIP_CHECK(!status);
    BLIP_CHECK(status.error().code == ErrorCode::missing_dependency);
    BLIP_CHECK(component.validate_calls == 0);
    BLIP_CHECK(component.start_calls == 0);
    return true;
}

bool dependency_cycle() {
    constexpr std::array<std::string_view, 1> service_a{"service.a"};
    constexpr std::array<std::string_view, 1> service_b{"service.b"};
    CallLog log{};
    TestComponent a{descriptor("test.a", service_a, service_b), log};
    TestComponent b{descriptor("test.b", service_b, service_a), log};
    Registry<2> registry{};
    BLIP_CHECK(registry.add(a));
    BLIP_CHECK(registry.add(b));
    const auto status = registry.validate();
    BLIP_CHECK(!status);
    BLIP_CHECK(status.error().code == ErrorCode::dependency_cycle);
    BLIP_CHECK(a.validate_calls == 0 && b.validate_calls == 0);
    return true;
}

bool partial_start_cleanup_and_causes() {
    CallLog log{};
    TestComponent a{descriptor("test.a"), log};
    TestComponent b{descriptor("test.b"), log};
    TestComponent c{descriptor("test.c"), log};
    b.stop_status = component_error("test.b", ErrorCode::stop_failed, "stop");
    c.start_status = component_error("test.c", ErrorCode::start_failed, "start");
    Registry<3> registry{};
    BLIP_CHECK(registry.add(c));
    BLIP_CHECK(registry.add(a));
    BLIP_CHECK(registry.add(b));
    BLIP_CHECK(registry.validate());

    const auto report = registry.start_all();
    BLIP_CHECK(!report.ok());
    BLIP_CHECK(report.primary.component == "test.c");
    BLIP_CHECK(report.cleanup_errors.size() == 1);
    BLIP_CHECK(report.cleanup_errors[0].component == "test.b");
    BLIP_CHECK(log[3].component == "test.a" && log[3].operation == "start");
    BLIP_CHECK(log[4].component == "test.b" && log[4].operation == "start");
    BLIP_CHECK(log[5].component == "test.c" && log[5].operation == "start");
    BLIP_CHECK(log[6].component == "test.b" && log[6].operation == "stop");
    BLIP_CHECK(log[7].component == "test.a" && log[7].operation == "stop");
    return true;
}

bool lifecycle_idempotence_and_quiescence() {
    CallLog log{};
    TestComponent component{descriptor("test.lifecycle"), log};
    Registry<1> registry{};
    BLIP_CHECK(registry.add(component));
    BLIP_CHECK(registry.validate());
    BLIP_CHECK(registry.start_all().ok());
    const auto repeated_start = registry.start_all();
    BLIP_CHECK(!repeated_start.ok());
    BLIP_CHECK(repeated_start.primary.detail == "already-started");
    BLIP_CHECK(registry.suspend_all());
    BLIP_CHECK(registry.suspend_all());
    BLIP_CHECK(registry.resume_all());
    BLIP_CHECK(registry.resume_all());
    BLIP_CHECK(registry.stop_all());
    BLIP_CHECK(registry.stop_all());
    BLIP_CHECK(component.suspend_calls == 1);
    BLIP_CHECK(component.resume_calls == 1);
    BLIP_CHECK(component.stop_calls == 1);

    CallLog active_log{};
    TestComponent active{descriptor("test.active"), active_log};
    active.quiesced = false;
    Registry<1> active_registry{};
    BLIP_CHECK(active_registry.add(active));
    BLIP_CHECK(active_registry.validate());
    BLIP_CHECK(active_registry.start_all().ok());
    const auto stop_status = active_registry.stop_all();
    BLIP_CHECK(!stop_status);
    BLIP_CHECK(stop_status.error().detail == "callbacks-active");
    return true;
}

bool bounded_extension_transaction() {
    constexpr ParameterDescriptor existing{"gain",
                                           "Gain",
                                           ValueType::number,
                                           blip::core::Access::read_write,
                                           true,
                                           ScalarValue::from_number(1.0),
                                           {},
                                           {}};
    constexpr std::array<ParameterDescriptor, 1> parameters{existing};
    CallLog log{};
    auto component_descriptor = descriptor("test.script");
    component_descriptor.parameters = parameters;
    TestComponent component{component_descriptor, log};
    Registry<1, 2> registry{};
    BLIP_CHECK(registry.add(component));
    BLIP_CHECK(registry.validate());

    auto duplicate = registry.begin_extension();
    BLIP_CHECK(duplicate.add(
        DynamicControl{"test.script", DynamicControlKind::parameter, "gain", ValueType::number}));
    BLIP_CHECK(!duplicate.commit());
    BLIP_CHECK(registry.dynamic_control_count() == 0);

    auto accepted = registry.begin_extension();
    BLIP_CHECK(accepted.add(
        DynamicControl{"test.script", DynamicControlKind::parameter, "speed", ValueType::number}));
    BLIP_CHECK(accepted.add(
        DynamicControl{"test.script", DynamicControlKind::event, "done", ValueType::boolean}));
    BLIP_CHECK(accepted.commit());
    BLIP_CHECK(registry.dynamic_control_count() == 2);

    auto overflow = registry.begin_extension();
    BLIP_CHECK(overflow.add(
        DynamicControl{"test.script", DynamicControlKind::action, "reset", ValueType::boolean}));
    const auto overflow_status = overflow.commit();
    BLIP_CHECK(!overflow_status);
    BLIP_CHECK(overflow_status.error().code == ErrorCode::capacity_exceeded);
    BLIP_CHECK(registry.dynamic_control_count() == 2);
    return true;
}

bool complete_descriptor_json() {
    constexpr std::array<std::string_view, 1> provided{"service.synthetic"};
    constexpr std::array<FieldDescriptor, 1> action_fields{
        FieldDescriptor{"value", ValueType::integer, true}};
    constexpr std::array<FieldDescriptor, 1> event_fields{
        FieldDescriptor{"ok", ValueType::boolean, true}};
    constexpr std::array<ParameterDescriptor, 1> parameters{
        ParameterDescriptor{"level",
                            "Level",
                            ValueType::integer,
                            blip::core::Access::read_write,
                            true,
                            ScalarValue::from_integer(1),
                            {true, 0.0, 10.0, 1.0},
                            "units"}};
    const std::array<ActionDescriptor, 1> actions{
        ActionDescriptor{"apply", "Apply", action_fields}};
    const std::array<EventDescriptor, 1> events{EventDescriptor{"changed", event_fields}};
    auto value = descriptor("test.synthetic", provided);
    value.display_name = "Synthetic";
    value.parameters = parameters;
    value.actions = actions;
    value.events = events;
    std::array<char, 1024> output{};
    const auto result = blip::core::write_descriptor_json(value, output);
    BLIP_CHECK(result);
    const std::string_view json{output.data(), result.value()};
    BLIP_CHECK(json.starts_with("{\"schema_version\":1"));
    BLIP_CHECK(json.find("\"id\":\"test.synthetic\"") != std::string_view::npos);
    BLIP_CHECK(json.find("\"parameters\":[{\"id\":\"level\"") != std::string_view::npos);
    BLIP_CHECK(json.find("\"actions\":[{\"id\":\"apply\"") != std::string_view::npos);
    BLIP_CHECK(json.find("\"events\":[{\"id\":\"changed\"") != std::string_view::npos);

    std::array<char, 16> too_small{};
    const auto overflow = blip::core::write_descriptor_json(value, too_small);
    BLIP_CHECK(!overflow);
    BLIP_CHECK(overflow.error().code == ErrorCode::serialization_overflow);
    return true;
}

} // namespace

int main() {
    const TestCase tests[]{
        {"registry ordering and stable ties", ordering_and_stable_ties},
        {"registry duplicate and capacity", duplicate_and_capacity},
        {"missing dependency without callbacks", missing_dependency_has_no_callbacks},
        {"dependency cycle", dependency_cycle},
        {"partial start cleanup and causes", partial_start_cleanup_and_causes},
        {"lifecycle idempotence and quiescence", lifecycle_idempotence_and_quiescence},
        {"bounded extension transaction", bounded_extension_transaction},
        {"complete descriptor JSON", complete_descriptor_json},
    };
    return run_tests(tests);
}
