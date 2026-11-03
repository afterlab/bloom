#include <algorithm>
#include <bloom/media/image.hpp>
#include <cctype>
#include <charconv>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

namespace bloom::media {
namespace {
constexpr std::size_t kMaxReportedMemberFailures = 32;
struct NumberedName {
    std::string prefix;
    std::string suffix;
    std::string extension;
    std::uint32_t padding = 0;
    std::int64_t frame = 0;
};
// The frame number is the LAST run of digits anywhere in the stem; whatever surrounds it is the
// member's identity. No naming convention is assumed (owner, 2026-09-15: "detect whatever the
// format of the name is, as long as it detects sequence numbering with whatever pad digits"):
// "shot.0001.png", "shot_0001.png", "shot0001.png", "0001.png" and "shot0001_left.png" are all
// members, and the padding may differ between members. The group is a sequence only when two of
// its members have consecutive frame numbers (#458); see listMembers().
std::optional<NumberedName> numberedName(const std::filesystem::path& path) {
    auto extension = path.extension().string();
    std::string lowered = extension;
    std::ranges::transform(lowered, lowered.begin(), [](const unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (lowered != ".png" && lowered != ".jpg" && lowered != ".jpeg" && lowered != ".exr")
        return {};
    const auto stem = path.stem().string();
    const auto isDigit = [](const char ch) { return ch >= '0' && ch <= '9'; };
    auto end = stem.size();
    while (end > 0 && !isDigit(stem[end - 1]))
        --end;
    if (end == 0)
        return {};
    auto begin = end;
    while (begin > 0 && isDigit(stem[begin - 1]))
        --begin;
    const auto digits = std::string_view(stem).substr(begin, end - begin);
    std::int64_t frame = 0;
    const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), frame);
    if (parsed.ec != std::errc{} || frame < 0)
        return {};
    return NumberedName{stem.substr(0, begin), stem.substr(end), lowered,
                        static_cast<std::uint32_t>(digits.size()), frame};
}
struct ListedMembers {
    std::vector<std::pair<std::int64_t, std::filesystem::path>> members;
    // One line per frame number written by more than one file, naming the file kept.
    std::vector<std::string> duplicates;
    std::size_t duplicateFrames = 0;
    std::string diagnostic;
    bool mixedPadding = false;
    // The group, ignoring any frame range, has two files numbered n and n + 1, or three at one
    // constant step (on twos: 1, 3, 5). A group without (camera files IMG_2041, IMG_2077,
    // IMG_3105) is not a sequence: each is a still.
    bool consecutive = false;
    bool exceeded = false;
    bool cancelled = false;
};
// Which of several files numbered with one frame stands for it: the file picked, then one written
// like the file picked (same padding, same extension spelling), then the first file name.
struct Candidate {
    std::int64_t frame = 0;
    bool picked = false;
    bool writtenAlike = false;
    std::filesystem::path path;
    std::uint32_t padding = 0;
};
bool preferred(const Candidate& left, const Candidate& right) {
    if (left.frame != right.frame)
        return left.frame < right.frame;
    if (left.picked != right.picked)
        return left.picked;
    if (left.writtenAlike != right.writtenAlike)
        return left.writtenAlike;
    return left.path.filename().native() < right.path.filename().native();
}
// Whether `frame` and frames already seen make three frames at one constant step (1, 3, 5 or
// 0010, 0020, 0030): a sequence rendered on twos or threes. Bounded per file so a directory of
// unrelated camera numbers stays linear in practice; frames too large to add safely are skipped.
bool completesStep(const std::unordered_set<std::int64_t>& seen, const std::int64_t frame) {
    constexpr std::size_t kMaxProbe = 1024;
    constexpr std::int64_t kMaxFrame = std::int64_t{1} << 40;
    if (seen.size() > kMaxProbe || frame > kMaxFrame || seen.contains(frame))
        return false;
    for (const auto other : seen) {
        if (other > kMaxFrame)
            continue;
        // `frame` ends the run, starts it, or is its middle.
        if (seen.contains(2 * other - frame) || seen.contains(2 * frame - other) ||
            ((other + frame) % 2 == 0 && seen.contains((other + frame) / 2)))
            return true;
    }
    return false;
}
// Every directory entry numbered like `name`, without opening any of them. `progress` gets the
// running count with a zero total: the total is what this pass discovers. `untilConsecutive`
// stops at the first consecutive pair, for a caller that only asks whether the group is a sequence.
ListedMembers listMembers(const std::filesystem::path& path, const NumberedName& name,
                          const CancelImageWork& cancel, const ImageProgress& progress,
                          const std::optional<SequenceFrameRange>& range,
                          const bool untilConsecutive = false) {
    ListedMembers listed;
    std::error_code error;
    auto directory = path.parent_path();
    if (directory.empty())
        directory = ".";
    const auto pickedName = path.filename().native();
    const auto pickedExtension = path.extension().native();
    std::filesystem::directory_iterator iterator(directory, error);
    if (error) {
        listed.diagnostic = "Sequence directory is unreadable";
        return listed;
    }
    std::vector<Candidate> found;
    // Frame numbers seen until the first consecutive pair; no longer needed after it.
    std::unordered_set<std::int64_t> seen;
    for (const auto end = std::filesystem::directory_iterator{}; iterator != end;
         iterator.increment(error)) {
        if (error) {
            listed.diagnostic = "Sequence directory enumeration failed";
            return listed;
        }
        if (cancel && cancel()) {
            listed.cancelled = true;
            return listed;
        }
        const auto candidate = numberedName(iterator->path());
        if (!candidate || candidate->prefix != name.prefix || candidate->suffix != name.suffix ||
            candidate->extension != name.extension)
            continue;
        if (!listed.consecutive) {
            const auto frame = candidate->frame;
            listed.consecutive =
                (frame > 0 && seen.contains(frame - 1)) ||
                (frame < std::numeric_limits<std::int64_t>::max() && seen.contains(frame + 1)) ||
                completesStep(seen, frame);
            if (listed.consecutive) {
                seen = {};
                if (untilConsecutive)
                    return listed;
            } else {
                seen.insert(frame);
            }
        }
        if (untilConsecutive)
            continue;
        if (range && (candidate->frame < range->first || candidate->frame > range->last))
            continue;
        // Past the limit the listing goes on by name only, to learn whether the group is a
        // sequence at all; a group that is not imports the picked file as a still.
        if (found.size() >= kMaxSequenceEntries) {
            listed.exceeded = true;
            continue;
        }
        found.push_back({candidate->frame, iterator->path().filename().native() == pickedName,
                         candidate->padding == name.padding &&
                             iterator->path().extension().native() == pickedExtension,
                         iterator->path(), candidate->padding});
        if (progress)
            progress(found.size(), 0);
    }
    if (error) {
        listed.diagnostic = "Sequence directory enumeration failed";
        return listed;
    }
    // Several files numbered with one frame ("shot.1.png" and "shot.01.png", or "a.0001.png" and
    // "a.0001.PNG") keep one of them and say which; they never fail the sequence.
    std::ranges::sort(found, preferred);
    for (std::size_t index = 0; index < found.size(); ++index) {
        const auto& candidate = found[index];
        if (index > 0 && found[index - 1].frame == candidate.frame)
            continue;
        auto next = index + 1;
        while (next < found.size() && found[next].frame == candidate.frame)
            ++next;
        if (next > index + 1 && ++listed.duplicateFrames <= kMaxReportedMemberFailures) {
            auto line = "Frame " + std::to_string(candidate.frame) + " has " +
                        std::to_string(next - index) + " files; using " +
                        candidate.path.filename().string() + " and leaving out";
            for (auto other = index + 1; other < next; ++other)
                line += (other == index + 1 ? " " : ", ") + found[other].path.filename().string();
            listed.duplicates.push_back(std::move(line));
        }
        // Mixed padding stays in the sequence: the number is what identifies a frame, the
        // padding is only how it was written. It is reported once so the artist knows.
        listed.mixedPadding = listed.mixedPadding || candidate.padding != name.padding;
        listed.members.emplace_back(candidate.frame, candidate.path);
    }
    return listed;
}
} // namespace
std::optional<std::uint64_t> countSequenceMembers(const std::filesystem::path& path,
                                                  const CancelImageWork& cancel,
                                                  const ImageProgress& progress,
                                                  const std::optional<SequenceFrameRange>& range) {
    try {
        const auto name = numberedName(path);
        if (!name)
            return std::uint64_t{1};
        const auto listed = listMembers(path, *name, cancel, progress, range);
        if (listed.cancelled)
            return std::nullopt;
        // A refused listing still counts as one item; reading reports why. A group that is not a
        // sequence is one still.
        if (!listed.diagnostic.empty() || !listed.consecutive || listed.exceeded)
            return std::uint64_t{1};
        return std::max<std::uint64_t>(1, listed.members.size());
    } catch (const std::exception&) {
        return std::uint64_t{1};
    }
}
std::optional<bool> formsSequence(const std::filesystem::path& path,
                                  const CancelImageWork& cancel) {
    try {
        const auto name = numberedName(path);
        if (!name)
            return false;
        const auto listed = listMembers(path, *name, cancel, {}, std::nullopt, true);
        if (listed.cancelled)
            return std::nullopt;
        // An unreadable directory is left to scanSequence(), which says why.
        return listed.consecutive || !listed.diagnostic.empty();
    } catch (const std::exception&) {
        return true;
    }
}
std::optional<std::string> sequenceKey(const std::filesystem::path& path) {
    const auto name = numberedName(path);
    if (!name)
        return std::nullopt;
    auto directory = path.parent_path();
    if (directory.empty())
        directory = ".";
    std::error_code error;
    auto absolute = std::filesystem::absolute(directory, error);
    if (error)
        absolute = directory;
    const auto folder = absolute.lexically_normal().generic_u8string();
    std::string key(reinterpret_cast<const char*>(folder.data()), folder.size());
    // NUL cannot occur in a path or file name, so the parts cannot run into each other.
    for (const auto* part : {&name->prefix, &name->suffix, &name->extension}) {
        key.push_back('\0');
        key += *part;
    }
    return key;
}
ImageResult<SequenceManifest> scanSequence(const std::filesystem::path& path,
                                           const CancelImageWork& cancel,
                                           const ImageProgress& progress,
                                           const std::optional<SequenceFrameRange>& range) {
    try {
        SequenceManifest manifest;
        const auto name = numberedName(path);
        if (!name)
            return {std::move(manifest), {}};
        manifest.padding = name->padding;
        manifest.pattern =
            name->prefix + std::string(name->padding, '#') + name->suffix + name->extension;
        // Pass 1 lists the members (directory entries only); pass 2 reads and hashes each one,
        // which is the slow part, so progress can report a known total while it runs.
        auto listed = listMembers(path, *name, cancel, progress, range);
        if (listed.cancelled)
            return {{}, {}, true};
        if (!listed.diagnostic.empty())
            return {{}, std::move(listed.diagnostic)};
        // Numbered files without consecutive or constant-step frame numbers are not a sequence: the picked
        // file is a still, as an unnumbered name is.
        if (!listed.consecutive)
            return {SequenceManifest{}, {}};
        if (listed.exceeded)
            return {{}, "Sequence exceeds 100000 matching members"};
        if (range && listed.members.empty())
            return {{}, "No frames in the frame range"};
        if (listed.mixedPadding)
            manifest.diagnostics.emplace_back("Sequence mixes frame-number padding");
        manifest.diagnostics.insert(manifest.diagnostics.end(),
                                    std::make_move_iterator(listed.duplicates.begin()),
                                    std::make_move_iterator(listed.duplicates.end()));
        if (listed.duplicateFrames > kMaxReportedMemberFailures)
            manifest.diagnostics.push_back(
                std::to_string(listed.duplicateFrames - kMaxReportedMemberFailures) +
                " further frames have more than one file");
        const auto total = static_cast<std::uint64_t>(listed.members.size());
        std::uint64_t read = 0;
        if (progress)
            progress(read, total);
        std::size_t failedMembers = 0;
        for (const auto& [frame, member] : listed.members) {
            if (cancel && cancel())
                return {{}, {}, true};
            const auto probe = probeImage(member, cancel);
            if (!probe.value.has_value()) {
                if (probe.cancelled || (cancel && cancel()))
                    return {{}, {}, true};
                // ADR 0026 / #373 batch policy: one unreadable member never aborts the sequence.
                // It is reported and its frame becomes a gap that holds the preceding frame.
                if (++failedMembers <= kMaxReportedMemberFailures)
                    manifest.diagnostics.push_back(
                        "Frame " + std::to_string(frame) +
                        " is unreadable and is treated as a gap: " + probe.diagnostic);
            } else {
                manifest.members.push_back({frame, member, probe.value->contentDigest});
            }
            if (progress)
                progress(++read, total);
        }
        std::ranges::sort(manifest.members, {}, &SequenceMember::frame);
        if (manifest.members.empty())
            return {{}, "Selected sequence member is missing"};
        manifest.first = manifest.members.front().frame;
        manifest.last = manifest.members.back().frame;
        // Keep the legacy per-frame encoding for spans older readers already accepted. A
        // sparse, wider span uses the sorted members as its compact gap intervals instead.
        const auto legacyGapList =
            static_cast<std::uint64_t>(manifest.last) - static_cast<std::uint64_t>(manifest.first) <
            kMaxSequenceEntries;
        for (std::size_t index = 1; index < manifest.members.size(); ++index) {
            if (legacyGapList) {
                for (auto frame = manifest.members[index - 1].frame + 1;
                     frame < manifest.members[index].frame; ++frame) {
                    if (cancel && cancel())
                        return {{}, {}, true};
                    manifest.gaps.push_back(frame);
                }
            }
        }
        if (failedMembers > kMaxReportedMemberFailures)
            manifest.diagnostics.push_back(
                std::to_string(failedMembers - kMaxReportedMemberFailures) +
                " further unreadable frames are treated as gaps");
        if (manifest.hasGaps())
            manifest.diagnostics.emplace_back("Sequence gaps hold the preceding frame");
        return {std::move(manifest), {}};
    } catch (const std::exception&) {
        return {{}, "Sequence scan allocation or I/O failed"};
    }
}
} // namespace bloom::media
