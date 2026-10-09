#include "blip/storage/automatic_file_store.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace blip::storage {
namespace {
constexpr std::size_t kHeaderBytes = 40;
constexpr std::size_t kTrailerBytes = 4;
constexpr std::uint32_t kMagic = 0x46424c42U; // BLBF, bulk file format 1.

core::Error error(core::ErrorCode code, std::string_view detail) noexcept {
    return {core::ErrorDomain::storage, code, "storage.files", "bulk-file", detail};
}
core::Status failed(core::ErrorCode code, std::string_view detail) noexcept {
    return core::Status::failure(error(code, detail));
}
std::uint32_t crc(std::uint32_t value, std::span<const std::byte> bytes) noexcept {
    for (const auto byte : bytes) {
        value ^= std::to_integer<unsigned char>(byte);
        for (unsigned bit = 0; bit < 8; ++bit)
            value = (value >> 1U) ^ (0xedb88320U & (0U - (value & 1U)));
    }
    return value;
}
std::uint64_t hash(std::string_view path) noexcept {
    std::uint64_t value = 14695981039346656037ULL;
    for (const auto character : path) {
        value ^= static_cast<unsigned char>(character);
        value *= 1099511628211ULL;
    }
    return value;
}
void put(std::span<std::byte> bytes, std::size_t offset, std::uint64_t value,
         std::size_t count) noexcept {
    for (std::size_t i = 0; i < count; ++i) bytes[offset + i] = std::byte(value >> (i * 8U));
}
std::uint64_t get(std::span<const std::byte> bytes, std::size_t offset,
                  std::size_t count) noexcept {
    std::uint64_t value{};
    for (std::size_t i = 0; i < count; ++i)
        value |= std::uint64_t(std::to_integer<unsigned char>(bytes[offset + i])) << (i * 8U);
    return value;
}
core::Status exact(WebAssetBackend& backend, std::string_view path, std::size_t offset,
                   std::span<std::byte> bytes) noexcept {
    const auto result = backend.read_at(path, offset, bytes);
    if (!result) return core::Status::failure(result.error());
    return result.value() == bytes.size() ? core::Status::success()
                                         : failed(core::ErrorCode::corrupt_data, "short-file");
}
} // namespace

bool AutomaticFileStore::valid_path(std::string_view path) noexcept {
    if (path.empty() || path.size() > kMaxLogicalPathBytes || path.front() == '/' ||
        path.back() == '/' || path.find("..") != path.npos || path.find("//") != path.npos ||
        path.starts_with("web/")) return false;
    return std::all_of(path.begin(), path.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '/' || c == '-' || c == '_' || c == '.';
    });
}

std::string_view AutomaticFileStore::physical(std::string_view path, unsigned slot,
                                               std::span<char> buffer) noexcept {
    std::memcpy(buffer.data(), path.data(), path.size());
    buffer[path.size()] = '.';
    buffer[path.size() + 1] = 'b';
    buffer[path.size() + 2] = slot == 0 ? '0' : '1';
    buffer[path.size() + 3] = '\0';
    return {buffer.data(), path.size() + 3};
}

std::uint32_t AutomaticFileStore::next_token() noexcept {
    if (++token_ == 0) ++token_;
    return token_;
}

core::Status AutomaticFileStore::set_external(WebAssetBackend* external) noexcept {
    std::lock_guard guard(mutex_);
    if (writer_.token || std::any_of(readers_.begin(), readers_.end(),
                                   [](const Reader& reader) { return reader.handle.token != 0; }))
        return failed(core::ErrorCode::invalid_state, "files-in-use");
    if (external == internal_) return failed(core::ErrorCode::invalid_argument, "same-medium");
    external_ = external;
    return core::Status::success();
}

bool AutomaticFileStore::external() const noexcept {
    std::lock_guard guard(mutex_);
    return external_ != nullptr;
}

