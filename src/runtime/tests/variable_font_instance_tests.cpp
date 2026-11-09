// Variable fonts: a point in the design space is applied to the outlines and advances, named
// instances and single axes included. The expected numbers were read from the same font instantiated
// by an independent reference implementation of OpenType font variations.

#include <bloom/runtime/variable_font_instance.hpp>

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
    return instantiateVariableFont(font, instance, settings);
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
} // namespace

int main() {
    axesAreListed();
    axisValuesChangeOutlinesAndAdvances();
    namedInstancesAndLimits();
    otherFilesAreLeftAlone();
    return failures == 0 ? 0 : 1;
}
