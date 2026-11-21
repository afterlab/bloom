#include <bloom/runtime/font_resolver.hpp>
#include <bloom/runtime/headless_font_transport.hpp>

#include <iostream>
#include <utility>

namespace bloom::runtime::fonts {

namespace {

bool endDownloads(FontResolver& resolver, const std::chrono::milliseconds deadline) {
    if (resolver.stopDownloads(deadline))
        return true;
    std::cerr << "A font download did not stop within " << deadline.count()
              << " ms of being cancelled; it ends with the program.\n";
    return false;
}

} // namespace

void installHeadlessFontTransport() {
    auto& resolver = processFontResolver();
    if (resolver.hasTransport())
        return;
    if (auto transport = makeHeadlessFontTransport())
        resolver.setTransport(std::move(transport));
}

bool endHeadlessFontDownloads(const std::chrono::milliseconds deadline) {
    return endDownloads(processFontResolver(), deadline);
}

HeadlessFontDownloads::HeadlessFontDownloads()
    : resolver_(processFontResolver()), deadline_(kEndFontDownloadsDeadline) {
    installHeadlessFontTransport();
}

HeadlessFontDownloads::HeadlessFontDownloads(FontResolver& resolver,
                                             std::shared_ptr<FontTransport> transport,
                                             const std::chrono::milliseconds deadline)
    : resolver_(resolver), deadline_(deadline) {
    if (transport)
        resolver_.setTransport(std::move(transport));
}

HeadlessFontDownloads::~HeadlessFontDownloads() {
    static_cast<void>(endDownloads(resolver_, deadline_));
}

} // namespace bloom::runtime::fonts
