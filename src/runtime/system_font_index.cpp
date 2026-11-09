#include <bloom/runtime/system_font_index.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <string>

namespace bloom::runtime::fonts {
namespace {

[[nodiscard]] bool sameName(const std::string_view a, const std::string_view b) {
    return a.size() == b.size() &&
           std::ranges::equal(a, b, [](const char x, const char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}

[[nodiscard]] bool isSystem(const platform::FontFace& face) {
    return face.source == platform::FontSource::System;
}

} // namespace

SystemFontIndex::~SystemFontIndex() {
    std::shared_future<platform::FontCatalogue> future;
    {
        std::lock_guard lock(mutex_);
        future = future_;
    }
    if (future.valid())
        future.wait();
}

void SystemFontIndex::startLocked() {
    started_ = true;
    future_ = std::async(std::launch::async,
                         [] { return platform::FontCatalogueProvider{}.enumerate({}, false); })
                  .share();
}

void SystemFontIndex::refresh() {
    std::lock_guard lock(mutex_);
    if (installedFixed_)
        return;
    if (future_.valid() && future_.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return;
    startLocked();
}

bool SystemFontIndex::poll() {
    std::lock_guard lock(mutex_);
    if (!future_.valid() || future_.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return false;
    catalogue_ = std::make_shared<const platform::FontCatalogue>(future_.get());
    future_ = {};
    return true;
}

bool SystemFontIndex::pending() const {
    std::lock_guard lock(mutex_);
    return future_.valid();
}

std::shared_ptr<const platform::FontCatalogue> SystemFontIndex::snapshot() const {
    std::lock_guard lock(mutex_);
    return catalogue_;
}

std::shared_ptr<const platform::FontCatalogue>
SystemFontIndex::snapshotBlocking(const std::function<bool()>& cancelled) {
    {
        std::lock_guard lock(mutex_);
        if (!started_ && !catalogue_)
            startLocked();
    }
    while (true) {
        std::shared_future<platform::FontCatalogue> future;
        {
            std::lock_guard lock(mutex_);
            if (!future_.valid())
                return catalogue_ ? catalogue_
                                  : std::make_shared<const platform::FontCatalogue>();
            future = future_;
        }
        if (future.wait_for(std::chrono::milliseconds(20)) == std::future_status::ready) {
            (void)poll();
            continue;
        }
        if (cancelled && cancelled())
            return snapshot();
    }
}

void SystemFontIndex::install(platform::FontCatalogue catalogue) {
    std::lock_guard lock(mutex_);
    catalogue_ = std::make_shared<const platform::FontCatalogue>(std::move(catalogue));
    future_ = {};
    started_ = true;
    installedFixed_ = true;
}

const platform::FontFace* matchSystemFace(const platform::FontCatalogue& catalogue,
                                          const document::FontId& id) {
    if (!id.postscriptName.empty())
        for (const auto& face : catalogue.faces)
            if (isSystem(face) && sameName(face.postscriptName, id.postscriptName) &&
                sameName(face.family, id.family))
                return &face;
    for (const auto& face : catalogue.faces)
        if (isSystem(face) && sameName(face.family, id.family) && sameName(face.style, id.style))
            return &face;
    for (const auto& face : catalogue.faces)
        if (isSystem(face) && sameName(face.family, id.family) && face.italic == id.italic &&
            face.weight == id.weight)
            return &face;
    return nullptr;
}

const platform::FontFace* closestSystemFace(const platform::FontCatalogue& catalogue,
                                            const std::string_view family,
                                            const std::int32_t weight, const bool italic) {
    const platform::FontFace* best = nullptr;
    for (const auto& face : catalogue.faces) {
        if (!isSystem(face) || !sameName(face.family, family))
            continue;
        const auto score = [&](const platform::FontFace& candidate) {
            return std::abs(candidate.weight - weight) + (candidate.italic == italic ? 0 : 10000);
        };
        if (best == nullptr || score(face) < score(*best))
            best = &face;
    }
    return best;
}

} // namespace bloom::runtime::fonts
