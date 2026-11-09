// Font resolution: the order (installed system font, per-user cache, Google download, fallback), the
// Google stylesheet and index, offline and missing-font behavior, the version-changed notice, and a
// shared project opening on a machine that has the font nowhere.

#include <bloom/runtime/font_resolver.hpp>
#include <bloom/runtime/google_catalogue.hpp>
#include <bloom/runtime/google_fonts.hpp>
#include <bloom/runtime/headless_font_transport.hpp>
#include <bloom/runtime/variable_font_instance.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

namespace {
using namespace bloom;
using namespace bloom::runtime::fonts;
using namespace std::chrono_literals;

int failures = 0;
void expect(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

const std::filesystem::path kFixtures{BLOOM_FONT_FIXTURE_DIRECTORY};

std::vector<std::uint8_t> readBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

// A transport that answers like the stylesheet endpoint and the font file host.
class FakeTransport final : public FontTransport {
  public:
    FontHttpResult get(const std::string& url, std::uint64_t,
                       const std::function<bool()>& cancelled,
                       const std::function<void(std::uint64_t, std::uint64_t)>& progress) override {
        requests.fetch_add(1);
        {
            std::lock_guard lock(mutex);
            urls.push_back(url);
        }
        if (offline) {
            FontHttpResult result;
            result.status = FontHttpStatus::Offline;
            return result;
        }
        if (block) {
            while (!cancelled())
                std::this_thread::sleep_for(5ms);
            FontHttpResult result;
            result.status = FontHttpStatus::Cancelled;
            return result;
        }
        FontHttpResult result;
        result.status = FontHttpStatus::Ok;
        if (url.find("family=Unknown+Family") != std::string::npos) {
            result.status = FontHttpStatus::HttpError;
            result.httpStatus = 400;
            return result;
        }
        if (url.starts_with("https://fonts.googleapis.com/css2")) {
            const std::string css =
                "@font-face {\n  font-family: 'Test Family';\n  font-style: normal;\n"
                "  font-weight: 700;\n  src: url(https://fonts.gstatic.com/s/testfamily/" +
                version + "/face700.ttf) format('truetype');\n}\n"
                "@font-face {\n  font-family: 'Test Family';\n  font-style: normal;\n"
                "  font-weight: 400;\n  src: url(https://fonts.gstatic.com/s/testfamily/" +
                version + "/face400.ttf) format('truetype');\n}\n"
                "@font-face {\n  font-family: 'Test Family';\n  font-style: normal;\n"
                "  font-weight: 100;\n  src: url(https://elsewhere.example/evil.ttf);\n}\n";
            result.body.assign(css.begin(), css.end());
            return result;
        }
        result.body = readBytes(kFixtures / (url.find("face700") != std::string::npos
                                                  ? "Inter-SemiBold.ttf"
                                                  : "Inter-Regular.ttf"));
        progress(result.body.size(), result.body.size());
        return result;
    }

    std::atomic<int> requests{0};
    std::atomic<bool> offline{false};
    std::atomic<bool> block{false};
    std::string version = "v7";
    std::mutex mutex;
    std::vector<std::string> urls;
};

struct Fixture final {
    std::filesystem::path cache;
    std::shared_ptr<FakeTransport> transport = std::make_shared<FakeTransport>();
    std::vector<FontNotice> notices;
    std::mutex noticeMutex;

    Fixture() {
        cache = std::filesystem::temp_directory_path() /
                ("bloom-font-resolver-" + std::to_string(std::chrono::steady_clock::now()
                                                              .time_since_epoch()
                                                              .count()));
    }
    ~Fixture() {
        std::error_code error;
        std::filesystem::remove_all(cache, error);
    }
    std::unique_ptr<FontResolver> make(const bool withTransport = true) {
        FontResolver::Options options;
        options.cacheDirectory = cache;
        if (withTransport)
            options.transport = transport;
        auto resolver = std::make_unique<FontResolver>(std::move(options));
        resolver->setNoticeSink([this](const FontNotice& notice) {
            std::lock_guard lock(noticeMutex);
            notices.push_back(notice);
        });
        platform::FontCatalogue installed;
        installed.status = platform::FontCatalogueStatus::Available;
        const std::array<std::filesystem::path, 1> directories{kFixtures};
        installed = platform::FontCatalogueProvider::enumerateDirectories(directories, {}, false);
        // The fixtures stand in for "Arial" too, so the sans fallback finds an installed family.
        auto arial = installed.faces.front();
        for (const auto& face : installed.faces)
            if (face.style == "Regular" && face.family == "Inter")
                arial = face;
        arial.family = "Arial";
        arial.style = "Regular";
        arial.postscriptName = "ArialMT";
        installed.faces.push_back(arial);
        resolver->systemFonts().install(std::move(installed));
        return resolver;
    }
    bool noticed(const FontNoticeKind kind) {
        std::lock_guard lock(noticeMutex);
        for (const auto& notice : notices)
            if (notice.kind == kind)
                return true;
        return false;
    }
};

document::FontId googleId(const std::string& family = "Test Family", const int weight = 700,
                          const std::string& version = {}) {
    return {document::FontSource::Google, family, weight == 700 ? "Bold" : "Regular", weight,
            false, {}, {}, version};
}

void waitFor(FontResolver& resolver, const std::uint64_t epoch) {
    for (int i = 0; i < 400 && resolver.epoch() == epoch; ++i)
        std::this_thread::sleep_for(5ms);
}

void catalogueAndStylesheet() {
    const auto& catalogue = googleCatalogue();
    expect(catalogue.families().size() > 1000, "the baked index lists the Google families");
    expect(!catalogue.info().indexDate.empty() && catalogue.info().sourceRevision.size() == 40,
           "the index records its date and source revision");
    const auto* roboto = catalogue.find("roboto");
    expect(roboto != nullptr && roboto->family == "Roboto" && !roboto->version.empty() &&
               roboto->category == FontCategory::Sans,
           "a family is found by name, case-insensitively, with its version and category");
    expect(catalogue.find("No Such Family 12345") == nullptr, "an unknown family is not indexed");
    const auto parsed = GoogleCatalogue::parse(
        "#index-date=2026-01-02\n#source-revision=abc\nFoo Mono\tmonospace\tv3\t400,700i\twght:100:900\n");
    expect(parsed.families().size() == 1 && parsed.families()[0].styles.size() == 2 &&
               parsed.families()[0].styles[1].italic && parsed.families()[0].axes.size() == 1 &&
               parsed.info().indexDate == "2026-01-02",
           "index text parses to family, styles, axes and header");

    const auto url = googleStylesheetUrl(googleId("Roboto Flex", 700), catalogue.find("Roboto Flex"));
    expect(url.find("family=Roboto+Flex") != std::string::npos &&
               url.find("wght@700") != std::string::npos,
           "a variable family is asked for one weight (a static instance): " + url);
    const auto staticUrl = googleStylesheetUrl(googleId("Not Indexed", 400), nullptr);
    expect(staticUrl.find(":ital,wght@0,400") != std::string::npos,
           "a family missing from the index is asked by name and weight: " + staticUrl);
    const auto subset = googleStylesheetUrl(googleId("Roboto", 400), catalogue.find("Roboto"), "Roboto");
    expect(subset.find("&text=Roboto") != std::string::npos, "a preview asks for the name subset");

    const auto faces = parseGoogleStylesheet(
        "@font-face {\n font-family: 'Roboto';\n font-style: italic;\n font-weight: 400;\n"
        " src: url(https://fonts.gstatic.com/s/roboto/v51/a.ttf) format('truetype');\n}\n"
        "@font-face {\n font-family: 'Roboto';\n font-style: normal;\n font-weight: 100 900;\n"
        " src: url(https://fonts.gstatic.com/s/roboto/v51/b.ttf) format('truetype');\n}\n"
        "@font-face {\n font-family: 'Roboto';\n font-style: normal;\n font-weight: 400;\n"
        " src: url(http://insecure.example/c.ttf);\n}\n");
    expect(faces.size() == 2 && faces[0].version == "v51" && faces[1].weightMaximum == 900,
           "a stylesheet reply yields faces and versions, and ignores another host");
    const auto* picked = pickGoogleStylesheetFace(faces, 700, false);
    expect(picked != nullptr && picked->url.ends_with("b.ttf"), "a range serves its weight");
    expect(googleVersionFromUrl("https://fonts.gstatic.com/l/font?kit=x&skey=y&v=v49") == "v49",
           "the subset address carries the version in its query");
}

void fallbackTableAndCategory() {
    expect(fallbackFamilies(FontCategory::Sans).front() == "Arial" &&
               fallbackFamilies(FontCategory::Sans).size() == 6,
           "the sans list is the documented one");
    expect(fallbackFamilies(FontCategory::Serif).front() == "Times New Roman" &&
               fallbackFamilies(FontCategory::Serif).back() == "Noto Serif",
           "the serif list is the documented one");
    expect(fallbackFamilies(FontCategory::Monospace).front() == "Courier New" &&
               fallbackFamilies(FontCategory::Monospace).size() == 5,
           "the monospace list is the documented one");
    expect(fallbackFamilies(FontCategory::Display).data() == fallbackFamilies(FontCategory::Sans).data() &&
               fallbackFamilies(FontCategory::Handwriting).data() ==
                   fallbackFamilies(FontCategory::Sans).data(),
           "anything else takes the sans list");
    expect(categoryForMissingFont({document::FontSource::System, "Playfair Display", "Bold", 700, false, {}, {}, {}}) ==
               googleCatalogue().find("Playfair Display")->category,
           "an indexed family uses the catalogue category");
    expect(categoryForMissingFont({document::FontSource::System, "Acme Typewriter Mono", "Regular", 400, false, {}, {}, {}}) ==
               FontCategory::Monospace,
           "a mono family is monospace");
    expect(categoryForMissingFont({document::FontSource::System, "Imaginary", "Serif Bold", 700, false, {}, {}, {}}) ==
               FontCategory::Serif,
           "a recorded serif style is serif");
    expect(categoryForMissingFont({document::FontSource::System, "Imaginary", "Bold", 700, false, {}, {}, {}}) ==
               FontCategory::Sans,
           "anything else is sans");
}

void systemFontWinsByPostscriptNameThenFamilyAndStyle() {
    Fixture fixture;
    auto resolver = fixture.make();
    document::FontId byName{document::FontSource::System, "Inter", "Whatever", 500, false, {},
                            "Inter-Medium", {}};
    auto resolution = resolver->resolve(byName, FontFetch::CachedOnly);
    expect(resolution.kind == FontResolutionKind::System && resolution.usedStyle == "Medium" &&
               resolution.bytes && !resolution.missing(),
           "the PostScript name finds the installed face first");
    document::FontId byStyle{document::FontSource::System, "inter", "semibold", 600, false, {}, {}, {}};
    resolution = resolver->resolve(byStyle, FontFetch::CachedOnly);
    expect(resolution.kind == FontResolutionKind::System && resolution.usedStyle == "SemiBold",
           "family and style find it next, ignoring case");
    document::FontId wrongWeight{document::FontSource::System, "Inter", "Heavy", 900, false, {}, {}, {}};
    resolution = resolver->resolve(wrongWeight, FontFetch::CachedOnly);
    expect(resolution.isFallback(),
           "a weight the family does not have is never answered by another weight");
    document::FontId builtin = document::defaultFontId();
    resolution = resolver->resolve(builtin, FontFetch::CachedOnly);
    expect(resolution.kind == FontResolutionKind::Builtin && resolution.builtinFace == 0,
           "the built-in face needs no file");
    expect(fixture.transport->requests.load() == 0, "an installed font causes no download");
}

void downloadsIntoTheCacheAndReusesIt() {
    Fixture fixture;
    {
        auto resolver = fixture.make();
        const auto resolution = resolver->resolve(googleId("Test Family", 700, "v7"), FontFetch::Wait);
        expect(resolution.kind == FontResolutionKind::Cache && resolution.bytes &&
                   resolution.familyVersion == "v7" && !resolution.versionChanged &&
                   resolution.diagnostic.empty(),
               "a Google font that is nowhere else is downloaded and used");
        expect(std::filesystem::exists(resolver->cachePathFor(googleId("Test Family", 700))),
               "the file lands in the per-user cache");
        expect(fixture.noticed(FontNoticeKind::DownloadStarted) &&
                   fixture.noticed(FontNoticeKind::DownloadFinished),
               "a download announces itself and its end");
        bool evilFetched = false;
        {
            std::lock_guard lock(fixture.transport->mutex);
            for (const auto& url : fixture.transport->urls)
                evilFetched = evilFetched || url.find("elsewhere.example") != std::string::npos;
        }
        expect(!evilFetched, "a stylesheet cannot send the download to another host");
    }
    // A shared project on a machine whose cache holds the font: no network involved.
    fixture.transport->requests = 0;
    auto offlineMachine = fixture.make(false);
    const auto resolution = offlineMachine->resolve(googleId("Test Family", 700, "v7"), FontFetch::CachedOnly);
    expect(resolution.kind == FontResolutionKind::Cache && fixture.transport->requests.load() == 0,
           "the cache answers without any network");
}

void sharedProjectOnAMachineWithoutTheFont() {
    Fixture fixture;
    auto resolver = fixture.make(false);
    const auto id = googleId("Test Family", 700, "v7");
    const auto resolution = resolver->resolve(id, FontFetch::Wait);
    expect(resolution.missing() && resolution.reason == FontMissingReason::DownloadUnavailable &&
               resolution.diagnostic.find("\"Test Family Bold\"") != std::string::npos &&
               resolution.diagnostic.find("Arial Regular") != std::string::npos,
           "a project opens with the font missing: the fallback is used and named: " +
               resolution.diagnostic);
    expect(resolution.bytes != nullptr, "the fallback is a real font, never blank");
    const auto status = resolver->status(id, false);
    expect(status.state == FontStatus::State::Missing, "the font reports as missing");
}

void offlineAndMissingFonts() {
    Fixture fixture;
    fixture.transport->offline = true;
    auto resolver = fixture.make();
    const auto id = googleId("Test Family", 700, "v7");
    auto resolution = resolver->resolve(id, FontFetch::Wait);
    expect(resolution.missing() && resolution.reason == FontMissingReason::Offline &&
               resolution.diagnostic.find("no network connection") != std::string::npos,
           "offline gives a clear fallback diagnostic: " + resolution.diagnostic);
    expect(fixture.noticed(FontNoticeKind::DownloadFailed), "the failure is announced once");
    // The recorded font ID is untouched; once the network returns the same ID resolves.
    fixture.transport->offline = false;
    const auto again = resolver->status(id, false);
    expect(again.state == FontStatus::State::Missing, "a recent failure is remembered briefly");
    auto second = fixture.make();
    resolution = second->resolve(id, FontFetch::Wait);
    expect(resolution.kind == FontResolutionKind::Cache, "installing or reconnecting restores it");

    // Not on Google and not installed.
    document::FontId unknown{document::FontSource::System, "Imaginary Roman Serif", "Regular", 400,
                             false, {}, "Imaginary-Regular", {}};
    resolution = second->resolve(unknown, FontFetch::Wait);
    expect(resolution.missing() && resolution.reason == FontMissingReason::NotInstalled &&
               resolution.diagnostic.find("Imaginary Roman Serif") != std::string::npos,
           "an unknown system font names itself in the diagnostic");
    document::FontId unknownGoogle{document::FontSource::Google, "Unknown Family", "Regular", 400,
                                   false, {}, {}, {}};
    resolution = second->resolve(unknownGoogle, FontFetch::Wait);
    expect(resolution.missing() && resolution.reason == FontMissingReason::UnknownFamily,
           "a Google family the server does not know is missing, not blank");
}

void viewerShowsFallbackUntilTheFontArrives() {
    Fixture fixture;
    auto resolver = fixture.make();
    const auto id = googleId("Test Family", 700, "v7");
    const auto before = resolver->epoch();
    const auto first = resolver->resolve(id, FontFetch::Background);
    expect(first.pendingDownload() && !first.missing() && first.bytes != nullptr,
           "the Viewer shows the fallback, not a wrong font labelled right, while it downloads");
    waitFor(*resolver, before);
    expect(resolver->epoch() > before, "the arrival bumps the epoch so plans and rows rebuild");
    const auto second = resolver->resolve(id, FontFetch::Background);
    expect(second.kind == FontResolutionKind::Cache, "after it arrives the real font is used");
}

void versionChangedUsesTheLatestAndAnnounces() {
    Fixture fixture;
    fixture.transport->version = "v9";
    auto resolver = fixture.make();
    const auto resolution = resolver->resolve(googleId("Test Family", 700, "v7"), FontFetch::Wait);
    expect(resolution.kind == FontResolutionKind::Cache && resolution.versionChanged &&
               resolution.familyVersion == "v9",
           "the downloaded version is used and flagged as changed");
    expect(fixture.noticed(FontNoticeKind::VersionChanged), "a version-changed notice is posted");
    const auto before = fixture.notices.size();
    (void)resolver->resolve(googleId("Test Family", 700, "v7"), FontFetch::Wait);
    expect(fixture.notices.size() == before, "the same change is not announced on every render");
}

void systemFontMissingButOnGoogleIsFetched() {
    Fixture fixture;
    auto resolver = fixture.make();
    // "Roboto" is in the baked index; the fake serves a face for any indexed family.
    document::FontId id{document::FontSource::System, "Roboto", "Regular", 400, false, {}, "Roboto-Regular", {}};
    const auto resolution = resolver->resolve(id, FontFetch::Wait);
    expect(resolution.kind == FontResolutionKind::Cache && resolution.fetchedInsteadOfSystem,
           "a missing system font that exists on Google is fetched from Google");
    expect(fixture.noticed(FontNoticeKind::FetchedInsteadOfSystem),
           "and the substitution is announced");
}

void cancellationStopsTheWait() {
    Fixture fixture;
    fixture.transport->block = true;
    auto resolver = fixture.make();
    std::atomic<bool> cancel{false};
    std::thread canceller([&] {
        std::this_thread::sleep_for(60ms);
        cancel = true;
    });
    const auto resolution =
        resolver->resolve(googleId("Test Family", 700), FontFetch::Wait, [&] { return cancel.load(); });
    canceller.join();
    expect(resolution.missing() && resolution.reason == FontMissingReason::Cancelled,
           "cancelling an export stops waiting for a download");
    resolver->cancelDownloads();
}

void previewSubsetIsFetchedOnlyOnRequest() {
    Fixture fixture;
    auto resolver = fixture.make();
    expect(resolver->previewSubset("Test Family", false) == nullptr &&
               fixture.transport->requests.load() == 0,
           "asking without a request fetches nothing");
    const auto before = resolver->previewEpoch();
    expect(resolver->previewSubset("Test Family", true) == nullptr,
           "a row shows the interface font until its subset arrives");
    for (int i = 0; i < 400 && resolver->previewEpoch() == before; ++i)
        std::this_thread::sleep_for(5ms);
    expect(resolver->epoch() == 0, "a name subset does not invalidate compiled plans");
    expect(resolver->previewSubset("Test Family", false) != nullptr, "then the subset is cached");
    {
        std::lock_guard lock(fixture.transport->mutex);
        bool text = false;
        for (const auto& url : fixture.transport->urls)
            text = text || url.find("&text=Test%20Family") != std::string::npos;
        expect(text, "the subset request carries only the family name");
    }
}
// The command-line tools, MCP and Python download through the same resolver and cache. Nothing
// here reaches the network: the address refuses connections.
void headlessTransportFailsOfflineAndCancels() {
    const auto transport = makeHeadlessFontTransport();
#if defined(__linux__)
    if (!transport) {
        std::cerr << "SKIP: the system HTTP client is not installed here\n";
        return;
    }
    const auto refused = transport->get("https://127.0.0.1:1/font", 1 << 20, [] { return false; }, {});
    expect(refused.status == FontHttpStatus::Offline && refused.body.empty(),
           "a refused connection is an offline error");
    const auto cancelled = transport->get("https://127.0.0.1:1/font", 1 << 20, [] { return true; }, {});
    expect(cancelled.status == FontHttpStatus::Cancelled, "a cancelled transfer says so");

    // Through the resolver: the wait ends in the fallback with the existing offline diagnostic.
    const auto root = std::filesystem::temp_directory_path() / "bloom-font-headless-test";
    std::filesystem::remove_all(root);
    FontResolver::Options options;
    options.cacheDirectory = root;
    options.transport = transport;
    options.stylesheetEndpoint = "https://127.0.0.1:1/css2";
    FontResolver resolver(std::move(options));
    const auto resolution = resolver.resolve(googleId("Test Family", 700, "v7"), FontFetch::Wait);
    expect(resolution.missing() && resolution.reason == FontMissingReason::Offline &&
               resolution.diagnostic.find("Test Family") != std::string::npos,
           "offline ends in the fallback with the diagnostic: " + resolution.diagnostic);
    expect(!std::filesystem::exists(resolver.cachePathFor(googleId("Test Family", 700, "v7"))),
           "nothing is cached for a failed download");
    std::filesystem::remove_all(root);
#else
    expect(!transport, "where no system client is wired the transport is absent");
#endif
}

// An installed variable font: the named instance in the face index and the font ID's recorded axis
// values both take effect, so the resolved file is the instance, and its digest is its identity.
void systemVariableFontAppliesInstanceAndAxes() {
    const auto variablePath = kFixtures / "InterVariable.ttf";
    const auto variableBytes = readBytes(variablePath);
    platform::FontCatalogue installed;
    installed.status = platform::FontCatalogueStatus::Available;
    platform::FontFace bold;
    bold.family = "Inter Variable";
    bold.style = "Bold";
    bold.weight = 700;
    bold.path = variablePath;
    bold.faceIndex = 7U << 16U; // The seventh named instance (weight 700).
    bold.postscriptName = "InterVariable-Bold";
    installed.faces.push_back(bold);
    platform::FontFace staticFace;
    staticFace.family = "Inter Static";
    staticFace.style = "Regular";
    staticFace.path = kFixtures / "Inter-Regular.ttf";
    staticFace.faceIndex = 1U << 16U; // An instance index on a font that has none.
    installed.faces.push_back(staticFace);
    FontResolver resolver;
    resolver.systemFonts().install(std::move(installed));

    document::FontId id{document::FontSource::System, "Inter Variable", "Bold", 700, false, {},
                        "InterVariable-Bold", {}};
    const auto plain = resolver.resolve(id, FontFetch::CachedOnly);
    const auto expectedBold = instantiateVariableFont(variableBytes, 7, {});
    expect(plain.kind == FontResolutionKind::System && plain.faceIndex == 0 && plain.bytes != nullptr &&
               *plain.bytes == expectedBold.bytes,
           "a named instance of an installed variable font resolves to that instance");

    id.axes = {{"opsz", 32.0}};
    const std::vector<VariableFontAxisSetting> settings{{"opsz", 32.0}};
    const auto withAxis = resolver.resolve(id, FontFetch::CachedOnly);
    const auto expectedAxis = instantiateVariableFont(variableBytes, 7, settings);
    expect(withAxis.kind == FontResolutionKind::System && withAxis.bytes != nullptr &&
               *withAxis.bytes == expectedAxis.bytes && !(withAxis.digest == plain.digest),
           "the recorded axis values change the resolved font and its identity");
    const auto again = resolver.resolve(id, FontFetch::CachedOnly);
    expect(again.bytes == withAxis.bytes, "the instance is reused, not rebuilt");

    document::FontId plainStatic{document::FontSource::System, "Inter Static", "Regular", 400, false,
                                 {}, {}, {}};
    const auto fixed = resolver.resolve(plainStatic, FontFetch::CachedOnly);
    expect(fixed.kind == FontResolutionKind::System && fixed.faceIndex == 0 && fixed.bytes != nullptr &&
               *fixed.bytes == readBytes(kFixtures / "Inter-Regular.ttf"),
           "an instance index on a static font drops to the font itself");
}

} // namespace

int main() {
    catalogueAndStylesheet();
    fallbackTableAndCategory();
    systemFontWinsByPostscriptNameThenFamilyAndStyle();
    downloadsIntoTheCacheAndReusesIt();
    sharedProjectOnAMachineWithoutTheFont();
    offlineAndMissingFonts();
    viewerShowsFallbackUntilTheFontArrives();
    versionChangedUsesTheLatestAndAnnounces();
    systemFontMissingButOnGoogleIsFetched();
    cancellationStopsTheWait();
    previewSubsetIsFetchedOnlyOnRequest();
    headlessTransportFailsOfflineAndCancels();
    systemVariableFontAppliesInstanceAndAxes();
    return failures == 0 ? 0 : 1;
}
