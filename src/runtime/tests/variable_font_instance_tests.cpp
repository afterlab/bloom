// Variable fonts: a point in the design space is applied to the outlines and advances, named
// instances and single axes included. The expected numbers were read from the same font instantiated
// by an independent reference implementation of OpenType font variations.

#include <bloom/runtime/variable_font_instance.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace {
using namespace bloom::runtime::fonts;

int failures = 0;
void expect(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

std::vector<std::uint8_t> readFile(const std::string& name) {
    std::ifstream input(std::filesystem::path(BLOOM_FONT_FIXTURE_DIRECTORY) / name, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::uint32_t be(const std::vector<std::uint8_t>& d, const std::size_t at, const int bytes) {
    std::uint32_t v = 0;
    for (int i = 0; i < bytes; ++i)
        v = (v << 8U) | (at + static_cast<std::size_t>(i) < d.size() ? d[at + static_cast<std::size_t>(i)] : 0U);
    return v;
}
int s16(const std::vector<std::uint8_t>& d, const std::size_t at) {
    return static_cast<std::int16_t>(be(d, at, 2));
}

struct GlyphFacts final {
    int advance = 0;
    int xMin = 0;
    int yMin = 0;
    int xMax = 0;
    int yMax = 0;
    bool found = false;
};

// The advance and the bounds of one glyph read straight from the font's tables.
GlyphFacts glyphFacts(const std::vector<std::uint8_t>& font, const std::uint32_t glyph) {
    std::map<std::string, std::pair<std::size_t, std::size_t>> tables;
    const auto count = be(font, 4, 2);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t record = 12 + static_cast<std::size_t>(i) * 16;
        std::string tag;
        for (std::size_t c = 0; c < 4; ++c)
            tag += static_cast<char>(font[record + c]);
        tables[tag] = {be(font, record + 8, 4), be(font, record + 12, 4)};
    }
    GlyphFacts facts;
    if (!tables.contains("head") || !tables.contains("loca") || !tables.contains("glyf") ||
        !tables.contains("hmtx") || !tables.contains("hhea"))
        return facts;
    const bool longLoca = s16(font, tables["head"].first + 50) != 0;
    const auto locaAt = tables["loca"].first;
    const auto begin = longLoca ? be(font, locaAt + glyph * 4U, 4) : be(font, locaAt + glyph * 2U, 2) * 2U;
    const auto metrics = be(font, tables["hhea"].first + 34, 2);
    facts.advance = static_cast<int>(
        be(font, tables["hmtx"].first + static_cast<std::size_t>(std::min(glyph, metrics - 1U)) * 4U, 2));
    const auto glyphAt = tables["glyf"].first + begin;
    facts.xMin = s16(font, glyphAt + 2);
    facts.yMin = s16(font, glyphAt + 4);
    facts.xMax = s16(font, glyphAt + 6);
    facts.yMax = s16(font, glyphAt + 8);
    facts.found = true;
    return facts;
}

bool near(const GlyphFacts& facts, const int advance, const int xMin, const int yMin, const int xMax,
          const int yMax) {
    const auto close = [](const int a, const int b) { return std::abs(a - b) <= 1; };
    return facts.found && close(facts.advance, advance) && close(facts.xMin, xMin) &&
           close(facts.yMin, yMin) && close(facts.xMax, xMax) && close(facts.yMax, yMax);
}

constexpr std::uint32_t kGlyphH = 161;
constexpr std::uint32_t kGlyphEacute = 618; // A composite: a base letter and an accent.

VariableFontInstance make(const std::vector<std::uint8_t>& font, const std::uint32_t instance,
                          const std::vector<VariableFontAxisSetting>& settings = {}) {
    return instantiateVariableFont(font, 0, instance, settings);
}

void axesAreListed() {
    const auto axes = variableFontAxes(readFile("InterVariable.ttf"));
    expect(axes.size() == 2 && axes[0].tag == "opsz" && axes[0].minimum == 14 &&
               axes[0].maximum == 32 && axes[1].tag == "wght" && axes[1].minimum == 100 &&
               axes[1].defaultValue == 400 && axes[1].maximum == 900,
           "the font's axes are listed with their ranges");
    expect(variableFontAxes(readFile("Inter-Regular.ttf")).empty(), "a static font has no axes");
}

void axisValuesChangeOutlinesAndAdvances() {
    const auto font = readFile("InterVariable.ttf");
    const auto standard = make(font, 0);
    expect(standard.status == VariableFontInstanceStatus::Instanced,
           "the default instance is written as a static font");
    expect(near(glyphFacts(standard.bytes, kGlyphH), 1522, 180, 0, 1342, 1490),
           "the default instance keeps the default outline");

    const auto bold = make(font, 0, {{"wght", 700}});
    expect(near(glyphFacts(bold.bytes, kGlyphH), 1530, 135, 0, 1395, 1490),
           "wght 700 widens the stems and the advance");
    expect(near(glyphFacts(bold.bytes, kGlyphEacute), 1220, 81, -22, 1143, 1571),
           "a composite glyph follows its components' variation");
    const auto thin = make(font, 0, {{"wght", 100}});
    expect(near(glyphFacts(thin.bytes, kGlyphH), 1504, 232, 0, 1272, 1490),
           "wght 100 thins the outline");
    const auto optical = make(font, 0, {{"opsz", 32}});
    expect(near(glyphFacts(optical.bytes, kGlyphH), 1450, 148, 0, 1302, 1490),
           "an axis other than weight takes effect alone");
    expect(bold.bytes != standard.bytes && thin.bytes != bold.bytes && optical.bytes != standard.bytes,
           "each coordinate gives its own font file");
}

void namedInstancesAndLimits() {
    const auto font = readFile("InterVariable.ttf");
    // The seventh named instance is weight 700 at the smallest optical size.
    expect(make(font, 7).bytes == make(font, 0, {{"wght", 700}}).bytes,
           "a named instance is the coordinates it names");
    expect(make(font, 7, {{"opsz", 32}}).bytes == make(font, 0, {{"wght", 700}, {"opsz", 32}}).bytes,
           "an axis value overrides the named instance");
    expect(make(font, 0, {{"wght", 5000}}).bytes == make(font, 0, {{"wght", 900}}).bytes,
           "a value past the axis range is clamped to it");
    expect(make(font, 0, {{"XXXX", 3}}).bytes == make(font, 0).bytes,
           "an axis the font does not have is ignored");
    expect(make(font, 0, {{"wght", 700}}).bytes == make(font, 0, {{"wght", 700}}).bytes,
           "instancing is deterministic, so the file digest is a stable identity");
}

void otherFilesAreLeftAlone() {
    expect(make(readFile("Inter-Regular.ttf"), 0, {{"wght", 700}}).status ==
               VariableFontInstanceStatus::NotVariable,
           "a static font is used as it is");
    const std::vector<std::uint8_t> junk(64, 0x5A);
    expect(make(junk, 0).status == VariableFontInstanceStatus::Unsupported,
           "bytes that are not a font are not touched");
    auto truncated = readFile("InterVariable.ttf");
    truncated.resize(truncated.size() / 2);
    expect(make(truncated, 0).status != VariableFontInstanceStatus::Instanced,
           "a damaged font is not instantiated");
}

using Tables = std::map<std::string, std::vector<std::uint8_t>>;

Tables tablesOf(const std::vector<std::uint8_t>& font) {
    Tables tables;
    const auto count = be(font, 4, 2);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t record = 12 + static_cast<std::size_t>(i) * 16;
        std::string tag;
        for (std::size_t c = 0; c < 4; ++c)
            tag += static_cast<char>(font[record + c]);
        const auto at = be(font, record + 8, 4);
        const auto size = be(font, record + 12, 4);
        tables[tag].assign(font.begin() + at, font.begin() + at + size);
    }
    return tables;
}

void put(std::vector<std::uint8_t>& out, const std::uint32_t value, const int bytes) {
    for (int i = bytes - 1; i >= 0; --i)
        out.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFU));
}

