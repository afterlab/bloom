// #752: the WebVTT parser against the web-platform-tests WebVTT parsing corpus
// (tests/fixtures/subtitles/wpt-webvtt, its PROVENANCE.md and LICENSE.md). Both corpora run:
// file parsing (every .vtt case, each asserted cue property and region relation) and cue text
// parsing (every #data case of every .dat file, the parsed tree printed as the corpus prints it).
// Every case is listed by name when it fails; the counts are checked against the corpus so a case
// cannot drop out silently.

#include <bloom/document/cue_markup.hpp>
#include <bloom/subtitles/subtitles.hpp>
#include <bloom/subtitles/webvtt.hpp>

#include <yyjson.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <span>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace webvtt = bloom::subtitles::webvtt;
namespace markup = bloom::document::cue_markup;

int failures = 0;

void expect(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::string readAll(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream text;
    text << stream.rdbuf();
    return text.str();
}

// ---------------------------------------------------------------------------------------------
// File parsing.

std::string_view directionName(const webvtt::Direction direction) {
    switch (direction) {
    case webvtt::Direction::Horizontal:
        return "";
    case webvtt::Direction::RightToLeft:
        return "rl";
    case webvtt::Direction::LeftToRight:
        return "lr";
    }
    return "?";
}
std::string_view lineAlignName(const webvtt::LineAlign align) {
    switch (align) {
    case webvtt::LineAlign::Start:
        return "start";
    case webvtt::LineAlign::Center:
        return "center";
    case webvtt::LineAlign::End:
        return "end";
    }
    return "?";
}
std::string_view positionAlignName(const webvtt::PositionAlign align) {
    switch (align) {
    case webvtt::PositionAlign::Auto:
        return "auto";
    case webvtt::PositionAlign::LineLeft:
        return "line-left";
    case webvtt::PositionAlign::Center:
        return "center";
    case webvtt::PositionAlign::LineRight:
        return "line-right";
    }
    return "?";
}
std::string_view textAlignName(const webvtt::TextAlign align) {
    switch (align) {
    case webvtt::TextAlign::Start:
        return "start";
    case webvtt::TextAlign::Center:
        return "center";
    case webvtt::TextAlign::End:
        return "end";
    case webvtt::TextAlign::Left:
        return "left";
    case webvtt::TextAlign::Right:
        return "right";
    }
    return "?";
}

std::string stringOf(yyjson_val* value) {
    return yyjson_is_str(value) ? std::string(yyjson_get_str(value), yyjson_get_len(value))
                                : std::string("<not a string>");
}

void checkNumber(const std::string& where, const std::optional<double> actual, yyjson_val* wanted) {
    if (yyjson_is_str(wanted)) {
        expect(stringOf(wanted) == "auto" && !actual, where + " is \"auto\"");
        return;
    }
    const auto expected = yyjson_get_num(wanted);
    expect(actual.has_value() && *actual == expected,
           where + " is " + std::to_string(expected) + ", read " +
               (actual ? std::to_string(*actual) : std::string("auto")));
}

void checkRegion(const std::string& where, const webvtt::File& file, const webvtt::Cue& cue,
                 yyjson_val* wanted) {
    if (yyjson_is_null(wanted)) {
        expect(!cue.region.has_value(), where + " has no region");
        return;
    }
    if (!cue.region.has_value()) {
        expect(false, where + " has a region");
        return;
    }
    const auto& region = file.regions.at(*cue.region);
    yyjson_obj_iter iterator = yyjson_obj_iter_with(wanted);
    yyjson_val* key = nullptr;
    while ((key = yyjson_obj_iter_next(&iterator)) != nullptr) {
        const std::string name = yyjson_get_str(key);
        auto* value = yyjson_obj_iter_get_val(key);
        const auto number = [&](const double actual) {
            expect(actual == yyjson_get_num(value), where + " region " + name + " is " +
                                                        std::to_string(yyjson_get_num(value)) +
                                                        ", read " + std::to_string(actual));
        };
        if (name == "lines")
            number(static_cast<double>(region.lines));
        else if (name == "width")
            number(region.width);
        else if (name == "regionAnchorX")
            number(region.anchorX);
        else if (name == "regionAnchorY")
            number(region.anchorY);
        else if (name == "viewportAnchorX")
            number(region.viewportAnchorX);
        else if (name == "viewportAnchorY")
            number(region.viewportAnchorY);
        else if (name == "scroll")
            expect((region.scrollUp ? "up" : "") == stringOf(value), where + " region scroll");
        else if (name == "id")
            expect(region.id == stringOf(value), where + " region id");
        else
            expect(false, where + " region property " + name + " is not checked by this test");
    }
}

std::size_t runFileParsing(const fs::path& directory) {
    const auto json = readAll(directory / "expected.json");
    yyjson_doc* document = yyjson_read(json.data(), json.size(), 0);
    expect(document != nullptr, "expected.json reads");
    if (document == nullptr)
        return 0;
    std::size_t cases = 0;
    yyjson_val* root = yyjson_doc_get_root(document);
    yyjson_arr_iter entries = yyjson_arr_iter_with(root);
    yyjson_val* entry = nullptr;
    while ((entry = yyjson_arr_iter_next(&entries)) != nullptr) {
        ++cases;
        const auto name = stringOf(yyjson_obj_get(entry, "case"));
        // Decoded as a load decodes it (a byte order mark is not text).
        const auto bytes = readAll(directory / (name + ".vtt"));
        const auto decoded = bloom::subtitles::decodeText(
            std::as_bytes(std::span(bytes.data(), bytes.size())), "");
        const auto* text = std::get_if<bloom::subtitles::DecodedText>(&decoded);
        expect(text != nullptr, name + ": the file decodes");
        const auto file = webvtt::parse(text != nullptr ? text->text : std::string{});
        const auto invalid = yyjson_get_bool(yyjson_obj_get(entry, "signatureInvalid"));
        expect(file.signatureValid == !invalid,
               name + ": the signature is " + (invalid ? "refused" : "accepted"));
        if (auto* count = yyjson_obj_get(entry, "cueCount"))
            expect(file.cues.size() == yyjson_get_uint(count),
                   name + ": " + std::to_string(yyjson_get_uint(count)) + " cues, read " +
                       std::to_string(file.cues.size()));
        auto* cues = yyjson_obj_get(entry, "cues");
        for (std::size_t index = 0; index < yyjson_arr_size(cues); ++index) {
            auto* wanted = yyjson_arr_get(cues, index);
            const auto where = name + " cue " + std::to_string(index);
            if (yyjson_obj_size(wanted) == 0)
                continue;
            if (index >= file.cues.size()) {
                expect(false, where + " exists");
                continue;
            }
            const auto& cue = file.cues[index];
            yyjson_obj_iter iterator = yyjson_obj_iter_with(wanted);
            yyjson_val* key = nullptr;
            while ((key = yyjson_obj_iter_next(&iterator)) != nullptr) {
                const std::string property = yyjson_get_str(key);
                auto* value = yyjson_obj_iter_get_val(key);
                if (property == "id") {
                    expect(cue.id == stringOf(value), where + " id is \"" + stringOf(value) +
                                                          "\", read \"" + cue.id + "\"");
                } else if (property == "text") {
                    expect(cue.text == stringOf(value), where + " text is \"" + stringOf(value) +
                                                            "\", read \"" + cue.text + "\"");
                } else if (property == "startTime" || property == "endTime") {
                    const auto ms = property == "startTime" ? cue.startMilliseconds
                                                            : cue.endMilliseconds;
                    expect(std::abs(static_cast<double>(ms) / 1000.0 - yyjson_get_num(value)) <
                               1e-9,
                           where + " " + property + " is " + std::to_string(yyjson_get_num(value)) +
                               ", read " + std::to_string(ms) + " ms");
                } else if (property == "line") {
                    checkNumber(where + " line", cue.line, value);
                } else if (property == "position") {
                    checkNumber(where + " position", cue.position, value);
                } else if (property == "size") {
                    checkNumber(where + " size", cue.size, value);
                } else if (property == "snapToLines") {
                    expect(cue.snapToLines == yyjson_get_bool(value), where + " snapToLines");
                } else if (property == "vertical") {
                    expect(directionName(cue.vertical) == stringOf(value),
                           where + " vertical is \"" + stringOf(value) + "\"");
                } else if (property == "align") {
                    expect(textAlignName(cue.align) == stringOf(value),
                           where + " align is " + stringOf(value) + ", read " +
                               std::string(textAlignName(cue.align)));
                } else if (property == "lineAlign") {
                    expect(lineAlignName(cue.lineAlign) == stringOf(value),
                           where + " lineAlign is " + stringOf(value));
                } else if (property == "positionAlign") {
                    expect(positionAlignName(cue.positionAlign) == stringOf(value),
                           where + " positionAlign is " + stringOf(value));
                } else if (property == "region") {
                    checkRegion(where, file, cue, value);
                } else if (property == "regionPresent") {
                    expect(cue.region.has_value(), where + " has a region");
                } else if (property == "regionSameAsCue") {
                    const auto other = static_cast<std::size_t>(yyjson_get_uint(value));
                    expect(other < file.cues.size() && cue.region.has_value() &&
                               cue.region == file.cues[other].region,
                           where + " shares cue " + std::to_string(other) + "'s region");
                } else {
                    expect(false, where + " property " + property + " is not checked by this test");
                }
            }
        }
        if (auto* distinct = yyjson_obj_get(entry, "regionsDistinct")) {
            for (std::size_t index = 0; index < yyjson_arr_size(distinct); ++index) {
                auto* pair = yyjson_arr_get(distinct, index);
                const auto a = static_cast<std::size_t>(yyjson_get_uint(yyjson_arr_get(pair, 0)));
                const auto b = static_cast<std::size_t>(yyjson_get_uint(yyjson_arr_get(pair, 1)));
                expect(a < file.cues.size() && b < file.cues.size() &&
                           file.cues[a].region != file.cues[b].region,
                       name + ": cues " + std::to_string(a) + " and " + std::to_string(b) +
                           " have different regions");
            }
        }
        // The one assertion the corpus states about a browser document, not the file model
        // ("stylesheets": a STYLE block adds no document style sheet), has no counterpart here;
        // the file's STYLE blocks are checked instead: they are read as style sheets of the file.
        auto* unconverted = yyjson_obj_get(entry, "unconverted");
        if (yyjson_arr_size(unconverted) > 0)
            expect(name == "stylesheets" && !file.styles.empty(),
                   name + ": an assertion this test does not convert");
    }
    yyjson_doc_free(document);
    return cases;
}

// ---------------------------------------------------------------------------------------------
// Cue text parsing.

// The corpus's escapes: \n, \t, \xHH, \uXXXX and \\ (Python's unicode_escape).
std::string unescape(const std::string_view text) {
    std::string out;
    const auto append = [&](const char32_t point) {
        if (point < 0x80) {
            out.push_back(static_cast<char>(point));
        } else if (point < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (point >> 6)));
            out.push_back(static_cast<char>(0x80 | (point & 0x3F)));
        } else if (point < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (point >> 12)));
            out.push_back(static_cast<char>(0x80 | ((point >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (point & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (point >> 18)));
            out.push_back(static_cast<char>(0x80 | ((point >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((point >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (point & 0x3F)));
        }
    };
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] != '\\' || index + 1 >= text.size()) {
            out.push_back(text[index]);
            continue;
        }
        const char kind = text[++index];
        if (kind == 'n') {
            out.push_back('\n');
        } else if (kind == 't') {
            out.push_back('\t');
        } else if (kind == 'r') {
            out.push_back('\r');
        } else if (kind == '\\') {
            out.push_back('\\');
        } else if (kind == 'x' || kind == 'u') {
            const std::size_t digits = kind == 'x' ? 2 : 4;
            append(static_cast<char32_t>(std::stoul(std::string(text.substr(index + 1, digits)),
                                                    nullptr, 16)));
            index += digits;
        } else {
            out.push_back('\\');
            out.push_back(kind);
        }
    }
    return out;
}

std::u32string codePoints(const std::string_view text) {
    std::u32string out;
    for (std::size_t index = 0; index < text.size();) {
        const auto byte = static_cast<unsigned char>(text[index]);
        char32_t point = byte;
        std::size_t length = 1;
        if (byte >= 0xF0) {
            point = byte & 0x07U;
            length = 4;
        } else if (byte >= 0xE0) {
            point = byte & 0x0FU;
            length = 3;
        } else if (byte >= 0xC0) {
            point = byte & 0x1FU;
            length = 2;
        }
        for (std::size_t more = 1; more < length && index + more < text.size(); ++more)
            point = (point << 6) | (static_cast<unsigned char>(text[index + more]) & 0x3FU);
        out.push_back(point);
        index += length;
    }
    return out;
}

// A string as the corpus prints it: printable ASCII as is (but '"'), '\n' as \n, the rest \uXXXX.
std::string printed(const std::string_view text) {
    std::string out = "\"";
    const auto hex = [&](const unsigned value) {
        static constexpr char kDigits[] = "0123456789ABCDEF";
        out += "\\u";
        for (int shift = 12; shift >= 0; shift -= 4)
            out.push_back(kDigits[(value >> shift) & 0xFU]);
    };
    for (const char32_t point : codePoints(text)) {
        if (point == '\n') {
            out += "\\n";
        } else if (point >= 0x20 && point < 0x7F && point != '"') {
            out.push_back(static_cast<char>(point));
        } else if (point >= 0x10000) {
            const auto value = point - 0x10000;
            hex(0xD800 + (value >> 10));
            hex(0xDC00 + (value & 0x3FF));
        } else {
            hex(point);
        }
    }
    return out + "\"";
}

void printTree(const std::vector<markup::Node>& nodes, const std::size_t depth, std::string& out) {
    const std::string indent = "| " + std::string(depth * 2, ' ');
    for (const auto& node : nodes) {
        std::map<std::string, std::string> attributes;
        std::string element;
        switch (node.kind) {
        case markup::NodeKind::Text:
            out += indent + printed(node.text) + "\n";
            continue;
        case markup::NodeKind::Timestamp:
            out += indent + "<?timestamp " + markup::timestampText(node.milliseconds) + ">\n";
            continue;
        case markup::NodeKind::Class:
            element = "span";
            break;
        case markup::NodeKind::Italic:
            element = "i";
            break;
        case markup::NodeKind::Bold:
            element = "b";
            break;
        case markup::NodeKind::Underline:
            element = "u";
            break;
        case markup::NodeKind::Ruby:
            element = "ruby";
            break;
        case markup::NodeKind::RubyText:
            element = "rt";
            break;
        case markup::NodeKind::Voice:
            element = "span";
            attributes["title"] = node.text;
            break;
        case markup::NodeKind::Language:
            element = "span";
            attributes["lang"] = node.text;
            break;
        }
        if (!node.classes.empty()) {
            std::string joined;
            for (const auto& name : node.classes)
                joined += (joined.empty() ? "" : " ") + name;
            attributes["class"] = joined;
        }
        out += indent + "<" + element + ">\n";
        for (const auto& [name, value] : attributes)
            out += "| " + std::string(depth * 2 + 2, ' ') + name + "=" + printed(value) + "\n";
        printTree(node.children, depth + 1, out);
    }
}

std::size_t runCueText(const fs::path& directory) {
    const auto counts = readAll(directory / "cases.json");
    yyjson_doc* document = yyjson_read(counts.data(), counts.size(), 0);
    expect(document != nullptr, "cases.json reads");
    if (document == nullptr)
        return 0;
    std::size_t total = 0;
    yyjson_obj_iter files = yyjson_obj_iter_with(yyjson_doc_get_root(document));
    yyjson_val* key = nullptr;
    while ((key = yyjson_obj_iter_next(&files)) != nullptr) {
        const std::string fileName = yyjson_get_str(key);
        const auto expectedCount = yyjson_get_uint(yyjson_obj_iter_get_val(key));
        std::istringstream lines(readAll(directory / fileName));
        std::string line;
        std::size_t cases = 0;
        std::string data;
        std::string expected;
        enum class Part { None, Data, Errors, Tree } part = Part::None;
        const auto finishCase = [&] {
            if (part == Part::None)
                return;
            ++cases;
            // Each case is a cue's text in a file, as the corpus's harness runs it.
            const auto file =
                webvtt::parse("WEBVTT\n\n00:00.000 --> 00:01.000\n" + unescape(data));
            std::string actual;
            if (!file.cues.empty())
                printTree(markup::parse(file.cues.front().text).tree.children, 0, actual);
            expect(actual == expected, fileName + " case " + std::to_string(cases) + " (" + data +
                                           "): expected\n" + expected + "read\n" + actual);
            data.clear();
            expected.clear();
        };
        while (std::getline(lines, line)) {
            if (line == "#data") {
                finishCase();
                part = Part::Data;
                continue;
            }
            if (line == "#errors") {
                part = Part::Errors;
                continue;
            }
            if (line == "#document-fragment") {
                part = Part::Tree;
                continue;
            }
            if (part == Part::Data)
                data += (data.empty() ? "" : "\n") + line;
            else if (part == Part::Tree && !line.empty())
                expected += line + "\n";
        }
        finishCase();
        expect(cases == expectedCount, fileName + ": " + std::to_string(expectedCount) +
                                           " cases, ran " + std::to_string(cases));
        total += cases;
    }
    yyjson_doc_free(document);
    return total;
}

} // namespace

int main(const int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: bloom_subtitles_webvtt_conformance_test <fixtures/subtitles>\n";
        return 2;
    }
    const fs::path fixtures = argv[1];
    const auto fileCases = runFileParsing(fixtures / "wpt-webvtt" / "file-parsing");
    const auto textCases = runCueText(fixtures / "wpt-webvtt" / "cue-text");
    expect(fileCases == 51, "51 file-parsing cases, ran " + std::to_string(fileCases));
    expect(textCases == 78, "78 cue-text cases, ran " + std::to_string(textCases));
    std::cout << "file parsing: " << fileCases << " cases; cue text: " << textCases << " cases\n";
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
