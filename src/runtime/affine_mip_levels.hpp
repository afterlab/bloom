#pragma once

// Private to src/runtime. #742: the CPU reference's mip levels for a filtered (Trilinear or
// Anisotropic) composed-matrix resample. Level n + 1 is the Bilinear composed-matrix resample of
// level n through render::affineMipReductionMatrix(), the same resample the GPU route's level
// commands run, so both routes read the same levels bit for bit. Levels are built only when the
// resolved plan reads one (the map shrinks the image), only up to the plan's upper level, row by
// row with cancellation checked per row, and every level is charged against the caller's pixel
// budget before it is allocated. Nothing partial is published: a refusal or cancellation returns no
// levels.

#include <bloom/render/gpu_affine.hpp>
#include <bloom/render/image.hpp>
#include <bloom/runtime/cancellation.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace bloom::runtime::detail {

class AffineMipLevels final {
  public:
    enum class Outcome : std::uint8_t { Built, OverBudget, Cancelled, Invalid };

    // Levels 1..plan.upperLevel of `source` (none for a plan that reads only level 0 or is not
    // filtered). `pixelBudget` bounds the bytes of every level together; `used` receives them.
    [[nodiscard]] static Outcome build(const render::Rgba32fImageView source,
                                       const render::GpuAffineFilterPlan& plan,
                                       const std::size_t pixelBudget,
                                       const CancellationToken& cancellation,
                                       AffineMipLevels& out) {
        out = AffineMipLevels{};
        out.source_ = source;
        if (!plan.filtered() || plan.upperLevel == 0)
            return Outcome::Built;
        const auto descriptor = source.descriptor();
        if (!descriptor)
            return Outcome::Invalid;
        const auto reduction = render::affineMipReductionMatrix();
        std::size_t used = 0;
        std::vector<render::Rgba32fImage> levels;
        levels.reserve(plan.upperLevel);
        for (std::uint32_t level = 1; level <= plan.upperLevel; ++level) {
            const auto previous = level == 1 ? std::optional{source} : [&]() {
                const auto view = levels.back().view();
                return view ? std::optional{*view.value()} : std::nullopt;
            }();
            if (!previous)
                return Outcome::Invalid;
            const auto [width, height] =
                render::affineMipLevelExtent(descriptor->dataWindow().extent().width(),
                                             descriptor->dataWindow().extent().height(), level);
            const auto window = render::ImageWindow::create(0, 0, width, height);
            if (!window)
                return Outcome::Invalid;
            const std::size_t bytes =
                static_cast<std::size_t>(width) * height * sizeof(render::Rgba32f);
            if (bytes > pixelBudget - used)
                return Outcome::OverBudget;
            const auto levelDescriptor = render::Rgba32fImageDescriptor::create(
                *window.value(), descriptor->displayWindow(), descriptor->pixelAspect());
            if (!levelDescriptor)
                return Outcome::Invalid;
            auto builder =
                render::Rgba32fImageBuilder::create(*levelDescriptor.value(), pixelBudget - used);
            if (!builder)
                return Outcome::OverBudget;
            for (std::int64_t y = 0; y < height; ++y) {
                if (cancellation.isCancellationRequested())
                    return Outcome::Cancelled;
                const auto row = builder.value()->row(y);
                if (!row || render::affineMatrixResampleRow(
                                *previous, reduction, 1.0F, render::GpuAffineFilter::Bilinear,
                                *window.value(), *window.value(), y, *row.value()))
                    return Outcome::Invalid;
            }
            auto frozen = std::move(*builder.value()).freeze();
            if (!frozen)
                return Outcome::Invalid;
            used += bytes;
            levels.push_back(std::move(*frozen.value()));
        }
        out.levels_ = std::move(levels);
        out.bytes_ = used;
        return Outcome::Built;
    }

    // The image of `level` (0 is the source).
    [[nodiscard]] std::optional<render::Rgba32fImageView> level(const std::uint32_t index) const {
        if (index == 0)
            return source_;
        if (index > levels_.size())
            return std::nullopt;
        const auto view = levels_[index - 1].view();
        return view ? std::optional{*view.value()} : std::nullopt;
    }
    [[nodiscard]] std::size_t bytes() const noexcept { return bytes_; }

  private:
    std::optional<render::Rgba32fImageView> source_;
    std::vector<render::Rgba32fImage> levels_;
    std::size_t bytes_ = 0;
};

} // namespace bloom::runtime::detail
