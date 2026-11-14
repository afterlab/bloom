#include <bloom/ui/ram_preview_pipeline.hpp>

#include <utility>

namespace bloom::ui {

bool RamPreviewPipeline::isInFlight(const PreviewFrameCacheKey& key) const {
    for (const auto& slot : slots_) {
        if (PreviewFrameCacheKey::forIdentity(slot.identity) == key) {
            return true;
        }
    }
    return false;
}

void RamPreviewPipeline::add(InFlight slot) {
    slots_.push_back(std::move(slot));
    if (slots_.size() > peakInFlight_) {
        peakInFlight_ = static_cast<std::uint32_t>(slots_.size());
    }
}

std::vector<RamPreviewPipeline::Ready> RamPreviewPipeline::takeReady() {
    std::vector<Ready> ready;
    for (std::size_t index = 0; index < slots_.size();) {
        auto result = slots_[index].handle.tryTakeResult();
        if (!result.has_value()) {
            ++index;
            continue;
        }
        ready.push_back(Ready{.frameIndex = slots_[index].frameIndex,
                              .identity = slots_[index].identity,
                              .result = std::move(*result)});
        slots_.erase(slots_.begin() + static_cast<std::ptrdiff_t>(index));
    }
    return ready;
}

std::size_t RamPreviewPipeline::cancelIf(const std::function<bool(const InFlight&)>& stale) {
    std::size_t cancelled = 0;
    for (auto it = slots_.begin(); it != slots_.end();) {
        if (stale(*it)) {
            if (detachObserver_)
                detachObserver_(it->identity);
            it->handle.cancel();
            it = slots_.erase(it);
            ++cancelled;
        } else {
            ++it;
        }
    }
    return cancelled;
}

void RamPreviewPipeline::cancelAllAndDetach() noexcept {
    for (auto& slot : slots_) {
        if (detachObserver_) {
            try {
                detachObserver_(slot.identity);
            } catch (...) {
            }
        }
        slot.handle.cancel();
    }
    slots_.clear();
}

} // namespace bloom::ui
