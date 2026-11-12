#pragma once

// The desktop application's network transport for Google Fonts (#489, docs/architecture/fonts.md):
// a blocking GET over Qt's secure transport, run on the font worker thread, cancellable, with a
// stall timeout, a limit on the whole download's time and a size cap. The cap is held by this
// class, not left to Qt: the body is taken from Qt in small pieces as it arrives and the download
// is cut off the moment it passes the cap, so a server that sends too much cannot make Bloom hold
// it in memory. The command-line tools, MCP and Python use the headless transport
// of the runtime instead (headless_font_transport.hpp).

#include <bloom/runtime/font_transport.hpp>

namespace bloom::ui {

class QtFontTransport final : public runtime::fonts::FontTransport {
  public:
    // The longest a whole download may take, in milliseconds.
    static constexpr int kDefaultTotalTimeoutMs = 120000;
    explicit QtFontTransport(int totalTimeoutMs = kDefaultTotalTimeoutMs)
        : totalTimeoutMs_(totalTimeoutMs) {}

    [[nodiscard]] runtime::fonts::FontHttpResult
    get(const std::string& url, std::uint64_t maximumBytes, const std::function<bool()>& cancelled,
        const std::function<void(std::uint64_t, std::uint64_t)>& progress) override;

  private:
    int totalTimeoutMs_;
};

} // namespace bloom::ui
