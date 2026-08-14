#include "blip/storage/atomic_file_store.hpp"
#include "test_harness.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace {

using blip::core::ErrorCode;
using blip::core::ErrorDomain;
using blip::core::Result;
using blip::core::Status;
using blip::storage::AtomicFileStore;
using blip::storage::FileBackend;
using blip::storage::FileCommitPolicy;
using blip::storage::FileSlot;

constexpr std::size_t kValueCapacity = 512;

class FakeFileBackend final : public FileBackend {
  public:
    enum class FailureMode : std::uint8_t { none, before_mutation, after_mutation };

    struct Entry {
        std::array<char, 104> path{};
        std::array<std::byte, kValueCapacity> value{};
        std::size_t size{};
        bool occupied{};
    };

    FakeFileBackend() = default;
    FakeFileBackend(const FakeFileBackend& other) noexcept
        : entries(other.entries), mutation_count(other.mutation_count),
          fail_on_mutation(other.fail_on_mutation), failure_mode(other.failure_mode) {}

    [[nodiscard]] Result<std::size_t> read(std::string_view path,
                                           std::span<std::byte> output) noexcept override {
        const Entry* entry = find(path);
        if (entry == nullptr) {
            return Result<std::size_t>::failure(
                {ErrorDomain::storage, ErrorCode::not_found, {}, "read", "missing"});
        }
        if (output.size() < entry->size) {
            return Result<std::size_t>::failure({ErrorDomain::storage,
                                                 ErrorCode::capacity_exceeded,
                                                 {},
                                                 "read",
                                                 "output-too-small"});
        }
        std::memcpy(output.data(), entry->value.data(), entry->size);
        return Result<std::size_t>::success(entry->size);
    }

    [[nodiscard]] Status write_durable(std::string_view path,
                                       std::span<const std::byte> value) noexcept override {
        ++mutation_count;
        if (should_fail(FailureMode::before_mutation)) {
            return power_cut();
        }
        Entry* entry = find_or_create(path);
        if (entry == nullptr || value.size() > entry->value.size()) {
            return Status::failure(
                {ErrorDomain::storage, ErrorCode::storage_full, {}, "write", "capacity"});
        }
        std::memcpy(entry->value.data(), value.data(), value.size());
        entry->size = value.size();
        if (should_fail(FailureMode::after_mutation)) {
            return power_cut();
        }
        return Status::success();
    }

    [[nodiscard]] Status replace(std::string_view source,
                                 std::string_view destination) noexcept override {
        ++mutation_count;
        if (should_fail(FailureMode::before_mutation)) {
            return power_cut();
        }
        Entry* source_entry = find(source);
        if (source_entry == nullptr) {
            return Status::failure(
                {ErrorDomain::storage, ErrorCode::not_found, {}, "replace", "source"});
        }
        Entry* destination_entry = find_or_create(destination);
        if (destination_entry == nullptr) {
            return Status::failure(
                {ErrorDomain::storage, ErrorCode::storage_full, {}, "replace", "capacity"});
        }
        destination_entry->value = source_entry->value;
        destination_entry->size = source_entry->size;
        source_entry->occupied = false;
        if (should_fail(FailureMode::after_mutation)) {
            return power_cut();
        }
        return Status::success();
    }

    [[nodiscard]] Status remove(std::string_view path) noexcept override {
        Entry* entry = find(path);
        if (entry != nullptr) {
            entry->occupied = false;
        }
        return Status::success();
    }

    void fail(std::size_t mutation, FailureMode mode) noexcept {
        fail_on_mutation = mutation;
        failure_mode = mode;
    }

    void clear_failure() noexcept {
        fail_on_mutation = 0;
        failure_mode = FailureMode::none;
    }

    [[nodiscard]] bool corrupt(std::string_view path, std::size_t offset) noexcept {
        Entry* entry = find(path);
        if (entry == nullptr || offset >= entry->size) {
            return false;
        }
        entry->value[offset] ^= std::byte{0x55};
        return true;
    }

    [[nodiscard]] std::span<const std::byte> value(std::string_view path) const noexcept {
        const Entry* entry = find(path);
        return entry == nullptr ? std::span<const std::byte>{}
                                : std::span<const std::byte>{entry->value.data(), entry->size};
    }

