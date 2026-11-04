#pragma once

#include <bloom/core/sha256.hpp>
#include <bloom/document/project.hpp>
#include <bloom/runtime/task_types.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// Verify footage (#495, docs/architecture/project-format.md "Verified Member Digests In Document
// 1.36"). A first use no longer hashes a still or a sequence member; this background pass reads
// each such file once at idle priority so the ADR 0025 memo learns its digest (the next save
// records it into the project) and a file whose bytes differ from its recorded digest is reported.
// Qt-free: the Assets controller runs it on the blocking-I/O worker and shows it in Jobs.
namespace bloom::ui {

struct FootageVerificationItem final {
    std::filesystem::path path;
    // The asset's display name and, for a sequence member, its frame number.
    std::string asset;
    std::optional<std::int64_t> frame;
    // The digest the project recorded for these bytes; absent for a pending member (stamp only).
    std::optional<core::Sha256Digest> recorded;
};

// The files to hash, one per path, in project order: every still image (its digest), and every
// image-sequence member that is pending (stamp only, while its file still carries the stamp) or
// legacy (digest only), plus a verified member whose file no longer carries its stamp. A file the
// memo already knows for its current identity, and a missing file, are skipped; video and every
// other asset kind are never hashed here. Observes each file once (one stat and a memo lookup),
// so it runs on the worker; it stops early when `cancel` returns true.
[[nodiscard]] std::vector<FootageVerificationItem>
footageVerificationWork(const document::Project& project, const std::filesystem::path& directory,
                        const std::function<bool()>& cancel = {});

struct FootageVerificationReport final {
    std::size_t files = 0;
    // Files hashed to the end (and recorded in the memo when their identity held still).
    std::size_t hashed = 0;
    // Hashes that differ from the recorded digest: the bytes changed since they were imported.
    std::size_t changed = 0;
    bool cancelled = false;
    // One Warning per changed file, naming the asset (and frame) with the relink suggestion.
    std::vector<runtime::TaskDiagnostic> diagnostics;
};

using FootageVerificationProgress =
    std::function<void(std::size_t completed, std::size_t total, const std::filesystem::path&)>;

// Hashes every item (platform::hashSourceFile, cancellable mid-file) with the calling thread at
// idle priority (platform::IdleThreadPriorityScope), restored when it returns. A file that is
// missing, unreadable or changing while it is read is skipped; the next pass sees it again.
[[nodiscard]] FootageVerificationReport
verifyFootageFiles(const std::vector<FootageVerificationItem>& items,
                   const std::function<bool()>& cancel = {},
                   const FootageVerificationProgress& progress = {});

} // namespace bloom::ui
