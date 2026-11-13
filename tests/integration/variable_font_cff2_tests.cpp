// #489: a variable font with compact font format outlines (CFF2) applies its named instances and
// axis values. The font is built here, table by table, so every answer is known: one weight axis
// (100, default 400, 900), a rectangle whose right edge moves 200 units over the upper half of the
// axis, a glyph drawn through a subroutine behind hints, and advances that follow HVAR. The
// instance is checked twice: its outlines are read back from the static font's tables, and the
// text rasterizer draws it.

#include <bloom/render/text_raster.hpp>
#include <bloom/runtime/variable_font_instance.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace bloom::runtime::fonts;
using Bytes = std::vector<std::uint8_t>;

int failures = 0;
void expect(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void u8(Bytes& out, const std::uint32_t value) { out.push_back(static_cast<std::uint8_t>(value & 0xFFU)); }
void u16(Bytes& out, const std::uint32_t value) {
    u8(out, value >> 8U);
    u8(out, value);
}
void u32(Bytes& out, const std::uint32_t value) {
    u16(out, value >> 16U);
    u16(out, value);
}
void s16(Bytes& out, const int value) { u16(out, static_cast<std::uint32_t>(value) & 0xFFFFU); }
void fixed(Bytes& out, const double value) {
    u32(out, static_cast<std::uint32_t>(static_cast<std::int32_t>(value * 65536.0)));
}
void f2dot14(Bytes& out, const double value) { s16(out, static_cast<int>(value * 16384.0)); }
void tag(Bytes& out, const char (&text)[5]) {
    for (std::size_t i = 0; i < 4; ++i)
        out.push_back(static_cast<std::uint8_t>(text[i]));
}
void append(Bytes& out, const Bytes& more) { out.insert(out.end(), more.begin(), more.end()); }

// A charstring or DICT operand as a 16-bit number.
void number(Bytes& out, const int value) {
    out.push_back(28);
    s16(out, value);
}
// A DICT operand of a fixed five bytes.
void offset(Bytes& out, const std::size_t value) {
    out.push_back(29);
    u32(out, static_cast<std::uint32_t>(value));
}

// A CFF2 INDEX: a 32-bit count, one-byte offsets.
Bytes index2(const std::vector<Bytes>& objects) {
    Bytes out;
    u32(out, static_cast<std::uint32_t>(objects.size()));
    if (objects.empty())
        return out;
    out.push_back(1);
    std::uint32_t at = 1;
    u8(out, at);
    for (const auto& object : objects) {
        at += static_cast<std::uint32_t>(object.size());
        u8(out, at);
    }
    for (const auto& object : objects)
        append(out, object);
    return out;
}

constexpr std::uint8_t kRmoveto = 21;
constexpr std::uint8_t kRlineto = 5;
constexpr std::uint8_t kHlineto = 6;
constexpr std::uint8_t kVlineto = 7;
constexpr std::uint8_t kRrcurveto = 8;
constexpr std::uint8_t kCallsubr = 10;
constexpr std::uint8_t kEndchar = 14;
constexpr std::uint8_t kBlend = 16;
constexpr std::uint8_t kHstemhm = 18;
constexpr std::uint8_t kHintmask = 19;

Bytes cff2Table() {
    // Glyph 0 draws nothing.
    // Glyph 1: a rectangle from (100, 0), 400 wide and 700 tall; the width gains 200 at the region.
    Bytes rectangle;
    number(rectangle, 100);
    number(rectangle, 0);
    rectangle.push_back(kRmoveto);
    number(rectangle, 400);
    number(rectangle, 200);
    number(rectangle, 1);
    rectangle.push_back(kBlend);
    rectangle.push_back(kHlineto);
    number(rectangle, 700);
    rectangle.push_back(kVlineto);
    number(rectangle, -400);
    number(rectangle, -200);
    number(rectangle, 1);
    rectangle.push_back(kBlend);
    rectangle.push_back(kHlineto);
    // Glyph 2: a stem hint and its mask, a move, a curve from the first local subroutine (number
    // -107 with one subroutine), then a line.
    Bytes curved;
    number(curved, 0);
    number(curved, 50);
    curved.push_back(kHstemhm);
    curved.push_back(kHintmask);
    curved.push_back(0x80);
    number(curved, 50);
    number(curved, 50);
    curved.push_back(kRmoveto);
    number(curved, -107);
    curved.push_back(kCallsubr);
    number(curved, 100);
    curved.push_back(kHlineto);
    Bytes subroutine;
    for (int i = 0; i < 6; ++i)
        number(subroutine, 100);
    subroutine.push_back(kRrcurveto);

    // The variation store: one axis, one region that rises over the upper half of the axis, and
    // one item variation data that names it.
    Bytes store;
    u16(store, 1);  // format
    u32(store, 12); // region list offset
    u16(store, 1);  // item variation data count
    u32(store, 22); // its offset
    u16(store, 1);  // axis count
    u16(store, 1);  // region count
    f2dot14(store, 0.0);
    f2dot14(store, 1.0);
    f2dot14(store, 1.0);
    u16(store, 0); // item count
    u16(store, 0); // word delta count
    u16(store, 1); // region index count
    u16(store, 0); // region 0
    Bytes vstore;
    u16(vstore, static_cast<std::uint32_t>(store.size()));
    append(vstore, store);

    const Bytes globalSubrs = index2({});
    const Bytes charStrings = index2({Bytes{}, rectangle, curved});
    const Bytes localSubrs = index2({subroutine});
    Bytes privateDict;
    offset(privateDict, 6); // Subrs: right after this dictionary (five bytes and the operator).
    privateDict.push_back(19);

    // Top DICT: three five-byte operands and their operators (one is two bytes).
    constexpr std::size_t kTopDictSize = 5 + 1 + 5 + 1 + 5 + 2;
    constexpr std::size_t kHeaderSize = 5;
    constexpr std::size_t kFontDictSize = 5 + 5 + 1;
    const std::size_t vstoreAt = kHeaderSize + kTopDictSize + globalSubrs.size();
    const std::size_t charStringsAt = vstoreAt + vstore.size();
    const std::size_t fdArrayAt = charStringsAt + charStrings.size();
    const std::size_t privateAt = fdArrayAt + 4 + 1 + 2 + kFontDictSize;
    Bytes fontDict;
    offset(fontDict, privateDict.size());
    offset(fontDict, privateAt);
    fontDict.push_back(18);

    Bytes table{2, 0, static_cast<std::uint8_t>(kHeaderSize)};
    u16(table, kTopDictSize);
    offset(table, charStringsAt);
    table.push_back(17);
    offset(table, vstoreAt);
    table.push_back(24);
    offset(table, fdArrayAt);
    table.push_back(12);
    table.push_back(36);
    append(table, globalSubrs);
    append(table, vstore);
    append(table, charStrings);
    append(table, index2({fontDict}));
    append(table, privateDict);
    append(table, localSubrs);
    return table;
}

Bytes sfnt(const std::uint32_t version, const std::map<std::string, Bytes>& tables) {
    Bytes out;
    u32(out, version);
    u16(out, static_cast<std::uint32_t>(tables.size()));
    u16(out, 0);
    u16(out, 0);
    u16(out, 0);
    std::size_t at = 12 + tables.size() * 16;
    for (const auto& [name, data] : tables) {
        for (const char c : name)
            out.push_back(static_cast<std::uint8_t>(c));
        u32(out, 0);
        u32(out, static_cast<std::uint32_t>(at));
        u32(out, static_cast<std::uint32_t>(data.size()));
        at += (data.size() + 3U) & ~std::size_t{3};
    }
    for (const auto& [name, data] : tables) {
        append(out, data);
        while (out.size() % 4 != 0)
            out.push_back(0);
    }
    return out;
}

Bytes variableFont() {
    std::map<std::string, Bytes> tables;
    auto& head = tables["head"];
    u32(head, 0x00010000);
    u32(head, 0x00010000);
    u32(head, 0);
    u32(head, 0x5F0F3CF5);
    u16(head, 0);
    u16(head, 1000); // units per em
    head.resize(head.size() + 16, 0);
    s16(head, 0);
    s16(head, 0);
    s16(head, 700);
    s16(head, 700);
    u16(head, 0);
    u16(head, 8);
    s16(head, 2);
    s16(head, 0);
    s16(head, 0);
    auto& hhea = tables["hhea"];
    u32(hhea, 0x00010000);
    s16(hhea, 800);
    s16(hhea, -200);
    s16(hhea, 0);
    u16(hhea, 600);
    s16(hhea, 0);
    s16(hhea, 0);
    s16(hhea, 700);
    s16(hhea, 1);
    s16(hhea, 0);
    s16(hhea, 0);
    hhea.resize(hhea.size() + 8, 0);
    s16(hhea, 0);
    u16(hhea, 3);
    auto& maxp = tables["maxp"];
    u32(maxp, 0x00005000);
    u16(maxp, 3);
    auto& hmtx = tables["hmtx"];
    for (const auto& [advance, bearing] : {std::pair{500, 0}, std::pair{600, 100}, std::pair{400, 50}}) {
        u16(hmtx, static_cast<std::uint32_t>(advance));
        s16(hmtx, bearing);
    }
    // 'A' is glyph 1 and 'B' glyph 2.
    auto& cmap = tables["cmap"];
    u16(cmap, 0);
    u16(cmap, 1);
    u16(cmap, 3);
    u16(cmap, 1);
    u32(cmap, 12);
    u16(cmap, 4);
    u16(cmap, 32);
    u16(cmap, 0);
    u16(cmap, 4);
    u16(cmap, 4);
    u16(cmap, 1);
    u16(cmap, 0);
    u16(cmap, 0x42);
    u16(cmap, 0xFFFF);
    u16(cmap, 0);
    u16(cmap, 0x41);
    u16(cmap, 0xFFFF);
    s16(cmap, 1 - 0x41);
    s16(cmap, 1);
    u16(cmap, 0);
    u16(cmap, 0);
    auto& fvar = tables["fvar"];
    u16(fvar, 1);
    u16(fvar, 0);
    u16(fvar, 16);
    u16(fvar, 2);
    u16(fvar, 1);  // axes
    u16(fvar, 20); // axis record size
    u16(fvar, 1);  // named instances
    u16(fvar, 8);  // instance record size
    tag(fvar, "wght");
    fixed(fvar, 100.0);
    fixed(fvar, 400.0);
    fixed(fvar, 900.0);
    u16(fvar, 0);
    u16(fvar, 256);
    u16(fvar, 257);
    u16(fvar, 0);
    fixed(fvar, 900.0);
    // Advances: glyph 1 gains 200 at the region.
    auto& hvar = tables["HVAR"];
    u32(hvar, 0x00010000);
    u32(hvar, 20);
    u32(hvar, 0);
    u32(hvar, 0);
    u32(hvar, 0);
    u16(hvar, 1);
    u32(hvar, 12);
    u16(hvar, 1);
    u32(hvar, 22);
    u16(hvar, 1);
    u16(hvar, 1);
    f2dot14(hvar, 0.0);
    f2dot14(hvar, 1.0);
    f2dot14(hvar, 1.0);
    u16(hvar, 3);
    u16(hvar, 1);
    u16(hvar, 1);
    u16(hvar, 0);
    s16(hvar, 0);
    s16(hvar, 200);
    s16(hvar, 0);
    tables["CFF2"] = cff2Table();
    return sfnt(0x4F54544F, tables); // 'OTTO'
}

std::uint32_t be(const Bytes& d, const std::size_t at, const std::size_t bytes) {
    std::uint32_t v = 0;
    for (std::size_t i = 0; i < bytes; ++i)
        v = (v << 8U) | (at + i < d.size() ? d[at + i] : 0U);
    return v;
}

std::pair<std::size_t, std::size_t> tableOf(const Bytes& font, const std::string& name) {
    const auto count = be(font, 4, 2);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t record = 12 + static_cast<std::size_t>(i) * 16;
        if (record + 16 <= font.size() &&
            std::string(font.begin() + static_cast<std::ptrdiff_t>(record),
                        font.begin() + static_cast<std::ptrdiff_t>(record) + 4) == name)
            return {be(font, record + 8, 4), be(font, record + 12, 4)};
    }
    return {0, 0};
}