    std::array<Entry, 16> entries{};
    std::size_t mutation_count{};
    std::size_t fail_on_mutation{};
    FailureMode failure_mode{FailureMode::none};

  private:
    [[nodiscard]] bool should_fail(FailureMode mode) const noexcept {
        return mutation_count == fail_on_mutation && failure_mode == mode;
    }

    [[nodiscard]] Entry* find(std::string_view path) noexcept {
        for (auto& entry : entries) {
            if (entry.occupied && std::string_view{entry.path.data()} == path) {
                return &entry;
            }
        }
        return nullptr;
    }

    [[nodiscard]] const Entry* find(std::string_view path) const noexcept {
        for (const auto& entry : entries) {
            if (entry.occupied && std::string_view{entry.path.data()} == path) {
                return &entry;
            }
        }
        return nullptr;
    }

    [[nodiscard]] Entry* find_or_create(std::string_view path) noexcept {
        Entry* entry = find(path);
        if (entry != nullptr) {
            return entry;
        }
        for (auto& candidate : entries) {
            if (!candidate.occupied && path.size() < candidate.path.size()) {
                candidate = {};
                std::memcpy(candidate.path.data(), path.data(), path.size());
                candidate.occupied = true;
                return &candidate;
            }
        }
        return nullptr;
    }

    [[nodiscard]] static Status power_cut() noexcept {
        return Status::failure(
            {ErrorDomain::storage, ErrorCode::io_failed, {}, "mutation", "power-cut"});
    }
};

