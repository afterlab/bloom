#pragma once

// What the Assets thumbnail worker already learned about a source file, kept between its runs
// (#718). A run observes each file's metadata identity (one stat) and finds the header probe it
// made when it last read that file; it reads a file again only when its identity changed or the
// memo never saw it. Thread-safe: the worker fills it and "Purge media cache" clears it.

#include <bloom/media/image.hpp>
#include <bloom/platform/source_identity.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>

namespace bloom::ui::detail {

class ThumbnailProbeMemo final {
  public:
    static constexpr std::size_t kEntryLimit = 4096;
    // The probe recorded for `path` while it had exactly `identity`.
    [[nodiscard]] std::optional<media::ImageProbe> find(const std::filesystem::path& path,
                                                        const platform::FileIdentity& identity) {
        const std::lock_guard lock(mutex_);
        const auto found = entries_.find(path.generic_string());
        if (found == entries_.end() || !(found->second.identity == identity))
            return std::nullopt;
        found->second.used = ++clock_;
        return found->second.probe;
    }
    void record(const std::filesystem::path& path, const platform::FileIdentity& identity,
                media::ImageProbe probe) {
        const std::lock_guard lock(mutex_);
        if (entries_.size() >= kEntryLimit) {
            auto oldest = entries_.begin();
            for (auto entry = entries_.begin(); entry != entries_.end(); ++entry)
                if (entry->second.used < oldest->second.used)
                    oldest = entry;
            entries_.erase(oldest);
        }
        entries_.insert_or_assign(path.generic_string(),
                                  Entry{identity, std::move(probe), ++clock_});
    }
    void clear() {
        const std::lock_guard lock(mutex_);
        entries_.clear();
    }
    [[nodiscard]] std::size_t size() const {
        const std::lock_guard lock(mutex_);
        return entries_.size();
    }

  private:
    struct Entry final {
        platform::FileIdentity identity;
        media::ImageProbe probe;
        std::uint64_t used = 0;
    };
    mutable std::mutex mutex_;
    std::map<std::string, Entry> entries_;
    std::uint64_t clock_ = 0;
};

} // namespace bloom::ui::detail
