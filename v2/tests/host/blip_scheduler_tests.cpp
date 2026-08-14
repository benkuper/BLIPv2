#include "blip/core/event_bus.hpp"
#include "blip/core/fixed_vector.hpp"
#include "blip/core/scheduler.hpp"
#include "test_harness.hpp"

#include <cstdint>
#include <string_view>

namespace {

using blip::core::CancellationToken;
using blip::core::Clock;
using blip::core::ErrorCode;
using blip::core::EventBus;
using blip::core::FixedVector;
using blip::core::Job;
using blip::core::OverflowPolicy;
using blip::core::Scheduler;
using blip::core::Status;
using blip::core::TaskClass;
using blip::core::Tick32Extender;

class FakeClock final : public Clock {
  public:
    [[nodiscard]] std::uint64_t now_us() noexcept override { return now; }
    std::uint64_t now{};
};

struct JobContext {
    FakeClock* clock{};
    Scheduler<2>* scheduler{};
    TaskClass expected{TaskClass::control};
    std::uint32_t advance_us{};
    int marker{};
    FixedVector<int, 16>* calls{};
    Status context_status{Status::success()};
};

[[nodiscard]] Status run_job(void* opaque) noexcept {
    auto& context = *static_cast<JobContext*>(opaque);
    context.context_status = context.scheduler->require_context(context.expected, "test.job");
    context.clock->now += context.advance_us;
    static_cast<void>(context.calls->push_back(context.marker));
    return Status::success();
}

[[nodiscard]] Job job(std::string_view name, std::string_view owner, JobContext& context,
                      OverflowPolicy policy = OverflowPolicy::reject_new,
                      CancellationToken* cancellation = nullptr,
                      std::uint32_t budget_us = 10) noexcept {
    return {name, owner, context.expected, policy, run_job, &context, cancellation, budget_us};
}

bool scheduler_overflow_policies() {
    FakeClock clock{};
    FixedVector<int, 16> calls{};
    Scheduler<2> reject_scheduler{clock};
    JobContext a{&clock, &reject_scheduler, TaskClass::control, 1, 1, &calls};
    JobContext b{&clock, &reject_scheduler, TaskClass::control, 1, 2, &calls};
    JobContext c{&clock, &reject_scheduler, TaskClass::control, 1, 3, &calls};
    BLIP_CHECK(reject_scheduler.schedule(job("a", "owner", a)));
    BLIP_CHECK(reject_scheduler.schedule(job("b", "owner", b)));
    const auto rejected = reject_scheduler.schedule(job("c", "owner", c));
    BLIP_CHECK(!rejected && rejected.error().code == ErrorCode::queue_full);
    BLIP_CHECK(reject_scheduler.metrics(TaskClass::control).rejected == 1);

    Scheduler<2> drop_scheduler{clock};
    a.scheduler = &drop_scheduler;
    b.scheduler = &drop_scheduler;
    c.scheduler = &drop_scheduler;
    BLIP_CHECK(drop_scheduler.schedule(job("a", "owner", a)));
    BLIP_CHECK(drop_scheduler.schedule(job("b", "owner", b)));
    BLIP_CHECK(drop_scheduler.schedule(job("c", "owner", c, OverflowPolicy::drop_oldest)));
    calls.clear();
    BLIP_CHECK(drop_scheduler.run_next(TaskClass::control));
    BLIP_CHECK(drop_scheduler.run_next(TaskClass::control));
    BLIP_CHECK(calls.size() == 2 && calls[0] == 2 && calls[1] == 3);
    BLIP_CHECK(drop_scheduler.metrics(TaskClass::control).dropped == 1);

    Scheduler<2> coalesce_scheduler{clock};
    a.scheduler = &coalesce_scheduler;
    b.scheduler = &coalesce_scheduler;
    c.scheduler = &coalesce_scheduler;
    BLIP_CHECK(coalesce_scheduler.schedule(job("same", "owner", a)));
    BLIP_CHECK(coalesce_scheduler.schedule(job("other", "owner", b)));
    BLIP_CHECK(
        coalesce_scheduler.schedule(job("same", "owner", c, OverflowPolicy::coalesce_by_key)));
    calls.clear();
    BLIP_CHECK(coalesce_scheduler.run_next(TaskClass::control));
    BLIP_CHECK(coalesce_scheduler.run_next(TaskClass::control));
    BLIP_CHECK(calls.size() == 2 && calls[0] == 3 && calls[1] == 2);
    BLIP_CHECK(coalesce_scheduler.metrics(TaskClass::control).coalesced == 1);

    Scheduler<2> fault_scheduler{clock};
    a.scheduler = &fault_scheduler;
    b.scheduler = &fault_scheduler;
    c.scheduler = &fault_scheduler;
    BLIP_CHECK(fault_scheduler.schedule(job("a", "owner", a)));
    BLIP_CHECK(fault_scheduler.schedule(job("b", "owner", b)));
    const auto faulted = fault_scheduler.schedule(job("c", "owner", c, OverflowPolicy::fault));
    BLIP_CHECK(!faulted && faulted.error().code == ErrorCode::queue_faulted);
    BLIP_CHECK(fault_scheduler.faulted(TaskClass::control));
    BLIP_CHECK(!fault_scheduler.schedule(job("later", "owner", c)));
    return true;
}

bool budget_context_cancellation_and_stop() {
    FakeClock clock{};
    FixedVector<int, 16> calls{};
    Scheduler<2> scheduler{clock};
    JobContext slow{&clock, &scheduler, TaskClass::render, 11, 1, &calls};
    BLIP_CHECK(scheduler.schedule(
        job("slow", "render.owner", slow, OverflowPolicy::reject_new, nullptr, 10)));
    const auto overrun = scheduler.run_next(TaskClass::render);
    BLIP_CHECK(!overrun && overrun.error().code == ErrorCode::budget_exceeded);
    BLIP_CHECK(slow.context_status);
    BLIP_CHECK(scheduler.metrics(TaskClass::render).budget_overruns == 1);
    BLIP_CHECK(!scheduler.require_context(TaskClass::render, "outside"));

    CancellationToken cancellation{true};
    JobContext cancelled{&clock, &scheduler, TaskClass::control, 1, 2, &calls};
    BLIP_CHECK(scheduler.schedule(
        job("cancelled", "owner.cancel", cancelled, OverflowPolicy::reject_new, &cancellation)));
    const auto cancelled_status = scheduler.run_next(TaskClass::control);
    BLIP_CHECK(!cancelled_status && cancelled_status.error().code == ErrorCode::cancelled);

    JobContext keep{&clock, &scheduler, TaskClass::transport, 1, 3, &calls};
    JobContext remove{&clock, &scheduler, TaskClass::transport, 1, 4, &calls};
    BLIP_CHECK(scheduler.schedule(job("keep", "owner.keep", keep)));
    BLIP_CHECK(scheduler.schedule(job("remove", "owner.remove", remove)));
    scheduler.cancel_owner("owner.remove");
    calls.clear();
    BLIP_CHECK(scheduler.run_next(TaskClass::transport));
    BLIP_CHECK(calls.size() == 1 && calls[0] == 3);
    BLIP_CHECK(scheduler.metrics(TaskClass::transport).cancelled == 1);
    return true;
}

bool monotonic_wrap_extension() {
    Tick32Extender extender{};
    const auto before = extender.update(0xfffffff0U);
    const auto after = extender.update(0x00000010U);
    BLIP_CHECK(after > before);
    BLIP_CHECK(after - before == 0x20U);
    BLIP_CHECK(extender.update(0x00000020U) == after + 0x10U);
    return true;
}

struct TestEvent {
    int value{};
};

struct EventContext {
    FixedVector<int, 16>* values{};
    EventBus<TestEvent, 2, 2>* bus{};
    EventBus<TestEvent, 2, 2>::ProducerToken producer{};
    Status recursive_status{Status::success()};
    bool publish_follow_up{};
};

[[nodiscard]] Status on_event(const TestEvent& event, void* opaque) noexcept {
    auto& context = *static_cast<EventContext*>(opaque);
    static_cast<void>(context.values->push_back(event.value));
    if (event.value == 1) {
        context.recursive_status = context.bus->dispatch_one(TaskClass::control);
        if (context.publish_follow_up) {
            return context.bus->publish(context.producer, TestEvent{2}, 2,
                                        OverflowPolicy::reject_new);
        }
    }
    return Status::success();
}

bool event_delivery_recursion_and_stop() {
    EventBus<TestEvent, 2, 2> bus{TaskClass::control};
    FixedVector<int, 16> values{};
    EventContext context{&values, &bus, {}, Status::success(), true};
    BLIP_CHECK(bus.subscribe("subscriber", on_event, &context));
    context.producer = bus.producer();
    BLIP_CHECK(bus.publish(context.producer, TestEvent{1}, 1, OverflowPolicy::reject_new));
    BLIP_CHECK(!bus.dispatch_one(TaskClass::transport));
    BLIP_CHECK(bus.dispatch_one(TaskClass::control));
    BLIP_CHECK(!context.recursive_status);
    BLIP_CHECK(context.recursive_status.error().code == ErrorCode::recursive_dispatch);
    BLIP_CHECK(values.size() == 1 && values[0] == 1);
    BLIP_CHECK(bus.queued() == 1);
    BLIP_CHECK(bus.dispatch_one(TaskClass::control));
    BLIP_CHECK(values.size() == 2 && values[1] == 2);

    BLIP_CHECK(bus.publish(context.producer, TestEvent{3}, 3, OverflowPolicy::reject_new));
    bus.stop();
    BLIP_CHECK(bus.queued() == 0);
    const auto stale = bus.publish(context.producer, TestEvent{4}, 4, OverflowPolicy::reject_new);
    BLIP_CHECK(!stale && stale.error().code == ErrorCode::cancelled);
    BLIP_CHECK(bus.metrics().stale_producers == 1);
    return true;
}

bool event_overflow_policies() {
    EventBus<TestEvent, 2, 2> reject_bus{TaskClass::control};
    const auto reject_producer = reject_bus.producer();
    BLIP_CHECK(reject_bus.publish(reject_producer, TestEvent{1}, 1, OverflowPolicy::reject_new));
    BLIP_CHECK(reject_bus.publish(reject_producer, TestEvent{2}, 2, OverflowPolicy::reject_new));
    BLIP_CHECK(!reject_bus.publish(reject_producer, TestEvent{3}, 3, OverflowPolicy::reject_new));
    BLIP_CHECK(reject_bus.metrics().rejected == 1);

    EventBus<TestEvent, 2, 2> drop_bus{TaskClass::control};
    FixedVector<int, 16> drop_values{};
    EventContext drop_context{&drop_values, nullptr, {}, Status::success(), false};
    BLIP_CHECK(drop_bus.subscribe("subscriber", on_event, &drop_context));
    drop_context.bus = &drop_bus;
    drop_context.producer = drop_bus.producer();
    BLIP_CHECK(
        drop_bus.publish(drop_context.producer, TestEvent{1}, 1, OverflowPolicy::reject_new));
    BLIP_CHECK(
        drop_bus.publish(drop_context.producer, TestEvent{2}, 2, OverflowPolicy::reject_new));
    BLIP_CHECK(
        drop_bus.publish(drop_context.producer, TestEvent{3}, 3, OverflowPolicy::drop_oldest));
    BLIP_CHECK(drop_bus.dispatch_one(TaskClass::control));
    BLIP_CHECK(drop_bus.dispatch_one(TaskClass::control));
    BLIP_CHECK(drop_values.size() == 2 && drop_values[0] == 2 && drop_values[1] == 3);

    EventBus<TestEvent, 2, 2> coalesce_bus{TaskClass::control};
    FixedVector<int, 16> coalesce_values{};
    EventContext coalesce_context{&coalesce_values, &coalesce_bus, {}, Status::success(), false};
    BLIP_CHECK(coalesce_bus.subscribe("subscriber", on_event, &coalesce_context));
    coalesce_context.producer = coalesce_bus.producer();
    BLIP_CHECK(coalesce_bus.publish(coalesce_context.producer, TestEvent{1}, 7,
                                    OverflowPolicy::reject_new));
    BLIP_CHECK(coalesce_bus.publish(coalesce_context.producer, TestEvent{2}, 8,
                                    OverflowPolicy::reject_new));
    BLIP_CHECK(coalesce_bus.publish(coalesce_context.producer, TestEvent{9}, 7,
                                    OverflowPolicy::coalesce_by_key));
    BLIP_CHECK(coalesce_bus.dispatch_one(TaskClass::control));
    BLIP_CHECK(coalesce_values.size() == 1 && coalesce_values[0] == 9);
    BLIP_CHECK(coalesce_bus.metrics().coalesced == 1);

    EventBus<TestEvent, 1, 1> fault_bus{TaskClass::control};
    const auto fault_producer = fault_bus.producer();
    BLIP_CHECK(fault_bus.publish(fault_producer, TestEvent{1}, 1, OverflowPolicy::reject_new));
    const auto fault = fault_bus.publish(fault_producer, TestEvent{2}, 2, OverflowPolicy::fault);
    BLIP_CHECK(!fault && fault.error().code == ErrorCode::queue_faulted);
    BLIP_CHECK(fault_bus.faulted());
    return true;
}

} // namespace

int main() {
    const TestCase tests[]{
        {"scheduler overflow policies", scheduler_overflow_policies},
        {"budget context cancellation and stop", budget_context_cancellation_and_stop},
        {"monotonic timer wrap", monotonic_wrap_extension},
        {"event delivery recursion and stop", event_delivery_recursion_and_stop},
        {"event overflow policies", event_overflow_policies},
    };
    return run_tests(tests);
}