// A single-font file of `tables`. The checksums are left zero: nothing here reads them.
std::vector<std::uint8_t> fontOf(const Tables& tables) {
    std::vector<std::uint8_t> out;
    put(out, 0x00010000, 4);
    put(out, static_cast<std::uint32_t>(tables.size()), 2);
    put(out, 0, 2);
    put(out, 0, 2);
    put(out, 0, 2);
    std::size_t at = 12 + tables.size() * 16;
    for (const auto& [tag, data] : tables) {
        for (const char c : tag)
            out.push_back(static_cast<std::uint8_t>(c));
        put(out, 0, 4);
        put(out, static_cast<std::uint32_t>(at), 4);
        put(out, static_cast<std::uint32_t>(data.size()), 4);
        at += (data.size() + 3U) & ~std::size_t{3};
    }
    for (const auto& [tag, data] : tables) {
        out.insert(out.end(), data.begin(), data.end());
        while (out.size() % 4 != 0)
            out.push_back(0);
    }
    return out;
}

// A collection file holding `fonts` in order: each font's bytes are placed whole and its table
// offsets moved, since a collection's offsets are from the start of the file.
std::vector<std::uint8_t> collectionOf(const std::vector<std::vector<std::uint8_t>>& fonts) {
    std::vector<std::uint8_t> out{'t', 't', 'c', 'f'};
    put(out, 0x00010000, 4);
    put(out, static_cast<std::uint32_t>(fonts.size()), 4);
    std::size_t at = 12 + fonts.size() * 4;
    std::vector<std::size_t> bases;
    for (const auto& font : fonts) {
        bases.push_back(at);
        put(out, static_cast<std::uint32_t>(at), 4);
        at += (font.size() + 3U) & ~std::size_t{3};
    }
    for (std::size_t f = 0; f < fonts.size(); ++f) {
        auto font = fonts[f];
        const auto count = be(font, 4, 2);
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::size_t field = 12 + static_cast<std::size_t>(i) * 16 + 8;
            const auto moved = be(font, field, 4) + static_cast<std::uint32_t>(bases[f]);
            for (std::size_t b = 0; b < 4; ++b)
                font[field + b] = static_cast<std::uint8_t>((moved >> (24U - 8U * b)) & 0xFFU);
        }
        out.insert(out.end(), font.begin(), font.end());
        while (out.size() % 4 != 0)
            out.push_back(0);
    }
    return out;
}

