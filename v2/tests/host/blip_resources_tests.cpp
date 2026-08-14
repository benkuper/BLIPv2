#include "blip/resources/broker.hpp"
#include "test_harness.hpp"

#include <array>
#include <string_view>
#include <utility>

namespace {

using blip::core::ErrorCode;
using blip::core::OwnershipMode;
using blip::core::ResourceClass;
using blip::core::ResourceRequest;
using blip::resources::Broker;
using blip::resources::ResourceSpec;

constexpr std::array<std::string_view, 1> gpio0{"gpio.0"};
constexpr std::array<std::string_view, 1> gpio1{"gpio.1"};
constexpr std::array<std::string_view, 2> gpio_reversed{"gpio.1", "gpio.0"};
constexpr std::array<std::string_view, 1> i2c0{"i2c.0"};
constexpr std::array<std::string_view, 1> memory0{"memory.internal"};
constexpr std::array<std::string_view, 1> radio0{"radio.0"};

[[nodiscard]] constexpr ResourceRequest request(ResourceClass resource_class,
                                                std::string_view logical_name,
                                                std::span<const std::string_view> alternatives,
                                                OwnershipMode ownership = OwnershipMode::exclusive,
                                                std::uint32_t amount = 1) noexcept {
    ResourceRequest value{};
    value.resource_class = resource_class;
    value.logical_name = logical_name;
    value.ownership = ownership;
    value.alternatives = alternatives;
    value.amount = amount;
    return value;
}

bool exclusive_conflict_names_claimant() {
    Broker<2, 2> broker{};
    BLIP_CHECK(broker.add_resource({ResourceClass::gpio, "gpio.0", 0, 1, {}}));
    auto first = broker.acquire("component.first", request(ResourceClass::gpio, "data", gpio0));
    BLIP_CHECK(first);
    auto second = broker.acquire("component.second", request(ResourceClass::gpio, "clock", gpio0));
    BLIP_CHECK(!second);
    BLIP_CHECK(second.error().code == ErrorCode::resource_conflict);
    BLIP_CHECK(second.error().component == "component.second");
    BLIP_CHECK(second.error().detail == "component.first");
    return true;
}

bool shared_and_bus_members() {
    Broker<2, 5> broker{};
    BLIP_CHECK(broker.add_resource({ResourceClass::gpio, "gpio.0", 0, 3, {}}));
    BLIP_CHECK(broker.add_resource({ResourceClass::i2c, "i2c.0", 0, 3, {}}));
    auto reader_a = broker.acquire(
        "component.a", request(ResourceClass::gpio, "sense-a", gpio0, OwnershipMode::shared_read));
    auto reader_b = broker.acquire(
        "component.b", request(ResourceClass::gpio, "sense-b", gpio0, OwnershipMode::shared_read));
    BLIP_CHECK(reader_a && reader_b);

    auto member_a_request =
        request(ResourceClass::i2c, "sensor-a", i2c0, OwnershipMode::bus_member);
    member_a_request.member_key = 0x28;
    auto member_b_request =
        request(ResourceClass::i2c, "sensor-b", i2c0, OwnershipMode::bus_member);
    member_b_request.member_key = 0x29;
    auto duplicate_request =
        request(ResourceClass::i2c, "sensor-c", i2c0, OwnershipMode::bus_member);
    duplicate_request.member_key = 0x28;
    auto member_a = broker.acquire("component.a", member_a_request);
    auto member_b = broker.acquire("component.b", member_b_request);
    BLIP_CHECK(member_a && member_b);
    auto duplicate = broker.acquire("component.c", duplicate_request);
    BLIP_CHECK(!duplicate);
    BLIP_CHECK(duplicate.error().detail == "component.a");
    return true;
}

bool deterministic_alternative_and_capability() {
    Broker<2, 1> broker{};
    BLIP_CHECK(broker.add_resource({ResourceClass::gpio, "gpio.1", 0b11, 1, {}}));
    BLIP_CHECK(broker.add_resource({ResourceClass::gpio, "gpio.0", 0b01, 1, {}}));
    auto candidate = request(ResourceClass::gpio, "candidate", gpio_reversed);
    candidate.required_capabilities = 0b01;
    auto result = broker.acquire("component.owner", candidate);
    BLIP_CHECK(result);
    BLIP_CHECK(result.value().resource_id() == "gpio.0");
    return true;
}

bool partial_batch_rolls_back() {
    Broker<2, 3> broker{};
    BLIP_CHECK(broker.add_resource({ResourceClass::gpio, "gpio.0", 0, 1, {}}));
    BLIP_CHECK(broker.add_resource({ResourceClass::gpio, "gpio.1", 0, 1, {}}));
    auto blocker = broker.acquire("component.blocker", request(ResourceClass::gpio, "held", gpio1));
    BLIP_CHECK(blocker);
    const std::array<ResourceRequest, 2> batch_requests{
        request(ResourceClass::gpio, "a-first", gpio0),
        request(ResourceClass::gpio, "z-conflict", gpio1),
    };
    auto batch = broker.acquire_batch("component.batch", batch_requests);
    BLIP_CHECK(!batch);
    BLIP_CHECK(broker.active_lease_count() == 1);
    auto released_candidate =
        broker.acquire("component.after", request(ResourceClass::gpio, "after", gpio0));
    BLIP_CHECK(released_candidate);
    return true;
}

bool weak_token_release_and_reacquire() {
    Broker<1, 1> broker{};
    BLIP_CHECK(broker.add_resource({ResourceClass::gpio, "gpio.0", 0, 1, {}}));
    auto first = broker.acquire("component.first", request(ResourceClass::gpio, "first", gpio0));
    BLIP_CHECK(first);
    const auto old_token = first.value().weak_token();
    BLIP_CHECK(old_token.valid());
    first.value().release();
    BLIP_CHECK(!old_token.valid());
    auto second = broker.acquire("component.second", request(ResourceClass::gpio, "second", gpio0));
    BLIP_CHECK(second);
    BLIP_CHECK(!old_token.valid());
    BLIP_CHECK(second.value().weak_token().valid());
    return true;
}

bool reservation_capacity_and_radio_conflict() {
    Broker<3, 4> broker{};
    BLIP_CHECK(broker.add_resource({ResourceClass::gpio, "gpio.0", 0, 1, "component.system"}));
    BLIP_CHECK(
        broker.add_resource({ResourceClass::internal_memory, "memory.internal", 0, 1024, {}}));
    BLIP_CHECK(broker.add_resource({ResourceClass::radio, "radio.0", 0, 2, {}}));

    auto reserved = broker.acquire("component.user", request(ResourceClass::gpio, "pin", gpio0));
    BLIP_CHECK(!reserved);
    BLIP_CHECK(reserved.error().detail == "component.system");
    auto system = broker.acquire("component.system", request(ResourceClass::gpio, "pin", gpio0));
    BLIP_CHECK(system);

    auto oversized =
        broker.acquire("component.memory", request(ResourceClass::internal_memory, "pool", memory0,
                                                   OwnershipMode::multiplexed, 2048));
    BLIP_CHECK(!oversized);

    auto wifi_request = request(ResourceClass::radio, "wifi", radio0, OwnershipMode::multiplexed);
    wifi_request.feature_mask = 0b01;
    wifi_request.incompatible_features = 0b10;
    auto classic_request =
        request(ResourceClass::radio, "classic", radio0, OwnershipMode::multiplexed);
    classic_request.feature_mask = 0b10;
    classic_request.incompatible_features = 0b01;
    auto wifi = broker.acquire("component.wifi", wifi_request);
    BLIP_CHECK(wifi);
    auto classic = broker.acquire("component.classic", classic_request);
    BLIP_CHECK(!classic);
    BLIP_CHECK(classic.error().detail == "component.wifi");
    return true;
}

} // namespace

int main() {
    const TestCase tests[]{
        {"exclusive conflict names claimant", exclusive_conflict_names_claimant},
        {"shared resources and bus members", shared_and_bus_members},
        {"deterministic alternative and capability", deterministic_alternative_and_capability},
        {"partial batch rollback", partial_batch_rolls_back},
        {"weak token release and reacquire", weak_token_release_and_reacquire},
        {"reservation capacity and radio conflict", reservation_capacity_and_radio_conflict},
    };
    return run_tests(tests);
}
