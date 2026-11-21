// #752: the subtitle operations (docs/architecture/subtitles.md, "Operations"): bloom.layer.add-
// subtitles and the bloom.subtitles.* edits. Cues are mappings {id?, start, end, text, speaker?,
// style?, placement?}; a time is a whole frame of the composition, exact seconds [n, d], or a
// timestamp "HH:MM:SS.mmm" / "HH:MM:SS,mmm" (exact in milliseconds, the form an SRT file shows).
// Files are read by the factory on the client thread (the script, MCP or CLI thread; never the UI
// thread), as one cancellable task, so the command it builds only stores the list it was given.
// write-file writes when it is created and changes nothing in the project.

#include <bloom/commands/operations.hpp>
#include <bloom/commands/subtitles_operations.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/cue_list.hpp>
#include <bloom/document/project.hpp>
#include <bloom/document/subtitles_nodes.hpp>
#include <bloom/scripting/operation_registry.hpp>
#include <bloom/scripting/subtitles_query.hpp>
#include <bloom/subtitles/subtitles.hpp>

#include "operation_registry_parity.hpp"
#include "operation_tasks.hpp"
#include "operation_values.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace bloom::scripting::detail {
namespace {

using parity::fromContext;
using parity::made;
using parity::maybe;
using parity::need;

constexpr std::string_view kTimeForms =
    "a whole frame (an integer), exact seconds [numerator, denominator], or a timestamp text "
    "HH:MM:SS.mmm (HH:MM:SS,mmm also reads)";

[[nodiscard]] OperationCreateResult refuse(const std::string& op, std::string code,
                                           std::string argument, std::string message) {
    return OperationCreateResult(nullptr, OperationDiagnostic{.code = std::move(code),
                                                              .operationId = op,
                                                              .argument = std::move(argument),
                                                              .message = std::move(message)});
}

[[nodiscard]] OperationCreateResult invalid(const std::string& op, std::string argument,
                                            std::string message) {
    return refuse(op, "bloom.scripting.invalid-argument", std::move(argument), std::move(message));
}

// A subtitle limit (docs/architecture/subtitles.md, "Limits"): the refusal names the limit, its
// value and the actual amount, which MCP publishes as diagnostic.limit.
[[nodiscard]] OperationCreateResult overLimit(const std::string& op, std::string argument,
                                              const std::string_view limit,
                                              const std::uint64_t value,
                                              const std::uint64_t actual, std::string message) {
    return OperationCreateResult(
        nullptr, OperationDiagnostic{.code = "bloom.scripting.invalid-argument",
                                     .operationId = op,
                                     .argument = std::move(argument),
                                     .message = std::move(message),
                                     .limit = OperationLimit{.name = std::string(limit),
                                                             .value = value,
                                                             .actual = actual}});
}

[[nodiscard]] OperationCreateResult cancelled(const std::string& op) {
    return refuse(op, "bloom.scripting.cancelled", {}, "The request was cancelled; nothing changed");
}

[[nodiscard]] OperationCreateResult noSource(const std::string& op) {
    return refuse(op, "bloom.scripting.source-unavailable", {},
                  "This client cannot read the project; run the operation through a session (MCP, "
                  "the Script editor or bloom-cli)");
}

[[nodiscard]] const Value* find(const Arguments& arguments, const std::string_view name) {
    const auto found = arguments.find(name);
    return found == arguments.end() ? nullptr : &found->second;
}

// A mapping key as its camelCase name: Python spells members in snake_case ("position_x").
[[nodiscard]] std::string canonicalKey(const std::string_view key) {
    std::string name;
    bool upper = false;
    for (const char letter : key) {
        if (letter == '_') {
            upper = !name.empty();
            continue;
        }
        name += upper && letter >= 'a' && letter <= 'z' ? static_cast<char>(letter - 'a' + 'A')
                                                        : letter;
        upper = false;
    }
    return name;
}

// ---------------------------------------------------------------------------------------------
// Times.

// A time argument after operation_frame_times.cpp turned frame counts into seconds: [n, d] or a
// timestamp string.
[[nodiscard]] std::optional<core::RationalTime> timeOf(const Value& value) {
    if (const auto* text = std::get_if<std::string>(&value.storage))
        return parseCueTimestamp(*text);
    return rationalOf(value);
}

// ---------------------------------------------------------------------------------------------
// Records.

struct Problem final {
    std::string argument;
    std::string message;
};

template <typename T> using Read = std::variant<T, Problem>;

[[nodiscard]] const std::string* textOf(const Value& value) {
    return std::get_if<std::string>(&value.storage);
}

[[nodiscard]] std::optional<core::Color4d> colorOf(const Value& value) {
    if (const auto* color = std::get_if<core::Color4d>(&value.storage))
        return *color;
    const auto* array = std::get_if<ValueArray>(&value.storage);
    if (array == nullptr || (array->size() != 3 && array->size() != 4))
        return std::nullopt;
    std::array<double, 4> parts{0.0, 0.0, 0.0, 1.0};
    for (std::size_t index = 0; index < array->size(); ++index) {
        const auto part = numericOf((*array)[index]);
        if (!part || !std::isfinite(*part) || *part < 0.0 || *part > 1.0)
            return std::nullopt;
        parts.at(index) = *part;
    }
    return core::Color4d{parts[0], parts[1], parts[2], parts[3]};
}

[[nodiscard]] Read<document::CuePlacement> placementOf(const Value& value,
                                                       const std::string& where) {
    const auto* mapping = std::get_if<ValueMapping>(&value.storage);
    if (mapping == nullptr)
        return Problem{where, "expects a placement {anchor?, positionX?, positionY?, lineOffset?, "
                              "width?, textAlign?}"};
    document::CuePlacement placement;
    for (std::size_t index = 0; index < mapping->keys.size(); ++index) {
        const auto key = canonicalKey(mapping->keys[index]);
        const auto& item = mapping->values.at(index);
        const auto fraction = [&](std::optional<double>& into) -> std::optional<Problem> {
            const auto number = numericOf(item);
            if (!number || !std::isfinite(*number) || *number < 0.0 || *number > 1.0)
                return Problem{where, "." + key + " is a fraction of the frame, 0 to 1"};
            into = *number;
            return std::nullopt;
        };
        std::optional<Problem> problem;
        if (key == "anchor") {
            const auto* name = textOf(item);
            placement.anchor = name ? document::cueAnchorFromName(*name) : std::nullopt;
            if (!placement.anchor)
                problem = Problem{where, ".anchor is one of top-left, top, top-right, left, "
                                         "center, right, bottom-left, bottom, bottom-right"};
        } else if (key == "positionX") {
            problem = fraction(placement.positionX);
        } else if (key == "positionY") {
            problem = fraction(placement.positionY);
        } else if (key == "width") {
            problem = fraction(placement.width);
        } else if (key == "lineOffset") {
            const auto lines = integerOf(item);
            if (!lines || *lines < -1000 || *lines > 1000)
                problem = Problem{where, ".lineOffset is a whole number of lines"};
            else
                placement.lineOffset = *lines;
        } else if (key == "textAlign") {
            const auto* name = textOf(item);
            placement.textAlign = name ? document::cueTextAlignFromName(*name) : std::nullopt;
            if (!placement.textAlign)
                problem = Problem{where, ".textAlign is left, center or right"};
        } else {
            problem = Problem{where, "has no member \"" + mapping->keys[index] +
                                         "\"; a placement has anchor, positionX, positionY, "
                                         "lineOffset, width and textAlign"};
        }
        if (problem)
            return *problem;
    }
    return placement;
}

// A cue mapping. `requireTimes` is false for nothing today; every cue names start, end and text.
[[nodiscard]] Read<document::Cue> cueOf(const Value& value, const std::string& where) {
    const auto* mapping = std::get_if<ValueMapping>(&value.storage);
    if (mapping == nullptr)
        return Problem{where, "expects a cue {id?, start, end, text, speaker?, style?, "
                              "placement?}"};
    document::Cue cue;
    bool hasStart = false;
    bool hasEnd = false;
    bool hasText = false;
    for (std::size_t index = 0; index < mapping->keys.size(); ++index) {
        const auto key = canonicalKey(mapping->keys[index]);
        const auto& item = mapping->values.at(index);
        if (key == "id") {
            const auto id = idOf(item);
            if (!id)
                return Problem{where, ".id is a positive cue id"};
            cue.id = *id;
        } else if (key == "start" || key == "end") {
            const auto time = timeOf(item);
            if (!time)
                return Problem{where, "." + key + " is " + std::string(kTimeForms)};
            (key == "start" ? cue.start : cue.end) = *time;
            (key == "start" ? hasStart : hasEnd) = true;
        } else if (key == "text" || key == "speaker" || key == "style") {
            const auto* text = textOf(item);
            if (text == nullptr)
                return Problem{where, "." + key + " is text"};
            if (key == "text") {
                if (text->size() > document::kMaximumCueTextBytes)
                    return Problem{where, "#limit:" + std::to_string(text->size())};
                cue.text = *text;
                hasText = true;
            } else {
                (key == "speaker" ? cue.speaker : cue.style) = *text;
            }
        } else if (key == "placement") {
            auto placement = placementOf(item, where + ".placement");
            if (auto* problem = std::get_if<Problem>(&placement))
                return std::move(*problem);
            cue.placement = std::get<document::CuePlacement>(placement);
        } else {
            return Problem{where, "has no member \"" + mapping->keys[index] +
                                      "\"; a cue has id, start, end, text, speaker, style and "
                                      "placement"};
        }
    }
    if (!hasStart || !hasEnd || !hasText)
        return Problem{where, std::string("names ") + (!hasStart ? "no start"
                                                       : !hasEnd ? "no end"
                                                                 : "no text") +
                                  "; a cue is {start, end, text} at least"};
    return cue;
}

[[nodiscard]] Read<document::CueStyle> styleOf(const Value& value, const std::string& where) {
    const auto* mapping = std::get_if<ValueMapping>(&value.storage);
    if (mapping == nullptr)
        return Problem{where, "expects a style {name, color?, backgroundColor?, outlineColor?, "
                              "outlineWidth?, bold?, italic?, underline?, fontFamily?, "
                              "fontSize?}"};
    document::CueStyle style;
    for (std::size_t index = 0; index < mapping->keys.size(); ++index) {
        const auto key = canonicalKey(mapping->keys[index]);
        const auto& item = mapping->values.at(index);
        if (key == "name" || key == "fontFamily") {
            const auto* text = textOf(item);
            if (text == nullptr)
                return Problem{where, "." + key + " is text"};
            if (key == "name")
                style.name = *text;
            else
                style.fontFamily = *text;
        } else if (key == "color" || key == "backgroundColor" || key == "outlineColor") {
            const auto color = colorOf(item);
            if (!color)
                return Problem{where, "." + key + " is [r, g, b] or [r, g, b, a], each 0 to 1"};
            (key == "color"             ? style.color
             : key == "backgroundColor" ? style.backgroundColor
                                        : style.outlineColor) = *color;
        } else if (key == "outlineWidth" || key == "fontSize") {
            const auto number = numericOf(item);
            if (!number || !std::isfinite(*number) || *number < 0.0 || *number > 1.0)
                return Problem{where, "." + key + " is a fraction of the frame height, 0 to 1"};
            (key == "outlineWidth" ? style.outlineWidth : style.fontSize) = *number;
        } else if (key == "bold" || key == "italic" || key == "underline") {
            const auto* flag = std::get_if<bool>(&item.storage);
            if (flag == nullptr)
                return Problem{where, "." + key + " is true or false"};
            (key == "bold" ? style.bold : key == "italic" ? style.italic : style.underline) = *flag;
        } else {
            return Problem{where, "has no member \"" + mapping->keys[index] + "\""};
        }
    }
    if (style.name.empty())
        return Problem{where, "names no style; a style is {name, ...}"};
    return style;
}

[[nodiscard]] Read<document::CueProvenance> provenanceOf(const Value& value,
                                                         const std::string& where) {
    const auto* mapping = std::get_if<ValueMapping>(&value.storage);
    const auto* kind = mapping != nullptr ? mapping->find("kind") : nullptr;
    const auto* kindName = kind != nullptr ? textOf(*kind) : nullptr;
    if (kindName == nullptr)
        return Problem{where, "expects {kind: \"authored\"} or {kind: \"producer\", name, "
                              "version?, parametersDigest?}"};
    const auto text = [&](const std::string_view name) -> std::string {
        for (std::size_t index = 0; index < mapping->keys.size(); ++index)
            if (canonicalKey(mapping->keys[index]) == name)
                if (const auto* found = textOf(mapping->values.at(index)))
                    return *found;
        return {};
    };
    for (const auto& key : mapping->keys) {
        const auto name = canonicalKey(key);
        if (name != "kind" && name != "name" && name != "version" && name != "parametersDigest")
            return Problem{where, "has no member \"" + key +
                                      "\"; a provenance is authored or a producer (a linked file "
                                      "is set by bloom.subtitles.load with link=true)"};
    }
    if (*kindName == "authored")
        return document::CueProvenance{document::CueAuthoredProvenance{}};
    if (*kindName == "producer") {
        document::CueProducerProvenance producer;
        producer.name = text("name");
        producer.version = text("version");
        producer.parametersDigest = text("parametersDigest");
        return document::CueProvenance{std::move(producer)};
    }
    return Problem{where, ".kind is \"authored\" or \"producer\"; a linked file is set by "
                          "bloom.subtitles.load with link=true"};
}

// The cues of an array argument, with the cue count and text limits.
[[nodiscard]] std::variant<std::vector<document::Cue>, OperationCreateResult>
cuesOf(const std::string& op, const Value* value, const std::string& name) {
    const auto* array = value ? std::get_if<ValueArray>(&value->storage) : nullptr;
    if (array == nullptr)
        return invalid(op, name, "expects a list of cues {id?, start, end, text, speaker?, "
                                 "style?, placement?}");
    if (array->size() > document::kMaximumCues)
        return overLimit(op, name, "subtitleCues", document::kMaximumCues, array->size(),
                         "A list holds at most " + std::to_string(document::kMaximumCues) +
                             " cues; this one has " + std::to_string(array->size()));
    std::vector<document::Cue> cues;
    cues.reserve(array->size());
    for (std::size_t index = 0; index < array->size(); ++index) {
        auto read = cueOf((*array)[index], name + "[" + std::to_string(index) + "]");
        if (auto* problem = std::get_if<Problem>(&read)) {
            if (problem->message.starts_with("#limit:")) {
                const auto actual = std::stoull(problem->message.substr(7));
                return overLimit(op, problem->argument, "subtitleCueTextBytes",
                                 document::kMaximumCueTextBytes, actual,
                                 "A cue's text is at most " +
                                     std::to_string(document::kMaximumCueTextBytes) +
                                     " bytes of UTF-8; cue " + std::to_string(index) + " has " +
                                     std::to_string(actual));
            }
            return invalid(op, problem->argument, problem->message);
        }
        cues.push_back(std::move(std::get<document::Cue>(read)));
    }
    return cues;
}

[[nodiscard]] std::variant<std::vector<document::CueStyle>, OperationCreateResult>
stylesOf(const std::string& op, const Value& value) {
    const auto* array = std::get_if<ValueArray>(&value.storage);
    if (array == nullptr)
        return invalid(op, "styles", "expects a list of styles {name, ...}");
    if (array->size() > document::kMaximumCueStyles)
        return overLimit(op, "styles", "subtitleStyles", document::kMaximumCueStyles,
                         array->size(),
                         "A list names at most " + std::to_string(document::kMaximumCueStyles) +
                             " styles; this one names " + std::to_string(array->size()));
    std::vector<document::CueStyle> styles;
    for (std::size_t index = 0; index < array->size(); ++index) {
        auto read = styleOf((*array)[index], "styles[" + std::to_string(index) + "]");
        if (auto* problem = std::get_if<Problem>(&read))
            return invalid(op, problem->argument, problem->message);
        styles.push_back(std::move(std::get<document::CueStyle>(read)));
    }
    std::ranges::sort(styles, {}, &document::CueStyle::name);
    return styles;
}

[[nodiscard]] std::variant<std::vector<commands::CueSelector>, OperationCreateResult>
selectorsOf(const std::string& op, const Value& value, const std::string& name,
            const bool indices) {
    const auto* array = std::get_if<ValueArray>(&value.storage);
    if (array == nullptr)
        return invalid(op, name, indices ? "expects a list of cue indices (0 is the first cue)"
                                         : "expects a list of cue ids");
    std::vector<commands::CueSelector> selectors;
    for (const auto& item : *array) {
        if (indices) {
            const auto index = integerOf(item);
            if (!index || *index < 0)
                return invalid(op, name, "expects a list of cue indices (0 is the first cue)");
            selectors.emplace_back(commands::CueByIndex{static_cast<std::size_t>(*index)});
        } else {
            const auto id = idOf(item);
            if (!id)
                return invalid(op, name, "expects a list of cue ids");
            selectors.emplace_back(commands::CueById{*id});
        }
    }
    return selectors;
}

[[nodiscard]] std::optional<document::NodeId> nodeOf(const Arguments& args) {
    const auto* value = find(args, "node");
    const auto id = value ? idOf(*value) : std::nullopt;
    return id ? std::optional(document::NodeId::fromRaw(*id)) : std::nullopt;
}

[[nodiscard]] std::optional<document::CompositionId> compositionOf(const Arguments& args) {
    const auto* value = find(args, "composition");
    const auto id = value ? idOf(*value) : std::nullopt;
    return id ? std::optional(document::CompositionId::fromRaw(*id)) : std::nullopt;
}

// ---------------------------------------------------------------------------------------------
// Reading files and text.

struct LoadedList final {
    document::CueListValue cues;
    std::vector<std::string> warnings;
};

struct ReadRequest final {
    const Value* file = nullptr;
    const Value* text = nullptr;
    const Value* format = nullptr;
    const Value* encoding = nullptr;
    bool link = false;
    bool strict = false;
};

[[nodiscard]] std::filesystem::path pathOf(const std::string& text) {
    return std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

// The load's failure as a refusal; a size refusal is the subtitleFileBytes limit.
[[nodiscard]] OperationCreateResult loadRefusal(const std::string& op, const std::string& argument,
                                                const subtitles::LoadFailure& failure,
                                                const std::uint64_t bytes) {
    if (failure.code == subtitles::codes::kCancelled)
        return cancelled(op);
    if (failure.code == subtitles::codes::kLimit && bytes > document::kMaximumSubtitleFileBytes)
        return overLimit(op, argument, "subtitleFileBytes", document::kMaximumSubtitleFileBytes,
                         bytes, failure.message);
    std::string message;
    if (failure.line != 0) {
        message = "line " + std::to_string(failure.line);
        if (failure.column != 0)
            message += ", column " + std::to_string(failure.column);
        message += ": ";
    }
    message += failure.message;
    if (failure.code == subtitles::codes::kStrict && !failure.reports.empty())
        message += " (" + std::to_string(failure.reports.size()) +
                   " report rows; load without strict to see them as warnings)";
    return refuse(op, "bloom.scripting.invalid-argument", argument,
                  "[" + failure.code + "] " + std::move(message));
}

// Reads a file or text into a list, off the calling thread's own work as one cancellable task. An
// absent file and text gives an empty list.
[[nodiscard]] std::variant<LoadedList, OperationCreateResult>
readList(const std::string& op, const ReadRequest& request, const OperationContext& context) {
    subtitles::LoadOptions options;
    options.strict = request.strict;
    options.stop = context.stop;
    if (request.format != nullptr) {
        const auto* name = textOf(*request.format);
        if (name == nullptr)
            return invalid(op, "format", "expects auto, srt, vtt, ttml or ass");
        if (*name != "auto") {
            options.format = subtitles::formatFromName(*name);
            if (!options.format)
                return invalid(op, "format", "expects auto, srt, vtt, ttml or ass, not \"" +
                                                 *name + "\"");
        }
    }
    if (request.encoding != nullptr) {
        const auto* name = textOf(*request.encoding);
        if (name == nullptr)
            return invalid(op, "encoding", "expects an encoding name, such as utf-8 or "
                                           "windows-1252");
        if (*name != "auto")
            options.encoding = *name;
    }
    // Text wins over a file when both are given.
    if (request.text != nullptr) {
        const auto* text = textOf(*request.text);
        if (text == nullptr)
            return invalid(op, "text", "expects the subtitle file's text");
        if (text->size() > document::kMaximumSubtitleFileBytes)
            return overLimit(op, "text", "subtitleFileBytes", document::kMaximumSubtitleFileBytes,
                             text->size(),
                             "The text is " + std::to_string(text->size()) +
                                 " bytes; Bloom reads subtitle text of at most 16777216 bytes "
                                 "(16 MiB)");
        if (request.link)
            return invalid(op, "link", "links a file; text has no file to follow");
        LoadedList loaded;
        auto outcome = runCancellable<subtitles::LoadOutcome>(
            context.stop, "Read Subtitles", runtime::TaskExecutor::Cpu,
            [&](const runtime::CancellationToken&) { return subtitles::load(*text, options); });
        if (outcome.cancelled || !outcome.value)
            return cancelled(op);
        if (const auto* failure = std::get_if<subtitles::LoadFailure>(outcome.value.get()))
            return loadRefusal(op, "text", *failure, text->size());
        auto& result = std::get<subtitles::LoadResult>(*outcome.value);
        for (const auto& report : result.reports)
            loaded.warnings.push_back(subtitles::describe(report));
        loaded.cues = std::move(result.cues);
        return loaded;
    }
    if (request.file == nullptr) {
        if (request.link)
            return invalid(op, "link", "links a file; name it with file");
        return LoadedList{};
    }
    const auto* file = textOf(*request.file);
    if (file == nullptr || file->empty())
        return invalid(op, "file", "expects the subtitle file's absolute path");
    const auto path = pathOf(*file);
    if (!path.is_absolute())
        return invalid(op, "file", "expects an absolute path; \"" + *file + "\" is relative");
    std::filesystem::path projectDirectory;
    if (context.source)
        if (const auto source = context.source())
            projectDirectory = source->assetBaseDirectory;
    const auto stamp = subtitles::statFile(path);
    if (stamp && stamp->size > document::kMaximumSubtitleFileBytes)
        return overLimit(op, "file", "subtitleFileBytes", document::kMaximumSubtitleFileBytes,
                         stamp->size,
                         "The file is " + std::to_string(stamp->size) +
                             " bytes; Bloom reads subtitle files of at most 16777216 bytes "
                             "(16 MiB)");
    auto outcome = runCancellable<subtitles::FileLoadOutcome>(
        context.stop, "Read Subtitles", runtime::TaskExecutor::BlockingIo,
        [&](const runtime::CancellationToken&) {
            return subtitles::loadFile({.path = path,
                                        .projectDirectory = projectDirectory,
                                        .options = options,
                                        .link = request.link});
        });
    if (outcome.cancelled || !outcome.value)
        return cancelled(op);
    if (const auto* failure = std::get_if<subtitles::LoadFailure>(outcome.value.get()))
        return loadRefusal(op, "file", *failure, stamp ? stamp->size : 0);
    auto& result = std::get<subtitles::FileLoad>(*outcome.value);
    return LoadedList{std::move(result.loaded.cues), std::move(result.warnings)};
}

[[nodiscard]] std::optional<bool> flagOf(const Arguments& args, const std::string_view name) {
    const auto* value = find(args, name);
    if (value == nullptr)
        return false;
    const auto* flag = std::get_if<bool>(&value->storage);
    return flag ? std::optional(*flag) : std::nullopt;
}

// The current list of a node in the session's project, for reload and write-file.
[[nodiscard]] std::variant<std::pair<document::CueListValue, std::filesystem::path>,
                           OperationCreateResult>
currentList(const std::string& op, const Arguments& args, const OperationContext& context) {
    const auto composition = compositionOf(args);
    const auto node = nodeOf(args);
    if (!composition)
        return invalid(op, "composition", "expects a positive stable id");
    if (!node)
        return invalid(op, "node", "expects the Subtitles node's id (query:nodes)");
    if (!context.source)
        return noSource(op);
    const auto source = context.source();
    if (!source)
        return noSource(op);
    const auto* found = source->snapshot.project().findComposition(*composition);
    if (found == nullptr)
        return invalid(op, "composition",
                       "Composition " + std::to_string(composition->value()) + " does not exist");
    auto located = commands::locateCueList(*found, *node, true);
    if (const auto* refused = std::get_if<commands::OperationResult>(&located)) {
        const auto& issues = refused->issues;
        return invalid(op, "node", issues.empty() ? std::string("The node has no cue list")
                                                  : issues.front().message);
    }
    return std::pair{*std::get<commands::CueListLocation>(located).list,
                     source->assetBaseDirectory};
}

// ---------------------------------------------------------------------------------------------
// Factories.

OperationCreateResult addSubtitles(const std::string& op, const Arguments& args,
                                   const OperationContext& context) {
    const auto composition = compositionOf(args);
    if (!composition)
        return invalid(op, "composition", "expects a positive stable id");
    std::string name = "Subtitles";
    if (const auto* value = find(args, "name")) {
        const auto* text = textOf(*value);
        if (text == nullptr || text->empty())
            return invalid(op, "name", "expects the layer's name");
        name = *text;
    }
    const auto link = flagOf(args, "link");
    if (!link)
        return invalid(op, "link", "expects true or false");
    const auto* cues = find(args, "cues");
    const auto* file = find(args, "file");
    const auto* text = find(args, "text");
    // One source: cues, else text, else a file.
    if (cues != nullptr) {
        if (*link)
            return invalid(op, "link", "links a file; name it with file");
        auto read = cuesOf(op, cues, "cues");
        if (auto* refused = std::get_if<OperationCreateResult>(&read))
            return std::move(*refused);
        document::CueListValue list;
        list.cues.reserve(std::get<0>(read).size());
        std::uint64_t next = 1;
        for (const auto& cue : std::get<0>(read))
            next = std::max(next, cue.id + 1);
        for (auto& cue : std::get<0>(read)) {
            if (cue.id == 0)
                cue.id = next++;
            list.cues.push_back(std::make_shared<const document::Cue>(std::move(cue)));
        }
        list.nextCueId = next;
        document::canonicalizeCueList(list);
        if (const auto problem = list.problem())
            return invalid(op, "cues", "The cues do not form a valid list: " + *problem);
        return made(std::make_unique<commands::AddSubtitlesLayer>(*composition, std::move(name),
                                                                  std::move(list)));
    }
    auto loaded = readList(op,
                           {.file = file,
                            .text = text,
                            .format = find(args, "format"),
                            .encoding = find(args, "encoding"),
                            .link = *link,
                            .strict = false},
                           context);
    if (auto* refused = std::get_if<OperationCreateResult>(&loaded))
        return std::move(*refused);
    auto& list = std::get<LoadedList>(loaded);
    return made(std::make_unique<commands::AddSubtitlesLayer>(
        *composition, std::move(name), std::move(list.cues), std::move(list.warnings)));
}

OperationCreateResult setCues(const std::string& op, const Arguments& args,
                              const OperationContext&) {
    const auto composition = compositionOf(args);
    const auto node = nodeOf(args);
    if (!composition)
        return invalid(op, "composition", "expects a positive stable id");
    if (!node)
        return invalid(op, "node", "expects the Subtitles node's id (query:nodes)");
    auto cues = cuesOf(op, find(args, "cues"), "cues");
    if (auto* refused = std::get_if<OperationCreateResult>(&cues))
        return std::move(*refused);
    std::optional<std::vector<document::CueStyle>> styles;
    if (const auto* value = find(args, "styles")) {
        auto read = stylesOf(op, *value);
        if (auto* refused = std::get_if<OperationCreateResult>(&read))
            return std::move(*refused);
        styles = std::move(std::get<0>(read));
    }
    std::optional<std::string> language;
    if (const auto* value = find(args, "language")) {
        const auto* text = textOf(*value);
        if (text == nullptr || !document::isWellFormedLanguageTag(*text))
            return invalid(op, "language", "expects a BCP 47 language tag, such as en or pt-BR "
                                           "(und when unknown)");
        language = *text;
    }
    std::optional<document::CueProvenance> provenance;
    if (const auto* value = find(args, "provenance")) {
        auto read = provenanceOf(*value, "provenance");
        if (auto* problem = std::get_if<Problem>(&read))
            return invalid(op, problem->argument, problem->message);
        provenance = std::move(std::get<document::CueProvenance>(read));
    }
    return made(std::make_unique<commands::SetCues>(*composition, *node,
                                                    std::move(std::get<0>(cues)),
                                                    std::move(styles), std::move(language),
                                                    std::move(provenance)));
}

// The members insert-cue and edit-cue take at the top level.
[[nodiscard]] std::variant<commands::CueEdit, OperationCreateResult>
cueEditOf(const std::string& op, const Arguments& args) {
    commands::CueEdit edit;
    for (const std::string_view name : {"start", "end"}) {
        if (const auto* value = find(args, name)) {
            const auto time = timeOf(*value);
            if (!time)
                return invalid(op, std::string(name), "expects " + std::string(kTimeForms));
            (name == "start" ? edit.start : edit.end) = *time;
        }
    }
    for (const std::string_view name : {"text", "speaker", "style"}) {
        if (const auto* value = find(args, name)) {
            const auto* text = textOf(*value);
            if (text == nullptr)
                return invalid(op, std::string(name), "expects text");
            if (name == "text" && text->size() > document::kMaximumCueTextBytes)
                return overLimit(op, "text", "subtitleCueTextBytes",
                                 document::kMaximumCueTextBytes, text->size(),
                                 "A cue's text is at most " +
                                     std::to_string(document::kMaximumCueTextBytes) +
                                     " bytes of UTF-8; this one has " +
                                     std::to_string(text->size()));
            (name == "text" ? edit.text : name == "speaker" ? edit.speaker : edit.style) = *text;
        }
    }
    if (const auto* value = find(args, "placement")) {
        auto read = placementOf(*value, "placement");
        if (auto* problem = std::get_if<Problem>(&read))
            return invalid(op, problem->argument, problem->message);
        edit.placement = std::get<document::CuePlacement>(read);
    }
    return edit;
}

OperationCreateResult insertCue(const std::string& op, const Arguments& args,
                                const OperationContext&) {
    const auto composition = compositionOf(args);
    const auto node = nodeOf(args);
    if (!composition)
        return invalid(op, "composition", "expects a positive stable id");
    if (!node)
        return invalid(op, "node", "expects the Subtitles node's id (query:nodes)");
    auto read = cueEditOf(op, args);
    if (auto* refused = std::get_if<OperationCreateResult>(&read))
        return std::move(*refused);
    auto& edit = std::get<commands::CueEdit>(read);
    if (!edit.start || !edit.end || !edit.text)
        return invalid(op, !edit.start ? "start" : !edit.end ? "end" : "text",
                       "is required: a new cue names start, end and text");
    document::Cue cue;
    cue.start = *edit.start;
    cue.end = *edit.end;
    cue.text = *edit.text;
    cue.speaker = edit.speaker.value_or(std::string());
    cue.style = edit.style.value_or(std::string());
    cue.placement = edit.placement.value_or(document::CuePlacement{});
    return made(std::make_unique<commands::InsertCue>(*composition, *node, std::move(cue)));
}

// `cue` (an id) or `index`, exactly one.
[[nodiscard]] std::variant<commands::CueSelector, OperationCreateResult>
selectorOf(const std::string& op, const Arguments& args) {
    const auto* cue = find(args, "cue");
    const auto* index = find(args, "index");
    // The id wins when both are given.
    if (cue == nullptr && index == nullptr)
        return invalid(op, "cue", "name the cue by its id (cue) or its position (index)");
    if (cue != nullptr) {
        const auto id = idOf(*cue);
        if (!id)
            return invalid(op, "cue", "expects a cue id (query kind=subtitles lists them)");
        return commands::CueSelector{commands::CueById{*id}};
    }
    const auto position = integerOf(*index);
    if (!position || *position < 0)
        return invalid(op, "index", "expects a cue index (0 is the first cue)");
    return commands::CueSelector{commands::CueByIndex{static_cast<std::size_t>(*position)}};
}

OperationCreateResult editCue(const std::string& op, const Arguments& args,
                              const OperationContext&) {
    const auto composition = compositionOf(args);
    const auto node = nodeOf(args);
    if (!composition)
        return invalid(op, "composition", "expects a positive stable id");
    if (!node)
        return invalid(op, "node", "expects the Subtitles node's id (query:nodes)");
    auto selector = selectorOf(op, args);
    if (auto* refused = std::get_if<OperationCreateResult>(&selector))
        return std::move(*refused);
    auto read = cueEditOf(op, args);
    if (auto* refused = std::get_if<OperationCreateResult>(&read))
        return std::move(*refused);
    return made(std::make_unique<commands::EditCue>(*composition, *node,
                                                    std::get<commands::CueSelector>(selector),
                                                    std::move(std::get<commands::CueEdit>(read))));
}

// `cues` (ids) or `indices`; `required` refuses neither.
[[nodiscard]] std::variant<std::optional<std::vector<commands::CueSelector>>,
                           OperationCreateResult>
chosenCues(const std::string& op, const Arguments& args, const bool required) {
    const auto* cues = find(args, "cues");
    const auto* indices = find(args, "indices");
    // Ids win when both are given.
    if (cues == nullptr && indices == nullptr) {
        if (required)
            return invalid(op, "cues", "name the cues by id (cues) or by position (indices)");
        return std::optional<std::vector<commands::CueSelector>>{};
    }
    auto read = selectorsOf(op, cues != nullptr ? *cues : *indices,
                            cues != nullptr ? "cues" : "indices", cues == nullptr);
    if (auto* refused = std::get_if<OperationCreateResult>(&read))
        return std::move(*refused);
    return std::optional(std::move(std::get<0>(read)));
}

OperationCreateResult removeCues(const std::string& op, const Arguments& args,
                                 const OperationContext&) {
    const auto composition = compositionOf(args);
    const auto node = nodeOf(args);
    if (!composition)
        return invalid(op, "composition", "expects a positive stable id");
    if (!node)
        return invalid(op, "node", "expects the Subtitles node's id (query:nodes)");
    auto chosen = chosenCues(op, args, true);
    if (auto* refused = std::get_if<OperationCreateResult>(&chosen))
        return std::move(*refused);
    return made(std::make_unique<commands::RemoveCues>(*composition, *node,
                                                       std::move(*std::get<0>(chosen))));
}

OperationCreateResult shiftCues(const std::string& op, const Arguments& args,
                                const OperationContext&) {
    const auto composition = compositionOf(args);
    const auto node = nodeOf(args);
    if (!composition)
        return invalid(op, "composition", "expects a positive stable id");
    if (!node)
        return invalid(op, "node", "expects the Subtitles node's id (query:nodes)");
    const auto* by = find(args, "by");
    const auto amount = by ? timeOf(*by) : std::nullopt;
    if (!amount)
        return invalid(op, "by", "expects " + std::string(kTimeForms) +
                                     " (negative moves the cues earlier)");
    auto chosen = chosenCues(op, args, false);
    if (auto* refused = std::get_if<OperationCreateResult>(&chosen))
        return std::move(*refused);
    return made(std::make_unique<commands::ShiftCues>(*composition, *node,
                                                      std::move(std::get<0>(chosen)), *amount));
}

OperationCreateResult scaleCues(const std::string& op, const Arguments& args,
                                const OperationContext&) {
    const auto composition = compositionOf(args);
    const auto node = nodeOf(args);
    if (!composition)
        return invalid(op, "composition", "expects a positive stable id");
    if (!node)
        return invalid(op, "node", "expects the Subtitles node's id (query:nodes)");
    const auto* factorValue = find(args, "factor");
    const auto* pair = factorValue ? std::get_if<ValueArray>(&factorValue->storage) : nullptr;
    std::optional<core::RationalTime> factor;
    if (pair != nullptr && pair->size() == 2) {
        const auto numerator = integerOf((*pair)[0]);
        const auto denominator = integerOf((*pair)[1]);
        if (numerator && denominator && *numerator > 0 && *denominator > 0)
            factor = core::RationalTime::create(*numerator, *denominator);
    }
    if (!factor)
        return invalid(op, "factor", "expects a positive ratio [numerator, denominator], such as "
                                     "[1001, 1000] or [25, 24]");
    auto origin = core::RationalTime::fromInteger(0);
    if (const auto* value = find(args, "origin")) {
        const auto parsed = timeOf(*value);
        if (!parsed)
            return invalid(op, "origin", "expects " + std::string(kTimeForms));
        origin = *parsed;
    }
    auto chosen = chosenCues(op, args, false);
    if (auto* refused = std::get_if<OperationCreateResult>(&chosen))
        return std::move(*refused);
    return made(std::make_unique<commands::ScaleCues>(
        *composition, *node, std::move(std::get<0>(chosen)), *factor, origin));
}

OperationCreateResult load(const std::string& op, const Arguments& args,
                           const OperationContext& context) {
    const auto composition = compositionOf(args);
    const auto node = nodeOf(args);
    if (!composition)
        return invalid(op, "composition", "expects a positive stable id");
    if (!node)
        return invalid(op, "node", "expects the Subtitles node's id (query:nodes)");
    const auto link = flagOf(args, "link");
    const auto strict = flagOf(args, "strict");
    if (!link)
        return invalid(op, "link", "expects true or false");
    if (!strict)
        return invalid(op, "strict", "expects true or false");
    const auto* file = find(args, "file");
    const auto* text = find(args, "text");
    if (file == nullptr && text == nullptr)
        return invalid(op, "file", "name a file (its absolute path) or give the text");
    auto loaded = readList(op,
                           {.file = file,
                            .text = text,
                            .format = find(args, "format"),
                            .encoding = find(args, "encoding"),
                            .link = *link,
                            .strict = *strict},
                           context);
    if (auto* refused = std::get_if<OperationCreateResult>(&loaded))
        return std::move(*refused);
    auto& list = std::get<LoadedList>(loaded);
    return made(std::make_unique<commands::ReplaceCues>(
        *composition, *node, std::move(list.cues), commands::ReplaceCues::Kind::Load,
        std::move(list.warnings)));
}

OperationCreateResult reload(const std::string& op, const Arguments& args,
                             const OperationContext& context) {
    auto current = currentList(op, args, context);
    if (auto* refused = std::get_if<OperationCreateResult>(&current))
        return std::move(*refused);
    const auto& [list, projectDirectory] = std::get<0>(current);
    const auto* file = std::get_if<document::CueFileProvenance>(&list.provenance);
    if (file == nullptr)
        return invalid(op, "node",
                       "The cues of node " + std::to_string(nodeOf(args)->value()) +
                           " are not linked to a file; load a file with link=true to follow it");
    const auto path = subtitles::linkedFilePath(*file, projectDirectory);
    const auto pathText = [&] {
        const auto text = path.u8string();
        return std::string(reinterpret_cast<const char*>(text.data()), text.size());
    }();
    const Value fileValue(pathText);
    const Value formatValue(file->format);
    const Value encodingValue(file->encoding);
    auto loaded = readList(op,
                           {.file = &fileValue,
                            .text = nullptr,
                            .format = &formatValue,
                            .encoding = &encodingValue,
                            .link = true,
                            .strict = false},
                           context);
    if (auto* refused = std::get_if<OperationCreateResult>(&loaded))
        return std::move(*refused);
    auto& fresh = std::get<LoadedList>(loaded);
    // The list keeps the locator it was linked with.
    if (auto* provenance = std::get_if<document::CueFileProvenance>(&fresh.cues.provenance)) {
        provenance->portability = file->portability;
        provenance->path = file->path;
    }
    return made(std::make_unique<commands::ReplaceCues>(
        *compositionOf(args), *nodeOf(args), std::move(fresh.cues),
        commands::ReplaceCues::Kind::Reload, std::move(fresh.warnings)));
}

OperationCreateResult unlink(const std::string& op, const Arguments& args,
                             const OperationContext&) {
    const auto composition = compositionOf(args);
    const auto node = nodeOf(args);
    if (!composition)
        return invalid(op, "composition", "expects a positive stable id");
    if (!node)
        return invalid(op, "node", "expects the Subtitles node's id (query:nodes)");
    return made(std::make_unique<commands::UnlinkCues>(*composition, *node));
}

// Changes nothing in the project: the factory has already written the file.
class WroteSubtitleFile final : public commands::Operation {
  public:
    explicit WroteSubtitleFile(std::vector<std::string> warnings)
        : warnings_(std::move(warnings)) {}
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.subtitles.write-file";
    }
    [[nodiscard]] bool renderAffecting() const noexcept override { return false; }
    [[nodiscard]] commands::OperationResult apply(document::Draft&) const override {
        auto result = commands::OperationResult::noChange({});
        for (const auto& warning : warnings_)
            result.issues.push_back({commands::OperationIssueCode::InvalidValue, warning});
        return result;
    }

  private:
    std::vector<std::string> warnings_;
};

OperationCreateResult writeFile(const std::string& op, const Arguments& args,
                                const OperationContext& context) {
    const auto* pathValue = find(args, "path");
    const auto* pathText = pathValue ? textOf(*pathValue) : nullptr;
    if (pathText == nullptr || pathText->empty())
        return invalid(op, "path", "expects the file's absolute path");
    const auto path = pathOf(*pathText);
    if (!path.is_absolute())
        return invalid(op, "path", "expects an absolute path; \"" + *pathText + "\" is relative");
    const auto* formatValue = find(args, "format");
    const auto* formatName = formatValue ? textOf(*formatValue) : nullptr;
    const auto format = formatName ? subtitles::formatFromName(*formatName) : std::nullopt;
    if (!format || !subtitles::isWritableFormat(*format))
        return invalid(op, "format", "expects srt, vtt or ttml");
    const auto overwrite = flagOf(args, "overwrite");
    if (!overwrite)
        return invalid(op, "overwrite", "expects true or false");
    auto current = currentList(op, args, context);
    if (auto* refused = std::get_if<OperationCreateResult>(&current))
        return std::move(*refused);
    const auto& list = std::get<0>(current).first;
    subtitles::WriteOptions options;
    if (const auto source = context.source ? context.source() : std::nullopt) {
        if (const auto* composition =
                source->snapshot.project().findComposition(*compositionOf(args))) {
            const auto rate = composition->format().frameRate();
            options.frameRate = std::pair{rate.numerator(), rate.denominator()};
        }
    }
    auto outcome = runCancellable<std::variant<subtitles::FileWriteResult, subtitles::LoadFailure>>(
        context.stop, "Write Subtitles", runtime::TaskExecutor::BlockingIo,
        [&](const runtime::CancellationToken&) {
            return subtitles::writeFile(list, *format, path, *overwrite, options);
        });
    if (outcome.cancelled || !outcome.value)
        return cancelled(op);
    if (const auto* failure = std::get_if<subtitles::LoadFailure>(outcome.value.get()))
        return invalid(op, "path", failure->message);
    std::vector<std::string> warnings;
    for (const auto& report : std::get<subtitles::FileWriteResult>(*outcome.value).reports)
        warnings.push_back(subtitles::describe(report));
    return made(std::make_unique<WroteSubtitleFile>(std::move(warnings)));
}

// ---------------------------------------------------------------------------------------------
// Schemas.

[[nodiscard]] Value exampleCue(const std::string& start, const std::string& end,
                               const std::string& text) {
    return Value(ValueMapping{.keys = {"start", "end", "text"},
                              .values = {Value(start), Value(end), Value(text)}});
}

[[nodiscard]] ArgumentSchema composition() {
    return fromContext("composition", ValueKind::Id, ContextSource::Composition, StableId{1},
                       "Id. The composition the Subtitles node is in (its frame rate counts "
                       "integer times). Defaults to the current composition.");
}

[[nodiscard]] ArgumentSchema node() {
    return need("node", ValueKind::Id, StableId{2},
                "Id. The Subtitles node (the subtitlesNode output of bloom.layer.add-subtitles, "
                "or query:nodes).");
}

[[nodiscard]] ArgumentSchema time(std::string name, const bool required, Value example,
                                  std::string summary) {
    auto argument = required ? need(std::move(name), ValueKind::Time, std::move(example),
                                    std::move(summary))
                             : maybe(std::move(name), ValueKind::Time, std::move(example),
                                     std::move(summary));
    return argument;
}

[[nodiscard]] ArgumentSchema format(const bool writable) {
    return writable
               ? need("format", ValueKind::String, "srt",
                      "The file's format: srt, vtt (WebVTT) or ttml (IMSC 1.1 Text).")
               : maybe("format", ValueKind::String, "auto",
                       "The file's format: auto (sniffed from the text and the file name, the "
                       "default), srt, vtt, ttml or ass (read only).");
}

[[nodiscard]] ArgumentSchema encoding() {
    return maybe("encoding", ValueKind::String, "auto",
                 "The file's text encoding: auto (a UTF-8 or UTF-16 byte order mark, else UTF-8; "
                 "the default), utf-8, utf-16le, utf-16be, or a WHATWG single-byte encoding label "
                 "such as windows-1252 or iso-8859-2.");
}

[[nodiscard]] ArgumentSchema cueText(const bool required) {
    const std::string summary =
        "The cue's text in WebVTT cue-text markup (<i>, <b>, <u>, <c.class>, <v Speaker>, <lang>, "
        "<ruby>, character references); a line break is \"\\n\". At most 4096 bytes "
        "(subtitleCueTextBytes).";
    return required ? need("text", ValueKind::String, "Hello", summary)
                    : maybe("text", ValueKind::String, "Hello", summary);
}

[[nodiscard]] std::vector<ArgumentSchema> cueMembers(const bool insert) {
    std::vector<ArgumentSchema> arguments;
    arguments.push_back(time("start", insert, std::int64_t{24},
                             "The cue's first time: " + std::string(kTimeForms) + "."));
    arguments.push_back(time("end", insert, std::int64_t{84},
                             "The time the cue stops showing (exclusive): " +
                                 std::string(kTimeForms) + "."));
    arguments.push_back(cueText(insert));
    arguments.push_back(maybe("speaker", ValueKind::String, "Ada",
                              "Who speaks the cue; empty text clears it."));
    arguments.push_back(maybe("style", ValueKind::String, "yellow",
                              "The name of a style of the list (or a WebVTT color class such as "
                              "yellow); empty text clears it."));
    arguments.push_back(maybe(
        "placement", ValueKind::Mapping,
        Value(ValueMapping{.keys = {"anchor"}, .values = {Value("top")}}),
        "Where the cue asks to be drawn: {anchor?, positionX?, positionY?, lineOffset?, width?, "
        "textAlign?}. anchor is top-left, top, top-right, left, center, right, bottom-left, "
        "bottom or bottom-right; positions and width are fractions of the frame; lineOffset is "
        "whole lines from the anchored edge; textAlign is left, center or right. It replaces the "
        "whole placement; {} clears it."));
    return arguments;
}

constexpr std::string_view kCuesSummary =
    "The cues, each {id?, start, end, text, speaker?, style?, placement?}: times are a whole "
    "frame, exact seconds [n, d] or \"HH:MM:SS.mmm\"; text is WebVTT cue-text markup. At most "
    "50000 cues (subtitleCues). A large list goes in {\"fromFile\": \"/abs/cues.json\"}.";

void add(std::vector<OperationDescriptor>& descriptors, std::string id,
         std::vector<ArgumentSchema> arguments,
         OperationCreateResult (*create)(const std::string&, const Arguments&,
                                         const OperationContext&)) {
    descriptors.push_back(
        {.schema = {.typeId = std::move(id), .arguments = std::move(arguments)},
         .factory = [create](const std::string& op,
                             const Arguments& args) { return create(op, args, {}); },
         .contextualFactory = create});
}

// An edit that reads nothing when it is created: its frame counts become seconds when it
// applies, at its composition's rate then (operation_frame_times.cpp).
void addPlain(std::vector<OperationDescriptor>& descriptors, std::string id,
              std::vector<ArgumentSchema> arguments,
              OperationCreateResult (*create)(const std::string&, const Arguments&,
                                              const OperationContext&)) {
    descriptors.push_back(
        {.schema = {.typeId = std::move(id), .arguments = std::move(arguments)},
         .factory = [create](const std::string& op,
                             const Arguments& args) { return create(op, args, {}); },
         .contextualFactory = {}});
}

} // namespace