// #489: a variable font inside a collection is instanced like the same font in its own file.
void fontsInsideCollections() {
    const auto variable = readFile("InterVariable.ttf");
    const auto collection = collectionOf({readFile("Inter-Regular.ttf"), variable});
    const std::vector<VariableFontAxisSetting> bold{{"wght", 700}};
    expect(variableFontAxes(collection, 1).size() == 2 && variableFontAxes(collection, 0).empty(),
           "the axes of a font in a collection are listed, by its place in the file");
    const auto made = instantiateVariableFont(collection, 1, 0, bold);
    expect(made.status == VariableFontInstanceStatus::Instanced &&
               made.bytes == make(variable, 0, {{"wght", 700}}).bytes,
           "a font in a collection gives the instance its own file gives");
    expect(instantiateVariableFont(collection, 1, 7, {}).bytes == made.bytes,
           "and its named instances apply");
    expect(instantiateVariableFont(collection, 0, 0, bold).status ==
               VariableFontInstanceStatus::NotVariable,
           "a static font in a collection is used as it is");
    expect(instantiateVariableFont(collection, 2, 0, bold).status ==
               VariableFontInstanceStatus::Unsupported,
           "a font the collection does not have is not instanced");
    expect(instantiateVariableFont(variable, 1, 0, bold).status ==
               VariableFontInstanceStatus::Unsupported,
           "a single-font file has only its first font");
}