// A CFF (version 1) INDEX at `at`: where each object starts and ends, and where the INDEX ends.
struct Index1 final {
    std::vector<std::pair<std::size_t, std::size_t>> objects;
    std::size_t end = 0;
};
Index1 index1(const Bytes& d, const std::size_t at) {
    Index1 index;
    const auto count = be(d, at, 2);
    if (count == 0) {
        index.end = at + 2;
        return index;
    }
    const std::size_t size = be(d, at + 2, 1);
    const std::size_t data = at + 3 + (count + 1U) * size;
    for (std::uint32_t i = 0; i < count; ++i)
        index.objects.emplace_back(data + be(d, at + 3 + i * size, size) - 1,
                                   data + be(d, at + 3 + (i + 1U) * size, size) - 1);
    index.end = index.objects.back().second;
    return index;
}

struct Outline final {
    bool read = false;
    int width = 0;
    // Every point in order, control points included.
    std::vector<std::pair<int, int>> points;
    std::string operators;
};

// The glyph's Type 2 charstring in the instance: its width and the points it visits.
Outline outlineOf(const Bytes& font, const std::size_t glyph) {
    Outline outline;
    const auto [cffAt, cffSize] = tableOf(font, "CFF ");
    if (cffSize < 4 || be(font, 0, 4) != 0x4F54544F)
        return outline;
    const Bytes cff(font.begin() + static_cast<std::ptrdiff_t>(cffAt),
                    font.begin() + static_cast<std::ptrdiff_t>(cffAt + cffSize));
    const auto names = index1(cff, cff[2]);
    const auto top = index1(cff, names.end);
    if (top.objects.size() != 1)
        return outline;
    // The Top DICT: the operand before operator 17 is where the charstrings are.
    std::size_t charStrings = 0;
    std::vector<long> operands;
    for (std::size_t at = top.objects[0].first; at < top.objects[0].second;) {
        const auto b = cff[at];
        if (b == 29) {
            operands.push_back(static_cast<std::int32_t>(be(cff, at + 1, 4)));
            at += 5;
        } else if (b == 28) {
            operands.push_back(static_cast<std::int16_t>(be(cff, at + 1, 2)));
            at += 3;
        } else if (b == 30) {
            ++at;
            while (at < cff.size() && (cff[at] & 0x0FU) != 0x0FU && (cff[at] >> 4U) != 0x0FU)
                ++at;
            ++at;
            operands.push_back(0);
        } else if (b >= 32 && b <= 246) {
            operands.push_back(static_cast<long>(b) - 139);
            ++at;
        } else if (b >= 247 && b <= 250) {
            operands.push_back((static_cast<long>(b) - 247) * 256 + cff[at + 1] + 108);
            at += 2;
        } else if (b >= 251 && b <= 254) {
            operands.push_back(-(static_cast<long>(b) - 251) * 256 - cff[at + 1] - 108);
            at += 2;
        } else {
            if (b == 17 && !operands.empty())
                charStrings = static_cast<std::size_t>(operands.back());
            at += b == 12 ? 2 : 1;
            operands.clear();
        }
    }
    if (charStrings == 0)
        return outline;
    const auto glyphs = index1(cff, charStrings);
    if (glyph >= glyphs.objects.size())
        return outline;
    std::vector<int> stack;
    int x = 0;
    int y = 0;
    bool widthTaken = false;
    const auto width = [&](const std::size_t arguments) {
        if (!widthTaken && stack.size() > arguments) {
            outline.width = stack.front();
            stack.erase(stack.begin());
        }
        widthTaken = true;
    };
    const auto visit = [&](const std::size_t at) {
        x += stack[at];
        y += stack[at + 1];
        outline.points.emplace_back(x, y);
    };
    for (std::size_t at = glyphs.objects[glyph].first; at < glyphs.objects[glyph].second;) {
        const auto b = cff[at];
        if (b == 28) {
            stack.push_back(static_cast<std::int16_t>(be(cff, at + 1, 2)));
            at += 3;
        } else if (b >= 32 && b <= 246) {
            stack.push_back(static_cast<int>(b) - 139);
            ++at;
        } else if (b >= 247 && b <= 250) {
            stack.push_back((static_cast<int>(b) - 247) * 256 + cff[at + 1] + 108);
            at += 2;
        } else if (b >= 251 && b <= 254) {
            stack.push_back(-(static_cast<int>(b) - 251) * 256 - cff[at + 1] - 108);
            at += 2;
        } else {
            ++at;
            if (b == kRmoveto) {
                width(2);
                outline.operators += 'm';
                if (stack.size() == 2)
                    visit(0);
            } else if (b == kRlineto && stack.size() == 2) {
                outline.operators += 'l';
                visit(0);
            } else if (b == kRrcurveto && stack.size() == 6) {
                outline.operators += 'c';
                visit(0);
                visit(2);
                visit(4);
            } else if (b == kEndchar) {
                width(0);
                outline.operators += 'e';
                outline.read = true;
            } else {
                return outline; // An operator the instance is not written with.
            }
            stack.clear();
        }
    }
    return outline;
}