core::Result<AutomaticFileStore::Record> AutomaticFileStore::inspect_slot(
    WebAssetBackend& backend, std::string_view path, unsigned slot) noexcept {
    using Result = core::Result<Record>;
    std::array<char, kMaxLogicalPathBytes + 4> buffer{};
    const auto name = physical(path, slot, buffer);
    const auto length = backend.file_size(name);
    if (!length) return Result::failure(length.error());
    if (length.value() < kHeaderBytes + kTrailerBytes ||
        length.value() > kMaximumFileBytes + kHeaderBytes + kTrailerBytes)
        return Result::failure(error(core::ErrorCode::corrupt_data, "file-length"));
    std::array<std::byte, kHeaderBytes> header{};
    auto status = exact(backend, name, 0, header);
    if (!status) return Result::failure(status.error());
    const auto generation = get(header, 8, 8);
    const auto size = get(header, 16, 8);
    if (get(header, 0, 4) != kMagic || get(header, 4, 2) != 1 ||
        get(header, 6, 2) != kHeaderBytes || !generation ||
        size != length.value() - kHeaderBytes - kTrailerBytes ||
        get(header, 24, 8) != hash(path) || get(header, 32, 8) > 1 ||
        (get(header, 32, 8) == 1 && size != 0))
        return Result::failure(error(core::ErrorCode::corrupt_data, "file-header"));
    auto checksum = crc(0xffffffffU, header);
    std::size_t offset = kHeaderBytes;
    const auto end = length.value() - kTrailerBytes;
    while (offset < end) {
        auto chunk = scratch_.first(std::min(scratch_.size(), end - offset));
        status = exact(backend, name, offset, chunk);
        if (!status) return Result::failure(status.error());
        checksum = crc(checksum, chunk);
        offset += chunk.size();
    }
    std::array<std::byte, kTrailerBytes> trailer{};
    status = exact(backend, name, end, trailer);
    if (!status) return Result::failure(status.error());
    if (get(trailer, 0, 4) != static_cast<std::uint64_t>(checksum ^ 0xffffffffU))
        return Result::failure(error(core::ErrorCode::corrupt_data, "file-checksum"));
    return Result::success({generation, static_cast<std::size_t>(size), slot, get(header, 32, 8) == 1});
}

core::Result<AutomaticFileStore::Record> AutomaticFileStore::inspect(
    WebAssetBackend& backend, std::string_view path) noexcept {
    const auto first = inspect_slot(backend, path, 0);
    const auto second = inspect_slot(backend, path, 1);
    // A failed medium is not a missing file. Do not silently overwrite it or
    // combine its generations with those from another filesystem.
    for (const auto* result : {&first, &second}) {
        if (!*result && result->error().code != core::ErrorCode::not_found &&
            result->error().code != core::ErrorCode::corrupt_data)
            return core::Result<Record>::failure(result->error());
    }
    if (first && second)
        return first.value().generation >= second.value().generation ? first : second;
    if (first) return first;
    if (second) return second;
    return first.error().code != core::ErrorCode::not_found ? first : second;
}

core::Result<FileReadHandle> AutomaticFileStore::open(std::string_view path) noexcept {
    std::lock_guard guard(mutex_);
    using Result = core::Result<FileReadHandle>;
    if (!valid_path(path) || scratch_.empty())
        return Result::failure(error(core::ErrorCode::invalid_argument, "path-or-scratch"));
    auto found = std::find_if(readers_.begin(), readers_.end(),
                              [](const Reader& reader) { return reader.handle.token == 0; });
    if (found == readers_.end())
        return Result::failure(error(core::ErrorCode::capacity_exceeded, "reader-limit"));
    auto* backend = external_ ? external_ : internal_;
    auto record = inspect(*backend, path);
    if (!record && backend != internal_ && record.error().code == core::ErrorCode::not_found) {
        backend = internal_;
        record = inspect(*backend, path);
    }
    if (!record) return Result::failure(record.error());
    if (record.value().deleted) return Result::failure(error(core::ErrorCode::not_found, "file-deleted"));
    const auto name = physical(path, record.value().slot, found->path);
    if (writer_.token && writer_.backend == backend && name == writer_.path.data())
        return Result::failure(error(core::ErrorCode::invalid_state, "file-being-written"));
    found->backend = backend;
    found->handle = {next_token(), record.value().generation, record.value().size};
    return Result::success(found->handle);
}