// The glyph's bytes in a font's glyph table.
std::vector<std::uint8_t> glyphData(const Tables& tables, const std::uint32_t glyph) {
    const auto& loca = tables.at("loca");
    const bool longLoca = s16(tables.at("head"), 50) != 0;
    const auto begin = longLoca ? be(loca, glyph * 4U, 4) : be(loca, glyph * 2U, 2) * 2U;
    const auto end = longLoca ? be(loca, glyph * 4U + 4U, 4) : be(loca, glyph * 2U + 2U, 2) * 2U;
    const auto& glyf = tables.at("glyf");
    return {glyf.begin() + begin, glyf.begin() + end};
}

// Where a simple glyph's instruction length is, and for a composite where its last component's
// flags are and where its components end.
struct GlyphLayout final {
    bool composite = false;
    std::size_t instructionLengthAt = 0;
    std::size_t lastFlagsAt = 0;
    std::size_t componentsEnd = 0;
};
GlyphLayout layoutOf(const std::vector<std::uint8_t>& glyph) {
    GlyphLayout layout;
    const int contours = s16(glyph, 0);
    if (contours >= 0) {
        layout.instructionLengthAt = 10 + static_cast<std::size_t>(contours) * 2U;
        return layout;
    }
    layout.composite = true;
    std::size_t at = 10;
    while (true) {
        const auto flags = be(glyph, at, 2);
        layout.lastFlagsAt = at;
        at += 4 + ((flags & 0x0001U) != 0 ? 4U : 2U);
        at += (flags & 0x0008U) != 0 ? 2U : (flags & 0x0040U) != 0 ? 4U : (flags & 0x0080U) != 0 ? 8U : 0U;
        if ((flags & 0x0020U) == 0)
            break;
    }
    layout.componentsEnd = at;
    return layout;
}