using Points = std::vector<std::pair<int, int>>;

VariableFontInstance make(const Bytes& font, const std::uint32_t instance,
                          const std::vector<VariableFontAxisSetting>& settings = {}) {
    return instantiateVariableFont(font, 0, instance, settings);
}

void outlinesFollowTheAxis() {
    const auto font = variableFont();
    const auto axes = variableFontAxes(font, 0);
    expect(axes.size() == 1 && axes[0].tag == "wght" && axes[0].minimum == 100 &&
               axes[0].defaultValue == 400 && axes[0].maximum == 900,
           "a CFF2 font's axes are listed");

    const auto standard = make(font, 0);
    expect(standard.status == VariableFontInstanceStatus::Instanced,
           "a CFF2 font is instanced: " + standard.detail);
    if (standard.status != VariableFontInstanceStatus::Instanced)
        return;
    const auto empty = outlineOf(standard.bytes, 0);
    expect(empty.read && empty.points.empty() && empty.width == 500 && empty.operators == "e",
           "a glyph with no outline keeps its advance and draws nothing");
    const auto rectangle = outlineOf(standard.bytes, 1);
    expect(rectangle.read && rectangle.width == 600 && rectangle.operators == "mllle" &&
               rectangle.points == Points{{100, 0}, {500, 0}, {500, 700}, {100, 700}},
           "the default instance is the default outline");
    const auto curved = outlineOf(standard.bytes, 2);
    expect(curved.read && curved.width == 400 && curved.operators == "mcle" &&
               curved.points == Points{{50, 50}, {150, 150}, {250, 250}, {350, 350}, {450, 350}},
           "hints are passed over and a subroutine's curve is drawn in place");

    // Weight 650 is half way up the region; 900 is its peak.
    const auto half = make(font, 0, {{"wght", 650}});
    const auto halfRectangle = outlineOf(half.bytes, 1);
    expect(halfRectangle.read && halfRectangle.width == 700 &&
               halfRectangle.points == Points{{100, 0}, {600, 0}, {600, 700}, {100, 700}},
           "half way into the region the outline and the advance take half the deltas");
    const auto bold = make(font, 0, {{"wght", 900}});
    const auto boldRectangle = outlineOf(bold.bytes, 1);
    expect(boldRectangle.read && boldRectangle.width == 800 &&
               boldRectangle.points == Points{{100, 0}, {700, 0}, {700, 700}, {100, 700}},
           "at the region's peak the outline and the advance take the whole deltas");
    expect(outlineOf(bold.bytes, 2).points == curved.points,
           "a glyph with no deltas does not move");
    const auto light = make(font, 0, {{"wght", 100}});
    expect(outlineOf(light.bytes, 1).points == rectangle.points,
           "below the default the region does not apply");
    expect(make(font, 1).bytes == bold.bytes && !bold.bytes.empty(),
           "a named instance is the coordinates it names");
    expect(make(font, 1, {{"wght", 650}}).bytes == half.bytes,
           "an axis value overrides the named instance");
    expect(make(font, 0, {{"wght", 900}}).bytes == bold.bytes,
           "instancing is deterministic, so the file digest is a stable identity");

    // The advance table and the font's bounds follow.
    const auto [hmtxAt, hmtxSize] = tableOf(bold.bytes, "hmtx");
    expect(hmtxSize == 12 && be(bold.bytes, hmtxAt + 4, 2) == 800 &&
               be(bold.bytes, hmtxAt + 6, 2) == 100 && be(bold.bytes, hmtxAt + 8, 2) == 400,
           "the advance table holds the instance's advances and left bearings");
    expect(tableOf(bold.bytes, "CFF2").second == 0 && tableOf(bold.bytes, "fvar").second == 0 &&
               tableOf(bold.bytes, "HVAR").second == 0 && tableOf(bold.bytes, "cmap").second != 0,
           "the instance is a static font: no variation tables, the other tables kept");
}

