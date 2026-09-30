#include "blip/led/playback.hpp"

#include <algorithm>

namespace blip::led {
namespace {
constexpr std::uint32_t kMagic = 0x42504c42U; // BLPB, little endian.
[[nodiscard]] std::uint16_t read16(std::span<const std::byte> input, std::size_t at) noexcept {
    return std::to_integer<std::uint8_t>(input[at]) |
           static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(input[at + 1U])) << 8U;
}
[[nodiscard]] std::uint32_t read32(std::span<const std::byte> input, std::size_t at) noexcept {
    std::uint32_t value{};
    for (std::size_t byte = 0; byte < 4U; ++byte)
        value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(input[at + byte]))
                 << (byte * 8U);
    return value;
}
void write16(std::span<std::byte> output, std::size_t at, std::uint16_t value) noexcept {
    output[at] = static_cast<std::byte>(value & 0xffU);
    output[at + 1U] = static_cast<std::byte>(value >> 8U);
}
void write32(std::span<std::byte> output, std::size_t at, std::uint32_t value) noexcept {
    for (std::size_t byte = 0; byte < 4U; ++byte)
        output[at + byte] = static_cast<std::byte>(value >> (byte * 8U));
}
[[nodiscard]] std::uint32_t crc32(std::span<const std::byte> input) noexcept {
    std::uint32_t crc = 0xffffffffU;
    for (const auto byte : input) {
        crc ^= std::to_integer<std::uint8_t>(byte);
        for (std::uint8_t bit = 0; bit < 8U; ++bit)
            crc = (crc >> 1U) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}
[[nodiscard]] core::Status error(core::ErrorCode code, std::string_view detail) noexcept {
    return core::Status::failure(
        {core::ErrorDomain::storage, code, "blip.led.playback", "read", detail});
}
[[nodiscard]] std::uint32_t parse_fps(std::string_view json) noexcept {
    const auto key = json.find("\"fps\"");
    if (key == std::string_view::npos)
        return 0U;
    auto cursor = json.find(':', key + 5U);
    if (cursor == std::string_view::npos)
        return 0U;
    ++cursor;
    while (cursor < json.size() && (json[cursor] == ' ' || json[cursor] == '\t'))
        ++cursor;
    std::uint32_t integer{};
    bool any{};
    while (cursor < json.size() && json[cursor] >= '0' && json[cursor] <= '9') {
        any = true;
        integer = integer * 10U + static_cast<std::uint32_t>(json[cursor++] - '0');
        if (integer > 1000U)
            return 0U;
    }
    if (!any || integer == 0U)
        return 0U;
    std::uint32_t fraction{};
    std::uint32_t scale{100U};
    if (cursor < json.size() && json[cursor] == '.') {
        ++cursor;
        while (cursor < json.size() && json[cursor] >= '0' && json[cursor] <= '9' && scale > 0U) {
            fraction += static_cast<std::uint32_t>(json[cursor++] - '0') * scale;
            scale /= 10U;
        }
    }
    return integer * 1000U + fraction;
}
} // namespace

core::Status PlaybackReader::open(std::span<const std::byte> file) noexcept {
    file_ = {};
    if (file.size() < kPlaybackHeaderBytes || read32(file, 0U) != kMagic ||
        read16(file, 4U) != kPlaybackVersion || read16(file, 6U) != kPlaybackHeaderBytes)
        return error(core::ErrorCode::incompatible_version, "invalid-header");
    PlaybackInfo candidate{read16(file, 8U), read32(file, 12U), read32(file, 16U),
                           static_cast<PixelFormat>(std::to_integer<std::uint8_t>(file[10U])),
                           read32(file, 20U)};
    if (candidate.pixel_count == 0U || candidate.frame_count == 0U || candidate.fps_milli == 0U ||
        candidate.format != PixelFormat::rgba16 ||
        candidate.data_bytes != candidate.pixel_count * candidate.frame_count * 8ULL ||
        file.size() != kPlaybackHeaderBytes + candidate.data_bytes ||
        read32(file, 24U) != crc32(file.subspan(kPlaybackHeaderBytes)))
        return error(core::ErrorCode::corrupt_data, "invalid-size-or-crc");
    auto header = file.first(kPlaybackHeaderBytes);
    std::array<std::byte, kPlaybackHeaderBytes> copy{};
    std::copy(header.begin(), header.end(), copy.begin());
    std::fill(copy.begin() + 28, copy.begin() + 32, std::byte{0});
    if (read32(file, 28U) != crc32(copy))
        return error(core::ErrorCode::corrupt_data, "invalid-header-crc");
    info_ = candidate;
    file_ = file;
    return core::Status::success();
}

core::Status PlaybackReader::read_frame(std::uint32_t index,
                                        std::span<LinearPixel> output) const noexcept {
    if (!open() || index >= info_.frame_count || output.size() < info_.pixel_count)
        return error(core::ErrorCode::invalid_argument, "frame-out-of-range");
    const auto frame = file_.subspan(kPlaybackHeaderBytes +
                                         static_cast<std::size_t>(index) * info_.pixel_count * 8U,
                                     static_cast<std::size_t>(info_.pixel_count) * 8U);
    for (std::size_t pixel = 0; pixel < info_.pixel_count; ++pixel) {
        const auto at = pixel * 8U;
        const auto big = [&](std::size_t offset) {
            return static_cast<std::uint16_t>(
                (static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(frame[at + offset]))
                 << 8U) |
                std::to_integer<std::uint8_t>(frame[at + offset + 1U]));
        };
        output[pixel] = {big(0U), big(2U), big(4U), 0U, big(6U)};
    }
    return core::Status::success();
}

