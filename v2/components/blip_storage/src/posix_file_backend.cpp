#include "blip/storage/posix_file_backend.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

namespace blip::storage {

PosixFileBackend::PosixFileBackend(std::string_view root) noexcept {
    if (root.empty() || root.size() > kMaxRootBytes || root.front() != '/' || root.ends_with('/')) {
        return;
    }
    std::memcpy(root_.data(), root.data(), root.size());
    root_[root.size()] = '\0';
    root_size_ = root.size();
    valid_ = true;
}

core::Error PosixFileBackend::errno_error(std::string_view operation, int value) noexcept {
    core::ErrorCode code = core::ErrorCode::io_failed;
    std::string_view detail = "filesystem-io";
    switch (value) {
    case ENOENT:
        code = core::ErrorCode::not_found;
        detail = "filesystem-not-found";
        break;
    case ENOSPC:
        code = core::ErrorCode::storage_full;
        detail = "filesystem-full";
        break;
    case ENAMETOOLONG:
    case EINVAL:
        code = core::ErrorCode::invalid_argument;
        detail = "filesystem-invalid-path";
        break;
    default:
        break;
    }
    return {core::ErrorDomain::storage, code, {}, operation, detail};
}

bool PosixFileBackend::full_path(std::string_view path, std::span<char> output) const noexcept {
    if (!valid_ || path.empty() || path.front() == '/' ||
        root_size_ + 1U + path.size() + 1U > output.size()) {
        return false;
    }
    std::memcpy(output.data(), root_.data(), root_size_);
    output[root_size_] = '/';
    std::memcpy(output.data() + root_size_ + 1U, path.data(), path.size());
    output[root_size_ + 1U + path.size()] = '\0';
    return true;
}

core::Status
PosixFileBackend::create_parent_directories(std::span<char> full_path_value) const noexcept {
    for (std::size_t index = root_size_ + 1U; index < full_path_value.size(); ++index) {
        if (full_path_value[index] == '\0') {
            break;
        }
        if (full_path_value[index] != '/') {
            continue;
        }
        full_path_value[index] = '\0';
        if (::mkdir(full_path_value.data(), 0755) != 0 && errno != EEXIST) {
            const int error = errno;
            full_path_value[index] = '/';
            return core::Status::failure(errno_error("mkdir", error));
        }
        full_path_value[index] = '/';
    }
    return core::Status::success();
}

core::Result<std::size_t> PosixFileBackend::read(std::string_view path,
                                                 std::span<std::byte> output) noexcept {
    std::array<char, kMaxFullPathBytes> resolved{};
    if (!full_path(path, resolved)) {
        return core::Result<std::size_t>::failure({core::ErrorDomain::storage,
                                                   core::ErrorCode::invalid_argument,
                                                   {},
                                                   "file-read",
                                                   "path-too-long"});
    }
    std::FILE* file = std::fopen(resolved.data(), "rb");
    if (file == nullptr) {
        return core::Result<std::size_t>::failure(errno_error("file-open-read", errno));
    }
    if (std::fseek(file, 0, SEEK_END) != 0) {
        const int error = errno;
        std::fclose(file);
        return core::Result<std::size_t>::failure(errno_error("file-seek", error));
    }
    const long length = std::ftell(file);
    if (length < 0 || static_cast<unsigned long>(length) > output.size()) {
        std::fclose(file);
        return core::Result<std::size_t>::failure({core::ErrorDomain::storage,
                                                   core::ErrorCode::capacity_exceeded,
                                                   {},
                                                   "file-read",
                                                   "output-too-small"});
    }
    std::rewind(file);
    const auto size = static_cast<std::size_t>(length);
    const std::size_t read_size = size == 0U ? 0U : std::fread(output.data(), 1, size, file);
    const int close_result = std::fclose(file);
    if (read_size != size || close_result != 0) {
        return core::Result<std::size_t>::failure(errno_error("file-read", errno));
    }
    return core::Result<std::size_t>::success(size);
}

core::Status PosixFileBackend::write_durable(std::string_view path,
                                             std::span<const std::byte> value) noexcept {
    std::array<char, kMaxFullPathBytes> resolved{};
    if (!full_path(path, resolved)) {
        return core::Status::failure({core::ErrorDomain::storage,
                                      core::ErrorCode::invalid_argument,
                                      {},
                                      "file-write",
                                      "path-too-long"});
    }
    auto status = create_parent_directories(resolved);
    if (!status) {
        return status;
    }
    std::FILE* file = std::fopen(resolved.data(), "wb");
    if (file == nullptr) {
        return core::Status::failure(errno_error("file-open-write", errno));
    }
    const std::size_t written =
        value.empty() ? 0U : std::fwrite(value.data(), 1, value.size(), file);
    if (written != value.size() || std::fflush(file) != 0 || ::fsync(::fileno(file)) != 0) {
        const int error = errno;
        std::fclose(file);
        return core::Status::failure(errno_error("file-flush", error));
    }
    if (std::fclose(file) != 0) {
        return core::Status::failure(errno_error("file-close", errno));
    }
    return core::Status::success();
}

core::Status PosixFileBackend::replace(std::string_view source,
                                       std::string_view destination) noexcept {
    std::array<char, kMaxFullPathBytes> source_path{};
    std::array<char, kMaxFullPathBytes> destination_path{};
    if (!full_path(source, source_path) || !full_path(destination, destination_path)) {
        return core::Status::failure({core::ErrorDomain::storage,
                                      core::ErrorCode::invalid_argument,
                                      {},
                                      "file-replace",
                                      "path-too-long"});
    }
    if (std::rename(source_path.data(), destination_path.data()) != 0) {
        return core::Status::failure(errno_error("file-replace", errno));
    }
    return core::Status::success();
}

core::Status PosixFileBackend::remove(std::string_view path) noexcept {
    std::array<char, kMaxFullPathBytes> resolved{};
    if (!full_path(path, resolved)) {
        return core::Status::failure({core::ErrorDomain::storage,
                                      core::ErrorCode::invalid_argument,
                                      {},
                                      "file-remove",
                                      "path-too-long"});
    }
    if (std::remove(resolved.data()) != 0 && errno != ENOENT) {
        return core::Status::failure(errno_error("file-remove", errno));
    }
    return core::Status::success();
}

} // namespace blip::storage
