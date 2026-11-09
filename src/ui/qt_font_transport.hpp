#pragma once

// The desktop application's network transport for Google Fonts (#489, docs/architecture/fonts.md):
// a blocking GET over Qt's secure transport, run on the font worker thread, cancellable, with a
// transfer timeout and a size cap. The command-line tools, MCP and Python use the headless transport
// of the runtime instead (headless_font_transport.hpp).

#include <bloom/runtime/font_transport.hpp>

namespace bloom::ui {

class QtFontTransport final : public runtime::fonts::FontTransport {
  public:
    [[nodiscard]] runtime::fonts::FontHttpResult
    get(const std::string& url, std::uint64_t maximumBytes, const std::function<bool()>& cancelled,
        const std::function<void(std::uint64_t, std::uint64_t)>& progress) override;
};

} // namespace bloom::ui