void addSubtitlesOperations(std::vector<OperationDescriptor>& descriptors) {
    const auto cuesExample =
        Value(ValueArray{exampleCue("00:00:01.000", "00:00:03.500", "Hello"),
                         exampleCue("00:00:04.000", "00:00:06.000", "<i>World</i>")});

    add(descriptors, "bloom.layer.add-subtitles",
        {composition(),
         maybe("cues", ValueKind::Array, cuesExample, std::string(kCuesSummary), true),
         maybe("file", ValueKind::String, "/subtitles/episode.srt",
               "The absolute path of a subtitle file to read (SRT, WebVTT, TTML/IMSC or ASS; at "
               "most 16 MiB, subtitleFileBytes). Give cues, file or text, or none for an empty "
               "list; given several, cues wins, then text."),
         maybe("text", ValueKind::String, "1\n00:00:01,000 --> 00:00:03,500\nHello\n",
               "A subtitle file's text, read as the file would be."),
         format(false), encoding(),
         maybe("link", ValueKind::Boolean, false,
               "With file: follow the file. The cues are still held in the project; the desktop "
               "application reloads them when the file changes, and headless renders warn."),
         maybe("name", ValueKind::String, "Subtitles", "The layer's name.")},
        addSubtitles);

    addPlain(descriptors, "bloom.subtitles.set-cues",
        {composition(), node(), need("cues", ValueKind::Array, cuesExample, std::string(kCuesSummary)),
         maybe("styles", ValueKind::Array,
               Value(ValueArray{Value(ValueMapping{
                   .keys = {"name", "color", "italic"},
                   .values = {Value("narrator"),
                              Value(ValueArray{Value(1.0), Value(1.0), Value(0.0)}),
                              Value(true)}})}),
               "The list's named styles, each {name, color?, backgroundColor?, outlineColor?, "
               "outlineWidth?, bold?, italic?, underline?, fontFamily?, fontSize?}: colors are "
               "[r, g, b] or [r, g, b, a] display sRGB 0 to 1; sizes are fractions of the frame "
               "height. At most 256 (subtitleStyles). Omitted, the styles stay."),
         maybe("language", ValueKind::String, "en",
               "The list's BCP 47 language tag (und when unknown). Omitted, it stays."),
         maybe("provenance", ValueKind::Mapping,
               Value(ValueMapping{.keys = {"kind"}, .values = {Value("authored")}}),
               "Where the list came from: {kind: \"authored\"} or {kind: \"producer\", name, "
               "version?, parametersDigest?}. A linked file is set by bloom.subtitles.load. "
               "Omitted, it stays.")},
        setCues);

    {
        auto arguments = std::vector<ArgumentSchema>{composition(), node()};
        for (auto& member : cueMembers(true))
            arguments.push_back(std::move(member));
        addPlain(descriptors, "bloom.subtitles.insert-cue", std::move(arguments), insertCue);
    }
    {
        auto arguments = std::vector<ArgumentSchema>{
            composition(), node(),
            maybe("cue", ValueKind::Id, StableId{1},
                  "Id. The cue to edit (query kind=subtitles lists the ids). Give cue or index; "
                  "cue wins when both are given.",
                  true),
            maybe("index", ValueKind::Integer, std::int64_t{0},
                  "The cue's position in the list, 0 first. Give cue or index.")};
        for (auto& member : cueMembers(false))
            arguments.push_back(std::move(member));
        addPlain(descriptors, "bloom.subtitles.edit-cue", std::move(arguments), editCue);
    }
    addPlain(descriptors, "bloom.subtitles.remove-cues",
        {composition(), node(),
         maybe("cues", ValueKind::Array, Value(ValueArray{Value(StableId{1})}),
               "The ids of the cues to remove. Give cues or indices; cues wins when both are "
               "given.", true),
         maybe("indices", ValueKind::Array, Value(ValueArray{Value(std::int64_t{0})}),
               "The positions of the cues to remove, 0 first. Give cues or indices.")},
        removeCues);
    addPlain(descriptors, "bloom.subtitles.shift-cues",
        {composition(), node(),
         time("by", true, Value(ValueArray{Value(std::int64_t{-1}), Value(std::int64_t{2})}),
              "How far to move the cues: " + std::string(kTimeForms) +
                  "; negative is earlier. A cue may not start before 0."),
         maybe("cues", ValueKind::Array, Value(ValueArray{Value(StableId{1})}),
               "The ids of the cues to move. Omitted, every cue moves."),
         maybe("indices", ValueKind::Array, Value(ValueArray{Value(std::int64_t{0})}),
               "The positions of the cues to move, instead of cues.")},
        shiftCues);
    addPlain(descriptors, "bloom.subtitles.scale-cues",
        {composition(), node(),
         need("factor", ValueKind::Array,
              Value(ValueArray{Value(std::int64_t{1001}), Value(std::int64_t{1000})}),
              "The exact positive ratio [numerator, denominator] times are scaled by about "
              "origin: [25, 24] retimes 24 fps cues for 25 fps playback of the same frames; "
              "[1001, 1000] converts 24 to 23.976."),
         time("origin", false, std::int64_t{0},
              "The time that stays put: " + std::string(kTimeForms) + ". Defaults to 0."),
         maybe("cues", ValueKind::Array, Value(ValueArray{Value(StableId{1})}),
               "The ids of the cues to scale. Omitted, every cue is scaled."),
         maybe("indices", ValueKind::Array, Value(ValueArray{Value(std::int64_t{0})}),
               "The positions of the cues to scale, instead of cues.")},
        scaleCues);
    add(descriptors, "bloom.subtitles.load",
        {composition(), node(),
         maybe("file", ValueKind::String, "/subtitles/episode.srt",
               "The absolute path of the file to read (at most 16 MiB, subtitleFileBytes). Give "
               "file or text; text wins when both are given."),
         maybe("text", ValueKind::String, "1\n00:00:01,000 --> 00:00:03,500\nHello\n",
               "The file's text, instead of a file.", true),
         format(false), encoding(),
         maybe("link", ValueKind::Boolean, false,
               "Follow the file: the list records its path, digest and stamp, edits other than "
               "reload and unlink are refused, and the desktop application reloads it when the "
               "file changes."),
         maybe("strict", ValueKind::Boolean, false,
               "Refuse the whole load when the reader would skip a cue or drop or approximate a "
               "feature, instead of reporting each as a warning.")},
        load);
    add(descriptors, "bloom.subtitles.reload", {composition(), node()}, reload);
    addPlain(descriptors, "bloom.subtitles.unlink", {composition(), node()}, unlink);
    add(descriptors, "bloom.subtitles.write-file",
        {composition(), node(),
         need("path", ValueKind::String, "/deliveries/episode.srt",
              "The absolute path of the file to write. It is written atomically: a temporary file "
              "in the same folder is renamed over it."),
         format(true),
         maybe("overwrite", ValueKind::Boolean, false,
               "Replace an existing file. Defaults to false, which refuses one.")},
        writeFile);
}

} // namespace bloom::scripting::detail
