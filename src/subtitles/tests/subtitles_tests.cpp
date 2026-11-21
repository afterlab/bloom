// #752: the subtitle readers and writers (docs/architecture/subtitles.md). A round trip per format,
// the encodings with their exact messages, malformed input and the strict refusal, the limits, the
// formats refused by name, TTML's XML hardening and the ASS subset with its dropped-tag report.

#include <bloom/document/cue_list.hpp>
#include <bloom/document/cue_markup.hpp>
#include <bloom/subtitles/subtitles.hpp>

#include "unique_temp_dir.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace bloom;
namespace subtitles = bloom::subtitles;
using subtitles::Format;

int failures = 0;

void expect(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void expectEqual(const std::string& actual, const std::string& wanted, const std::string& what) {
    expect(actual == wanted, what + ":\n  wanted: " + wanted + "\n  read:   " + actual);
}

std::span<const std::byte> bytesOf(const std::string_view text) {
    return std::as_bytes(std::span(text.data(), text.size()));
}

subtitles::LoadOptions options(const std::optional<Format> format = std::nullopt,
                               const bool strict = false, std::string encoding = {}) {
    subtitles::LoadOptions result;
    result.format = format;
    result.strict = strict;
    result.encoding = std::move(encoding);
    return result;
}

// A load that must succeed.
subtitles::LoadResult loaded(const std::string_view text, const subtitles::LoadOptions& with,
                             const std::string& what) {
    auto outcome = subtitles::load(bytesOf(text), with);
    if (const auto* failure = std::get_if<subtitles::LoadFailure>(&outcome)) {
        expect(false, what + " loads; it failed: " + failure->code + ": " + failure->message);
        return {};
    }
    return std::get<subtitles::LoadResult>(std::move(outcome));
}

// A load that must fail.
subtitles::LoadFailure refused(const std::string_view text, const subtitles::LoadOptions& with,
                               const std::string& what) {
    auto outcome = subtitles::load(bytesOf(text), with);
    if (std::holds_alternative<subtitles::LoadResult>(outcome)) {
        expect(false, what + " is refused");
        return {};
    }
    return std::get<subtitles::LoadFailure>(std::move(outcome));
}

core::RationalTime ms(const std::int64_t value) { return *core::RationalTime::create(value, 1000); }

std::string reportsText(const std::vector<subtitles::Report>& reports) {
    std::string out;
    for (const auto& report : reports)
        out += std::to_string(report.line) + ":" + std::to_string(report.column) + " " +
               report.code + " " + report.message + "\n";
    return out;
}

// -------------------------------------------------------------------------------------------
// Round trips.

constexpr std::string_view kSrt = "1\n"
                                  "00:00:01,000 --> 00:00:02,500\n"
                                  "Hello <i>there</i>\n"
                                  "\n"
                                  "2\n"
                                  "00:00:03,000 --> 00:00:04,000\n"
                                  "{\\an8}Top &amp; <b>bold</b>\n"
                                  "second line\n"
                                  "\n"
                                  "3\n"
                                  "00:00:05,000 --> 00:00:06,000\n"
                                  "<font color=\"#ff0000\">Red</font>\n";

void srtRoundTrip() {
    const auto first = loaded(kSrt, options(), "the SRT sample");
    expect(first.format == Format::Srt, "the SRT sample sniffs as SRT");
    expect(first.encoding == "utf-8", "the SRT sample is UTF-8");
    expect(first.reports.empty(), "the SRT sample reads without reports:\n" + reportsText(first.reports));
    const auto& cues = first.cues.cues;
    expect(cues.size() == 3, "the SRT sample has three cues");
    if (cues.size() != 3)
        return;
    expect(cues[0]->start == ms(1000) && cues[0]->end == ms(2500), "cue 1's times");
    expectEqual(cues[0]->text, "Hello <i>there</i>", "cue 1's text");
    // SubRip has no character references: "&amp;" in the file is those five characters.
    expectEqual(cues[1]->text, "Top &amp;amp; <b>bold</b>\nsecond line", "cue 2's text");
    expect(cues[1]->placement.anchor == document::CueAnchor::Top, "{\\an8} is the top anchor");
    expectEqual(cues[2]->text, "<c.srt-ff0000>Red</c>", "a font color is a generated class");
    expect(first.cues.findStyle("srt-ff0000") != nullptr, "the color class is a style");
    expect(first.cues.isValid(), "the SRT list is valid");

    const auto written = subtitles::write(first.cues, Format::Srt);
    expect(written.has_value(), "SRT writes");
    if (!written)
        return;
    expectEqual(written->text, std::string(kSrt), "SRT writes back the same bytes");
    const auto second = loaded(written->text, options(Format::Srt), "the written SRT");
    expect(second.cues == first.cues, "the written SRT reads back to the same list");
}

constexpr std::string_view kVtt = "WEBVTT\n"
                                  "Language: de\n"
                                  "\n"
                                  "STYLE\n"
                                  "::cue(.loud) {\n"
                                  "  color: #ffff00;\n"
                                  "  font-weight: bold;\n"
                                  "}\n"
                                  "\n"
                                  "00:00:01.000 --> 00:00:02.000 line:0 align:left\n"
                                  "<v Anna>Guten Tag</v>\n"
                                  "\n"
                                  "00:00:02.000 --> 00:00:04.000\n"
                                  "<c.loud>Laut</c> und <00:00:03.000>leise\n";

void vttRoundTrip() {
    const auto first = loaded(kVtt, options(), "the WebVTT sample");
    expect(first.format == Format::WebVtt, "the WebVTT sample sniffs as WebVTT");
    expect(first.cues.language == "de", "the Language header is the list's language");
    expect(first.reports.empty(), "the WebVTT sample reads without reports:\n" +
                                      reportsText(first.reports));
    const auto* loud = first.cues.findStyle("loud");
    expect(loud != nullptr && loud->bold == true && loud->color.has_value(),
           "the STYLE block's ::cue(.loud) is a bold yellow style");
    const auto& cues = first.cues.cues;
    expect(cues.size() == 2, "the WebVTT sample has two cues");
    if (cues.size() != 2)
        return;
    expectEqual(cues[0]->speaker, "Anna", "the first <v> is the speaker");
    expect(cues[0]->placement.anchor.has_value() &&
               cues[0]->placement.textAlign == document::CueTextAlign::Left,
           "line:0 align:left place the cue");
    const auto written = subtitles::write(first.cues, Format::WebVtt);
    expect(written.has_value(), "WebVTT writes");
    if (!written)
        return;
    const auto second = loaded(written->text, options(Format::WebVtt), "the written WebVTT");
    expect(second.cues == first.cues, "the written WebVTT reads back to the same list:\n" +
                                          written->text);
    const auto third = subtitles::write(second.cues, Format::WebVtt);
    expect(third && third->text == written->text, "WebVTT writing is stable");
}

constexpr std::string_view kTtml =
    R"(<?xml version="1.0" encoding="UTF-8"?>
<tt xmlns="http://www.w3.org/ns/ttml" xmlns:ttp="http://www.w3.org/ns/ttml#parameter"
    xmlns:tts="http://www.w3.org/ns/ttml#styling" xmlns:ttm="http://www.w3.org/ns/ttml#metadata"
    ttp:timeBase="media" ttp:frameRate="25" ttp:tickRate="10000000" xml:lang="fr">
  <head>
    <metadata><ttm:agent xml:id="m"><ttm:name>Marie</ttm:name></ttm:agent></metadata>
    <styling><style xml:id="y" tts:color="#ffff00"/></styling>
    <layout><region xml:id="top" tts:origin="10% 5%" tts:extent="80% 20%" tts:displayAlign="before" tts:textAlign="center"/></layout>
  </head>
  <body>
    <div begin="1s">
      <p begin="0s" end="25f" ttm:agent="m">Bonjour<br/>tout le <span tts:fontStyle="italic">monde</span></p>
      <p begin="2s" dur="5000000t" region="top"><span style="y">Haut</span></p>
    </div>
    <p begin="00:00:10.000" end="00:00:11:12">Clock</p>
  </body>
</tt>
)";