template <std::size_t Count>
[[nodiscard]] std::span<const std::byte>
bytes(const std::array<std::uint8_t, Count>& value) noexcept {
    return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

template <std::size_t Count>
[[nodiscard]] bool equals(std::span<const std::byte> actual,
                          const std::array<std::uint8_t, Count>& expected) noexcept {
    return actual.size() == expected.size() &&
           std::memcmp(actual.data(), expected.data(), expected.size()) == 0;
}

bool round_trip_both_policies() {
    constexpr std::array<std::uint8_t, 3> first{1, 2, 3};
    constexpr std::array<std::uint8_t, 2> second{8, 9};
    for (const auto policy : {FileCommitPolicy::atomic_rename, FileCommitPolicy::two_slot}) {
        FakeFileBackend backend{};
        std::array<std::byte, kValueCapacity> scratch{};
        std::array<std::byte, 16> output{};
        AtomicFileStore store{backend, scratch, policy};
        BLIP_CHECK(store.save("config/device.bin", bytes(first)));
        auto loaded = store.load("config/device.bin", output);
        BLIP_CHECK(loaded && loaded.value().generation == 1);
        BLIP_CHECK(
            equals(std::span<const std::byte>{output.data(), loaded.value().payload_size}, first));
        BLIP_CHECK(store.save("config/device.bin", bytes(second)));
        loaded = store.load("config/device.bin", output);
        BLIP_CHECK(loaded && loaded.value().generation == 2);
        BLIP_CHECK(
            equals(std::span<const std::byte>{output.data(), loaded.value().payload_size}, second));
    }
    return true;
}

bool interrupted_updates_are_atomic() {
    constexpr std::array<std::uint8_t, 2> old_value{1, 1};
    constexpr std::array<std::uint8_t, 3> new_value{2, 2, 2};
    struct Scenario {
        std::size_t mutation;
        FakeFileBackend::FailureMode mode;
        bool expect_new;
    };
    const Scenario scenarios[]{
        {3, FakeFileBackend::FailureMode::before_mutation, false},
        {3, FakeFileBackend::FailureMode::after_mutation, false},
        {4, FakeFileBackend::FailureMode::before_mutation, false},
        {4, FakeFileBackend::FailureMode::after_mutation, true},
    };
    for (const auto policy : {FileCommitPolicy::atomic_rename, FileCommitPolicy::two_slot}) {
        FakeFileBackend baseline{};
        std::array<std::byte, kValueCapacity> baseline_scratch{};
        AtomicFileStore baseline_store{baseline, baseline_scratch, policy};
        BLIP_CHECK(baseline_store.save("state.bin", bytes(old_value)));
        BLIP_CHECK(baseline.mutation_count == 2);
        for (const auto& scenario : scenarios) {
            auto interrupted = baseline;
            std::array<std::byte, kValueCapacity> scratch{};
            AtomicFileStore store{interrupted, scratch, policy};
            interrupted.fail(scenario.mutation, scenario.mode);
            BLIP_CHECK(!store.save("state.bin", bytes(new_value)));
            interrupted.clear_failure();
            std::array<std::byte, 8> output{};
            const auto loaded = store.load("state.bin", output);
            BLIP_CHECK(loaded);
            const auto payload =
                std::span<const std::byte>{output.data(), loaded.value().payload_size};
            BLIP_CHECK(scenario.expect_new ? equals(payload, new_value)
                                           : equals(payload, old_value));
        }
    }
    return true;
}

bool slot_pointer_and_data_corruption_recover() {
    constexpr std::array<std::uint8_t, 1> old_value{1};
    constexpr std::array<std::uint8_t, 1> new_value{2};
    FakeFileBackend baseline{};
    std::array<std::byte, kValueCapacity> scratch{};
    AtomicFileStore store{baseline, scratch, FileCommitPolicy::two_slot};
    BLIP_CHECK(store.save("show.bin", bytes(old_value)));
    BLIP_CHECK(store.save("show.bin", bytes(new_value)));

    auto torn_pointer = baseline;
    BLIP_CHECK(torn_pointer.corrupt("show.bin.ptr", 4));
    std::array<std::byte, kValueCapacity> torn_scratch{};
    AtomicFileStore torn_store{torn_pointer, torn_scratch, FileCommitPolicy::two_slot};
    std::array<std::byte, 8> output{};
    auto loaded = torn_store.load("show.bin", output);
    BLIP_CHECK(loaded && loaded.value().generation == 2);
    BLIP_CHECK(
        equals(std::span<const std::byte>{output.data(), loaded.value().payload_size}, new_value));

    auto corrupt_selected = baseline;
    BLIP_CHECK(corrupt_selected.corrupt("show.bin.b", 0));
    std::array<std::byte, kValueCapacity> corrupt_scratch{};
    AtomicFileStore corrupt_store{corrupt_selected, corrupt_scratch, FileCommitPolicy::two_slot};
    loaded = corrupt_store.load("show.bin", output);
    BLIP_CHECK(loaded && loaded.value().generation == 1);
    BLIP_CHECK(
        equals(std::span<const std::byte>{output.data(), loaded.value().payload_size}, old_value));
    return true;
}

bool record_format_and_corruption() {
    constexpr std::array<std::uint8_t, 2> value{0xaa, 0xbb};
    FakeFileBackend backend{};
    std::array<std::byte, kValueCapacity> scratch{};
    AtomicFileStore store{backend, scratch, FileCommitPolicy::atomic_rename};
    BLIP_CHECK(store.save("golden.bin", bytes(value)));
    const auto record = backend.value("golden.bin");
    BLIP_CHECK(record.size() == 44);
    BLIP_CHECK(record[0] == std::byte{0x42} && record[1] == std::byte{0x4c} &&
               record[2] == std::byte{0x49} && record[3] == std::byte{0x46});
    BLIP_CHECK(record[4] == std::byte{1} && record[6] == std::byte{32});
    BLIP_CHECK(backend.corrupt("golden.bin", record.size() - 1));
    std::array<std::byte, 8> output{};
    const auto loaded = store.load("golden.bin", output);
    BLIP_CHECK(!loaded && loaded.error().code == ErrorCode::corrupt_data);
    return true;
}

bool path_and_capacity_bounds() {
    FakeFileBackend backend{};
    std::array<std::byte, 40> scratch{};
    AtomicFileStore store{backend, scratch, FileCommitPolicy::atomic_rename};
    constexpr std::array<std::uint8_t, 8> value{};
    auto status = store.save("../escape", bytes(value));
    BLIP_CHECK(!status && status.error().code == ErrorCode::invalid_argument);
    status = store.save("a.bin", bytes(value));
    BLIP_CHECK(!status && status.error().code == ErrorCode::capacity_exceeded);
    return true;
}

} // namespace

int main() {
    const TestCase tests[]{
        {"file round trip policies", round_trip_both_policies},
        {"file interrupted updates", interrupted_updates_are_atomic},
        {"file slot corruption recovery", slot_pointer_and_data_corruption_recover},
        {"file format and corruption", record_format_and_corruption},
        {"file path and capacity bounds", path_and_capacity_bounds},
    };
    return run_tests(tests);
}
