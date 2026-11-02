#include "product_names.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iterator>
#include <regex>
#include <string>

namespace bloom::quality {
namespace {
constexpr auto npos = std::string_view::npos;

// Text that is someone else's or owned elsewhere: vendored third-party sources, the node catalogue
// the owner edits, and the benchmark guide, whose media generator is an external program it has to
// name on the command line.
bool exempt(const std::string& path) {
    return path.starts_with("third_party/") || path.find("/third_party/") != std::string::npos ||
           (path.starts_with("docs/product/node-catalogue") && path.ends_with(".md")) ||
           path == "docs/development/benchmarks.md";
}
// TEMPORARY: files that in-flight engine lanes are changing, left unreworded to avoid rebase
// churn (the evaluation/media lanes, and Lane C #412 for colour). Their product names are reworded
// and these entries removed once those lanes land; nothing may be added here.
constexpr auto kDeferredUntilEngineLands = std::to_array<std::string_view>({
    "docs/architecture/color-management.md",
    "docs/architecture/evaluation-primitives.md",
    "docs/architecture/layer-graph-model.md",
    "docs/user-guide/color-management.md",
    "src/color/ocio_builtin_registry.cpp",
});
bool deferred(const std::string& path) {
    return std::ranges::find(kDeferredUntilEngineLands, path) != kDeferredUntilEngineLands.end();
}
bool documentation(const std::string& path) {
    return path == "README.md" || (path.starts_with("docs/") && path.ends_with(".md"));
}
bool source(const std::string& path) {
    return path.starts_with("src/") && (path.ends_with(".cpp") || path.ends_with(".hpp"));
}

// The source with everything except comment text and string-literal contents blanked. Newlines
// stay, so an offset keeps its line; identifiers and other code can never produce a finding.
std::string commentsAndStrings(std::string_view text) {
    std::string kept(text.size(), ' ');
    for (std::size_t i = 0; i < text.size(); ++i)
        if (text[i] == '\n')
            kept[i] = '\n';
    const auto keep = [&](std::size_t from, const std::size_t to) {
        for (; from < to && from < text.size(); ++from)
            kept[from] = text[from];
    };
    // Past a quoted literal's closing quote, honouring backslash escapes.
    const auto quoted = [&](std::size_t at, const char quote) {
        while (at < text.size() && text[at] != quote && text[at] != '\n')
            at += text[at] == '\\' ? 2 : 1;
        return at;
    };
    std::size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        const char next = i + 1 < text.size() ? text[i + 1] : '\0';
        if (c == '/' && next == '/') {
            const auto end = std::min(text.find('\n', i), text.size());
            keep(i + 2, end);
            i = end;
        } else if (c == '/' && next == '*') {
            const auto close = text.find("*/", i + 2);
            keep(i + 2, close == npos ? text.size() : close);
            i = close == npos ? text.size() : close + 2;
        } else if (c == '"' && i > 0 && text[i - 1] == 'R' && text.find('(', i) != npos &&
                   text.find('(', i) - i <= 17) {
            const auto open = text.find('(', i);
            const auto terminator = ")" + std::string(text.substr(i + 1, open - i - 1)) + "\"";
            const auto close = text.find(terminator, open + 1);
            keep(open + 1, close == npos ? text.size() : close);
            i = close == npos ? text.size() : close + terminator.size();
        } else if (c == '"') {
            const auto end = quoted(i + 1, '"');
            keep(i + 1, end);
            i = end + 1;
        } else if (c == '\'' &&
                   !(i > 0 && std::isxdigit(static_cast<unsigned char>(text[i - 1])))) {
            // A character literal; after a digit the quote is a digit separator instead.
            i = quoted(i + 1, '\'') + 1;
        } else {
            ++i;
        }
    }
    return kept;
}

std::string read(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
} // namespace

std::vector<RepositoryFinding> productNameViolations(const std::filesystem::path& path,
                                                     std::string_view text) {
    std::vector<RepositoryFinding> findings;
    const auto generic = path.generic_string();
    if (exempt(generic) || deferred(generic) || (!documentation(generic) && !source(generic)))
        return findings;
    const auto searched = source(generic) ? commentsAndStrings(text) : std::string(text);
    // Proper names, matched case-sensitively so ordinary words ("resolve", "premiere", "nuke")
    // stay free. DaVinci covers its Resolve; Fusion is checked apart because Qt has a style of
    // that name.
    static const std::regex products(
        R"(\b(?:After\s+Effects|AE|Blender|Nuke|Photoshop|Premiere|DaVinci|Illustrator|Adobe|Blackmagic|Autodesk|Maya|Houdini|Cinema\s+4D|Final\s+Cut|Figma|Fusion)\b)");
    static const std::regex qtStyle(R"(Qt|[Ss]tyle)");
    for (std::sregex_iterator it(searched.begin(), searched.end(), products), end; it != end;
         ++it) {
        const auto offset = static_cast<std::size_t>(it->position());
        // One name however the copy wraps it.
        const auto name = std::regex_replace(it->str(), std::regex(R"(\s+)"), " ");
        if (name == "Fusion") {
            const auto begin = text.rfind('\n', offset);
            const auto start = begin == npos ? 0 : begin + 1;
            const auto line = std::string(text.substr(start, text.find('\n', offset) - start));
            if (std::regex_search(line, qtStyle))
                continue;
        }
        findings.push_back(
            {path, "product-name",
             "public copy names a commercial product (" + name +
                 "); describe the workflow instead, e.g. \"a layer-based compositor\"",
             1U + static_cast<std::size_t>(std::count(
                      text.begin(), text.begin() + static_cast<std::ptrdiff_t>(offset), '\n'))});
    }
    return findings;
}

std::vector<RepositoryFinding> scanProductNames(const std::filesystem::path& root,
                                                std::span<const std::filesystem::path> files) {
    std::vector<RepositoryFinding> result;
    for (const auto& path : files)
        for (auto& finding : productNameViolations(path, read(root / path)))
            result.push_back(std::move(finding));
    return result;
}
} // namespace bloom::quality