void ttmlRoundTrip() {
    const auto first = loaded(kTtml, options(), "the TTML sample");
    expect(first.format == Format::Ttml, "the TTML sample sniffs as TTML");
    expect(first.cues.language == "fr", "xml:lang is the list's language");
    const auto& cues = first.cues.cues;
    expect(cues.size() == 3, "the TTML sample has three cues");
    if (cues.size() != 3)
        return;
    expect(cues[0]->start == ms(1000) && cues[0]->end == ms(2000),
           "a paragraph's begin is relative to its div; 25f at 25 fps is a second");
    expectEqual(cues[0]->text, "Bonjour\ntout le <i>monde</i>", "br and an italic span");
    expectEqual(cues[0]->speaker, "Marie", "ttm:agent's name is the speaker");
    expect(cues[1]->start == ms(3000) && cues[1]->end == ms(3500), "dur in ticks");
    expect(cues[1]->placement.anchor == document::CueAnchor::Top,
           "the top region places the cue at the top");
    expect(cues[2]->start == ms(10000) &&
               cues[2]->end == *core::RationalTime::create(11 * 25 + 12, 25),
           "clock times with frames");

    subtitles::WriteOptions writeOptions;
    writeOptions.frameRate = std::pair<std::uint32_t, std::uint32_t>{25, 1};
    // 3.5 s is not a whole frame at 25 fps: the times are written in ticks, the least common
    // multiple of their denominators (1, 2 and 25).
    const auto written = subtitles::write(first.cues, Format::Ttml, writeOptions);
    expect(written.has_value(), "TTML writes");
    if (!written)
        return;
    expect(written->text.find("ttp:tickRate=\"50\"") != std::string::npos &&
               written->text.find("begin=\"175t\"") == std::string::npos &&
               written->text.find("end=\"175t\"") != std::string::npos,
           "times off the frame grid are written in exact ticks:\n" + written->text);
    const auto second = loaded(written->text, options(Format::Ttml), "the written TTML");
    expect(second.reports.empty(), "the written TTML reads without reports:\n" +
                                       reportsText(second.reports));
    expect(second.cues.cues.size() == 3, "the written TTML has three cues");
    for (std::size_t index = 0; index < std::min<std::size_t>(3, second.cues.cues.size()); ++index) {
        const auto& back = *second.cues.cues[index];
        const auto& original = *cues[index];
        expect(back.start == original.start && back.end == original.end,
               "the written TTML keeps cue " + std::to_string(index + 1) + "'s times");
        expectEqual(back.text, original.text,
                    "the written TTML keeps cue " + std::to_string(index + 1) + "'s text");
        expectEqual(back.speaker, original.speaker,
                    "the written TTML keeps cue " + std::to_string(index + 1) + "'s speaker");
        expectEqual(back.style, original.style,
                    "the written TTML keeps cue " + std::to_string(index + 1) + "'s style");
    }
    expect(second.cues.styles == first.cues.styles, "the written TTML keeps the styles");
    // A cue the node's layout places is written in the default bottom region, which reads back as
    // that region's placement; from there, writing and reading again changes nothing.
    const auto rewritten = subtitles::write(second.cues, Format::Ttml, writeOptions);
    expect(rewritten && rewritten->text == written->text, "TTML writing is stable:\n" +
                                                             (rewritten ? rewritten->text : ""));
    const auto third = loaded(rewritten ? rewritten->text : "", options(Format::Ttml),
                              "the rewritten TTML");
    expect(third.cues == second.cues, "the rewritten TTML reads back to the same list");

    // Every time a whole frame: frames.
    auto list = first.cues;
    auto moved = std::make_shared<document::Cue>(*list.cues[1]);
    moved->end = *core::RationalTime::create(88, 25);
    list.cues[1] = moved;
    const auto frames = subtitles::write(list, Format::Ttml, writeOptions);
    expect(frames && frames->text.find("ttp:frameRate=\"25\"") != std::string::npos &&
               frames->text.find("end=\"88f\"") != std::string::npos,
           "whole-frame times are written in frames:\n" + (frames ? frames->text : ""));
    // 30000/1001 is written as 30 with a multiplier.
    auto ntsc = first.cues;
    for (auto& cue : ntsc.cues) {
        auto copy = std::make_shared<document::Cue>(*cue);
        copy->start = *core::RationalTime::create(1001 * 30, 30000);
        copy->end = *core::RationalTime::create(1001 * 60, 30000);
        cue = copy;
    }
    ntsc.cues.resize(1);
    writeOptions.frameRate = std::pair<std::uint32_t, std::uint32_t>{30000, 1001};
    const auto drop = subtitles::write(ntsc, Format::Ttml, writeOptions);
    expect(drop && drop->text.find("ttp:frameRate=\"30\" ttp:frameRateMultiplier=\"1000 1001\"") !=
                       std::string::npos,
           "an NTSC rate is written with its multiplier:\n" + (drop ? drop->text : ""));
    if (drop) {
        const auto back = loaded(drop->text, options(Format::Ttml), "the NTSC TTML");
        expect(!back.cues.cues.empty() && back.cues.cues[0]->start == ntsc.cues[0]->start &&
                   back.cues.cues[0]->end == ntsc.cues[0]->end,
               "NTSC frame times read back exactly");
    }
}

