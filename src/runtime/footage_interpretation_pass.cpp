#include "footage_interpretation_pass.hpp"

#include <bloom/color/ocio_cpu_color_space_processor.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <span>

namespace bloom::runtime::detail {
namespace {
[[nodiscard]] std::optional<render::Rgba32fImageBuilder>
builderFor(const render::Rgba32fImage& image, const std::size_t budget) {
    auto builder = render::Rgba32fImageBuilder::create(*image.descriptor(), budget);
    if (!builder)
        return std::nullopt;
    return std::move(*builder.value());
}
[[nodiscard]] std::string literal(const float value) {
    char text[64];
    const auto end =
        std::to_chars(text, text + sizeof(text), value, std::chars_format::scientific, 9);
    return {text, end.ptr};
}
} // namespace

std::vector<float> FootagePass::parameters() const {
    return {
        static_cast<float>(alpha),     invert ? 1.0F : 0.0F,         matte[0], matte[1], matte[2],
        static_cast<float>(keepField), static_cast<float>(rowParity)};
}

std::string FootagePass::identity() const {
    std::string text = "footage-pass-v1";
    for (const auto value : parameters())
        text += ';' + literal(value);
    return text;
}

std::optional<FootagePass> footagePassFor(const document::AssetRecord& asset,
                                          const media::ImageAlphaAssociation association) {
    const auto& value = asset.interpretation;
    FootagePass pass;
    pass.alpha = value.alphaMode == document::AssetAlphaMode::Ignore ? FootagePass::Alpha::Ignore
                 : association == media::ImageAlphaAssociation::Premultiplied
                     ? FootagePass::Alpha::Premultiplied
                     : FootagePass::Alpha::Straight;
    pass.invert = value.invertAlpha;
    if (pass.alpha == FootagePass::Alpha::Premultiplied)
        for (std::size_t channel = 0; channel < 3; ++channel)
            pass.matte[channel] = static_cast<float>(value.matteColor[channel]);
    const auto order = document::footageFieldOrder(asset);
    if (value.deinterlace && order >= 2 && order <= 5)
        pass.keepField = order == 2 || order == 4 ? 1 : 2;
    const bool needed = pass.alpha == FootagePass::Alpha::Ignore || pass.invert ||
                        pass.matte != std::array<float, 3>{} || pass.keepField != 0;
    return needed ? std::optional{pass} : std::nullopt;
}

std::variant<render::Rgba32fImage, std::string>
applyFootagePass(const FootagePass& pass, const render::Rgba32fImage& stored,
                 const std::size_t budget, const std::function<bool()>& cancelled) {
    if (stored.descriptor() == nullptr)
        return std::string("Footage has no descriptor");
    auto builder = builderFor(stored, budget);
    if (!builder)
        return std::string("Footage exceeds the pixel budget");
    const auto window = stored.descriptor()->dataWindow();
    const auto width = static_cast<std::int64_t>(window.extent().width());
    const auto height = static_cast<std::int64_t>(window.extent().height());
    const auto at = [&](const std::int64_t x, const std::int64_t y) {
        return stored.pixels()[static_cast<std::size_t>(y * width + x)];
    };
    for (std::int64_t y = 0; y < height; ++y) {
        if (cancelled && cancelled())
            return std::string{};
        auto row = builder->row(window.originY() + y);
        if (!row)
            return std::string("Footage row is unavailable");
        // A row of the field that is not kept is the mean of its neighbours (one at an edge).
        const bool interpolate =
            pass.keepField != 0 && (((y + pass.rowParity) & 1) != (pass.keepField == 1 ? 0 : 1));
        std::int64_t up = y - 1;
        std::int64_t down = y + 1;
        if (up < 0)
            up = down;
        if (down >= height)
            down = up;
        const bool blend = interpolate && up >= 0 && up < height;
        for (std::int64_t x = 0; x < width; ++x) {
            std::array<float, 4> sample{};
            if (blend) {
                const auto a = at(x, up);
                const auto b = at(x, down);
                sample = {(a.red() + b.red()) * 0.5F, (a.green() + b.green()) * 0.5F,
                          (a.blue() + b.blue()) * 0.5F, (a.alpha() + b.alpha()) * 0.5F};
            } else {
                const auto p = at(x, y);
                sample = {p.red(), p.green(), p.blue(), p.alpha()};
            }
            float alpha = std::clamp(std::fma(sample[3], 2.0F, -1.0F), 0.0F, 1.0F);
            if (pass.invert)
                alpha = 1.0F - alpha;
            std::array<float, 4> out{};
            switch (pass.alpha) {
            case FootagePass::Alpha::Ignore:
                out = {sample[0], sample[1], sample[2], 1.0F};
                break;
            case FootagePass::Alpha::Straight:
                out = {sample[0] * alpha, sample[1] * alpha, sample[2] * alpha, alpha};
                break;
            case FootagePass::Alpha::Premultiplied: {
                const float cover = -(1.0F - alpha);
                out = {std::fma(cover, pass.matte[0], sample[0]),
                       std::fma(cover, pass.matte[1], sample[1]),
                       std::fma(cover, pass.matte[2], sample[2]), alpha};
                break;
            }
            }
            auto pixel = render::Rgba32f::fromPremultiplied(out[0], out[1], out[2], out[3]);
            if (!pixel)
                return std::string("Footage interpretation produced an invalid pixel");
            (*row.value())[static_cast<std::size_t>(x)] = *pixel.value();
        }
    }
    auto frozen = std::move(*builder).freeze();
    if (!frozen)
        return std::string("Footage interpretation produced an invalid image");
    return std::move(*frozen.value());
}

std::variant<render::Rgba32fImage, std::string>
applyInputProcessorPremultiplied(const render::Rgba32fImage& image,
                                 const color::CpuColorSpaceProcessor& processor,
                                 const std::size_t budget, const std::function<bool()>& cancelled) {
    if (image.descriptor() == nullptr)
        return std::string("Footage has no descriptor");
    auto builder = builderFor(image, budget);
    if (!builder)
        return std::string("Footage exceeds the pixel budget");
    const auto window = image.descriptor()->dataWindow();
    const auto width = static_cast<std::size_t>(window.extent().width());
    std::vector<std::array<float, 4>> row(width);
    for (std::uint64_t y = 0; y < window.extent().height(); ++y) {
        if (cancelled && cancelled())
            return std::string{};
        const auto* source = image.pixels().data() + y * width;
        for (std::size_t x = 0; x < width; ++x) {
            const auto a = source[x].alpha();
            row[x] = a == 0.0F ? std::array<float, 4>{}
                               : std::array<float, 4>{source[x].red() / a, source[x].green() / a,
                                                      source[x].blue() / a, a};
        }
        if (!processor.apply(row))
            return std::string("Input color conversion failed");
        auto target = builder->row(window.originY() + static_cast<std::int64_t>(y));
        if (!target)
            return std::string("Footage row is unavailable");
        for (std::size_t x = 0; x < width; ++x) {
            const auto a = source[x].alpha();
            auto pixel = a == 0.0F ? render::Rgba32f::fromPremultiplied(0, 0, 0, 0)
                                   : render::Rgba32f::fromPremultiplied(
                                         row[x][0] * a, row[x][1] * a, row[x][2] * a, a);
            if (!pixel)
                return std::string("Input color conversion produced an invalid pixel");
            (*target.value())[x] = *pixel.value();
        }
    }
    auto frozen = std::move(*builder).freeze();
    if (!frozen)
        return std::string("Input color conversion produced an invalid image");
    return std::move(*frozen.value());
}

render::OcioGpuProgramResult buildFootagePassProgram(const std::span<const float> parameters) {
    if (parameters.size() != 7 || parameters[0] < 0.0F || parameters[0] > 2.0F ||
        parameters[5] < 0.0F || parameters[5] > 2.0F)
        return render::OcioGpuProgramResult::failure(render::OcioGpuProgramError::InvalidRequest);
    for (const auto value : parameters)
        if (!std::isfinite(value))
            return render::OcioGpuProgramResult::failure(
                render::OcioGpuProgramError::InvalidRequest);
    const auto alpha = static_cast<int>(parameters[0]);
    const bool invert = parameters[1] != 0.0F;
    const auto keep = static_cast<int>(parameters[5]);
    const auto parity = static_cast<int>(parameters[6]) & 1;
    // The CPU reference's arithmetic exactly: fma, one rounding per operation, no reassociation.
    std::string shader = "vec4 bloom_footage_pass(ivec2 c) {\n"
                         " ivec2 size = imageSize(bloom_ocio_input);\n"
                         " precise vec4 s = imageLoad(bloom_ocio_input, c);\n";
    if (keep != 0) {
        shader += " if (((c.y + " + std::to_string(parity) +
                  ") & 1) != " + std::to_string(keep == 1 ? 0 : 1) +
                  ") {\n"
                  "  int up = c.y - 1; int down = c.y + 1;\n"
                  "  if (up < 0) up = down;\n"
                  "  if (down >= size.y) down = up;\n"
                  "  if (up >= 0 && up < size.y) {\n"
                  "   precise vec4 sum = imageLoad(bloom_ocio_input, ivec2(c.x, up)) + "
                  "imageLoad(bloom_ocio_input, ivec2(c.x, down));\n"
                  "   s = sum * 0.5;\n"
                  "  }\n"
                  " }\n";
    }
    shader += " precise float a = clamp(fma(s.a, 2.0, -1.0), 0.0, 1.0);\n";
    if (invert)
        shader += " a = 1.0 - a;\n";
    if (alpha == 0)
        shader += " return vec4(s.rgb, 1.0);\n";
    else if (alpha == 1)
        shader += " return vec4(s.rgb * a, a);\n";
    else
        shader += " precise float cover = -(1.0 - a);\n"
                  " return vec4(fma(cover, " +
                  literal(parameters[2]) + ", s.r), fma(cover, " + literal(parameters[3]) +
                  ", s.g), fma(cover, " + literal(parameters[4]) + ", s.b), a);\n";
    shader += "}\n";
    render::OcioGpuProgramDesc program;
    program.stage = render::OcioGpuProgramStage::ProcessNeighborhood;
    program.functionName = "bloom_footage_pass";
    program.resourcePrefix = "bloom_footage_";
    program.shaderText = std::move(shader);
    program.semanticsId = "bloom.render.footage-interpretation.v1";
    program.shaderTextDigest = render::computeOcioGpuShaderTextDigest(program.shaderText);
    program.resourceDigest = render::computeOcioGpuResourceDigest(program);
    program.contentIdentity = render::computeOcioGpuContentIdentity(
        {program.semanticsId, {}, {}, program.shaderTextDigest, program.resourceDigest});
    return render::OcioGpuProgramResult::success(std::move(program));
}

} // namespace bloom::runtime::detail
