#pragma once

// Private to src/runtime. #742: the CPU reference's mip levels for a filtered (Trilinear or
// Anisotropic) composed-matrix resample and a filtered field-raster Image read. Level n + 1 is the
// Bilinear composed-matrix resample of level n through render::affineMipReductionMatrix(), the same
// resample the GPU route's level commands run, so both routes read the same levels bit for bit.
// Levels are built only when the resolved plan reads one (the map shrinks the image), only up to
// the deepest level it reads, row by row with cancellation checked per row, and every level is
// charged against the caller's pixel budget before it is used. Nothing partial is published: a
// refusal or cancellation returns no levels.
//
// With a MipLevelCache the levels are retained in the evaluator's operation cache per source image
// identity (the source operation's content key and the level index), as the GPU route's scene cache
// retains its level commands: a warm frame, or a changed map over the same source (a turned layer),
// reads them instead of building them again. A retained level is charged to the frame budget like a
// built one, since the frame holds it while it reads it.

#include "operation_key.hpp"

#include <bloom/render/gpu_affine.hpp>
#include <bloom/render/image.hpp>
#include <bloom/runtime/cancellation.hpp>
#include <bloom/runtime/operation_cache.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace bloom::runtime::detail {

// Where retained levels live: the operation cache and the source's content identity. Disabled
// (nothing retained) without a cache or a key, as for a region-of-interest-dependent source.
struct MipLevelCache final {
    OperationCache* cache = nullptr;
    std::string sourceKey;
    document::Revision revision{};
    LinearPassRetention* linearPass = nullptr;
    [[nodiscard]] bool enabled() const noexcept { return cache != nullptr && !sourceKey.empty(); }
};

class AffineMipLevels final {
  public:
    enum class Outcome : std::uint8_t { Built, OverBudget, Cancelled, Invalid };

    // Levels 1..plan.upperLevel of `source` (none for a plan that reads only level 0 or is not
    // filtered). `pixelBudget` bounds the bytes of every level together.
    [[nodiscard]] static Outcome build(const render::Rgba32fImageView source,
                                       const render::GpuAffineFilterPlan& plan,
                                       const std::size_t pixelBudget,
                                       const CancellationToken& cancellation,
                                       AffineMipLevels& out,
                                       const MipLevelCache* retained = nullptr) {
        return buildLevels(source, plan.filtered() ? plan.upperLevel : 0U, pixelBudget,
                           cancellation, out, retained);
    }

    // Levels 1..count of `source`, the chain stopping at 1x1 (a field raster's filtered read).
    [[nodiscard]] static Outcome buildLevels(const render::Rgba32fImageView source,
                                             const std::uint32_t count,
                                             const std::size_t pixelBudget,
                                             const CancellationToken& cancellation,
                                             AffineMipLevels& out,
                                             const MipLevelCache* retained = nullptr) {
        out = AffineMipLevels{};
        out.source_ = source;
        if (count == 0)
            return Outcome::Built;
        const auto descriptor = source.descriptor();
        if (!descriptor)
            return Outcome::Invalid;
        const auto sourceWidth = descriptor->dataWindow().extent().width();
        const auto sourceHeight = descriptor->dataWindow().extent().height();
        const auto deepest = render::affineMipDeepestLevel(sourceWidth, sourceHeight);
        const auto reduction = render::affineMipReductionMatrix();
        const bool cached = retained != nullptr && retained->enabled();
        std::size_t used = 0;
        std::vector<std::shared_ptr<const render::Rgba32fImage>> levels;
        levels.reserve(count);
        for (std::uint32_t level = 1; level <= count && level <= deepest; ++level) {
            const auto previous = level == 1 ? std::optional{source} : [&]() {
                const auto view = levels.back()->view();
                return view ? std::optional{*view.value()} : std::nullopt;
            }();
            if (!previous)
                return Outcome::Invalid;
            const auto [width, height] =
                render::affineMipLevelExtent(sourceWidth, sourceHeight, level);
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
            OperationKey key;
            if (cached) {
                key.add(std::string{"cpu-mip-level-v1"});
                key.add(retained->sourceKey);
                key.add(sourceWidth);
                key.add(sourceHeight);
                key.add(level);
                if (auto hit = retained->cache->find(key.bytes(), retained->revision);
                    hit && hit->image && hit->image->descriptor() &&
                    *hit->image->descriptor() == *levelDescriptor.value()) {
                    used += bytes;
                    ++out.retainedLevels_;
                    levels.push_back(std::move(hit->image));
                    continue;
                }
            }
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
            auto image = std::make_shared<const render::Rgba32fImage>(std::move(*frozen.value()));
            if (cached) {
                OperationCacheValue value;
                value.image = image;
                retained->cache->store(key.bytes(), retained->revision, std::move(value),
                                       OperationCacheEntryKind::Operation, {},
                                       retained->linearPass);
            }
            levels.push_back(std::move(image));
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
        const auto view = levels_[index - 1]->view();
        return view ? std::optional{*view.value()} : std::nullopt;
    }
    // Level `index` (1..count()) as the image it is.
    [[nodiscard]] const render::Rgba32fImage* image(const std::uint32_t index) const {
        return index >= 1 && index <= levels_.size() ? levels_[index - 1].get() : nullptr;
    }
    [[nodiscard]] std::uint32_t count() const noexcept {
        return static_cast<std::uint32_t>(levels_.size());
    }
    [[nodiscard]] std::size_t bytes() const noexcept { return bytes_; }
    // How many levels came from the operation cache instead of being built.
    [[nodiscard]] std::uint32_t retainedLevels() const noexcept { return retainedLevels_; }

  private:
    std::optional<render::Rgba32fImageView> source_;
    std::vector<std::shared_ptr<const render::Rgba32fImage>> levels_;
    std::size_t bytes_ = 0;
    std::uint32_t retainedLevels_ = 0;
};

} // namespace bloom::runtime::detail
