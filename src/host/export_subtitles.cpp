#include "export_subtitles.hpp"

#include <bloom/core/int128.hpp>
#include <bloom/document/cue_markup.hpp>
#include <bloom/host/publication_coordinator.hpp>
#include <bloom/runtime/font_fetch.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/subtitle_tracks.hpp>
#include <bloom/subtitles/subtitles.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <map>
#include <span>
#include <utility>

namespace bloom::host::detail {
namespace {

using document::ExportSubtitleFormat;
using media::provider::SubtitleCueV1;
using media::provider::SubtitleTrackV1;

// ISO 639-1 to ISO 639-2/T, the languages a two-letter primary subtag names (with BCP 47's
// deprecated iw, in and ji).
constexpr std::array<std::pair<std::string_view, std::string_view>, 187> kTwoLetter{{
    {"aa", "aar"}, {"ab", "abk"}, {"ae", "ave"}, {"af", "afr"}, {"ak", "aka"}, {"am", "amh"},
    {"an", "arg"}, {"ar", "ara"}, {"as", "asm"}, {"av", "ava"}, {"ay", "aym"}, {"az", "aze"},
    {"ba", "bak"}, {"be", "bel"}, {"bg", "bul"}, {"bh", "bih"}, {"bi", "bis"}, {"bm", "bam"},
    {"bn", "ben"}, {"bo", "bod"}, {"br", "bre"}, {"bs", "bos"}, {"ca", "cat"}, {"ce", "che"},
    {"ch", "cha"}, {"co", "cos"}, {"cr", "cre"}, {"cs", "ces"}, {"cu", "chu"}, {"cv", "chv"},
    {"cy", "cym"}, {"da", "dan"}, {"de", "deu"}, {"dv", "div"}, {"dz", "dzo"}, {"ee", "ewe"},
    {"el", "ell"}, {"en", "eng"}, {"eo", "epo"}, {"es", "spa"}, {"et", "est"}, {"eu", "eus"},
    {"fa", "fas"}, {"ff", "ful"}, {"fi", "fin"}, {"fj", "fij"}, {"fo", "fao"}, {"fr", "fra"},
    {"fy", "fry"}, {"ga", "gle"}, {"gd", "gla"}, {"gl", "glg"}, {"gn", "grn"}, {"gu", "guj"},
    {"gv", "glv"}, {"ha", "hau"}, {"he", "heb"}, {"hi", "hin"}, {"ho", "hmo"}, {"hr", "hrv"},
    {"ht", "hat"}, {"hu", "hun"}, {"hy", "hye"}, {"hz", "her"}, {"ia", "ina"}, {"id", "ind"},
    {"ie", "ile"}, {"ig", "ibo"}, {"ii", "iii"}, {"ik", "ipk"}, {"in", "ind"}, {"io", "ido"},
    {"is", "isl"}, {"it", "ita"}, {"iu", "iku"}, {"iw", "heb"}, {"ja", "jpn"}, {"ji", "yid"},
    {"jv", "jav"}, {"ka", "kat"}, {"kg", "kon"}, {"ki", "kik"}, {"kj", "kua"}, {"kk", "kaz"},
    {"kl", "kal"}, {"km", "khm"}, {"kn", "kan"}, {"ko", "kor"}, {"kr", "kau"}, {"ks", "kas"},
    {"ku", "kur"}, {"kv", "kom"}, {"kw", "cor"}, {"ky", "kir"}, {"la", "lat"}, {"lb", "ltz"},
    {"lg", "lug"}, {"li", "lim"}, {"ln", "lin"}, {"lo", "lao"}, {"lt", "lit"}, {"lu", "lub"},
    {"lv", "lav"}, {"mg", "mlg"}, {"mh", "mah"}, {"mi", "mri"}, {"mk", "mkd"}, {"ml", "mal"},
    {"mn", "mon"}, {"mr", "mar"}, {"ms", "msa"}, {"mt", "mlt"}, {"my", "mya"}, {"na", "nau"},
    {"nb", "nob"}, {"nd", "nde"}, {"ne", "nep"}, {"ng", "ndo"}, {"nl", "nld"}, {"nn", "nno"},
    {"no", "nor"}, {"nr", "nbl"}, {"nv", "nav"}, {"ny", "nya"}, {"oc", "oci"}, {"oj", "oji"},
    {"om", "orm"}, {"or", "ori"}, {"os", "oss"}, {"pa", "pan"}, {"pi", "pli"}, {"pl", "pol"},
    {"ps", "pus"}, {"pt", "por"}, {"qu", "que"}, {"rm", "roh"}, {"rn", "run"}, {"ro", "ron"},
    {"ru", "rus"}, {"rw", "kin"}, {"sa", "san"}, {"sc", "srd"}, {"sd", "snd"}, {"se", "sme"},
    {"sg", "sag"}, {"si", "sin"}, {"sk", "slk"}, {"sl", "slv"}, {"sm", "smo"}, {"sn", "sna"},
    {"so", "som"}, {"sq", "sqi"}, {"sr", "srp"}, {"ss", "ssw"}, {"st", "sot"}, {"su", "sun"},
    {"sv", "swe"}, {"sw", "swa"}, {"ta", "tam"}, {"te", "tel"}, {"tg", "tgk"}, {"th", "tha"},
    {"ti", "tir"}, {"tk", "tuk"}, {"tl", "tgl"}, {"tn", "tsn"}, {"to", "ton"}, {"tr", "tur"},
    {"ts", "tso"}, {"tt", "tat"}, {"tw", "twi"}, {"ty", "tah"}, {"ug", "uig"}, {"uk", "ukr"},
    {"ur", "urd"}, {"uz", "uzb"}, {"ve", "ven"}, {"vi", "vie"}, {"vo", "vol"}, {"wa", "wln"},
    {"wo", "wol"}, {"xh", "xho"}, {"yi", "yid"}, {"yo", "yor"}, {"za", "zha"}, {"zh", "zho"},
    {"zu", "zul"},
}};

// The twenty languages whose ISO 639-2 bibliographic code differs from the terminology code.
constexpr std::array<std::pair<std::string_view, std::string_view>, 20> kBibliographic{{
    {"bod", "tib"}, {"ces", "cze"}, {"cym", "wel"}, {"deu", "ger"}, {"ell", "gre"},
    {"eus", "baq"}, {"fas", "per"}, {"fra", "fre"}, {"hye", "arm"}, {"isl", "ice"},
    {"kat", "geo"}, {"mkd", "mac"}, {"mri", "mao"}, {"msa", "may"}, {"mya", "bur"},
    {"nld", "dut"}, {"ron", "rum"}, {"slk", "slo"}, {"sqi", "alb"}, {"zho", "chi"},
}};

[[nodiscard]] std::string lower(std::string_view text) {
    std::string out(text);
    std::ranges::transform(
        out, out.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

[[nodiscard]] subtitles::Format writerFormat(const ExportSubtitleFormat format) {
    switch (format) {
    case ExportSubtitleFormat::Srt:
        return subtitles::Format::Srt;
    case ExportSubtitleFormat::WebVtt:
        return subtitles::Format::WebVtt;
    case ExportSubtitleFormat::Ttml:
        return subtitles::Format::Ttml;
    }
    return subtitles::Format::Srt;
}

[[nodiscard]] std::string_view formatLabel(const ExportSubtitleFormat format) {
    switch (format) {
    case ExportSubtitleFormat::Srt:
        return "SRT";
    case ExportSubtitleFormat::WebVtt:
        return "WebVTT";
    case ExportSubtitleFormat::Ttml:
        return "TTML";
    }
    return "SRT";
}

// The first frame at `rate` whose time is at or after `time` (time >= 0).
[[nodiscard]] std::optional<std::uint64_t> frameAtOrAfter(const core::RationalTime time,
                                                          const media::provider::Rational rate) {
    if (time.numerator() < 0 || rate.numerator <= 0 || rate.denominator <= 0)
        return std::nullopt;
    const auto numerator = static_cast<core::Int128>(time.numerator()) * rate.numerator;
    const auto denominator = static_cast<core::Int128>(time.denominator()) * rate.denominator;
    const auto frames = (numerator + denominator - 1) / denominator;
    if (frames > static_cast<core::Int128>(std::numeric_limits<std::int64_t>::max()))
        return std::nullopt;
    return static_cast<std::uint64_t>(frames);
}

// A track cue's frames: its edges are whole output frames.
[[nodiscard]] std::optional<std::pair<std::uint64_t, std::uint64_t>>
cueFrames(const document::Cue& cue, const media::provider::Rational rate) {
    const auto first = frameAtOrAfter(cue.start, rate);
    const auto end = frameAtOrAfter(cue.end, rate);
    if (!first || !end || *end <= *first)
        return std::nullopt;
    return std::pair{*first, *end};
}

// The WebVTT cue payload a one-cue file holds for `cue`: what a WebM WebVTT block carries.
[[nodiscard]] std::optional<std::string> webVttPayload(const document::CueListValue& list,
                                                       const document::CueHandle& cue) {
    document::CueListValue single;
    single.language = list.language;
    single.styles = list.styles;
    single.nextCueId = 2;
    auto copy = std::make_shared<document::Cue>(*cue);
    copy->id = 1;
    single.cues.push_back(std::move(copy));
    const auto written = subtitles::write(single, subtitles::Format::WebVtt);
    if (!written)
        return std::nullopt;
    const auto& text = written->text;
    const auto timing = text.find("-->");
    if (timing == std::string::npos)
        return std::nullopt;
    const auto start = text.find('\n', timing);
    if (start == std::string::npos)
        return std::nullopt;
    auto end = text.find("\n\n", start + 1);
    if (end == std::string::npos)
        end = text.size();
    auto payload = text.substr(start + 1, end - start - 1);
    while (!payload.empty() && payload.back() == '\n')
        payload.pop_back();
    return payload;
}

// The cues of a MOV or MP4 track, which may not overlap: cues shown together are joined, one
// line group per cue, into segments where the shown set does not change.
[[nodiscard]] std::vector<SubtitleCueV1> joined(const std::vector<SubtitleCueV1>& cues) {
    std::vector<std::uint64_t> edges;
    for (const auto& cue : cues) {
        edges.push_back(cue.first);
        edges.push_back(cue.end);
    }
    std::ranges::sort(edges);
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
    std::vector<SubtitleCueV1> out;
    for (std::size_t index = 0; index + 1 < edges.size(); ++index) {
        const auto first = edges[index], end = edges[index + 1];
        std::string text;
        for (const auto& cue : cues)
            if (cue.first <= first && end <= cue.end)
                text += (text.empty() ? "" : "\n") + cue.text;
        if (text.empty())
            continue;
        if (!out.empty() && out.back().end == first && out.back().text == text)
            out.back().end = end;
        else
            out.push_back({first, end, std::move(text)});
    }
    return out;
}

[[nodiscard]] ExportSubtitleFailure failed(const ExportFailureCode code, std::string diagnostic,
                                           std::string reason) {
    return {code, std::move(diagnostic), std::move(reason), false};
}

} // namespace

std::string iso6392Language(const std::string_view bcp47, const bool bibliographic) {
    const auto dash = bcp47.find_first_of("-_");
    const auto primary = lower(bcp47.substr(0, dash));
    std::string code;
    if (primary.size() == 3 &&
        std::ranges::all_of(primary, [](const char c) { return c >= 'a' && c <= 'z'; })) {
        code = primary;
    } else if (primary.size() == 2) {
        for (const auto& [two, three] : kTwoLetter)
            if (two == primary)
                code = three;
    }
    if (code.empty())
        return "und";
    if (bibliographic)
        for (const auto& [terminology, bibliographicCode] : kBibliographic)
            if (terminology == code)
                return std::string(bibliographicCode);
    if (!bibliographic)
        for (const auto& [terminology, bibliographicCode] : kBibliographic)
            if (bibliographicCode == code)
                return std::string(terminology);
    return code;
}

std::string sidecarLanguage(const std::string_view bcp47) {
    std::string out;
    for (const char c : bcp47)
        if (std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-')
            out.push_back(c);
    while (!out.empty() && out.front() == '-')
        out.erase(out.begin());
    while (!out.empty() && out.back() == '-')
        out.pop_back();
    return out.empty() ? std::string("und") : out;
}

std::filesystem::path sidecarPath(const std::filesystem::path& basePath,
                                  const std::string_view language,
                                  const std::optional<std::size_t> number,
                                  const ExportSubtitleFormat format) {
    auto name = basePath.stem().string() + "." + std::string(language);
    if (number)
        name += "." + std::to_string(*number);
    name += "." + std::string(document::exportSubtitleFormatToken(format));
    return basePath.parent_path() / name;
}

std::string sidecarDifference(const std::string_view text, const ExportSubtitleFormat format,
                              const std::vector<SubtitleCueV1>& expected,
                              const media::provider::Rational rate) {
    subtitles::LoadOptions options;
    options.format = writerFormat(format);
    const auto loaded = subtitles::load(text, options);
    if (const auto* refused = std::get_if<subtitles::LoadFailure>(&loaded))
        return "it does not read back (" + refused->message + ")";
    const auto& cues = std::get<subtitles::LoadResult>(loaded).cues.cues;
    if (cues.size() != expected.size())
        return "it reads back with " + std::to_string(cues.size()) + " cues, the track has " +
               std::to_string(expected.size());
    for (std::size_t index = 0; index < cues.size(); ++index) {
        const auto frames = cueFrames(*cues[index], rate);
        const auto& want = expected[index];
        const auto cue = "cue " + std::to_string(index + 1) + " ";
        if (!frames || frames->first != want.first || frames->second != want.end)
            return cue + "reads back on other frames than the picture shows it";
        if (document::cue_markup::plainText(cues[index]->text) != want.text)
            return cue + "reads back with other text";
    }
    return {};
}

ExportSubtitlePlanOutcome
prepareExportSubtitles(const ExportSubtitleRequest& request,
                       const runtime::SnapshotCompiler& compiler,
                       const runtime::CancellationToken& cancellation,
                       const std::function<void(std::uint64_t, std::uint64_t)>& progress) {
    const auto* composition = request.snapshot.project().findComposition(request.composition);
    if (composition == nullptr)
        return failed(ExportFailureCode::CompositionMissing, "export.subtitles.composition",
                      "The job's composition no longer exists");
    const auto frameRate = composition->format().frameRate();
    const media::provider::Rational rate{frameRate.numerator(), frameRate.denominator()};
    runtime::SnapshotCompileRequest compile{.snapshot = request.snapshot,
                                            .compositionId = request.composition};
    compile.fontFetch = runtime::fonts::FontFetch::Wait;
    auto compiled = compiler.compile(compile, cancellation);
    if (compiled.status == runtime::SnapshotCompileStatus::Cancelled)
        return ExportSubtitleFailure{
            ExportFailureCode::Internal, "export.subtitles.cancelled", {}, true};
    if (compiled.plan == nullptr)
        return failed(ExportFailureCode::CompileFailed, "export.compile.failed",
                      "The composition could not be compiled for export. " +
                          runtime::describeCompileFailure(compiled, request.snapshot.project(),
                                                          request.composition));
    const auto outcome = runtime::collectSubtitleTracks(
        {compiled.plan, request.firstFrame, request.lastFrame, request.settings.nodes},
        request.snapshot.project(), cancellation, progress);
    if (const auto* refused = std::get_if<runtime::SubtitleTrackFailure>(&outcome)) {
        if (refused->cancelled)
            return ExportSubtitleFailure{
                ExportFailureCode::Internal, "export.subtitles.cancelled", {}, true};
        return failed(ExportFailureCode::EvaluationFailed, "export.subtitles.tracks",
                      "The subtitle tracks could not be prepared: " + refused->message);
    }
    const auto& found = std::get<runtime::SubtitleTracks>(outcome);
    ExportSubtitlePlan plan;
    plan.notes = found.notes;
    if (found.tracks.empty())
        plan.notes.push_back("No Subtitles node shows a cue in the exported range, so the output "
                             "has no subtitle track");
    // Sidecar names: a language shared by several tracks numbers each of them.
    std::map<std::string, std::size_t> shared;
    for (const auto& track : found.tracks)
        ++shared[sidecarLanguage(track.language)];
    std::map<std::string, std::size_t> numbered;
    const bool webm = request.format == document::ExportFormat::Vp9Webm ||
                      request.format == document::ExportFormat::Av1Webm;
    const bool embed =
        request.settings.embed && document::exportFormatEmbedsSubtitles(request.format);
    for (const auto& track : found.tracks) {
        if (cancellation.isCancellationRequested())
            return ExportSubtitleFailure{
                ExportFailureCode::Internal, "export.subtitles.cancelled", {}, true};
        const auto name = "Subtitles node " + std::to_string(track.node.value());
        // What the picture shows, cue by cue, in output frames.
        std::vector<SubtitleCueV1> shown;
        for (std::size_t index = 0; index < track.cues.cues.size(); ++index) {
            const auto frames = cueFrames(*track.cues.cues[index], rate);
            if (!frames)
                return failed(ExportFailureCode::Internal, "export.subtitles.frames",
                              name + "'s track has a cue that is not on whole frames");
            shown.push_back({frames->first, frames->second, track.plainText[index]});
        }
        plan.notes.push_back(name + " exports " + std::to_string(shown.size()) + " cues (" +
                             track.language + (track.title.empty() ? "" : ", " + track.title) +
                             ")");
        const auto language = sidecarLanguage(track.language);
        std::optional<std::size_t> number;
        if (shared[language] > 1)
            number = ++numbered[language];
        for (const auto format : request.settings.sidecars) {
            subtitles::WriteOptions options;
            options.frameRate = std::pair{static_cast<std::uint32_t>(frameRate.numerator()),
                                          static_cast<std::uint32_t>(frameRate.denominator())};
            const auto written = subtitles::write(track.cues, writerFormat(format), options);
            if (!written)
                return failed(ExportFailureCode::WriteFailed, "export.subtitles.write",
                              "The " + std::string(formatLabel(format)) + " file of " + name +
                                  " could not be written");
            for (const auto& report : written->reports)
                plan.notes.push_back(std::string(formatLabel(format)) + " of " + name + ": " +
                                     subtitles::describe(report));
            const auto difference = sidecarDifference(written->text, format, shown, rate);
            if (!difference.empty())
                return failed(ExportFailureCode::VerificationFailed, "export.subtitles.parse-back",
                              "The " + std::string(formatLabel(format)) + " file of " + name +
                                  " did not read back as its track (" + difference +
                                  "), so the export stopped before rendering.");
            ExportSubtitleSidecar sidecar;
            sidecar.node = track.node;
            sidecar.language = track.language;
            sidecar.title = track.title;
            sidecar.format = format;
            sidecar.target = sidecarPath(request.basePath, language, number, format);
            sidecar.text = written->text;
            sidecar.rate = rate;
            sidecar.expected = shown;
            std::error_code error;
            if (request.sidecarPolicy == platform::ArtifactOverwritePolicy::CreateOnly &&
                std::filesystem::exists(sidecar.target, error))
                return failed(ExportFailureCode::PublicationConflict, "export.subtitles.existing",
                              sidecar.target.filename().string() +
                                  " already exists and this output does not replace it, so the "
                                  "export stopped before rendering.");
            plan.sidecars.push_back(std::move(sidecar));
        }
        if (!embed)
            continue;
        SubtitleTrackV1 embedded;
        embedded.language = iso6392Language(track.language, webm);
        embedded.title = track.title;
        if (webm) {
            // WebM carries each cue's WebVTT payload, markup included; cues may overlap.
            for (std::size_t index = 0; index < track.cues.cues.size(); ++index) {
                auto payload = webVttPayload(track.cues, track.cues.cues[index]);
                if (!payload || payload->empty())
                    return failed(ExportFailureCode::WriteFailed, "export.subtitles.webvtt",
                                  name + "'s WebVTT track could not be written");
                embedded.cues.push_back(
                    {shown[index].first, shown[index].end, std::move(*payload)});
            }
            std::ranges::stable_sort(embedded.cues, {}, &SubtitleCueV1::first);
        } else {
            // 3GPP Timed Text carries plain text; cues shown together are joined.
            embedded.cues = joined(shown);
        }
        if (std::ranges::any_of(embedded.cues, [](const SubtitleCueV1& cue) {
                return cue.text.size() > media::provider::kSubtitleCueTextBytes;
            }))
            return failed(ExportFailureCode::WriteFailed, "export.subtitles.cue-size",
                          name + " has a cue longer than an embedded track carries (16 KB)");
        if (plan.embedded.size() >= media::provider::kSubtitleTracksMaximum)
            return failed(ExportFailureCode::WriteFailed, "export.subtitles.track-count",
                          "A movie carries at most 64 subtitle tracks");
        plan.embeddedFiles.push_back({.node = track.node,
                                      .language = embedded.language,
                                      .title = embedded.title,
                                      .sidecar = std::nullopt,
                                      .path = {},
                                      .cues = embedded.cues.size(),
                                      .bytes = 0,
                                      .digest = std::nullopt});
        plan.embedded.push_back(std::move(embedded));
    }
    return plan;
}

ExportSidecarPublication
publishExportSubtitleSidecars(PublicationCoordinator& coordinator,
                              platform::StagedArtifactCoordinator& artifacts,
                              const std::vector<ExportSubtitleSidecar>& sidecars,
                              const platform::ArtifactOverwritePolicy policy,
                              const runtime::CancellationToken& cancellation) {
    ExportSidecarPublication result;
    for (const auto& sidecar : sidecars) {
        const auto name = sidecar.target.filename().string();
        const auto fail = [&](ExportFailureCode code, std::string diagnostic, std::string reason) {
            result.failure = failed(code, std::move(diagnostic), std::move(reason));
            return result;
        };
        if (cancellation.isCancellationRequested()) {
            result.failure = ExportSubtitleFailure{
                ExportFailureCode::Internal, "export.subtitles.cancelled", {}, true};
            return result;
        }
        auto admitted = coordinator.admit();
        if (!admitted)
            return fail(ExportFailureCode::WriteFailed, "export.subtitles.admission",
                        name + " could not be written (publication is unavailable)");
        auto admission = std::move(admitted).takeAdmission();
        auto preflight = artifacts.preflight({.targetPath = sidecar.target,
                                              .overwritePolicy = policy,
                                              .expectedTarget = std::nullopt});
        if (!preflight)
            return fail(preflight.error() == platform::StagedArtifactError::OverwritePolicyConflict
                            ? ExportFailureCode::PublicationConflict
                            : ExportFailureCode::WriteFailed,
                        "export.subtitles.preflight",
                        preflight.error() == platform::StagedArtifactError::OverwritePolicyConflict
                            ? name + " appeared after the export started and this output does "
                                     "not replace it"
                            : name + " could not be written at the destination");
        auto target = std::move(preflight).takeTarget();
        auto registered = coordinator.registerTarget(std::move(admission), target.targetKey());
        if (!registered)
            return fail(ExportFailureCode::WriteFailed, "export.subtitles.registration",
                        name + " could not be written (its target is busy)");
        auto claim = std::move(registered).takeClaim();
        auto staged = artifacts.stage(std::move(target));
        if (!staged)
            return fail(ExportFailureCode::WriteFailed, "export.subtitles.stage",
                        name + " could not be written at the destination");
        auto lease = std::move(staged).takeLease();
        const auto bytes = std::as_bytes(std::span(sidecar.text));
        if (!lease.write(bytes) || !lease.finishWriting())
            return fail(ExportFailureCode::WriteFailed, "export.subtitles.write",
                        name + " could not be written at the destination");
        // Read the staged file back whole, then parse it and compare it with the track.
        std::string reread(sidecar.text.size(), '\0');
        std::uint64_t offset = 0;
        while (offset < reread.size()) {
            const auto read = lease.readForVerification(
                offset, std::as_writable_bytes(std::span(reread)).subspan(offset));
            if (!read || read.bytesRead == 0)
                break;
            offset += read.bytesRead;
        }
        const auto readBack =
            std::span(reinterpret_cast<const std::byte*>(reread.data()), reread.size());
        std::string difference;
        if (offset != reread.size() || reread != sidecar.text)
            difference = "the written bytes differ";
        else
            difference = sidecarDifference(reread, sidecar.format, sidecar.expected, sidecar.rate);
        if (!difference.empty()) {
            (void)lease.rejectVerification();
            return fail(ExportFailureCode::VerificationFailed, "export.subtitles.parse-back",
                        name + " did not read back as its track (" + difference +
                            "), so it was not published");
        }
        if (!lease.acceptVerification())
            return fail(ExportFailureCode::WriteFailed, "export.subtitles.verify",
                        name + " could not be verified");
        auto guard = claim.tryEnterPublication();
        if (guard.status() != PublicationGuardStatus::Entered)
            return fail(ExportFailureCode::PublicationConflict, "export.subtitles.superseded",
                        name + " was superseded by another write and was not published");
        auto entered = std::move(guard).takeGuard();
        const auto published = lease.publish(platform::PublicationDisposition::Proceed);
        if (!published.targetWasPublished())
            return fail(
                published.outcome ==
                        platform::StagedArtifactPublicationOutcome::ExternalModificationConflict
                    ? ExportFailureCode::PublicationConflict
                    : ExportFailureCode::WriteFailed,
                "export.subtitles.publication",
                name + " could not be put in place at the destination");
        result.files.push_back({.node = sidecar.node,
                                .language = sidecar.language,
                                .title = sidecar.title,
                                .sidecar = sidecar.format,
                                .path = sidecar.target,
                                .cues = sidecar.expected.size(),
                                .bytes = readBack.size(),
                                .digest = core::Sha256Hasher::hash(readBack)});
    }
    return result;
}

} // namespace bloom::host::detail