constexpr std::string_view kAss = "[Script Info]\n"
                                  "ScriptType: v4.00+\n"
                                  "PlayResX: 1920\n"
                                  "PlayResY: 1080\n"
                                  "\n"
                                  "[V4+ Styles]\n"
                                  "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, "
                                  "OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, "
                                  "ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, "
                                  "Alignment, MarginL, MarginR, MarginV, Encoding\n"
                                  "Style: Default,Arial,54,&H00FFFFFF,&H000000FF,&H00000000,"
                                  "&H00000000,0,0,0,0,100,100,0,0,1,2,0,2,10,10,10,1\n"
                                  "Style: Sign,Arial,54,&H0000FFFF,&H000000FF,&H00000000,"
                                  "&H00000000,-1,0,0,0,100,100,0,0,1,2,0,8,10,10,10,1\n"
                                  "\n"
                                  "[Events]\n"
                                  "Format: Layer, Start, End, Style, Name, MarginL, MarginR, "
                                  "MarginV, Effect, Text\n"
                                  "Dialogue: 0,0:00:01.00,0:00:03.00,Default,Ann,0,0,0,,"
                                  "Hello, {\\i1}world{\\i0}\\Nline two\n"
                                  "Dialogue: 0,0:00:04.00,0:00:06.00,Sign,,0,0,0,,"
                                  "{\\pos(960,100)\\c&H0000FF&}Red {\\k50}ka{\\k50}ra\n"
                                  "Dialogue: 0,0:00:07.00,0:00:08.00,Default,,0,0,0,,"
                                  "{\\fad(200,200)\\blur3}Soft{\\t(\\fs80)}\n"
                                  "Comment: 0,0:00:09.00,0:00:10.00,Default,,0,0,0,,not a cue\n";

