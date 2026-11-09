// Opening a project that still has Font assets: each text font becomes a font ID, the Font assets
// are dropped, and the text keeps the font it drew in.

#include <bloom/document/asset.hpp>
#include <bloom/document/font_id.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/project/document_decode.hpp>

#include <iostream>
#include <string>

namespace {
using namespace bloom;
using namespace bloom::project;

int failures = 0;
void expect(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

document::AssetRecord fontAsset(const std::uint64_t id, const std::string& family,
                                const std::string& style, const bool builtin,
                                const std::uint32_t index = 0) {
    document::AssetRecord asset;
    asset.id = document::AssetId::fromRaw(id);
    asset.kind = document::AssetKind::Font;
    asset.fontFamily = family;
    asset.fontStyle = style;
    asset.fontIndex = index;
    asset.locator = builtin ? document::AssetLocator{"font", "builtin",
                                                      "embedded/" + std::to_string(index),
                                                      "font:embedded:" + family + "|" + style}
                            : document::AssetLocator{"font", "system", "/usr/share/fonts/x.ttf",
                                                      "file:///usr/share/fonts/x.ttf"};
    return asset;
}

document::ParameterRecord fontParameter(const std::uint64_t id, document::ParameterValue value) {
    return {document::ParameterId::fromRaw(id), std::string(document::kTextFontParameterSchemaKey),
            document::ConstantValueSource{std::move(value)}};
}

std::string storedFont(const DecodedDocumentEnvelope& envelope, const std::uint64_t id) {
    for (const auto& record : envelope.compositions.front().parameters)
        if (record.id == document::ParameterId::fromRaw(id))
            if (const auto* constant = std::get_if<document::ConstantValueSource>(&record.source))
                if (const auto* text = std::get_if<std::string>(&constant->value))
                    return *text;
    return {};
}

void oldProjectsKeepTheirFonts() {
    DecodedDocumentEnvelope envelope;
    envelope.assets.push_back(fontAsset(3, "DejaVu Sans", "Book", true, 0));
    envelope.assets.push_back(fontAsset(4, "Inter", "SemiBold", true, 3));
    envelope.assets.push_back(fontAsset(5, "Helvetica Neue", "Bold Italic", false));
    envelope.assets.push_back(fontAsset(6, "Not Referenced", "Regular", false));
    document::AssetRecord image;
    image.id = document::AssetId::fromRaw(9);
    image.kind = document::AssetKind::Image;
    envelope.assets.push_back(image);
    DecodedComposition composition;
    composition.parameters = {
        fontParameter(10, std::string("3")),
        fontParameter(11, std::string("4")),
        fontParameter(12, std::string("5")),
        fontParameter(13, std::int64_t{2}),
        fontParameter(14, std::string("Inter|Medium|97ad806f526e41546d46365bb3a393145f75b7b1568913db74549ad8b8dba872")),
        fontParameter(15, std::string("Futura|Condensed Medium|0000000000000000000000000000000000000000000000000000000000000000")),
        fontParameter(16, std::string("99")),
        fontParameter(17, document::encodeFontId(document::defaultFontId())),
    };
    envelope.compositions.push_back(std::move(composition));

    migrateLegacyFontAssets(envelope);

    expect(envelope.assets.size() == 1 && envelope.assets.front().kind == document::AssetKind::Image,
           "the Font assets are dropped and other assets stay");
    const auto id = [&](const std::uint64_t parameter) {
        return document::decodeFontId(storedFont(envelope, parameter));
    };
    expect(id(10) && id(10)->source == document::FontSource::Builtin && id(10)->family == "DejaVu Sans" &&
               id(10)->style == "Book",
           "an embedded DejaVu asset becomes the built-in face");
    expect(id(11) && id(11)->source == document::FontSource::Builtin && id(11)->family == "Inter" &&
               id(11)->style == "SemiBold" && id(11)->weight == 600,
           "an embedded Inter asset keeps its weight");
    expect(id(12) && id(12)->source == document::FontSource::System &&
               id(12)->family == "Helvetica Neue" && id(12)->style == "Bold Italic" &&
               id(12)->weight == 700 && id(12)->italic,
           "a system font asset becomes a system font ID with the weight and slope its style states");
    expect(id(13) && id(13)->family == "Inter" && id(13)->style == "Medium",
           "a face number of the first font-aware documents becomes the same built-in face");
    expect(id(14) && id(14)->source == document::FontSource::Builtin && id(14)->style == "Medium",
           "a family|style|digest reference naming an embedded digest becomes that built-in face");
    expect(id(15) && id(15)->source == document::FontSource::System && id(15)->family == "Futura",
           "a reference to an installed font becomes a system font ID");
    expect(id(16) && *id(16) == document::defaultFontId(),
           "a reference that names no Font asset drew in DejaVu Sans, and still does");
    expect(storedFont(envelope, 17) == document::encodeFontId(document::defaultFontId()),
           "a font ID is left alone");

    const auto once = envelope;
    migrateLegacyFontAssets(envelope);
    expect(envelope == once, "migrating again changes nothing");
}
} // namespace

int main() {
    oldProjectsKeepTheirFonts();
    return failures == 0 ? 0 : 1;
}