core::Result<std::size_t> import_legacy_playback(std::string_view metadata_json,
                                                 std::span<const std::byte> argb_frames,
                                                 std::uint16_t pixel_count,
                                                 std::span<std::byte> output) noexcept {
    const auto fps = parse_fps(metadata_json);
    const auto legacy_frame_bytes = static_cast<std::size_t>(pixel_count) * 4U;
    if (fps == 0U || pixel_count == 0U || argb_frames.empty() ||
        argb_frames.size() % legacy_frame_bytes != 0U) {
        return core::Result<std::size_t>::failure(
            error(core::ErrorCode::corrupt_data, "invalid-legacy-pair").error());
    }
    const auto frames = static_cast<std::uint64_t>(argb_frames.size() / legacy_frame_bytes);
    const auto data_bytes_wide = frames * pixel_count * 8ULL;
    if (frames > 0xffffffffULL || data_bytes_wide > 0xffffffffULL) {
        return core::Result<std::size_t>::failure(
            error(core::ErrorCode::capacity_exceeded, "legacy-file-too-large").error());
    }
    const auto data_bytes = static_cast<std::size_t>(data_bytes_wide);
    if (data_bytes > output.size() || kPlaybackHeaderBytes > output.size() - data_bytes)
        return core::Result<std::size_t>::failure(
            error(core::ErrorCode::serialization_overflow, "output-too-small").error());
    std::fill(output.begin(), output.begin() + kPlaybackHeaderBytes, std::byte{0});
    write32(output, 0U, kMagic);
    write16(output, 4U, kPlaybackVersion);
    write16(output, 6U, kPlaybackHeaderBytes);
    write16(output, 8U, pixel_count);
    output[10U] = static_cast<std::byte>(PixelFormat::rgba16);
    write32(output, 12U, static_cast<std::uint32_t>(frames));
    write32(output, 16U, fps);
    write32(output, 20U, static_cast<std::uint32_t>(data_bytes));
    auto data = output.subspan(kPlaybackHeaderBytes, data_bytes);
    for (std::size_t pixel = 0; pixel < frames * pixel_count; ++pixel) {
        const auto source = pixel * 4U;
        const auto target = pixel * 8U;
        const std::array<std::uint8_t, 4> rgba{
            std::to_integer<std::uint8_t>(argb_frames[source + 1U]),
            std::to_integer<std::uint8_t>(argb_frames[source + 2U]),
            std::to_integer<std::uint8_t>(argb_frames[source + 3U]),
            std::to_integer<std::uint8_t>(argb_frames[source])};
        for (std::size_t channel = 0; channel < 4U; ++channel) {
            data[target + channel * 2U] = static_cast<std::byte>(rgba[channel]);
            data[target + channel * 2U + 1U] = static_cast<std::byte>(rgba[channel]);
        }
    }
    write32(output, 24U, crc32(data));
    std::array<std::byte, kPlaybackHeaderBytes> header{};
    std::copy_n(output.begin(), kPlaybackHeaderBytes, header.begin());
    write32(output, 28U, crc32(header));
    return core::Result<std::size_t>::success(kPlaybackHeaderBytes + data_bytes);
}

void PlaybackClock::play(std::uint64_t now_us, std::uint32_t frame, bool loop) noexcept {
    state_ = PlaybackState::playing;
    origin_us_ = now_us;
    origin_frame_ = frame;
    paused_elapsed_us_ = 0U;
    loop_ = loop;
}
void PlaybackClock::pause(std::uint64_t now_us, std::uint32_t) noexcept {
    if (state_ == PlaybackState::playing) {
        paused_elapsed_us_ = now_us - origin_us_;
        state_ = PlaybackState::paused;
    }
}
void PlaybackClock::resume(std::uint64_t now_us) noexcept {
    if (state_ == PlaybackState::paused) {
        origin_us_ = now_us - paused_elapsed_us_;
        state_ = PlaybackState::playing;
    }
}
void PlaybackClock::stop() noexcept {
    state_ = PlaybackState::stopped;
    origin_frame_ = 0U;
    paused_elapsed_us_ = 0U;
}
core::Status PlaybackClock::seek(std::uint32_t frame_value, std::uint32_t frame_count,
                                 std::uint64_t now_us) noexcept {
    if (frame_value >= frame_count)
        return error(core::ErrorCode::invalid_argument, "seek-out-of-range");
    origin_frame_ = frame_value;
    origin_us_ = now_us;
    paused_elapsed_us_ = 0U;
    return core::Status::success();
}
std::uint32_t PlaybackClock::frame(std::uint64_t now_us, std::uint32_t fps_milli,
                                   std::uint32_t frame_count) noexcept {
    if (state_ == PlaybackState::stopped || frame_count == 0U || fps_milli == 0U)
        return origin_frame_;
    const auto elapsed = state_ == PlaybackState::paused ? paused_elapsed_us_ : now_us - origin_us_;
    auto result = origin_frame_ + static_cast<std::uint32_t>((elapsed * fps_milli) / 1000000000ULL);
    if (result >= frame_count) {
        if (loop_)
            result %= frame_count;
        else {
            result = frame_count - 1U;
            state_ = PlaybackState::stopped;
        }
    }
    return result;
}

} // namespace blip::led