void assSubset() {
    const auto read = loaded(kAss, options(), "the ASS sample");
    expect(read.format == Format::Ass, "the ASS sample sniffs as ASS");
    const auto& cues = read.cues.cues;
    expect(cues.size() == 3, "the ASS sample has three cues (a Comment is not one)");
    if (cues.size() != 3)
        return;
    expectEqual(cues[0]->text, "Hello, <i>world</i>\nline two", "\\i and \\N");
    expectEqual(cues[0]->speaker, "Ann", "Name is the speaker");
    expectEqual(cues[0]->style, "Default", "the event's style");
    const auto* sign = read.cues.findStyle("Sign");
    expect(sign != nullptr && sign->bold == true && sign->fontSize &&
               std::abs(*sign->fontSize - 54.0 / 1080.0) < 1e-12,
           "Fontsize is a fraction of PlayResY");
    expect(cues[1]->placement.anchor == document::CueAnchor::Top &&
               cues[1]->placement.positionX && std::abs(*cues[1]->placement.positionX - 0.5) < 1e-12,
           "\\pos and the style's alignment place the cue");
    expectEqual(cues[1]->text, "<c.ass-ff0000>Red ka</c><00:00:04.500><c.ass-ff0000>ra</c>",
                "\\c is a color class and \\k a timestamp");
    expectEqual(cues[2]->text, "Soft", "dropped tags leave the text");
    expectEqual(reportsText(read.reports),
                "15:0 dropped \\fad and \\fade (the node's fades are used): dropped, on line 15\n"
                "15:0 dropped \\blur: dropped, on line 15\n"
                "15:0 dropped \\t animated tags: dropped, on line 15\n",
                "the dropped tags are reported");
    expect(!subtitles::write(read.cues, Format::Ass).has_value(), "ASS is not written");
}

