// #752: the cue list value, docs/architecture/subtitles.md "The cue value".
#include <bloom/document/cue_list.hpp>

#include <bloom/core/int128.hpp>
#include <bloom/core/utf8.hpp>
#include <bloom/document/cue_markup.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <utility>

namespace bloom::document {
namespace {

constexpr std::array<std::string_view, kCueAnchorCount> kAnchorNames{
    "top-left",    "top",    "top-right",    "left",        "center",
    "right",       "bottom-left", "bottom", "bottom-right"};
constexpr std::array<std::string_view, 3> kTextAlignNames{"left", "center", "right"};

struct BuiltInClass {
    std::string_view name;
    std::uint8_t red;
    std::uint8_t green;
    std::uint8_t blue;
};
// WebVTT's default classes (W3C WebVTT, "Default text colour" and "Default text background").
constexpr std::array<BuiltInClass, 8> kBuiltInClasses{{{"black", 0x00, 0x00, 0x00},
                                                       {"blue", 0x00, 0x00, 0xFF},
                                                       {"cyan", 0x00, 0xFF, 0xFF},
                                                       {"lime", 0x00, 0xFF, 0x00},
                                                       {"magenta", 0xFF, 0x00, 0xFF},
                                                       {"red", 0xFF, 0x00, 0x00},
                                                       {"white", 0xFF, 0xFF, 0xFF},
                                                       {"yellow", 0xFF, 0xFF, 0x00}}};

[[nodiscard]] bool validName(const std::string_view name) {
    return !name.empty() && name.size() <= kMaximumCueNameBytes && core::isValidUtf8(name) &&
           std::ranges::none_of(name, [](const char c) {
               return static_cast<unsigned char>(c) < 0x20 || c == 0x7F;
           });
}

[[nodiscard]] bool validColor(const core::Color4d& color) {
    const auto unit = [](const double value) {
        return std::isfinite(value) && value >= 0.0 && value <= 1.0;
    };
    return unit(color.red) && unit(color.green) && unit(color.blue) && unit(color.alpha);
}

[[nodiscard]] bool validFraction(const std::optional<double>& value, const double low,
                                 const double high) {
    return !value || (std::isfinite(*value) && *value >= low && *value <= high);
}

[[nodiscard]] std::optional<std::string> styleProblem(const CueStyle& style) {
    if (!validName(style.name))
        return "a style name is 1 to " + std::to_string(kMaximumCueNameBytes) +
               " bytes of UTF-8 text with no control characters";
    const auto where = " of style \"" + style.name + "\"";
    for (const auto* color : {&style.color, &style.backgroundColor, &style.outlineColor})
        if (*color && !validColor(**color))
            return "every color" + where + " has four components in 0..1";
    if (!validFraction(style.fontSize, 1.0e-4, 1.0))
        return "the font size" + where + " is a fraction of the frame height in (0, 1]";
    if (!validFraction(style.outlineWidth, 0.0, 0.25))
        return "the outline width" + where + " is a fraction of the frame height in 0..0.25";
    if (style.fontFamily &&
        (style.fontFamily->empty() || !validName(*style.fontFamily)))
        return "the font family" + where + " is 1 to " + std::to_string(kMaximumCueNameBytes) +
               " bytes of text";
    return std::nullopt;
}

[[nodiscard]] std::optional<std::string> placementProblem(const CuePlacement& placement) {
    if (!validFraction(placement.positionX, -1.0, 2.0) ||
        !validFraction(placement.positionY, -1.0, 2.0))
        return std::string("a placement position is a fraction of the frame in -1..2");
    if (!validFraction(placement.width, 0.0, 1.0) || (placement.width && *placement.width <= 0.0))
        return std::string("a placement width is a fraction of the frame width in (0, 1]");
    if (placement.lineOffset && (*placement.lineOffset < -1000 || *placement.lineOffset > 1000))
        return std::string("a placement line offset is -1000 to 1000 lines");
    return std::nullopt;
}

[[nodiscard]] std::string idText(const Cue& cue) { return "cue " + std::to_string(cue.id); }

} // namespace

std::string_view cueAnchorName(const CueAnchor anchor) noexcept {
    const auto index = static_cast<std::size_t>(anchor);
    return index < kAnchorNames.size() ? kAnchorNames[index] : std::string_view("bottom");
}

std::optional<CueAnchor> cueAnchorFromName(const std::string_view name) noexcept {
    for (std::size_t index = 0; index < kAnchorNames.size(); ++index)
        if (kAnchorNames[index] == name)
            return static_cast<CueAnchor>(index);
    return std::nullopt;
}

std::string_view cueTextAlignName(const CueTextAlign align) noexcept {
    const auto index = static_cast<std::size_t>(align);
    return index < kTextAlignNames.size() ? kTextAlignNames[index] : std::string_view("center");
}

std::optional<CueTextAlign> cueTextAlignFromName(const std::string_view name) noexcept {
    for (std::size_t index = 0; index < kTextAlignNames.size(); ++index)
        if (kTextAlignNames[index] == name)
            return static_cast<CueTextAlign>(index);
    return std::nullopt;
}

bool cueOrderLess(const Cue& left, const Cue& right) noexcept {
    if (left.start != right.start)
        return left.start < right.start;
    if (left.end != right.end)
        return left.end < right.end;
    return left.id < right.id;
}

void canonicalizeCueList(CueListValue& list) {
    std::ranges::stable_sort(list.cues, [](const CueHandle& left, const CueHandle& right) {
        return cueOrderLess(*left, *right);
    });
    std::ranges::stable_sort(list.styles, [](const CueStyle& left, const CueStyle& right) {
        return core::compareUtf8Bytes(left.name, right.name) < 0;
    });
}

bool isBuiltInCueStyleName(const std::string_view name) noexcept {
    const auto base = name.starts_with("bg_") ? name.substr(3) : name;
    return std::ranges::any_of(kBuiltInClasses,
                               [&](const BuiltInClass& entry) { return entry.name == base; });
}

std::optional<core::Color4d> builtInCueClassColor(const std::string_view name) noexcept {
    const auto base = name.starts_with("bg_") ? name.substr(3) : name;
    for (const auto& entry : kBuiltInClasses)
        if (entry.name == base)
            return core::Color4d{entry.red / 255.0, entry.green / 255.0, entry.blue / 255.0, 1.0};
    return std::nullopt;
}

bool isWellFormedLanguageTag(const std::string_view tag) noexcept {
    if (tag.empty() || tag.size() > kMaximumCueLanguageBytes)
        return false;
    std::size_t start = 0;
    bool first = true;
    while (start <= tag.size()) {
        const auto end = std::min(tag.find('-', start), tag.size());
        const auto part = tag.substr(start, end - start);
        if (part.empty() || part.size() > 8)
            return false;
        for (const char c : part) {
            const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
            const bool digit = c >= '0' && c <= '9';
            if (!letter && !(digit && !first))
                return false;
        }
        if (first && part.size() < 2)
            return false;
        first = false;
        if (end == tag.size())
            break;
        start = end + 1;
    }
    return true;
}

std::optional<std::string> cueTextProblem(const std::string_view text) {
    if (text.size() > kMaximumCueTextBytes)
        return "the text is " + std::to_string(text.size()) + " bytes; a cue holds at most " +
               std::to_string(kMaximumCueTextBytes);
    if (!core::isValidUtf8(text))
        return std::string("the text is not UTF-8");
    std::size_t lines = 1;
    for (std::size_t offset = 0; offset < text.size(); ++offset) {
        const auto byte = static_cast<unsigned char>(text[offset]);
        if (byte == '\n') {
            ++lines;
        } else if ((byte < 0x20 && byte != '\t') || byte == 0x7F) {
            return "the text holds the control character U+00" +
                   std::string(1, "0123456789ABCDEF"[byte >> 4]) +
                   std::string(1, "0123456789ABCDEF"[byte & 0xF]) + " at byte " +
                   std::to_string(offset) + "; only tab and line feed are allowed";
        }
    }
    if (lines > kMaximumCueLines)
        return "the text has " + std::to_string(lines) + " lines; a cue holds at most " +
               std::to_string(kMaximumCueLines);
    if (auto markup = cue_markup::strictProblem(text))
        return markup;
    return std::nullopt;
}

CueOverlap maximumCueOverlap(const CueListValue& list) {
    // A sweep over every start and end; an end at the same time as a start comes first, because
    // the end is exclusive.
    std::vector<std::pair<core::RationalTime, int>> events;
    events.reserve(list.cues.size() * 2);
    for (const auto& cue : list.cues) {
        events.emplace_back(cue->start, 1);
        events.emplace_back(cue->end, -1);
    }
    std::ranges::sort(events, [](const auto& left, const auto& right) {
        if (left.first != right.first)
            return left.first < right.first;
        return left.second < right.second;
    });
    CueOverlap result;
    std::size_t active = 0;
    for (const auto& [time, delta] : events) {
        if (delta < 0) {
            --active;
            continue;
        }
        ++active;
        if (active > result.count) {
            result.count = active;
            result.at = time;
        }
    }
    return result;
}

std::optional<std::string> CueListValue::problem() const {
    if (!isWellFormedLanguageTag(language))
        return "the language \"" + language + "\" is not a BCP 47 language tag";
    if (cues.size() > kMaximumCues)
        return "the list holds " + std::to_string(cues.size()) + " cues; a list holds at most " +
               std::to_string(kMaximumCues);
    if (styles.size() > kMaximumCueStyles)
        return "the list holds " + std::to_string(styles.size()) +
               " styles; a list holds at most " + std::to_string(kMaximumCueStyles);
    for (std::size_t index = 0; index < styles.size(); ++index) {
        if (auto problem = styleProblem(styles[index]))
            return problem;
        if (isBuiltInCueStyleName(styles[index].name))
            return "the style \"" + styles[index].name +
                   "\" is a built-in WebVTT class and cannot be redefined";
        if (index > 0 && core::compareUtf8Bytes(styles[index - 1].name, styles[index].name) >= 0)
            return std::string("the styles are not in name order, or two share a name");
    }
    if (const auto* file = std::get_if<CueFileProvenance>(&provenance)) {
        if ((file->portability != "project-relative" && file->portability != "absolute") ||
            file->path.empty() || file->path.size() > kMaximumCueLocatorBytes ||
            !core::isValidUtf8(file->path))
            return std::string("a linked file's locator is a project-relative or absolute path");
        if (file->format != "srt" && file->format != "vtt" && file->format != "ttml" &&
            file->format != "ass")
            return "the linked file's format \"" + file->format + "\" is not srt, vtt, ttml or ass";
        if (file->encoding.empty() || file->encoding.size() > 64)
            return std::string("the linked file's encoding is not named");
        if (file->digest.size() != 7 + 64 || !file->digest.starts_with("sha256:"))
            return std::string("the linked file's digest is \"sha256:\" and 64 hex digits");
    } else if (const auto* producer = std::get_if<CueProducerProvenance>(&provenance)) {
        if (!validName(producer->name) ||
            (!producer->version.empty() && !validName(producer->version)) ||
            producer->parametersDigest.size() > kMaximumCueNameBytes ||
            !core::isValidUtf8(producer->parametersDigest))
            return std::string("a producer is named by 1 to 256 bytes of text, with an optional "
                               "version and parameters digest");
    }
    std::uint64_t highest = 0;
    const Cue* previous = nullptr;
    const auto limit = core::RationalTime::fromInteger(kMaximumCueSeconds);
    for (const auto& handle : cues) {
        if (handle == nullptr)
            return std::string("the list holds an empty cue");
        const auto& cue = *handle;
        if (cue.id == 0)
            return std::string("a cue id is a positive integer");
        highest = std::max(highest, cue.id);
        if (cue.start < core::RationalTime{})
            return idText(cue) + " starts before 0";
        if (!(cue.start < cue.end))
            return idText(cue) + " ends at or before its start";
        if (limit < cue.end)
            return idText(cue) + " ends after " + std::to_string(kMaximumCueSeconds) + " seconds";
        if (auto text = cueTextProblem(cue.text))
            return idText(cue) + ": " + *text;
        if (!cue.speaker.empty() && !validName(cue.speaker))
            return idText(cue) + ": a speaker is 1 to " + std::to_string(kMaximumCueNameBytes) +
                   " bytes of text with no control characters";
        if (!cue.style.empty() && !isBuiltInCueStyleName(cue.style) &&
            findStyle(cue.style) == nullptr)
            return idText(cue) + " names the style \"" + cue.style + "\", which the list lacks";
        if (auto placement = placementProblem(cue.placement))
            return idText(cue) + ": " + *placement;
        if (previous != nullptr) {
            if (previous->id == cue.id)
                return "two cues share the id " + std::to_string(cue.id);
            if (!cueOrderLess(*previous, cue))
                return std::string("the cues are not in (start, end, id) order");
        }
        previous = &cue;
    }
    // Ids are unique (sorted order alone does not prove it across different times).
    {
        std::vector<std::uint64_t> ids;
        ids.reserve(cues.size());
        for (const auto& cue : cues)
            ids.push_back(cue->id);
        std::ranges::sort(ids);
        if (std::ranges::adjacent_find(ids) != ids.end())
            return std::string("two cues share an id");
    }
    if (nextCueId <= highest)
        return "the next cue id " + std::to_string(nextCueId) + " is not above the highest id " +
               std::to_string(highest);
    if (const auto overlap = maximumCueOverlap(*this); overlap.count > kMaximumActiveCues)
        return std::to_string(overlap.count) + " cues show at once at " +
               cueTimestampText(overlap.at) + "; at most " + std::to_string(kMaximumActiveCues) +
               " may";
    return std::nullopt;
}

bool CueListValue::isValid() const { return !problem().has_value(); }

const CueStyle* CueListValue::findStyle(const std::string_view name) const noexcept {
    const auto found = std::ranges::lower_bound(
        styles, name, [](const std::string_view left, const std::string_view right) {
            return core::compareUtf8Bytes(left, right) < 0;
        },
        &CueStyle::name);
    return found != styles.end() && found->name == name ? &*found : nullptr;
}

const Cue* CueListValue::findCue(const std::uint64_t id) const noexcept {
    for (const auto& cue : cues)
        if (cue->id == id)
            return cue.get();
    return nullptr;
}

std::optional<std::size_t> CueListValue::indexOf(const std::uint64_t id) const noexcept {
    for (std::size_t index = 0; index < cues.size(); ++index)
        if (cues[index]->id == id)
            return index;
    return std::nullopt;
}

bool operator==(const CueListValue& left, const CueListValue& right) {
    if (left.language != right.language || left.nextCueId != right.nextCueId ||
        left.provenance != right.provenance || left.styles != right.styles ||
        left.cues.size() != right.cues.size())
        return false;
    for (std::size_t index = 0; index < left.cues.size(); ++index) {
        const auto& a = left.cues[index];
        const auto& b = right.cues[index];
        if (a != b && (a == nullptr || b == nullptr || *a != *b))
            return false;
    }
    return true;
}

std::string cueTimestampText(const core::RationalTime time) {
    if (time < core::RationalTime{})
        return cue_markup::timestampText(0);
    // floor(time * 1000) exactly.
    const auto numerator = core::Int128(time.numerator()) * core::Int128(1000);
    const auto denominator = core::Int128(time.denominator());
    return cue_markup::timestampText(static_cast<std::int64_t>(numerator / denominator));
}

} // namespace bloom::document
