#include <bloom/runtime/font_resolver.hpp>
#include <bloom/runtime/headless_font_transport.hpp>

namespace bloom::runtime::fonts {

void installHeadlessFontTransport() {
    auto& resolver = processFontResolver();
    if (resolver.hasTransport())
        return;
    if (auto transport = makeHeadlessFontTransport())
        resolver.setTransport(std::move(transport));
}

} // namespace bloom::runtime::fonts
