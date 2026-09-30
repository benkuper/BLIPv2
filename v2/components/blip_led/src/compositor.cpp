#include "blip/led/compositor.hpp"

#include <algorithm>

namespace blip::led {
namespace {
[[nodiscard]] constexpr std::size_t index_of(LayerId id) noexcept {
    return static_cast<std::size_t>(id);
}
[[nodiscard]] constexpr std::uint16_t mul(std::uint16_t a, std::uint16_t b) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint32_t>(a) * b + 32767U) / 65535U);
}
[[nodiscard]] constexpr std::uint16_t add_sat(std::uint16_t a, std::uint16_t b) noexcept {
    return static_cast<std::uint16_t>(
        std::min<std::uint32_t>(65535U, static_cast<std::uint32_t>(a) + b));
}
[[nodiscard]] LinearPixel blend_pixel(LinearPixel dst, LinearPixel src, BlendMode mode,
                                      std::uint16_t opacity) noexcept {
    const auto alpha = mul(src.alpha, opacity);
    const auto inverse = static_cast<std::uint16_t>(65535U - alpha);
    const auto over = [=](std::uint16_t d, std::uint16_t s) {
        return add_sat(mul(s, alpha), mul(d, inverse));
    };
    if (mode == BlendMode::replace) {
        const auto replace_inverse = static_cast<std::uint16_t>(65535U - opacity);
        const auto replace = [=](std::uint16_t d, std::uint16_t s) {
            return add_sat(mul(s, opacity), mul(d, replace_inverse));
        };
        src.red = replace(dst.red, src.red);
        src.green = replace(dst.green, src.green);
        src.blue = replace(dst.blue, src.blue);
        src.white = replace(dst.white, src.white);
        src.alpha = replace(dst.alpha, src.alpha);
        return src;
    }
    if (mode == BlendMode::add) {
        dst.red = add_sat(dst.red, mul(src.red, alpha));
        dst.green = add_sat(dst.green, mul(src.green, alpha));
        dst.blue = add_sat(dst.blue, mul(src.blue, alpha));
        dst.white = add_sat(dst.white, mul(src.white, alpha));
        dst.alpha = add_sat(dst.alpha, alpha);
        return dst;
    }
    if (mode == BlendMode::multiply) {
        const auto apply = [=](std::uint16_t d, std::uint16_t s) {
            return mul(d, add_sat(inverse, mul(s, alpha)));
        };
        dst.red = apply(dst.red, src.red);
        dst.green = apply(dst.green, src.green);
        dst.blue = apply(dst.blue, src.blue);
        dst.white = apply(dst.white, src.white);
        dst.alpha = add_sat(alpha, mul(dst.alpha, inverse));
        return dst;
    }
    dst.red = over(dst.red, src.red);
    dst.green = over(dst.green, src.green);
    dst.blue = over(dst.blue, src.blue);
    dst.white = over(dst.white, src.white);
    dst.alpha = add_sat(alpha, mul(dst.alpha, inverse));
    return dst;
}
} // namespace

core::Status Compositor::set_layer(LayerView layer_value) noexcept {
    if (index_of(layer_value.id) >= layers_.size()) {
        return core::Status::failure({core::ErrorDomain::control, core::ErrorCode::invalid_argument,
                                      "blip.led.compositor", "set-layer", "invalid-layer"});
    }
    layers_[index_of(layer_value.id)] = layer_value;
    return core::Status::success();
}

void Compositor::clear_layer(LayerId id) noexcept {
    if (index_of(id) < layers_.size()) {
        layers_[index_of(id)] = LayerView{id};
    }
}

const LayerView& Compositor::layer(LayerId id) const noexcept { return layers_[index_of(id)]; }

core::Status Compositor::compose(std::span<LinearPixel> output) const noexcept {
    std::fill(output.begin(), output.end(), LinearPixel{0U, 0U, 0U, 0U, 0U});
    for (const auto& layer_value : layers_) {
        if (!layer_value.enabled) {
            continue;
        }
        if (layer_value.pixels.size() != output.size()) {
            return core::Status::failure({core::ErrorDomain::control,
                                          core::ErrorCode::validation_failed, "blip.led.compositor",
                                          "compose", "surface-size-mismatch"});
        }
        for (std::size_t pixel = 0; pixel < output.size(); ++pixel) {
            output[pixel] = blend_pixel(output[pixel], layer_value.pixels[pixel], layer_value.blend,
                                        layer_value.opacity);
        }
    }
    return core::Status::success();
}

} // namespace blip::led
