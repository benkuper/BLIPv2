#include "blip/wasm/service.hpp"
#include "blip/wasm/module_upload.hpp"
#include <array>
#include <cstring>
#include <iostream>

namespace {
using namespace blip::wasm;
using blip::core::ErrorCode;
using blip::core::Result;
using blip::core::Status;
#define CHECK(x) do { if (!(x)) { std::cerr << "FAIL " << __func__ << ':' << __LINE__ << " " #x "\n"; return false; } } while (false)
Status failure(ErrorCode code) {
    return Status::failure({blip::core::ErrorDomain::control, code, "test.wasm", "fake", "test-fault"});
}
constexpr std::array binary{std::byte{0}, std::byte{0x61}, std::byte{0x73}, std::byte{0x6d},
                            std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{42}};

// Two independent behaviors behind the same engine boundary. The service
// client never refers to these classes or any implementation-specific handles.
class TestRuntime : public Runtime {
  public:
    std::uint32_t initialized{}, loads{}, invocations{}, unloads{}, shutdowns{};
    bool fail_initialize{}, fail_load{}, trap{}, wrong_count{}, wrong_type{};
    std::span<std::byte> module{};
    std::string_view name() const noexcept override { return "test.direct"; }
    Status initialize(std::span<std::byte>, Limits) noexcept override {
        ++initialized; return fail_initialize ? failure(ErrorCode::resource_unavailable) : Status::success();
    }
    Status load(std::span<std::byte> data) noexcept override {
        ++loads; module = data;
        return fail_load ? failure(ErrorCode::corrupt_data) : Status::success();
    }
    void unload() noexcept override { ++unloads; module = {}; }
    void shutdown() noexcept override { ++shutdowns; }
    Result<Signature> signature(std::string_view name) noexcept override {
        if (name != "echo") return Result<Signature>::failure(failure(ErrorCode::not_found).error());
        Signature signature{};
        signature.argument_count = 1;
        signature.result_count = 1;
        signature.arguments[0] = ValueType::f64;
        signature.results[0] = ValueType::f64;
        return Result<Signature>::success(signature);
    }
    Status invoke(std::string_view, std::span<const Value> arguments, ExecutionBudget,
                  std::span<Value> results, std::size_t& count) noexcept override {
        ++invocations;
        if (trap) return failure(ErrorCode::verification_failed);
        results[0] = wrong_type ? Value::i32(7) : arguments[0];
        count = wrong_count ? kMaximumResults + 1 : 1;
        return Status::success();
    }
    void request_cancel() noexcept override {}
    RuntimeSnapshot snapshot() const noexcept override {
        RuntimeSnapshot snapshot{};
        if (trap || fail_load) std::memcpy(snapshot.fault.data(), "owned-engine-fault", 19);
        return snapshot;
    }
};
class OtherRuntime final : public TestRuntime {
  public:
    std::string_view name() const noexcept override { return "test.indirect"; }
    Status invoke(std::string_view, std::span<const Value> arguments, ExecutionBudget,
                  std::span<Value> results, std::size_t& count) noexcept override {
        ++invocations;
        // Round-trip through an unrelated bit-oriented implementation.
        const auto value = std::bit_cast<double>(arguments[0].bits);
        results[0] = Value::f64(value);
        count = 1;
        return Status::success();
    }
};

struct Fixture {
    std::array<std::byte, 128> pool{}, bytes{};
    TestRuntime runtime{};
    Service service{runtime, pool, bytes};
    Limits limits{65536, 4096, 128};
    std::array<Value, kMaximumResults> results{};
    std::size_t count{};
};

bool runtime_replacement_and_bit_values() {
    TestRuntime first{}; OtherRuntime second{};
    for (Runtime* runtime : std::array<Runtime*, 2>{&first, &second}) {
        std::array<std::byte, 128> pool{}, bytes{};
        Service service{*runtime, pool, bytes};
        CHECK(service.start({65536, 4096, 128})); CHECK(service.load(binary));
        // NaN payloads and signed zero must not be normalized by the service.
        for (const auto bits : {0x7ff8000000001234ULL, 0x8000000000000000ULL, 0x3ff0000000000000ULL}) {
            const std::array args{Value{ValueType::f64, bits}};
            std::array<Value, 1> result{}; std::size_t count{};
            CHECK(service.call("echo", args, {}, result, count));
            CHECK(count == 1 && result[0].type == ValueType::f64 && result[0].bits == bits);
        }
        service.stop();
    }
    CHECK(first.invocations == 3 && second.invocations == 3);
    return true;
}
bool caller_lifetime_and_prevalidation() {
    Fixture f; CHECK(f.service.start(f.limits));
    auto input = binary;
    CHECK(f.service.load(input)); CHECK(f.service.snapshot().generation == 1);
    input.back() = std::byte{99}; CHECK(f.runtime.module.back() == std::byte{42});
    auto bad = binary; bad[4] = std::byte{2};
    CHECK(!f.service.load(bad)); CHECK(f.runtime.loads == 1 && f.runtime.unloads == 0);
    CHECK(f.service.snapshot().state == State::loaded);
    const auto borrowed = f.runtime.module;
    CHECK(f.service.load(borrowed)); CHECK(f.runtime.module.back() == std::byte{42});
    CHECK(f.service.snapshot().generation == 2);
    f.service.stop();
    return true;
}
bool invalid_calls_never_enter_engine() {
    Fixture f; CHECK(f.service.start(f.limits)); CHECK(f.service.load(binary));
    const std::array args{Value::f64(3.0)};
    CHECK(!f.service.call("missing", args, {}, f.results, f.count));
    CHECK(!f.service.call("echo", {}, {}, f.results, f.count));
    CHECK(!f.service.call("echo", args, {}, {}, f.count));
    const std::array wrong{Value::i32(3)};
    CHECK(!f.service.call("echo", wrong, {}, f.results, f.count));
    ExecutionBudget zero{}; zero.instructions = 0;
    CHECK(!f.service.call("echo", args, zero, f.results, f.count));
    std::atomic<bool> cancelled{true}; ExecutionBudget cancellation{}; cancellation.cancellation = &cancelled;
    CHECK(!f.service.call("echo", args, cancellation, f.results, f.count));
    const std::array name{'e','c','h','o','\0','x'};
    CHECK(!f.service.call({name.data(), name.size()}, args, {}, f.results, f.count));
    CHECK(f.runtime.invocations == 0 && f.service.snapshot().state == State::loaded);
    f.service.stop();
    return true;
}
bool fault_requires_reload_and_retains_diagnostic() {
    Fixture f; CHECK(f.service.start(f.limits)); CHECK(f.service.load(binary));
    const std::array args{Value::f64(7)};
    f.runtime.trap = true;
    CHECK(!f.service.call("echo", args, {}, f.results, f.count));
    const auto diagnostic = f.service.snapshot();
    CHECK(diagnostic.state == State::faulted && diagnostic.faults == 1 && f.count == 0);
    CHECK(std::string_view(diagnostic.runtime.fault.data()) == "owned-engine-fault");
    CHECK(!f.service.call("echo", args, {}, f.results, f.count)); CHECK(f.runtime.invocations == 1);
    f.runtime.trap = false;
    CHECK(f.service.load(binary)); CHECK(f.service.snapshot().generation == 2);
    CHECK(f.service.call("echo", args, {}, f.results, f.count));
    CHECK(std::string_view(diagnostic.runtime.fault.data()) == "owned-engine-fault");
    f.service.stop();
    return true;
}
bool engine_failures_and_idempotent_cleanup() {
    Fixture f; f.runtime.fail_initialize = true;
    CHECK(!f.service.start(f.limits)); CHECK(f.service.snapshot().state == State::stopped);
    CHECK(f.runtime.shutdowns == 1); f.service.stop(); CHECK(f.runtime.shutdowns == 1);
    f.runtime.fail_initialize = false; CHECK(f.service.start(f.limits));
    f.runtime.fail_load = true; CHECK(!f.service.load(binary));
    CHECK(f.service.snapshot().state == State::ready && f.runtime.module.empty());
    f.runtime.fail_load = false; CHECK(f.service.load(binary));
    f.service.stop(); f.service.stop(); CHECK(f.runtime.shutdowns == 2);
    CHECK(f.service.start(f.limits)); CHECK(f.service.load(binary)); f.service.stop();
    return true;
}
bool backend_contract_violations_are_faults() {
    for (bool wrong_count : {true, false}) {
        Fixture f; CHECK(f.service.start(f.limits)); CHECK(f.service.load(binary));
        f.runtime.wrong_count = wrong_count; f.runtime.wrong_type = !wrong_count;
        const std::array args{Value::f64(2)};
        CHECK(!f.service.call("echo", args, {}, f.results, f.count));
        CHECK(f.count == 0 && f.service.snapshot().state == State::faulted);
        f.service.stop();
    }
    return true;
}
bool bounded_upload_crc_and_lifecycle() {
    std::array<std::byte, 16> storage{};
    ModuleUpload upload(storage);
    constexpr char text[] = "123456789";
    const auto input = std::as_bytes(std::span(text, 9));
    CHECK(!upload.append(0, input)); CHECK(!upload.begin(17, 0));
    CHECK(upload.begin(9, 0xcbf43926)); CHECK(upload.append(0, input.first(4)));
    CHECK(!upload.begin(1, 0)); CHECK(upload.received() == 4);
    CHECK(!upload.append(0, input.first(4))); CHECK(!upload.append(5, input.last(5)));
    CHECK(!upload.append(4, input.first(6))); CHECK(!upload.finish());
    CHECK(upload.append(4, input.last(5)));
    const auto valid = upload.finish(); CHECK(valid && valid.value().size() == 9);
    CHECK(std::memcmp(valid.value().data(), text, 9) == 0);
    CHECK(!upload.append(9, input.first(1)));
    CHECK(upload.begin(9, 0)); CHECK(upload.append(0, input));
    CHECK(!upload.finish()); CHECK(!upload.finish());
    CHECK(upload.begin(9, 0xcbf43926)); upload.cancel();
    CHECK(upload.received() == 0 && !upload.finish() && !upload.append(0, input));
    CHECK(upload.begin(9, 0xcbf43926)); CHECK(upload.append(0, input)); CHECK(upload.finish());
    return true;
}
}

int main() {
    if (!runtime_replacement_and_bit_values() || !caller_lifetime_and_prevalidation() ||
        !invalid_calls_never_enter_engine() || !fault_requires_reload_and_retains_diagnostic() ||
        !engine_failures_and_idempotent_cleanup() || !backend_contract_violations_are_faults() ||
        !bounded_upload_crc_and_lifecycle()) return 1;
    std::cout << "WASM service: 7 lifecycle, ownership, replacement, signature and upload cases passed\n";
    return 0;
}