// #489: instancing keeps hinting. The glyphs' programs and the font's are carried as they are,
// and the control values the programs read follow the location.
void hintingIsKept() {
    auto tables = tablesOf(readFile("InterVariable.ttf"));
    const std::vector<std::uint8_t> program{0x01, 0x02, 0x03};
    // The font's own programs and four control values: 10, 20, 30, 40.
    tables["fpgm"] = {0xB0, 0x00};
    tables["prep"] = {0xB0, 0x01, 0x2B};
    auto& cvt = tables["cvt "];
    for (const int value : {10, 20, 30, 40})
        put(cvt, static_cast<std::uint32_t>(value) & 0xFFFFU, 2);
    // One tuple at the top of the weight axis (the second axis), listing every control value:
    // +100, 0, -50, +7.
    auto& cvar = tables["cvar"];
    put(cvar, 1, 2);
    put(cvar, 0, 2);
    put(cvar, 1, 2);       // one tuple
    put(cvar, 16, 2);      // data offset
    put(cvar, 10, 2);      // the tuple's data size
    put(cvar, 0xA000, 2);  // its own peak, its own point numbers
    put(cvar, 0, 2);       // opsz 0
    put(cvar, 0x4000, 2);  // wght 1.0
    cvar.push_back(0);     // every control value
    cvar.push_back(0x43);  // four deltas as words
    for (const int delta : {100, 0, -50, 7})
        put(cvar, static_cast<std::uint32_t>(delta) & 0xFFFFU, 2);

    // The glyph table again, with a program in a simple glyph and in a composite.
    const auto glyphCount = be(tables.at("maxp"), 4, 2);
    std::vector<std::uint8_t> glyf;
    std::vector<std::uint8_t> loca;
    bool fixtureFits = true;
    for (std::uint32_t g = 0; g < glyphCount; ++g) {
        put(loca, static_cast<std::uint32_t>(glyf.size()), 4);
        auto data = glyphData(tables, g);
        if (g == kGlyphH || g == kGlyphEacute) {
            const auto layout = layoutOf(data);
            if (g == kGlyphH && !layout.composite && be(data, layout.instructionLengthAt, 2) == 0) {
                data[layout.instructionLengthAt + 1] = static_cast<std::uint8_t>(program.size());
                data.insert(data.begin() + static_cast<std::ptrdiff_t>(layout.instructionLengthAt) + 2,
                            program.begin(), program.end());
            } else if (g == kGlyphEacute && layout.composite &&
                       (be(data, layout.lastFlagsAt, 2) & 0x0100U) == 0) {
                data[layout.lastFlagsAt] |= 0x01; // The high byte: a program follows.
                data.resize(layout.componentsEnd);
                put(data, static_cast<std::uint32_t>(program.size()), 2);
                data.insert(data.end(), program.begin(), program.end());
            } else {
                fixtureFits = false;
            }
            while (data.size() % 4 != 0)
                data.push_back(0);
        }
        glyf.insert(glyf.end(), data.begin(), data.end());
    }
    put(loca, static_cast<std::uint32_t>(glyf.size()), 4);
    expect(fixtureFits, "the fixture glyphs are a simple glyph and a composite without programs");
    tables["glyf"] = std::move(glyf);
    tables["loca"] = std::move(loca);
    tables["head"][50] = 0;
    tables["head"][51] = 1;
    const auto hinted = fontOf(tables);

    const auto heavy = make(hinted, 0, {{"wght", 900}});
    expect(heavy.status == VariableFontInstanceStatus::Instanced, "a hinted variable font is instanced");
    if (heavy.status != VariableFontInstanceStatus::Instanced)
        return;
    const auto made = tablesOf(heavy.bytes);
    expect(made.contains("fpgm") && made.at("fpgm") == tables.at("fpgm") && made.contains("prep") &&
               made.at("prep") == tables.at("prep"),
           "the font's hinting programs are carried as they are");
    expect(!made.contains("cvar"), "the instance holds no variation of the control values");
    const auto values = [](const Tables& from) {
        std::vector<int> out;
        if (from.contains("cvt "))
            for (std::size_t at = 0; at + 1 < from.at("cvt ").size(); at += 2)
                out.push_back(s16(from.at("cvt "), at));
        return out;
    };
    expect(values(made) == std::vector<int>{110, 20, -20, 47},
           "at the top of the weight axis the control values take their deltas");
    expect(values(tablesOf(make(hinted, 0).bytes)) == std::vector<int>{10, 20, 30, 40},
           "the default instance keeps the default control values");
    // Weight 100 is the other side of the default: the tuple does not apply.
    expect(values(tablesOf(make(hinted, 0, {{"wght", 100}}).bytes)) == std::vector<int>{10, 20, 30, 40},
           "a location outside the tuple's region leaves the control values alone");

    const auto simple = glyphData(made, kGlyphH);
    const auto simpleLayout = layoutOf(simple);
    expect(!simpleLayout.composite && be(simple, simpleLayout.instructionLengthAt, 2) == program.size() &&
               std::equal(program.begin(), program.end(),
                          simple.begin() + static_cast<std::ptrdiff_t>(simpleLayout.instructionLengthAt) + 2),
           "a simple glyph keeps its hinting program");
    const auto composite = glyphData(made, kGlyphEacute);
    const auto compositeLayout = layoutOf(composite);
    expect(compositeLayout.composite && (be(composite, compositeLayout.lastFlagsAt, 2) & 0x0100U) != 0 &&
               be(composite, compositeLayout.componentsEnd, 2) == program.size() &&
               std::equal(program.begin(), program.end(),
                          composite.begin() + static_cast<std::ptrdiff_t>(compositeLayout.componentsEnd) + 2),
           "a composite glyph keeps its hinting program after its last component");
    expect(near(glyphFacts(heavy.bytes, kGlyphH), glyphFacts(make(readFile("InterVariable.ttf"), 0,
                                                                  {{"wght", 900}})
                                                                 .bytes,
                                                             kGlyphH)
                                                      .advance,
                glyphFacts(heavy.bytes, kGlyphH).xMin, 0, glyphFacts(heavy.bytes, kGlyphH).xMax, 1490),
           "the hinted glyph's outline varies as the unhinted one's does");
}
} // namespace

int main() {
    axesAreListed();
    axisValuesChangeOutlinesAndAdvances();
    namedInstancesAndLimits();
    otherFilesAreLeftAlone();
    fontsInsideCollections();
    hintingIsKept();
    return failures == 0 ? 0 : 1;
}
