#include "blip/storage/web_asset_store.hpp"
#include "test_harness.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <span>
#include <string_view>
#include <vector>

namespace {

using blip::core::ErrorCode;
using blip::core::ErrorDomain;
using blip::core::Result;
using blip::core::Status;
using blip::storage::kMaxWebAssetBundleBytes;
using blip::storage::WebAssetBackend;
using blip::storage::WebAssetStore;
using blip::storage::WebContentEncoding;

class FakeAssetBackend final : public WebAssetBackend {
  public:
    enum class ReplaceFailure : std::uint8_t { none, before_mutation, after_mutation };

    [[nodiscard]] Result<std::size_t> file_size(std::string_view path) noexcept override {
        const auto* file = find(path);
        return file == nullptr
                   ? Result<std::size_t>::failure(
                         {ErrorDomain::storage, ErrorCode::not_found, {}, "size", "missing"})
                   : Result<std::size_t>::success(file->size());
    }

    [[nodiscard]] Result<std::size_t> read_at(std::string_view path, std::size_t offset,
                                              std::span<std::byte> output) noexcept override {
        const auto* file = find(path);
        if (file == nullptr) {
            return Result<std::size_t>::failure(
                {ErrorDomain::storage, ErrorCode::not_found, {}, "read", "missing"});
        }
        if (offset > file->size()) {
            return Result<std::size_t>::failure(
                {ErrorDomain::storage, ErrorCode::invalid_argument, {}, "read", "offset"});
        }
        const std::size_t count = std::min(output.size(), file->size() - offset);
        std::copy_n(file->begin() + static_cast<std::ptrdiff_t>(offset), count, output.begin());
        return Result<std::size_t>::success(count);
    }

    [[nodiscard]] Status begin_write(std::string_view path) noexcept override {
        if (writing_ || path != WebAssetStore::kUploadPath) {
            return failure(ErrorCode::invalid_state, "begin");
        }
        upload_.clear();
        upload_present_ = true;
        writing_ = true;
        return Status::success();
    }

    [[nodiscard]] Status append_write(std::span<const std::byte> value) noexcept override {
        if (!writing_) {
            return failure(ErrorCode::invalid_state, "append");
        }
        upload_.insert(upload_.end(), value.begin(), value.end());
        return Status::success();
    }

    [[nodiscard]] Status finish_write() noexcept override {
        if (!writing_) {
            return failure(ErrorCode::invalid_state, "finish");
        }
        writing_ = false;
        return Status::success();
    }

    void abort_write() noexcept override { writing_ = false; }

    [[nodiscard]] Status replace(std::string_view source,
                                 std::string_view destination) noexcept override {
        if (source != WebAssetStore::kUploadPath || destination != WebAssetStore::kActivePath ||
            !upload_present_) {
            return failure(ErrorCode::not_found, "replace");
        }
        if (replace_failure == ReplaceFailure::before_mutation) {
            return failure(ErrorCode::io_failed, "replace");
        }
        active_ = upload_;
        active_present_ = true;
        upload_.clear();
        upload_present_ = false;
        if (replace_failure == ReplaceFailure::after_mutation) {
            return failure(ErrorCode::io_failed, "replace");
        }
        return Status::success();
    }

    [[nodiscard]] Status remove(std::string_view path) noexcept override {
        if (path == WebAssetStore::kUploadPath) {
            upload_.clear();
            upload_present_ = false;
            return Status::success();
        }
        if (path == WebAssetStore::kActivePath) {
            active_.clear();
            active_present_ = false;
            return Status::success();
        }
        return failure(ErrorCode::invalid_argument, "remove");
    }

    void simulate_reset() noexcept { writing_ = false; }

    void seed_active(std::span<const std::byte> value) {
        active_.assign(value.begin(), value.end());
        active_present_ = true;
    }

    [[nodiscard]] bool upload_present() const noexcept { return upload_present_; }

    ReplaceFailure replace_failure{ReplaceFailure::none};

  private:
    [[nodiscard]] const std::vector<std::byte>* find(std::string_view path) const noexcept {
        if (path == WebAssetStore::kActivePath && active_present_) {
            return &active_;
        }
        if (path == WebAssetStore::kUploadPath && upload_present_) {
            return &upload_;
        }
        return nullptr;
    }

    [[nodiscard]] static Status failure(ErrorCode code, std::string_view operation) noexcept {
        return Status::failure({ErrorDomain::storage, code, {}, operation, "injected"});
    }