// -------------------------------------------------------------------------------------------
// Encodings.

void encodings() {
    // A UTF-8 byte order mark, and UTF-16 in both byte orders.
    const std::string body = "1\n00:00:01,000 --> 00:00:02,000\n\xC3\xA9t\xC3\xA9\n";
    auto bom = std::string("\xEF\xBB\xBF") + body;
    expect(loaded(bom, options(), "UTF-8 with a BOM").encoding == "utf-8", "a UTF-8 BOM");
    std::string little = "\xFF\xFE";
    std::string big = "\xFE\xFF";
    for (const char c : std::string("1\n00:00:01,000 --> 00:00:02,000\nete\n")) {
        little += c;
        little += '\0';
        big += '\0';
        big += c;
    }
    expect(loaded(little, options(), "UTF-16LE").encoding == "utf-16le", "a UTF-16LE BOM");
    expect(loaded(big, options(), "UTF-16BE").encoding == "utf-16be", "a UTF-16BE BOM");

    // Latin-1 bytes without a BOM are refused, naming the line and the byte offset.
    const std::string latin = "1\n00:00:01,000 --> 00:00:02,000\n\xE9t\xE9\n";
    const auto failure = refused(latin, options(), "Latin-1 without a named encoding");
    expectEqual(failure.code, "encoding", "the refusal's code");
    expect(failure.line == 3, "the refusal names line 3");
    expectEqual(failure.message,
                "line 3: the byte at offset 32 is not UTF-8; name the file's encoding, or save it "
                "as UTF-8",
                "the UTF-8 refusal");
    // Named, it decodes from the WHATWG table (latin1 is windows-1252).
    const auto named = loaded(latin, options(std::nullopt, false, "latin1"), "named windows-1252");
    expectEqual(named.encoding, "windows-1252", "latin1 names windows-1252");
    expect(!named.cues.cues.empty() && named.cues.cues[0]->text == "\xC3\xA9t\xC3\xA9",
           "windows-1252 decodes e-acute");
    expect(subtitles::singleByteEncodingNames().size() == 28, "28 WHATWG single-byte encodings");
    expect(subtitles::singleByteEncodingName("iso_8859-2") == "iso-8859-2", "labels resolve");
    // Multibyte legacy encodings are refused with advice.
    const auto multibyte = refused(latin, options(std::nullopt, false, "shift_jis"), "Shift_JIS");
    expectEqual(multibyte.message,
                "the multibyte encoding \"shift_jis\" is not read; save the file as UTF-8",
                "the multibyte refusal");
    // An unpaired surrogate in UTF-16.
    const std::string broken = std::string("\xFF\xFE", 2) + std::string("1\0\n\0\x00\xD8", 6);
    const auto surrogate = refused(broken, options(), "an unpaired surrogate");
    expectEqual(surrogate.code, "encoding", "an unpaired surrogate is an encoding failure");
}

// -------------------------------------------------------------------------------------------
// Malformed input, strict, limits.

