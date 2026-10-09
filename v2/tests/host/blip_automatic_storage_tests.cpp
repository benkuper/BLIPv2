#include "blip/storage/automatic_file_store.hpp"
#include "blip/storage/automatic_web_backend.hpp"
#include "test_harness.hpp"

#include <algorithm>
#include <map>
#include <fstream>
#include <string>
#include <vector>

namespace {
using namespace blip::storage;
using blip::core::ErrorCode;
using blip::core::Result;
using blip::core::Status;

class MemoryVolume final : public WebAssetBackend {
  public:
    std::map<std::string, std::vector<std::byte>, std::less<>> files;
    std::string writing;
    bool available{true};
    bool fail_finish{};
    static Status failure(ErrorCode code) noexcept {
        return Status::failure({blip::core::ErrorDomain::storage, code, {}, {}, "test-volume"});
    }
    Result<std::size_t> file_size(std::string_view path) noexcept override {
        if (!available) return Result<std::size_t>::failure(failure(ErrorCode::io_failed).error());
        const auto found = files.find(path);
        if (found == files.end()) return Result<std::size_t>::failure(failure(ErrorCode::not_found).error());
        return Result<std::size_t>::success(found->second.size());
    }
    Result<std::size_t> read_at(std::string_view path, std::size_t offset,
                                std::span<std::byte> output) noexcept override {
        const auto size = file_size(path);
        if (!size) return size;
        if (offset > size.value()) return Result<std::size_t>::failure(failure(ErrorCode::invalid_argument).error());
        const auto count = std::min(output.size(), size.value() - offset);
        std::copy_n(files.find(path)->second.begin() + static_cast<std::ptrdiff_t>(offset), count, output.begin());
        return Result<std::size_t>::success(count);
    }
    Status begin_write(std::string_view path) noexcept override {
        if (!available) return failure(ErrorCode::io_failed);
        if (!writing.empty()) return failure(ErrorCode::invalid_state);
        writing = path;
        files[writing].clear();
        return Status::success();
    }
    Status append_write(std::span<const std::byte> value) noexcept override {
        if (!available) return failure(ErrorCode::io_failed);
        if (writing.empty()) return failure(ErrorCode::invalid_state);
        auto& file = files[writing];
        file.insert(file.end(), value.begin(), value.end());
        return Status::success();
    }
    Status finish_write() noexcept override {
        if (!available || fail_finish) return failure(ErrorCode::io_failed);
        if (writing.empty()) return failure(ErrorCode::invalid_state);
        writing.clear();
        return Status::success();
    }
    void abort_write() noexcept override { writing.clear(); }
    Status resume_write(std::string_view path) noexcept override {
        if (!available) return failure(ErrorCode::io_failed);
        if (!writing.empty() || files.find(path) == files.end()) return failure(ErrorCode::invalid_state);
        writing = path;
        return Status::success();
    }
    Status replace(std::string_view, std::string_view) noexcept override {
        return failure(ErrorCode::invalid_state); // Bulk commit must never rename.
    }
    Status remove(std::string_view path) noexcept override {
        if (!available) return failure(ErrorCode::io_failed);
        const auto found = files.find(path);
        if (found != files.end()) files.erase(found);
        return Status::success();
    }
};

constexpr std::array first{std::byte{1}, std::byte{2}, std::byte{3}};
constexpr std::array second{std::byte{4}, std::byte{5}, std::byte{6}, std::byte{7}};

bool selects_external_and_reads_internal_fallback() {
    MemoryVolume internal, external;
    std::array<std::byte, 64> scratch{}, output{};
    AutomaticFileStore store(internal, scratch);
    BLIP_CHECK(store.save("scripts/show.wasm", first));
    BLIP_CHECK(store.set_external(&external));
    BLIP_CHECK(store.external());
    auto loaded = store.load("scripts/show.wasm", output);
    BLIP_CHECK(loaded && loaded.value().payload_size == first.size() && output[0] == first[0]);
    BLIP_CHECK(store.save("scripts/show.wasm", second));
    loaded = store.load("scripts/show.wasm", output);
    BLIP_CHECK(loaded && loaded.value().payload_size == second.size() && output[0] == second[0]);
    BLIP_CHECK(internal.files.size() == 1 && external.files.size() == 1);
    BLIP_CHECK(store.set_external(nullptr));
    loaded = store.load("scripts/show.wasm", output);
    BLIP_CHECK(loaded && output[0] == first[0]);
    return true;
}

bool streams_beyond_scratch_and_seeks() {
    MemoryVolume internal;
    std::array<std::byte, 53> scratch{};
    AutomaticFileStore store(internal, scratch);
    std::vector<std::byte> value(32769);
    for (std::size_t i = 0; i < value.size(); ++i) value[i] = std::byte(i % 251);
    const auto started = store.begin("playback/scene.bin", value.size());
    BLIP_CHECK(started);
    for (std::size_t offset = 0; offset < value.size(); offset += 97)
        BLIP_CHECK(store.append(started.value(), std::span(value).subspan(offset, std::min<std::size_t>(97, value.size() - offset))));
    BLIP_CHECK(store.finish(started.value()));
    const auto opened = store.open("playback/scene.bin");
    BLIP_CHECK(opened && opened.value().size == value.size());
    std::array<std::byte, 19> output{};
    auto read = store.read(opened.value(), 32001, output);
    BLIP_CHECK(read && read.value() == output.size());
    BLIP_CHECK(std::equal(output.begin(), output.end(), value.begin() + 32001));
    read = store.read(opened.value(), value.size() - 3, output);
    BLIP_CHECK(read && read.value() == 3);
    store.close(opened.value());
    BLIP_CHECK(!store.read(opened.value(), 0, output));
    return true;
}

bool every_truncated_generation_recovers() {
    MemoryVolume baseline;
    std::array<std::byte, 64> scratch{};
    AutomaticFileStore initial(baseline, scratch);
    BLIP_CHECK(initial.save("sequences/a", first));
    BLIP_CHECK(initial.save("sequences/a", second));
    const auto complete = baseline.files.at("sequences/a.b1");
    for (std::size_t prefix = 0; prefix < complete.size(); ++prefix) {
        MemoryVolume volume;
        volume.files = baseline.files;
        volume.files["sequences/a.b1"].resize(prefix);
        AutomaticFileStore rebooted(volume, scratch);
        std::array<std::byte, 8> output{};
        const auto loaded = rebooted.load("sequences/a", output);
        BLIP_CHECK(loaded && loaded.value().generation == 1 && output[0] == first[0]);
    }
    return true;
}

bool readers_pin_generations() {
    MemoryVolume volume;
    std::array<std::byte, 64> scratch{}, output{};
    AutomaticFileStore store(volume, scratch);
    BLIP_CHECK(store.save("scripts/a", first));
    const auto reader = store.open("scripts/a");
    BLIP_CHECK(reader);
    BLIP_CHECK(store.save("scripts/a", second));
    BLIP_CHECK(!store.save("scripts/a", first)); // Would overwrite leased generation 1.
    BLIP_CHECK(store.read(reader.value(), 0, output) && output[0] == first[0]);
    BLIP_CHECK(!store.set_external(&volume));
    store.close(reader.value());
    BLIP_CHECK(store.save("scripts/a", first));
    const auto loaded = store.load("scripts/a", output);
    BLIP_CHECK(loaded && loaded.value().generation == 3);
    return true;
}

bool media_removal_does_not_redirect_operations() {
    MemoryVolume internal, external;
    std::array<std::byte, 64> scratch{}, output{};
    AutomaticFileStore store(internal, scratch);
    BLIP_CHECK(store.save("scripts/a", first));
    BLIP_CHECK(store.set_external(&external));
    BLIP_CHECK(store.save("scripts/a", second));
    const auto reader = store.open("scripts/a");
    BLIP_CHECK(reader);
    external.available = false;
    const auto read = store.read(reader.value(), 0, output);
    BLIP_CHECK(!read && read.error().code == ErrorCode::io_failed);
    BLIP_CHECK(!store.load("scripts/a", output));
    store.close(reader.value());
    external.available = true;
    const auto writer = store.begin("playback/new", 3);
    BLIP_CHECK(writer);
    external.available = false;
    BLIP_CHECK(!store.append(writer.value(), first));
    BLIP_CHECK(!internal.files.contains("playback/new.b0"));
    BLIP_CHECK(store.set_external(nullptr));
    BLIP_CHECK(store.load("scripts/a", output) && output[0] == first[0]);
    return true;
}

bool corruption_short_write_and_flush_failure() {
    MemoryVolume volume;
    std::array<std::byte, 64> scratch{}, output{};
    AutomaticFileStore store(volume, scratch);
    BLIP_CHECK(store.save("scripts/a", first));
    auto writer = store.begin("scripts/a", second.size());
    BLIP_CHECK(writer && store.append(writer.value(), first));
    BLIP_CHECK(!store.finish(writer.value()));
    BLIP_CHECK(store.load("scripts/a", output) && output[0] == first[0]);
    volume.fail_finish = true;
    BLIP_CHECK(!store.save("scripts/a", second));
    volume.fail_finish = false;
    BLIP_CHECK(store.load("scripts/a", output) && output[0] == first[0]);
    BLIP_CHECK(store.save("scripts/a", second));
    volume.files.at("scripts/a.b1")[40] ^= std::byte{1};
    BLIP_CHECK(store.load("scripts/a", output) && output[0] == first[0]);
    volume.files.at("scripts/a.b0")[40] ^= std::byte{1};
    BLIP_CHECK(!store.load("scripts/a", output));
    return true;
}

bool bounds_and_stale_writer_ownership() {
    MemoryVolume volume;
    std::array<std::byte, 64> scratch{};
    AutomaticFileStore store(volume, scratch);
    for (const auto path : {"../escape", "/absolute", "a//b", "a/", "web/assets.bundle", "a\\b", "a?b"})
        BLIP_CHECK(!store.begin(path, 0));
    BLIP_CHECK(!store.begin("scripts/a", AutomaticFileStore::kMaximumFileBytes + 1));
    auto writer = store.begin("scripts/a", first.size());
    BLIP_CHECK(writer);
    BLIP_CHECK(!store.set_external(nullptr));
    BLIP_CHECK(!store.begin("scripts/b", 1));
    BLIP_CHECK(!store.append(writer.value() + 1, first));
    store.cancel(writer.value() + 1);
    BLIP_CHECK(store.append(writer.value(), first));
    BLIP_CHECK(store.finish(writer.value()));
    BLIP_CHECK(!store.finish(writer.value()));
    BLIP_CHECK(store.save("scripts/empty", {}));
    std::array<FileReadHandle, AutomaticFileStore::kMaximumReaders> handles{};
    for (auto& handle : handles) {
        const auto opened = store.open("scripts/empty");
        BLIP_CHECK(opened); handle = opened.value();
    }
    BLIP_CHECK(!store.open("scripts/a"));
    for (const auto handle : handles) store.close(handle);
    BLIP_CHECK(store.set_external(nullptr));
    return true;
}

bool prepared_candidate_requires_commit_and_recovers_after_reboot() {
    MemoryVolume volume;
    std::array<std::byte, 64> scratch{}, output{};
    AutomaticFileStore store(volume, scratch);
    BLIP_CHECK(store.save("scripts/a", first));
    const auto writer = store.begin("scripts/a", second.size());
    BLIP_CHECK(writer && store.append(writer.value(), second) && store.prepare(writer.value()));
    BLIP_CHECK(!store.append(writer.value(), {}));
    auto pending = store.read_pending(writer.value(), 0, output);
    BLIP_CHECK(pending && pending.value() == second.size() && output[0] == second[0]);
    BLIP_CHECK(store.load("scripts/a", output) && output[0] == first[0]);
    MemoryVolume rebooted_volume;
    rebooted_volume.files = volume.files;
    AutomaticFileStore rebooted(rebooted_volume, scratch);
    BLIP_CHECK(rebooted.load("scripts/a", output) && output[0] == first[0]);
    BLIP_CHECK(store.commit(writer.value()));
    const auto loaded = store.load("scripts/a", output);
    BLIP_CHECK(loaded && loaded.value().generation == 2 && output[0] == second[0]);
    // A first-ever interrupted write must also be replaceable after reboot.
    const auto interrupted = store.begin("scripts/new", second.size());
    BLIP_CHECK(interrupted && store.append(interrupted.value(), first));
    rebooted_volume.files = volume.files;
    BLIP_CHECK(rebooted.save("scripts/new", second));
    BLIP_CHECK(rebooted.load("scripts/new", output) && output[0] == second[0]);
    store.cancel(interrupted.value());
    return true;
}

bool web_uses_external_bulk_store_and_retains_factory_and_previous_bundle() {
    std::ifstream input(BLIP_FACTORY_WEB_BUNDLE, std::ios::binary);
    BLIP_CHECK(input.good());
    const std::vector<char> characters{std::istreambuf_iterator<char>(input), {}};
    std::vector<std::byte> bundle(characters.size());
    std::transform(characters.begin(), characters.end(), bundle.begin(),
                    [](char c) { return std::byte(static_cast<unsigned char>(c)); });
    MemoryVolume internal, external;
    internal.files[std::string(WebAssetStore::kActivePath)] = bundle;
    std::array<std::byte, 71> scratch{};
    std::array<std::byte, 512> web_scratch{};
    AutomaticFileStore files(internal, scratch);
    BLIP_CHECK(files.set_external(&external));
    AutomaticWebBackend backend(files, internal);
    WebAssetStore assets(backend, web_scratch);
    BLIP_CHECK(assets.load_active());
    BLIP_CHECK(backend.using_factory() && assets.info().bundle_version == 3007);
    BLIP_CHECK(assets.begin_install(bundle.size()));
    for (std::size_t offset = 0; offset < bundle.size(); offset += 17)
        BLIP_CHECK(assets.append_install(std::span(bundle).subspan(offset, std::min<std::size_t>(17, bundle.size() - offset))));
    BLIP_CHECK(assets.finish_install());
    BLIP_CHECK(!backend.using_factory());
    BLIP_CHECK(external.files.contains("server/assets.bundle.b0"));
    BLIP_CHECK(internal.files.at(std::string(WebAssetStore::kActivePath)) == bundle);
    BLIP_CHECK(assets.find("/index.html"));
    auto corrupt = bundle;
    corrupt.back() ^= std::byte{1};
    BLIP_CHECK(assets.begin_install(corrupt.size()) && assets.append_install(corrupt));
    BLIP_CHECK(!assets.finish_install());
    BLIP_CHECK(assets.active() && assets.info().bundle_version == 3007);
    BLIP_CHECK(!external.files.contains("server/assets.bundle.b1"));
    // Repeat to ensure the web reader releases the old generation on commit.
    for (unsigned i = 0; i < 4; ++i) {
        BLIP_CHECK(assets.begin_install(bundle.size()) && assets.append_install(bundle));
        BLIP_CHECK(assets.finish_install());
    }
    const auto asset = assets.find("/index.html");
    BLIP_CHECK(asset);
    const auto read = assets.read(*asset, 0, web_scratch);
    BLIP_CHECK(read && read.value());
    external.available = false;
    BLIP_CHECK(!assets.read(*asset, 0, web_scratch));
    backend.reset();
    BLIP_CHECK(files.set_external(nullptr));
    BLIP_CHECK(assets.load_active() && backend.using_factory());
    return true;
}

bool deletion_hides_internal_fallback_and_empty_files_remain_distinct() {
    MemoryVolume internal, external;
    std::array<std::byte, 64> scratch{}, output{};
    AutomaticFileStore store(internal, scratch);
    BLIP_CHECK(store.save("sequences/a", first));
    BLIP_CHECK(store.set_external(&external));
    BLIP_CHECK(store.erase("sequences/a"));
    auto read = store.load("sequences/a", output);
    BLIP_CHECK(!read && read.error().code == ErrorCode::not_found);
    BLIP_CHECK(store.save("sequences/a", {}));
    read = store.load("sequences/a", output);
    BLIP_CHECK(read && read.value().payload_size == 0);
    BLIP_CHECK(store.erase("sequences/a"));
    const auto deletion = external.files.at("sequences/a.b0");
    for (std::size_t length = 0; length < deletion.size(); ++length) {
        MemoryVolume cut;
        cut.files = external.files;
        cut.files["sequences/a.b0"].resize(length);
        AutomaticFileStore rebooted(internal, scratch);
        BLIP_CHECK(rebooted.set_external(&cut));
        const auto recovered = rebooted.load("sequences/a", output);
        BLIP_CHECK(recovered && recovered.value().payload_size == 0);
    }
    return true;
}

bool validation_cancellation_does_not_publish_pin_or_fall_back() {
    MemoryVolume internal, external;
    std::array<std::byte, 1024> scratch{};
    AutomaticFileStore store(internal, scratch);
    const std::vector<std::byte> value(4096, std::byte{42});
    BLIP_CHECK(store.save("scripts/show.wasm", first));
    BLIP_CHECK(store.set_external(&external));
    BLIP_CHECK(store.save("scripts/show.wasm", value));
    BLIP_CHECK(store.save("scripts/show.wasm", value));
    struct Check { unsigned remaining; unsigned calls{}; } probe{1000};
    const auto requested = [](void* context) noexcept {
        auto& state = *static_cast<Check*>(context); ++state.calls;
        return --state.remaining == 0;
    };
    const auto complete = store.open("scripts/show.wasm", {&probe, requested});
    BLIP_CHECK(complete && probe.calls > 3); store.close(complete.value());
    // Cancel at every check, including the older/newer generations, the final
    // trailer boundary and immediately before reader publication.
    for (unsigned at = 1; at <= probe.calls; ++at) {
        Check check{at};
        const auto cancelled = store.open("scripts/show.wasm", {&check, requested});
        BLIP_CHECK(!cancelled && cancelled.error().code == ErrorCode::cancelled && check.calls == at);
        BLIP_CHECK(store.set_external(&external));
    }
    // No leaked reader/writer and no fallback to the smaller internal copy.
    BLIP_CHECK(store.set_external(&external));
    std::array<FileReadHandle, AutomaticFileStore::kMaximumReaders> handles{};
    for (auto& handle : handles) {
        const auto opened = store.open("scripts/show.wasm");
        BLIP_CHECK(opened && opened.value().size == value.size()); handle = opened.value();
    }
    for (auto handle : handles) store.close(handle);
    BLIP_CHECK(store.save("scripts/show.wasm", second));
    const auto opened = store.open("scripts/show.wasm");
    BLIP_CHECK(opened && opened.value().size == second.size()); store.close(opened.value());
    return true;
}
} // namespace

int main() {
    const TestCase tests[]{
        {"automatic external selection and internal fallback", selects_external_and_reads_internal_fallback},
        {"bounded bulk streaming and seek", streams_beyond_scratch_and_seeks},
        {"every truncated generation recovers", every_truncated_generation_recovers},
        {"readers pin complete generations", readers_pin_generations},
        {"media removal never changes an open operation", media_removal_does_not_redirect_operations},
        {"checksums incomplete writes and flush failure", corruption_short_write_and_flush_failure},
        {"bounds and stale writer ownership", bounds_and_stale_writer_ownership},
        {"prepared candidate and reboot recovery", prepared_candidate_requires_commit_and_recovers_after_reboot},
        {"web uses shared external file storage", web_uses_external_bulk_store_and_retains_factory_and_previous_bundle},
        {"durable deletion and empty files", deletion_hides_internal_fallback_and_empty_files_remain_distinct},
        {"cancel validation without pins or fallback", validation_cancellation_does_not_publish_pin_or_fall_back},
    };
    return run_tests(tests);
}
