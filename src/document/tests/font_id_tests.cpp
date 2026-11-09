// The font ID a text node stores: encoding, decoding and bounds.

#include <bloom/document/font_id.hpp>

#include <iostream>
#include <string>

namespace {
using namespace bloom::document;
int failures = 0;
void expect(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << message << '\n';
    }
}

void roundTripsEverySource() {
    FontId google{FontSource::Google, "Roboto Flex", "Bold Italic", 700, true,
                  {{"opsz", 14.0}, {"wdth", 110.5}}, {}, "v31"};
    FontId system{FontSource::System, "Helvetica Neue", "Medium", 500, false, {},
                  "HelveticaNeue-Medium", {}};
    for (const auto& id : {google, system, defaultFontId()}) {
        const auto text = encodeFontId(id);
        const auto decoded = decodeFontId(text);
        expect(decoded && *decoded == id, "the font ID round-trips: " + text);
        expect(decoded && encodeFontId(*decoded) == text, "the text is canonical: " + text);
    }
    expect(encodeFontId(defaultFontId()) ==
               "source=builtin;family=DejaVu Sans;style=Book;weight=400;italic=false",
           "the default font ID text is stable");
}

void escapesSeparatorsInNames() {
    FontId id{FontSource::System, "Odd;Name=With|Marks:And%Sign", "Style;1", 400, false, {},
              "Odd-Name", {}};
    const auto text = encodeFontId(id);
    expect(text.find("Odd;Name") == std::string::npos, "separators inside a family are escaped");
    const auto decoded = decodeFontId(text);
    expect(decoded && *decoded == id, "an escaped family decodes to the same name");
}

void refusesWhatIsNotAFontId() {
    expect(!decodeFontId("").has_value(), "empty text is not a font ID");
    expect(!decodeFontId("3").has_value(), "an asset id is not a font ID");
    expect(!decodeFontId("DejaVu Sans|Book|abcd").has_value(), "a legacy reference is not one");
    expect(!decodeFontId("source=google;weight=400").has_value(), "a family is required");
    expect(!decodeFontId("source=elsewhere;family=A").has_value(), "the source is closed");
    expect(!decodeFontId("source=system;family=A;weight=0").has_value(), "weight is 1..1000");
    expect(!decodeFontId("source=system;family=A;weight=x").has_value(), "weight is a number");
    expect(!decodeFontId("source=system;family=A;axes=wdth:nan").has_value(),
           "axis values are finite numbers");
    expect(!decodeFontId("source=system;family=A;axes=toolong:1").has_value(),
           "axis tags are four letters");
    expect(!decodeFontId("source=system;family=A;axes=wght:1|wdth:2").has_value(),
           "axis tags are sorted");
    expect(!decodeFontId("source=system;family=A%2").has_value(), "escapes are complete");
}

void ignoresMembersFromANewerBloom() {
    const auto decoded = decodeFontId("source=google;family=A;future=1;weight=300");
    expect(decoded && decoded->family == "A" && decoded->weight == 300,
           "an unknown member does not stop an older Bloom opening the project");
}

} // namespace

int main() {
    roundTripsEverySource();
    escapesSeparatorsInNames();
    refusesWhatIsNotAFontId();
    ignoresMembersFromANewerBloom();
    return failures == 0 ? 0 : 1;
}
