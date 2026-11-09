#pragma once

#include <bloom/document/font_id.hpp>
#include <bloom/platform/font_catalog.hpp>

#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <vector>

namespace bloom::runtime::fonts {

// The installed fonts of this machine, scanned once on a worker through the platform font database
// (fontconfig, CoreText, or the Windows font folders). Lookups need no content digest, so the scan
// does not hash the font files.
class SystemFontIndex final {
  public:
    SystemFontIndex() = default;
    ~SystemFontIndex();
    SystemFontIndex(const SystemFontIndex&) = delete;
    SystemFontIndex& operator=(const SystemFontIndex&) = delete;

    // Starts a scan on a worker unless one is running. The previous snapshot stays usable until the
    // new one is installed by poll().
    void refresh();
    // Installs a finished scan. True when the snapshot changed.
    [[nodiscard]] bool poll();
    [[nodiscard]] bool pending() const;
    // The installed fonts so far (empty before the first scan finishes).
    [[nodiscard]] std::shared_ptr<const platform::FontCatalogue> snapshot() const;
    // Waits for the first scan on the calling thread (a compile or export worker, never the UI
    // thread), starting it when needed, and returns the snapshot. Gives up when `cancelled` says so.
    [[nodiscard]] std::shared_ptr<const platform::FontCatalogue>
    snapshotBlocking(const std::function<bool()>& cancelled = {});
    // Replaces the scan with a fixed catalogue (tests, and a platform without a font database).
    void install(platform::FontCatalogue catalogue);

  private:
    void startLocked();

    mutable std::mutex mutex_;
    std::shared_ptr<const platform::FontCatalogue> catalogue_;
    std::shared_future<platform::FontCatalogue> future_;
    bool started_ = false;
    bool installedFixed_ = false;
};

// The installed face a font ID names: its PostScript name first, then its family and style, then
// the same family with the requested weight and slope. Never a different family or a different
// weight labelled as the requested one.
[[nodiscard]] const platform::FontFace* matchSystemFace(const platform::FontCatalogue& catalogue,
                                                        const document::FontId& id);
// The installed face of a family closest to the wanted weight and slope, for a fallback family.
[[nodiscard]] const platform::FontFace* closestSystemFace(const platform::FontCatalogue& catalogue,
                                                          std::string_view family,
                                                          std::int32_t weight, bool italic);

} // namespace bloom::runtime::fonts