// The text rasterizer draws the instance: the rectangle is as wide as the weight makes it.
void theInstanceDraws() {
    const auto font = variableFont();
    const auto parameters = bloom::render::TextRasterParameters::create(100.0, 100.0);
    expect(static_cast<bool>(parameters), "raster parameters are valid");
    if (!parameters)
        return;
    const auto widthOf = [&](const VariableFontInstance& instance) -> std::int64_t {
        if (instance.status != VariableFontInstanceStatus::Instanced)
            return -1;
        const bloom::render::TextFont face = bloom::render::ExternalFontFile{
            .bytes = std::make_shared<const Bytes>(instance.bytes), .contentDigest = {}, .faceIndex = 0};
        const auto drawn = bloom::render::TextCoverageBitmap::rasterizeText(
            face, "A", *parameters.value(), std::size_t{16} * 1024U * 1024U);
        if (!drawn || !drawn.value()->hasCoverage())
            return -1;
        // The inked columns: the rectangle's width in pixels, a tenth of its font units.
        std::int64_t first = -1;
        std::int64_t last = -1;
        for (std::uint32_t row = 0; row < drawn.value()->height(); ++row) {
            const auto pixels = drawn.value()->row(row);
            for (std::size_t column = 0; column < pixels.size(); ++column)
                if (pixels[column] > 127) {
                    const auto c = static_cast<std::int64_t>(column);
                    first = first < 0 ? c : std::min(first, c);
                    last = std::max(last, c);
                }
        }
        return first < 0 ? -1 : last - first + 1;
    };
    const auto regular = widthOf(make(font, 0));
    const auto bold = widthOf(make(font, 0, {{"wght", 900}}));
    expect(regular >= 39 && regular <= 41,
           "the default instance draws a 400-unit rectangle 40 pixels wide: " +
               std::to_string(regular));
    expect(bold >= 59 && bold <= 61,
           "weight 900 draws it 60 pixels wide: " + std::to_string(bold));
}

void damagedFontsAreLeftAlone() {
    auto font = variableFont();
    // The first charstring operator of glyph 1 becomes one a CFF2 charstring does not have.
    const auto [cffAt, cffSize] = tableOf(font, "CFF2");
    bool patched = false;
    for (std::size_t at = cffAt; at + 1 < cffAt + cffSize && !patched; ++at)
        if (font[at] == kRmoveto && font[at + 1] == 28) {
            font[at] = kEndchar;
            patched = true;
        }
    expect(patched, "the fixture's charstring is found");
    const auto damaged = make(font, 0, {{"wght", 900}});
    expect(damaged.status == VariableFontInstanceStatus::Unsupported && damaged.bytes.empty() &&
               !damaged.detail.empty(),
           "a charstring that cannot be run leaves the font as it is, and says why");
    auto truncated = variableFont();
    truncated.resize(truncated.size() - 40);
    expect(make(truncated, 0).status != VariableFontInstanceStatus::Instanced,
           "a cut-off font is not instanced");
}
} // namespace

int main() {
    outlinesFollowTheAxis();
    theInstanceDraws();
    damagedFontsAreLeftAlone();
    return failures == 0 ? 0 : 1;
}
