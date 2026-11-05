#include <bloom/ui/viewer_preview_pool.hpp>

#include <bloom/ui/composition_preview_controller.hpp>

#include <algorithm>
#include <utility>

namespace bloom::ui {

ViewerPreviewPool::ViewerPreviewPool(Factory factory, QObject* parent)
    : QObject(parent), factory_(std::move(factory)) {}

ViewerPreviewPool::~ViewerPreviewPool() = default;

std::unique_ptr<CompositionPreviewController> ViewerPreviewPool::create() {
    if (shuttingDown_ || !factory_)
        return nullptr;
    auto controller = factory_();
    if (controller == nullptr)
        return nullptr;
    std::erase_if(live_, [](const auto& entry) { return entry.isNull(); });
    live_.emplace_back(controller.get());
    return controller;
}

std::size_t ViewerPreviewPool::liveCount() const {
    return static_cast<std::size_t>(
        std::ranges::count_if(live_, [](const auto& entry) { return !entry.isNull(); }));
}

void ViewerPreviewPool::beginShutdown() {
    shuttingDown_ = true;
    for (const auto& controller : live_)
        if (controller != nullptr)
            controller->beginShutdown();
}

void ViewerPreviewPool::releasePublishedFramesForShutdown() {
    for (const auto& controller : live_)
        if (controller != nullptr)
            controller->releasePublishedFramesForShutdown();
}

} // namespace bloom::ui