void malformed() {
    const std::string text = "1\n"
                             "00:00:01,000 --> 00:00:02,000\n"
                             "good\n"
                             "\n"
                             "2\n"
                             "00:00:05,000 --> 00:00:04,000\n"
                             "backwards\n"
                             "\n"
                             "3\n"
                             "00:00:06,000 --> 00:00:06,000\n"
                             "empty span\n"
                             "\n"
                             "00:00:07,000 --> 00:00:08,000\n"
                             "no counter\n";
    const auto read = loaded(text, options(), "the malformed SRT");
    expect(read.cues.cues.size() == 2, "the good cues are kept");
    expectEqual(reportsText(read.reports),
                "6:1 skipped-cue line 6: the cue ends at 00:00:04.000, before it starts at "
                "00:00:05.000; the cue is skipped\n"
                "10:1 zero-length-cue line 10: the cue starts and ends at 00:00:06.000, so it can "
                "never be seen; it is dropped\n"
                "13:0 counter a missing cue counter: tolerated, on line 13\n",
                "the malformed SRT's report");
    const auto strict = refused(text, options(std::nullopt, true), "the strict malformed SRT");
    expectEqual(strict.code, "strict", "a strict load refuses");
    expectEqual(strict.message,
                "A strict load refuses any warning; the first is: line 6: the cue ends at "
                "00:00:04.000, before it starts at 00:00:05.000; the cue is skipped",
                "the strict refusal");
    expect(strict.reports.size() == 3, "the strict refusal carries every row");

    const auto vtt = refused("WEBVT\n\n00:00.000 --> 00:01.000\nx\n", options(Format::WebVtt),
                             "a bad WebVTT signature");
    expectEqual(vtt.message, "line 1: a WebVTT file starts with WEBVTT", "the signature refusal");
    const auto none = refused("hello\n", options(), "text that is no format");
    expectEqual(none.code, "unrecognized", "unrecognized text");
    const auto nothing = refused("WEBVTT\n\nNOTE only a note\n", options(), "no cues");
    expectEqual(nothing.code, "no-cues", "a file without cues");
    // A C0 control in cue text.
    const auto control = loaded("1\n00:00:01,000 --> 00:00:02,000\nok\n\n2\n00:00:03,000 --> "
                                "00:00:04,000\nbell\x07\n",
                                options(), "a control character");
    expect(control.cues.cues.size() == 1 && control.reports.size() == 1 &&
               control.reports[0].code == "skipped-cue",
           "a cue with a C0 control is skipped:\n" + reportsText(control.reports));
}

void limits() {
    const std::string big(document::kMaximumSubtitleFileBytes + 1, 'a');
    const auto tooBig = refused(big, options(), "a file over the size limit");
    expectEqual(tooBig.code, "limit", "the size limit's code");
    expectEqual(tooBig.message,
                "The file is 16777217 bytes; Bloom reads subtitle files of at most 16777216 bytes "
                "(16 MiB)",
                "the size limit's message");
    std::string many = "WEBVTT\n\n";
    for (int index = 0; index < 17; ++index)
        many += "00:00:01.000 --> 00:00:02.000\nx\n\n";
    const auto overlap = refused(many, options(), "seventeen cues at once");
    expectEqual(overlap.message, "17 cues show at once at 00:00:01.000; at most 16 may",
                "the active-cue limit");
    std::string text(document::kMaximumCueTextBytes + 1, 'x');
    const auto longCue = loaded("1\n00:00:01,000 --> 00:00:02,000\n" + text +
                                    "\n\n2\n00:00:03,000 --> 00:00:04,000\nok\n",
                                options(), "a cue over the text limit");
    expect(longCue.cues.cues.size() == 1 && !longCue.reports.empty(),
           "a cue over the text limit is skipped");
}

void refusedFormats() {
    const auto check = [](const std::string_view text, const std::string& name,
                          const std::string& wanted, const std::string& what) {
        auto with = options();
        with.sourceName = name;
        const auto failure = refused(text, with, what);
        expectEqual(failure.code, "refused-format", what + "'s code");
        expectEqual(failure.message, wanted, what + "'s message");
    };
    check("Scenarist_SCC V1.0\n", "", "SCC (CEA-608 caption data) is not read; convert it to SRT, WebVTT or TTML", "SCC");
    check(std::string_view("PG\x00\x00", 4), "", "PGS image subtitles are not read; Bloom reads text subtitles", "PGS");
    check("0:00:01.000,0:00:02.000\nhi\n", "talk.sbv", "SBV is not read; convert it to SRT or WebVTT", "SBV");
    check("[00:01.00]la\n", "song.lrc", "LRC lyrics are not read; convert them to SRT or WebVTT", "LRC");
    check(R"(<tt xmlns="http://www.w3.org/ns/ttml" ttp:timeBase="smpte" xmlns:ttp="http://www.w3.org/ns/ttml#parameter"><body/></tt>)",
          "", "line 1: ttp:timeBase=\"smpte\" is not read; Bloom reads media time only", "an SMPTE time base");
    check(R"(<tt xmlns="http://www.w3.org/ns/ttml"><body><div><image src="a.png"/></div></body></tt>)",
          "", "line 1: image subtitles (IMSC Image, SMPTE-TT images) are not read; Bloom reads text subtitles",
          "IMSC Image");
}