core::Result<std::size_t> AutomaticFileStore::read(FileReadHandle handle, std::size_t offset,
                                                  std::span<std::byte> output) noexcept {
    std::lock_guard guard(mutex_);
    const auto found = std::find_if(readers_.begin(), readers_.end(), [&](const Reader& reader) {
        return handle.token && reader.handle.token == handle.token;
    });
    if (found == readers_.end() || offset > found->handle.size)
        return core::Result<std::size_t>::failure(error(core::ErrorCode::invalid_argument, "reader-or-offset"));
    const auto count = std::min(output.size(), found->handle.size - offset);
    const auto status = exact(*found->backend, found->path.data(), kHeaderBytes + offset,
                               output.first(count));
    return status ? core::Result<std::size_t>::success(count)
                  : core::Result<std::size_t>::failure(status.error());
}

void AutomaticFileStore::close(FileReadHandle handle) noexcept {
    std::lock_guard guard(mutex_);
    for (auto& reader : readers_) if (reader.handle.token == handle.token) reader = {};
}

core::Result<std::uint32_t> AutomaticFileStore::begin(std::string_view path,
                                                    std::size_t size) noexcept {
    return begin_record(path, size, false);
}

core::Result<std::uint32_t> AutomaticFileStore::begin_record(std::string_view path,
    std::size_t size, bool deleted) noexcept {
    std::lock_guard guard(mutex_);
    using Result = core::Result<std::uint32_t>;
    if (!valid_path(path) || size > kMaximumFileBytes || scratch_.empty())
        return Result::failure(error(core::ErrorCode::invalid_argument, "path-size-or-scratch"));
    if (writer_.token) return Result::failure(error(core::ErrorCode::invalid_state, "writer-active"));
    auto* backend = external_ ? external_ : internal_;
    const auto record = inspect(*backend, path);
    if (!record && record.error().code != core::ErrorCode::not_found &&
        record.error().code != core::ErrorCode::corrupt_data)
        return Result::failure(record.error());
    if (record && record.value().generation == std::numeric_limits<std::uint64_t>::max())
        return Result::failure(error(core::ErrorCode::capacity_exceeded, "generation-limit"));
    const auto slot = record ? 1U - record.value().slot : 0U;
    const auto name = physical(path, slot, writer_.path);
    for (const auto& reader : readers_) {
        if (reader.handle.token && reader.backend == backend && name == reader.path.data())
            return Result::failure(error(core::ErrorCode::invalid_state, "generation-in-use"));
    }
    auto status = backend->begin_write(name);
    if (!status) { writer_ = {}; return Result::failure(status.error()); }
    writer_.backend = backend;
    writer_.token = next_token();
    writer_.expected = size;
    std::array<std::byte, kHeaderBytes> header{};
    put(header, 0, kMagic, 4); put(header, 4, 1, 2); put(header, 6, kHeaderBytes, 2);
    put(header, 8, record ? record.value().generation + 1 : 1, 8);
    put(header, 16, size, 8); put(header, 24, hash(path), 8);
    put(header, 32, deleted ? 1 : 0, 8);
    writer_.crc = crc(0xffffffffU, header);
    status = backend->append_write(header);
    if (!status) { cancel_writer(); return Result::failure(status.error()); }
    return Result::success(writer_.token);
}

core::Status AutomaticFileStore::append(std::uint32_t token,
                                        std::span<const std::byte> bytes) noexcept {
    std::lock_guard guard(mutex_);
    if (!token || writer_.token != token || writer_.prepared)
        return failed(core::ErrorCode::invalid_state, "writer-token");
    if (bytes.size() > writer_.expected - writer_.received) {
        cancel_writer(); return failed(core::ErrorCode::capacity_exceeded, "file-too-long");
    }
    if (bytes.empty()) return core::Status::success();
    const auto status = writer_.backend->append_write(bytes);
    if (!status) { cancel_writer(); return status; }
    writer_.crc = crc(writer_.crc, bytes);
    writer_.received += bytes.size();
    return core::Status::success();
}

void AutomaticFileStore::cancel_writer() noexcept {
    writer_.backend->abort_write();
    static_cast<void>(writer_.backend->remove(writer_.path.data()));
    writer_ = {};
}

void AutomaticFileStore::cancel(std::uint32_t token) noexcept {
    std::lock_guard guard(mutex_);
    if (token && writer_.token == token) cancel_writer();
}

