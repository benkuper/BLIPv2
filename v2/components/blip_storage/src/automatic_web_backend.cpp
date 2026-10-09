#include "blip/storage/automatic_web_backend.hpp"

#include <algorithm>
#include <cstring>

namespace blip::storage {
namespace {
constexpr std::string_view kStoredBundle = "server/assets.bundle";
core::Status failure(std::string_view detail) noexcept {
    return core::Status::failure({core::ErrorDomain::storage, core::ErrorCode::invalid_state,
                                  "storage.web_assets", "web-file", detail});
}
} // namespace

AutomaticWebBackend::~AutomaticWebBackend() { reset(); }

void AutomaticWebBackend::reset() noexcept {
    abort_write();
    files_->close(active_);
    active_ = {};
    using_factory_ = false;
}
void AutomaticWebBackend::use_factory() noexcept {
    reset();
    using_factory_ = true;
}

core::Result<std::size_t> AutomaticWebBackend::file_size(std::string_view path) noexcept {
    using Result = core::Result<std::size_t>;
    if (path == WebAssetStore::kUploadPath) {
        return prepared_ ? Result::success(expected_)
                         : Result::failure(failure("candidate-not-prepared").error());
    }
    if (path != WebAssetStore::kActivePath)
        return Result::failure(failure("unknown-file").error());
    if (active_.token) return Result::success(active_.size);
    if (using_factory_) return factory_->file_size(WebAssetStore::kActivePath);
    const auto opened = files_->open(kStoredBundle);
    if (opened) {
        active_ = opened.value();
        return Result::success(active_.size);
    }
    if (opened.error().code != core::ErrorCode::not_found &&
        opened.error().code != core::ErrorCode::corrupt_data)
        return Result::failure(opened.error());
    const auto factory_size = factory_->file_size(WebAssetStore::kActivePath);
    if (factory_size) using_factory_ = true;
    return factory_size;
}

core::Result<std::size_t> AutomaticWebBackend::read_at(std::string_view path, std::size_t offset,
                                                      std::span<std::byte> output) noexcept {
    if (path == WebAssetStore::kUploadPath && prepared_)
        return files_->read_pending(writer_, offset, output);
    if (path == WebAssetStore::kActivePath) {
        if (active_.token) return files_->read(active_, offset, output);
        if (using_factory_) return factory_->read_at(path, offset, output);
    }
    return core::Result<std::size_t>::failure(failure("file-not-open").error());
}

core::Status AutomaticWebBackend::begin_write(std::string_view path) noexcept {
    if (writing_ || prepared_ || path != WebAssetStore::kUploadPath)
        return failure("write-active-or-path");
    header_size_ = 0;
    expected_ = 0;
    writing_ = true;
    return core::Status::success();
}

core::Status AutomaticWebBackend::append_write(std::span<const std::byte> value) noexcept {
    if (!writing_) return failure("write-not-started");
    if (header_size_ < header_.size()) {
        const auto count = std::min(value.size(), header_.size() - header_size_);
        std::memcpy(header_.data() + header_size_, value.data(), count);
        header_size_ += count;
        value = value.subspan(count);
        if (header_size_ < header_.size()) return core::Status::success();
        for (std::size_t i = 0; i < 4; ++i)
            expected_ |= std::size_t(std::to_integer<unsigned char>(header_[24 + i])) << (i * 8U);
        if (expected_ < kWebAssetBundleHeaderBytes || expected_ > kMaxWebAssetBundleBytes) {
            abort_write(); return failure("bundle-size");
        }
        const auto started = files_->begin(kStoredBundle, expected_);
        if (!started) { abort_write(); return core::Status::failure(started.error()); }
        writer_ = started.value();
        const auto status = files_->append(writer_, header_);
        if (!status) { abort_write(); return status; }
    }
    const auto status = files_->append(writer_, value);
    if (!status) abort_write();
    return status;
}

core::Status AutomaticWebBackend::finish_write() noexcept {
    if (!writing_ || !writer_) { abort_write(); return failure("incomplete-header"); }
    const auto status = files_->prepare(writer_);
    if (!status) { abort_write(); return status; }
    writing_ = false;
    prepared_ = true;
    return core::Status::success();
}

void AutomaticWebBackend::abort_write() noexcept {
    files_->cancel(writer_);
    writer_ = 0;
    writing_ = false;
    prepared_ = false;
    header_size_ = 0;
    expected_ = 0;
}

core::Status AutomaticWebBackend::replace(std::string_view source,
                                         std::string_view destination) noexcept {
    if (!prepared_ || source != WebAssetStore::kUploadPath || destination != WebAssetStore::kActivePath)
        return failure("candidate-not-prepared-or-path");
    const auto status = files_->commit(writer_);
    writer_ = 0;
    prepared_ = false;
    if (!status) return status;
    files_->close(active_);
    active_ = {};
    using_factory_ = false;
    return core::Status::success();
}

core::Status AutomaticWebBackend::remove(std::string_view path) noexcept {
    if (path != WebAssetStore::kUploadPath) return failure("reserved-active-file");
    abort_write();
    return core::Status::success();
}
} // namespace blip::storage