void xmlHardening() {
    const auto check = [](const std::string& text, const std::string& wanted, const std::string& what) {
        const auto failure = refused(text, options(Format::Ttml), what);
        expectEqual(failure.message, wanted, what);
    };
    check("<?xml version=\"1.0\"?>\n<!DOCTYPE tt [<!ENTITY a \"aaaa\">]>\n<tt xmlns=\"http://www.w3.org/ns/ttml\"/>",
          "line 2: a document type declaration (DTD) is refused in subtitle files", "a DTD");
    std::string deep = "<tt xmlns=\"http://www.w3.org/ns/ttml\">";
    for (int index = 0; index < 70; ++index)
        deep += "<span>";
    check(deep, "line 1: the document nests elements deeper than 64 levels", "deep nesting");
    std::string attributes = "<tt xmlns=\"http://www.w3.org/ns/ttml\"><body";
    for (int index = 0; index < 257; ++index)
        attributes += " a" + std::to_string(index) + "=\"x\"";
    attributes += "/></tt>";
    check(attributes, "line 1: an element carries more than 256 attributes", "too many attributes");
    check("<tt xmlns=\"http://www.w3.org/ns/ttml\"><body>", "line 1, column 45: the XML does not read: no element found",
          "truncated XML");
    check("<tt xmlns=\"http://example.com/other\"/>",
          "the XML is not TTML: its root is not <tt> in the TTML namespace http://www.w3.org/ns/ttml",
          "another namespace");
}

void files() {
    const auto directory = test_support::processTempDirectory() / "bloom-subtitles-files";
    std::filesystem::create_directories(directory);
    const auto path = directory / "sample.srt";
    {
        std::ofstream out(path, std::ios::binary);
        out << kSrt;
    }
    const auto read = subtitles::readFile(path);
    const auto* contents = std::get_if<subtitles::FileContents>(&read);
    expect(contents != nullptr, "the file reads");
    if (contents != nullptr) {
        expect(contents->size == kSrt.size(), "the file's size");
        expectEqual(contents->digest, subtitles::digestOf(bytesOf(kSrt)), "the file's digest");
        expect(contents->digest.starts_with("sha256:") && contents->digest.size() == 71,
               "a digest is sha256:<64 hex>");
        const auto stamp = subtitles::statFile(path);
        expect(stamp && stamp->size == contents->size &&
                   stamp->modifiedNanoseconds == contents->modifiedNanoseconds,
               "the stamp matches the read");
    }
    const auto missing = subtitles::readFile(directory / "missing.srt");
    expect(std::holds_alternative<subtitles::LoadFailure>(missing) &&
               std::get<subtitles::LoadFailure>(missing).code == "file",
           "a missing file is a file failure");
    expect(!subtitles::statFile(directory / "missing.srt").has_value(), "a missing file has no stamp");
    std::filesystem::remove_all(directory);
}

void names() {
    expect(subtitles::formatFromName("WebVTT") == Format::WebVtt, "webvtt names WebVTT");
    expect(subtitles::formatFromName("ssa") == Format::Ass, "ssa names ASS");
    expect(subtitles::formatFromExtension(".DFXP") == Format::Ttml, ".dfxp is TTML");
    expect(!subtitles::formatFromExtension(".txt").has_value(), ".txt is no format");
    expect(subtitles::formatName(Format::WebVtt) == "vtt", "WebVTT's name");
    expect(!subtitles::isWritableFormat(Format::Ass) && subtitles::isWritableFormat(Format::Ttml),
           "the writable formats");
    // A cancelled load.
    std::stop_source stop;
    stop.request_stop();
    auto with = options();
    with.stop = stop.get_token();
    expectEqual(refused(kSrt, with, "a cancelled load").code, "cancelled", "a cancelled load");
}

} // namespace

int main() {
    srtRoundTrip();
    vttRoundTrip();
    ttmlRoundTrip();
    assSubset();
    encodings();
    malformed();
    limits();
    refusedFormats();
    xmlHardening();
    files();
    names();
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