    std::vector<std::byte> active_{};
    std::vector<std::byte> upload_{};
    bool active_present_{};
    bool upload_present_{};
    bool writing_{};
};

[[nodiscard]] std::vector<std::byte> factory_bundle() {
    std::ifstream input{BLIP_FACTORY_WEB_BUNDLE, std::ios::binary};
    const std::vector<char> source{std::istreambuf_iterator<char>{input},
                                   std::istreambuf_iterator<char>{}};
    std::vector<std::byte> result(source.size());
    for (std::size_t index = 0; index < source.size(); ++index) {
        result[index] = static_cast<std::byte>(static_cast<unsigned char>(source[index]));
    }
    return result;
}

[[nodiscard]] std::uint32_t crc32(std::span<const std::byte> value) noexcept {
    std::uint32_t crc = 0xffffffffU;
    for (const std::byte byte : value) {
        crc ^= std::to_integer<std::uint8_t>(byte);
        for (std::size_t bit = 0; bit < 8U; ++bit) {
            const std::uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return crc ^ 0xffffffffU;
}

void put_u32(std::span<std::byte> value, std::size_t offset, std::uint32_t word) noexcept {
    for (std::size_t index = 0; index < 4U; ++index) {
        value[offset + index] = static_cast<std::byte>((word >> (index * 8U)) & 0xffU);
    }
}

[[nodiscard]] std::vector<std::byte> versioned_bundle(std::uint32_t version) {
    auto bundle = factory_bundle();
    put_u32(bundle, 8, version);
    put_u32(bundle, 28, 0U);
    put_u32(bundle, 28, crc32(bundle));
    return bundle;
}

[[nodiscard]] Status install(WebAssetStore& store, std::span<const std::byte> bundle,
                             std::size_t chunk_size = 257U) noexcept {
    auto status = store.begin_install(bundle.size());
    if (!status) {
        return status;
    }
    for (std::size_t offset = 0; offset < bundle.size(); offset += chunk_size) {
        status = store.append_install(
            bundle.subspan(offset, std::min(chunk_size, bundle.size() - offset)));
        if (!status) {
            return status;
        }
    }
    const auto result = store.finish_install();
    return result ? Status::success() : Status::failure(result.error());
}

bool factory_bundle_loads_and_streams() {
    const auto bundle = factory_bundle();
    BLIP_CHECK(!bundle.empty());
    FakeAssetBackend backend{};
    std::array<std::byte, 127> scratch{};
    WebAssetStore store{backend, scratch};
    const auto factory_status = store.ensure_factory(bundle);
    if (!factory_status) {
        std::fprintf(stderr, "factory status operation=%.*s detail=%.*s code=%u\n",
                     static_cast<int>(factory_status.error().operation.size()),
                     factory_status.error().operation.data(),
                     static_cast<int>(factory_status.error().detail.size()),
                     factory_status.error().detail.data(),
                     static_cast<unsigned>(factory_status.error().code));
    }
    BLIP_CHECK(factory_status);
    BLIP_CHECK(store.active());
    BLIP_CHECK(store.info().bundle_version == 2000U);
    BLIP_CHECK(store.info().asset_count == 10U);

    const auto* index = store.find("/");
    BLIP_CHECK(index != nullptr && index->path_view() == "/index.html");
    BLIP_CHECK(index->encoding == WebContentEncoding::gzip);
    std::vector<std::byte> streamed(index->stored_size);
    std::size_t offset{};
    while (offset < streamed.size()) {
        const auto result =
            store.read(*index, offset,
                       std::span<std::byte>{streamed}.subspan(
                           offset, std::min<std::size_t>(31, streamed.size() - offset)));
        BLIP_CHECK(result && result.value() != 0U);
        offset += result.value();
    }
    BLIP_CHECK(crc32(streamed) == index->crc32);
    BLIP_CHECK(store.find("/missing.js") == nullptr);
    return true;
}

bool corrupt_update_retains_active_bundle() {
    const auto factory = factory_bundle();
    auto corrupt = versioned_bundle(2001U);
    corrupt.back() ^= std::byte{0x55};
    FakeAssetBackend backend{};
    std::array<std::byte, 131> scratch{};
    WebAssetStore store{backend, scratch};
    BLIP_CHECK(store.ensure_factory(factory));
    BLIP_CHECK(!install(store, corrupt));
    BLIP_CHECK(store.active() && store.info().bundle_version == 2000U);

    std::array<std::byte, 137> reboot_scratch{};
    WebAssetStore rebooted{backend, reboot_scratch};
    const auto loaded = rebooted.load_active();
    BLIP_CHECK(loaded && loaded.value().bundle_version == 2000U);
    return true;
}

bool boot_cleans_staging_and_recovers_corrupt_active() {
    const auto factory = factory_bundle();
    auto corrupt = factory;
    corrupt.back() ^= std::byte{0x55};
    FakeAssetBackend backend{};
    backend.seed_active(corrupt);
    BLIP_CHECK(backend.begin_write(WebAssetStore::kUploadPath));
    BLIP_CHECK(backend.append_write(std::span<const std::byte>{factory}.first(100U)));
    BLIP_CHECK(backend.finish_write());

    std::array<std::byte, 149> scratch{};
    WebAssetStore store{backend, scratch};
    BLIP_CHECK(store.ensure_factory(factory));
    BLIP_CHECK(store.active() && store.info().bundle_version == 2000U);
    BLIP_CHECK(!backend.upload_present());
    return true;
}

bool interrupted_and_complete_updates_are_atomic() {
    const auto factory = factory_bundle();
    const auto update = versioned_bundle(2001U);
    FakeAssetBackend backend{};
    std::array<std::byte, 193> scratch{};
    WebAssetStore store{backend, scratch};
    BLIP_CHECK(store.ensure_factory(factory));
    BLIP_CHECK(store.begin_install(update.size()));
    BLIP_CHECK(store.append_install(std::span<const std::byte>{update}.first(update.size() / 2U)));
    backend.simulate_reset();

    std::array<std::byte, 197> reboot_scratch{};
    WebAssetStore rebooted{backend, reboot_scratch};
    auto loaded = rebooted.load_active();
    BLIP_CHECK(loaded && loaded.value().bundle_version == 2000U);
    BLIP_CHECK(install(rebooted, update));
    loaded = rebooted.load_active();
    BLIP_CHECK(loaded && loaded.value().bundle_version == 2001U);
    return true;
}

bool rename_failures_recover_old_or_new_bundle() {
    const auto factory = factory_bundle();
    const auto update = versioned_bundle(2001U);
    for (const auto scenario : {FakeAssetBackend::ReplaceFailure::before_mutation,
                                FakeAssetBackend::ReplaceFailure::after_mutation}) {
        FakeAssetBackend backend{};
        std::array<std::byte, 211> scratch{};
        WebAssetStore store{backend, scratch};
        BLIP_CHECK(store.ensure_factory(factory));
        backend.replace_failure = scenario;
        const auto status = install(store, update);
        if (scenario == FakeAssetBackend::ReplaceFailure::before_mutation) {
            BLIP_CHECK(!status);
        } else {
            BLIP_CHECK(status);
        }
        std::array<std::byte, 223> reboot_scratch{};
        WebAssetStore rebooted{backend, reboot_scratch};
        const auto loaded = rebooted.load_active();
        const std::uint32_t expected =
            scenario == FakeAssetBackend::ReplaceFailure::before_mutation ? 2000U : 2001U;
        BLIP_CHECK(loaded && loaded.value().bundle_version == expected);
    }
    return true;
}

bool install_and_read_bounds_fail_closed() {
    FakeAssetBackend backend{};
    std::array<std::byte, 95> too_small{};
    WebAssetStore invalid{backend, too_small};
    BLIP_CHECK(!invalid.ensure_factory(factory_bundle()));

    std::array<std::byte, 128> scratch{};
    WebAssetStore store{backend, scratch};
    auto status = store.begin_install(kMaxWebAssetBundleBytes + 1U);
    BLIP_CHECK(!status && status.error().code == ErrorCode::capacity_exceeded);
    BLIP_CHECK(store.ensure_factory(factory_bundle()));
    const auto* index = store.find("/");
    BLIP_CHECK(index != nullptr);
    std::array<std::byte, 1> output{};
    const auto read = store.read(*index, index->stored_size + 1U, output);
    BLIP_CHECK(!read && read.error().code == ErrorCode::invalid_argument);
    return true;
}

constexpr TestCase kTests[]{
    {"factory web bundle", factory_bundle_loads_and_streams},
    {"corrupt update", corrupt_update_retains_active_bundle},
    {"boot recovery", boot_cleans_staging_and_recovers_corrupt_active},
    {"interrupted web update", interrupted_and_complete_updates_are_atomic},
    {"rename recovery", rename_failures_recover_old_or_new_bundle},
    {"web bundle bounds", install_and_read_bounds_fail_closed},
};

} // namespace

int main() { return run_tests(kTests); }