core::Status AutomaticFileStore::finish(std::uint32_t token) noexcept {
    std::lock_guard guard(mutex_);
    if (!token || writer_.token != token || writer_.prepared)
        return failed(core::ErrorCode::invalid_state, "writer-token");
    if (writer_.received != writer_.expected) {
        cancel_writer(); return failed(core::ErrorCode::corrupt_data, "incomplete-file");
    }
    std::array<std::byte, kTrailerBytes> trailer{};
    put(trailer, 0, static_cast<std::uint64_t>(writer_.crc ^ 0xffffffffU), 4);
    auto status = writer_.backend->append_write(trailer);
    if (status) status = writer_.backend->finish_write();
    if (!status) { cancel_writer(); return status; }
    // The complete durable generation is the commit record. No pointer file,
    // rename or deletion of the previous generation is needed on FAT.
    writer_ = {};
    return core::Status::success();
}

core::Status AutomaticFileStore::prepare(std::uint32_t token) noexcept {
    std::lock_guard guard(mutex_);
    if (!token || writer_.token != token || writer_.prepared)
        return failed(core::ErrorCode::invalid_state, "writer-token");
    if (writer_.received != writer_.expected) {
        cancel_writer(); return failed(core::ErrorCode::corrupt_data, "incomplete-file");
    }
    const auto status = writer_.backend->finish_write();
    if (!status) { cancel_writer(); return status; }
    writer_.prepared = true;
    return core::Status::success();
}

core::Result<std::size_t> AutomaticFileStore::read_pending(std::uint32_t token,
    std::size_t offset, std::span<std::byte> output) noexcept {
    std::lock_guard guard(mutex_);
    if (!token || writer_.token != token || !writer_.prepared || offset > writer_.expected)
        return core::Result<std::size_t>::failure(error(core::ErrorCode::invalid_state, "pending-reader"));
    const auto count = std::min(output.size(), writer_.expected - offset);
    const auto status = exact(*writer_.backend, writer_.path.data(), kHeaderBytes + offset,
                               output.first(count));
    return status ? core::Result<std::size_t>::success(count)
                  : core::Result<std::size_t>::failure(status.error());
}

core::Status AutomaticFileStore::commit(std::uint32_t token) noexcept {
    std::lock_guard guard(mutex_);
    if (!token || writer_.token != token || !writer_.prepared)
        return failed(core::ErrorCode::invalid_state, "pending-writer");
    auto status = writer_.backend->resume_write(writer_.path.data());
    std::array<std::byte, kTrailerBytes> trailer{};
    put(trailer, 0, static_cast<std::uint64_t>(writer_.crc ^ 0xffffffffU), 4);
    if (status) status = writer_.backend->append_write(trailer);
    if (status) status = writer_.backend->finish_write();
    if (!status) { cancel_writer(); return status; }
    writer_ = {};
    return core::Status::success();
}

core::Result<LoadedFile> AutomaticFileStore::load(std::string_view path,
                                                 std::span<std::byte> output) noexcept {
    std::lock_guard guard(mutex_);
    const auto opened = open(path);
    if (!opened) return core::Result<LoadedFile>::failure(opened.error());
    const auto handle = opened.value();
    if (handle.size > output.size()) {
        close(handle);
        return core::Result<LoadedFile>::failure(error(core::ErrorCode::capacity_exceeded, "output-too-small"));
    }
    const auto loaded = read(handle, 0, output);
    close(handle);
    return loaded ? core::Result<LoadedFile>::success({handle.generation, loaded.value(), FileSlot::a})
                  : core::Result<LoadedFile>::failure(loaded.error());
}

core::Status AutomaticFileStore::save(std::string_view path,
                                      std::span<const std::byte> input) noexcept {
    std::lock_guard guard(mutex_);
    const auto started = begin(path, input.size());
    if (!started) return core::Status::failure(started.error());
    auto status = append(started.value(), input);
    if (status) status = finish(started.value());
    return status;
}

core::Status AutomaticFileStore::erase(std::string_view path) noexcept {
    std::lock_guard guard(mutex_);
    const auto started = begin_record(path, 0, true);
    return started ? finish(started.value()) : core::Status::failure(started.error());
}
} // namespace blip::storage
