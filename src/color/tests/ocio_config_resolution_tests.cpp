// The supervised OCIO helper for external configs (issue #465; docs/architecture/
// color-management.md, "Supervised OCIO Execution"). A project-relative archive, an external
// archive and an external loose config resolve through bloom-color-worker, and what they produce
// is compared with the same config resolved by the reference: the in-process built-in for a copy
// of the ACES CG config, and OpenColorIO called directly by this test for a config with a LUT.

#include "lut_worker_protocol.hpp"
#include "ocio_config_protocol.hpp"
#include "ocio_config_session.hpp"

#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/color/ocio_config_resolution.hpp>
#include <bloom/color/ocio_content_revision.hpp>
#include <bloom/color/ocio_cpu_color_space_processor.hpp>
#include <bloom/color/ocio_cpu_display_frame.hpp>
#include <bloom/color/ocio_cpu_display_processor.hpp>
#include <bloom/color/ocio_cpu_input_processor.hpp>
#include <bloom/color/ocio_gpu_program.hpp>
#include <bloom/core/file_uri.hpp>

#include <OpenColorIO/OpenColorIO.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef __linux__
#include <csignal>
#include <unistd.h>
#endif

namespace {

using namespace bloom;
using namespace bloom::color;
namespace OCIO = OCIO_NAMESPACE;

int failures = 0;
void expect(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

#ifdef __linux__
// Everything below drives the confined helper, which only Linux has; main() says what the
// other targets check.
void write(const std::filesystem::path& path, const std::string& contents) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    file << contents;
}

std::vector<std::byte> read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(text.size());
    std::memcpy(bytes.data(), text.data(), text.size());
    return bytes;
}

OcioConfigLocation loose(const std::filesystem::path& root) {
    return {OcioConfigLocatorKind::ExternalLooseConfig, (root / "config.ocio").string(), {}};
}

// The current revision of a location; the resolution of an unknown revision reports it.
std::optional<core::Sha256Digest> revisionOf(const OcioConfigLocation& location,
                                             const std::string& working = {}) {
    const auto result = resolveOcioConfig(location, core::Sha256Digest{}, working);
    return result.recomputedRevision();
}

const std::array<std::array<float, 4>, 8> kPixels{{{0.0F, 0.0F, 0.0F, 1.0F},
                                                   {0.18F, 0.18F, 0.18F, 1.0F},
                                                   {1.0F, 0.5F, 0.25F, 1.0F},
                                                   {0.02F, 0.9F, 0.4F, 0.5F},
                                                   {4.0F, 2.0F, 1.0F, 1.0F},
                                                   {-0.1F, 0.05F, 0.3F, 1.0F},
                                                   {0.6F, 0.6F, 0.1F, 0.25F},
                                                   {0.33F, 0.66F, 0.99F, 1.0F}}};

// The display bytes a handle produces for the fixture pixels, through the public chunk call.
std::optional<std::vector<render::Rgba8>>
displayBytes(const PreparedCpuDisplayProcessorHandle& handle) {
    std::vector<render::Rgba32f> source;
    for (const auto& pixel : kPixels) {
        const auto alpha = pixel[3];
        const auto made = render::Rgba32f::fromPremultiplied(pixel[0] * alpha, pixel[1] * alpha,
                                                             pixel[2] * alpha, alpha);
        if (!made)
            return std::nullopt;
        source.push_back(*made.value());
    }
    std::vector<float> scratch(source.size() * 4);
    std::vector<render::Rgba8> target(source.size());
    if (applyBloomNeutralDisplayChunk(handle, source, scratch, target).has_value())
        return std::nullopt;
    return target;
}

// A copy of the ACES CG built-in as a loose config. It reads no file, so the helper's result
// must equal the built-in's in every respect but the revision.
void testLooseCopyMatchesBuiltIn(const std::filesystem::path& root) {
    const auto builtIn =
        OCIO::Config::CreateFromBuiltinConfig(std::string(kAcesCgV1BuiltinConfigName).c_str());
    std::ostringstream text;
    builtIn->serialize(text);
    write(root / "aces" / "config.ocio", text.str());
    const auto location = loose(root / "aces");
    const std::string working(kAcesCgV1SceneLinearColorSpaceId);

    const auto unknown = resolveOcioConfig(location, core::Sha256Digest{}, working);
    expect(unknown.outcome() == OcioBuiltInRegistryOutcome::Changed &&
               unknown.recomputedRevision().has_value(),
           "loose copy: an unknown revision is Changed and reports the current one");
    if (!unknown.recomputedRevision())
        return;
    const auto revision = *unknown.recomputedRevision();
    auto resolution = resolveOcioConfig(ocioConfigLocationText(location), revision, working);
    expect(resolution.ready(), "loose copy: the current revision resolves Ready (helper error " +
                                   std::string(ocioHelperErrorName(resolution.helperError())) +
                                   ")");
    const auto helper = std::move(resolution).takeResolved();
    const auto builtInRevision =
        ocioBuiltInContentRevision(OcioConfigLocatorKind::BloomBuiltIn, kAcesCgV1ConfigUri);
    auto reference = builtInRevision
                         ? resolveOcioBuiltIn(OcioConfigLocatorKind::BloomBuiltIn,
                                              kAcesCgV1ConfigUri, *builtInRevision, working)
                               .takeResolved()
                         : std::nullopt;
    expect(reference.has_value(), "loose copy: the built-in reference resolves");
    if (!helper || !reference)
        return;

    expect(helper->expectedRevision() == revision && revision != *builtInRevision,
           "loose copy: the product carries the loose revision, not the built-in's");
    expect(helper->processColorSpaceId() == reference->processColorSpaceId() &&
               helper->outputColorSpaceId() == reference->outputColorSpaceId() &&
               helper->displayName() == reference->displayName() &&
               helper->viewName() == reference->viewName() &&
               helper->configName() == reference->configName() &&
               helper->sRgbTextureColorSpaceId() == reference->sRgbTextureColorSpaceId() &&
               helper->rec709VideoColorSpaceId() == reference->rec709VideoColorSpaceId() &&
               helper->rec2100PqColorSpaceId() == reference->rec2100PqColorSpaceId() &&
               helper->rec2100HlgColorSpaceId() == reference->rec2100HlgColorSpaceId(),
           "loose copy: the helper describes the config as the registry does");
    expect(helper->colorSpaces() == reference->colorSpaces(),
           "loose copy: the same color spaces, synthesized inputs included");
    expect(std::ranges::equal(helper->displays(), reference->displays()),
           "loose copy: the same displays, views and defaults");

    // Display: every enumerated pair, bytes and authoring colours.
    std::size_t pairs = 0;
    for (const auto& entry : reference->displays()) {
        auto mine =
            buildCpuDisplayProcessorForView(*helper, entry.display, entry.view).takeHandle();
        auto theirs =
            buildCpuDisplayProcessorForView(*reference, entry.display, entry.view).takeHandle();
        expect(mine.has_value() == theirs.has_value(),
               "loose copy: a display processor builds where the reference's does: " +
                   entry.display + " / " + entry.view);
        if (!mine || !theirs)
            continue;
        ++pairs;
        expect(mine->lease().kind() == DisplayProcessorLeaseKind::HelperToken &&
                   theirs->lease().kind() == DisplayProcessorLeaseKind::InProcess,
               "loose copy: the helper's processor is a helper-token lease");
        expect(mine->provenance().processorCacheId == theirs->provenance().processorCacheId &&
                   mine->provenance().ocioVersion == theirs->provenance().ocioVersion,
               "loose copy: the same processor, by OpenColorIO's own cache id");
        const auto mineBytes = displayBytes(*mine);
        const auto theirBytes = displayBytes(*theirs);
        expect(mineBytes.has_value() && theirBytes.has_value() && *mineBytes == *theirBytes,
               "loose copy: identical display bytes for " + entry.display + " / " + entry.view);
        const core::Color4d colour{0.4, 0.2, 0.7, 0.5};
        expect(mine->referenceToDisplay(colour) == theirs->referenceToDisplay(colour) &&
                   mine->referenceToDisplayLinear(colour) ==
                       theirs->referenceToDisplayLinear(colour) &&
                   mine->displayToReference(colour) == theirs->displayToReference(colour),
               "loose copy: identical authoring colours for " + entry.display + " / " + entry.view);
    }
    expect(pairs >= 2, "loose copy: several display and view pairs were compared");

    // Color-space processors: every space to the working space.
    std::size_t spaces = 0;
    for (const auto& space : reference->colorSpaces()) {
        const auto mine = CpuColorSpaceProcessor::prepare(*helper, space.id, working);
        const auto theirs = CpuColorSpaceProcessor::prepare(*reference, space.id, working);
        expect(mine.error() == theirs.error(),
               "loose copy: the same color-space result for " + space.id);
        if (!mine || !theirs)
            continue;
        ++spaces;
        auto a = kPixels;
        auto b = kPixels;
        const bool appliedMine = mine.processor()->apply(a);
        const bool appliedTheirs = theirs.processor()->apply(b);
        expect(appliedMine == appliedTheirs &&
                   (!appliedMine || std::memcmp(a.data(), b.data(), sizeof(a)) == 0) &&
                   mine.processor()->isIdentity() == theirs.processor()->isIdentity(),
               "loose copy: bit-identical pixels from " + space.id);
    }
    expect(spaces >= 10, "loose copy: the config's color spaces were compared");
    expect(CpuColorSpaceProcessor::prepare(*helper, "No Such Space", working).error() ==
                   OcioColorSpaceProcessorError::MissingInputColorSpace &&
               CpuColorSpaceProcessor::prepare(*helper, working, "Raw").error() ==
                   CpuColorSpaceProcessor::prepare(*reference, working, "Raw").error(),
           "loose copy: a missing or data space is refused with the reference's reason");

    const auto mineInput = CpuInputProcessor::prepare(*helper);
    const auto theirInput = CpuInputProcessor::prepare(*reference);
    expect(mineInput && theirInput, "loose copy: the input processor prepares");
    if (mineInput && theirInput) {
        // Display-referred values only: the inverse of a display transform.
        std::array<std::array<float, 4>, 3> a{
            {{0.1F, 0.5F, 0.9F, 1.0F}, {0.0F, 0.0F, 0.0F, 1.0F}, {1.0F, 1.0F, 1.0F, 0.5F}}};
        auto b = a;
        expect(mineInput->apply(a) == theirInput->apply(b) &&
                   std::memcmp(a.data(), b.data(), sizeof(a)) == 0,
               "loose copy: bit-identical input pixels");
    }

    // GPU programs: the helper reflects the same shader and resources; only the config revision
    // in the content identity differs.
    const auto mineProgram =
        buildOcioGpuProgramForDisplay(*helper, helper->displayName(), helper->viewName());
    const auto theirProgram =
        buildOcioGpuProgramForDisplay(*reference, reference->displayName(), reference->viewName());
    expect(mineProgram.succeeded() && theirProgram.succeeded(),
           "loose copy: the display GPU program is extracted (" +
               std::string(render::ocioGpuProgramErrorName(mineProgram.error())) + ")");
    if (mineProgram.succeeded() && theirProgram.succeeded()) {
        expect(
            mineProgram.program()->shaderText == theirProgram.program()->shaderText &&
                mineProgram.program()->shaderTextDigest ==
                    theirProgram.program()->shaderTextDigest &&
                mineProgram.program()->resourceDigest == theirProgram.program()->resourceDigest &&
                mineProgram.program()->contentIdentity != theirProgram.program()->contentIdentity,
            "loose copy: the same display shader and resources under its own revision");
    }
    const auto mineCst = buildOcioGpuProgramForCst(*helper, "sRGB - Texture", working);
    const auto theirCst = buildOcioGpuProgramForCst(*reference, "sRGB - Texture", working);
    expect(mineCst.succeeded() && theirCst.succeeded() &&
               mineCst.program()->shaderText == theirCst.program()->shaderText &&
               mineCst.program()->resourceDigest == theirCst.program()->resourceDigest,
           "loose copy: the same color-space GPU program");
    expect(buildOcioGpuProgramForCst(*helper, working, working).error() ==
                   render::OcioGpuProgramError::IdentityTransform &&
               buildOcioGpuProgramForCst(*helper, "No Such Space", working).error() ==
                   render::OcioGpuProgramError::UnsupportedColorSpace,
           "loose copy: identity and unknown spaces keep their typed GPU results");
    const auto mineAffine = buildOcioGpuProgramForPointwiseAffine(*helper, 2.0F, 0.25F);
    const auto theirAffine = buildOcioGpuProgramForPointwiseAffine(*reference, 2.0F, 0.25F);
    expect(mineAffine.succeeded() && theirAffine.succeeded() &&
               mineAffine.program()->shaderText == theirAffine.program()->shaderText,
           "loose copy: a trusted-parameter program needs no helper and is the same");

    // A second resolution reuses the kept helper; a wrong revision is Changed, not Ready.
    const auto again = resolveOcioConfig(location, revision, working);
    expect(again.ready(), "loose copy: the kept config resolves again");
    core::Sha256Digest::Bytes wrong{};
    std::ranges::copy(revision.bytes(), wrong.begin());
    wrong[0] = static_cast<std::uint8_t>(wrong[0] ^ 0xffU);
    expect(resolveOcioConfig(location, core::Sha256Digest::fromBytes(wrong), working).outcome() ==
               OcioBuiltInRegistryOutcome::Changed,
           "loose copy: a different expected revision is Changed");
    expect(resolveOcioConfig(location, revision, "sRGB - Texture").invalidReason() ==
                   OcioBuiltInInvalidReason::WorkingColorSpaceNotSceneLinear &&
               resolveOcioConfig(location, revision, "No Such Space").invalidReason() ==
                   OcioBuiltInInvalidReason::WorkingColorSpaceMissing,
           "loose copy: the working space is proven as for a built-in");
}

// A small OCIO v2 config whose "graded" space and default view read a 1D LUT through the search
// path. `lut` is the file name the FileTransform names; it may hold a context variable.
std::string lutConfig(const std::string& lut, const std::string& environment = {}) {
    return "ocio_profile_version: 2\n" + environment +
           "search_path: luts\n"
           "strictparsing: true\n"
           "roles:\n"
           "  default: raw\n"
           "  scene_linear: lin\n"
           "displays:\n"
           "  sRGB:\n"
           "    - !<View> {name: Graded, colorspace: graded}\n"
           "active_displays: [sRGB]\n"
           "active_views: [Graded]\n"
           "colorspaces:\n"
           "  - !<ColorSpace>\n"
           "    name: lin\n"
           "    encoding: scene-linear\n"
           "    isdata: false\n"
           "  - !<ColorSpace>\n"
           "    name: raw\n"
           "    isdata: true\n"
           "  - !<ColorSpace>\n"
           "    name: graded\n"
           "    isdata: false\n"
           "    from_scene_reference: !<FileTransform> {src: \"" +
           lut + "\", interpolation: linear}\n";
}

std::string curve(const float middle) {
    return "Version 1\nFrom 0 1\nLength 3\nComponents 1\n{\n0\n" + std::to_string(middle) +
           "\n1\n}\n";
}

// lin to graded for the fixture pixels, by OpenColorIO called directly on the files on disk.
std::optional<std::array<std::array<float, 4>, 8>>
referenceGraded(const std::filesystem::path& root, const char* variable = nullptr,
                const char* value = nullptr) {
    try {
        // OpenColorIO keeps files it has read by path; the fixtures rewrite them.
        OCIO::ClearAllCaches();
        const auto config = OCIO::Config::CreateFromFile((root / "config.ocio").string().c_str());
        auto context = config->getCurrentContext()->createEditableCopy();
        if (variable != nullptr)
            context->setStringVar(variable, value);
        const auto cpu = config->getProcessor(context, "lin", "graded")->getDefaultCPUProcessor();
        auto pixels = kPixels;
        for (auto& pixel : pixels)
            cpu->applyRGB(pixel.data());
        return pixels;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

// lin to graded through the helper, or nothing when the location does not resolve Ready.
std::optional<std::array<std::array<float, 4>, 8>>
helperGraded(const OcioConfigLocation& location) {
    const auto revision = revisionOf(location);
    if (!revision)
        return std::nullopt;
    auto resolved = resolveOcioConfig(location, *revision).takeResolved();
    if (!resolved)
        return std::nullopt;
    const auto prepared = CpuColorSpaceProcessor::prepare(*resolved, "lin", "graded");
    auto pixels = kPixels;
    if (!prepared || !prepared.processor()->apply(pixels))
        return std::nullopt;
    return pixels;
}

bool same(const std::optional<std::array<std::array<float, 4>, 8>>& a,
          const std::optional<std::array<std::array<float, 4>, 8>>& b) {
    return a && b && std::memcmp(a->data(), b->data(), sizeof(*a)) == 0;
}

// A loose config with a LUT: the resource table, the consumed set and what a change does.
void testLooseWithLut(const std::filesystem::path& root) {
    const auto folder = root / "graded";
    write(folder / "config.ocio", lutConfig("grade.spi1d"));
    write(folder / "luts" / "grade.spi1d", curve(0.3F));
    write(folder / "notes.txt", "not read by the config\n");
    const auto location = loose(folder);

    const auto first = revisionOf(location);
    expect(first.has_value(), "lut config: a revision is reported");
    if (!first)
        return;
    auto resolution = resolveOcioConfig(location, *first);
    expect(resolution.ready(), "lut config: resolves Ready");
    auto resolved = std::move(resolution).takeResolved();
    if (!resolved)
        return;
    expect(resolved->processColorSpaceId() == "lin" && resolved->displayName() == "sRGB" &&
               resolved->viewName() == "Graded",
           "lut config: the scene_linear role and the default display and view are described");
    const auto reference = referenceGraded(folder);
    expect(same(helperGraded(location), reference) && reference && (*reference)[1][0] != 0.18F,
           "lut config: the LUT is applied in the helper exactly as OpenColorIO applies it");

    // The default view goes through the same LUT: display bytes against a direct reference.
    auto handle = buildCpuDisplayProcessorForView(*resolved, "sRGB", "Graded").takeHandle();
    expect(handle.has_value(), "lut config: the display processor builds");
    if (handle && reference) {
        const auto bytes = displayBytes(*handle);
        bool equal = bytes.has_value();
        for (std::size_t index = 0; equal && index < kPixels.size(); ++index) {
            const auto quantize = [](const float value) {
                return static_cast<std::uint8_t>(
                    std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
            };
            equal = (*bytes)[index].red == quantize((*reference)[index][0]) &&
                    (*bytes)[index].green == quantize((*reference)[index][1]) &&
                    (*bytes)[index].blue == quantize((*reference)[index][2]);
        }
        expect(equal, "lut config: display bytes equal the reference's quantized pixels");
    }

    // A file the config never reads is not part of its revision.
    write(folder / "notes.txt", "changed, and still not read by the config\n");
    expect(revisionOf(location) == first && resolveOcioConfig(location, *first).ready(),
           "lut config: changing an unreferenced file keeps the revision and stays Ready");

    // A LUT the config reads is.
    write(folder / "luts" / "grade.spi1d", curve(0.6F));
    const auto changed = resolveOcioConfig(location, *first);
    expect(changed.outcome() == OcioBuiltInRegistryOutcome::Changed &&
               changed.recomputedRevision().has_value() && *changed.recomputedRevision() != *first,
           "lut config: a changed LUT is Changed with a new revision");
    expect(same(helperGraded(location), referenceGraded(folder)) &&
               !same(referenceGraded(folder), reference),
           "lut config: the new revision applies the new LUT");

    // The same content again is the first revision again.
    write(folder / "luts" / "grade.spi1d", curve(0.3F));
    expect(revisionOf(location) == first, "lut config: the revision is a function of content");

    std::filesystem::remove(folder / "luts" / "grade.spi1d");
    expect(resolveOcioConfig(location, *first).outcome() ==
               OcioBuiltInRegistryOutcome::MissingResource,
           "lut config: a missing LUT is MissingResource");

    // Nothing outside the root is a resource, by a parent path or through a symbolic link.
    write(root / "outside.spi1d", curve(0.3F));
    write(folder / "config.ocio", lutConfig("../../outside.spi1d"));
    expect(resolveOcioConfig(location, *first).outcome() ==
               OcioBuiltInRegistryOutcome::MissingResource,
           "lut config: a file above the root is not a resource");
    write(folder / "config.ocio", lutConfig((root / "outside.spi1d").string()));
    expect(resolveOcioConfig(location, *first).outcome() ==
               OcioBuiltInRegistryOutcome::MissingResource,
           "lut config: an absolute path is not a resource");
    write(folder / "config.ocio", lutConfig("grade.spi1d"));
    std::error_code linked;
    std::filesystem::create_symlink(root / "outside.spi1d", folder / "luts" / "grade.spi1d",
                                    linked);
    expect(!linked && resolveOcioConfig(location, *first).outcome() ==
                          OcioBuiltInRegistryOutcome::MissingResource,
           "lut config: a symbolic link is never followed");
    std::filesystem::remove(folder / "luts" / "grade.spi1d");
    write(folder / "luts" / "grade.spi1d", curve(0.3F));
    expect(revisionOf(location) == first && resolveOcioConfig(location, *first).ready(),
           "lut config: the restored root resolves Ready at the first revision");
}

// The same config as an archive, external and project-relative.
void testArchives(const std::filesystem::path& root) {
    const auto folder = root / "graded";
    const auto archive = root / "shot.ocioz";
    try {
        const auto config = OCIO::Config::CreateFromFile((folder / "config.ocio").string().c_str());
        expect(config->isArchivable(), "archive: the fixture config can be archived");
        std::ofstream file(archive, std::ios::binary);
        config->archive(file);
    } catch (const std::exception& error) {
        expect(false, std::string("archive: the fixture archive is written: ") + error.what());
        return;
    }
    const auto bytes = read(archive);
    const auto reference = referenceGraded(folder);

    const OcioConfigLocation external{OcioConfigLocatorKind::ExternalArchive, archive.string(), {}};
    const auto externalRevision =
        computeOcioContentRevisionV1(OcioContentLocatorKind::ExternalArchive, bytes);
    expect(externalRevision && revisionOf(external) == *externalRevision.revision(),
           "archive: the external revision is the content revision of the archive bytes");
    expect(externalRevision && resolveOcioConfig(external, *externalRevision.revision()).ready() &&
               same(helperGraded(external), reference),
           "archive: an external archive resolves and applies its LUT as the loose config does");
    // Through the one helper that percent-encodes, so a work folder with a space in its path
    // gives a valid URI (#665).
    const auto archiveUri = bloom::core::fileUriFromAbsolutePath(archive).value();
    expect(parseOcioConfigLocation(archiveUri) ==
                   OcioConfigLocation{OcioConfigLocatorKind::ExternalArchive, archiveUri, {}} &&
               same(helperGraded(parseOcioConfigLocation(archiveUri)), reference),
           "archive: a file URI names the same archive");

    const OcioConfigLocation relative{
        OcioConfigLocatorKind::ProjectRelativeArchive, archive.string(), {}};
    const auto relativeRevision =
        computeOcioContentRevisionV1(OcioContentLocatorKind::ProjectRelativeArchive, bytes);
    expect(relativeRevision && externalRevision &&
               *relativeRevision.revision() != *externalRevision.revision() &&
               revisionOf(relative) == *relativeRevision.revision(),
           "archive: a project-relative archive has its own revision of the same bytes");
    const auto text = ocioConfigLocationText(relative);
    expect(parseOcioConfigLocation(text) == relative &&
               resolveOcioConfig(text, *relativeRevision.revision()).ready() &&
               same(helperGraded(relative), reference),
           "archive: a project-relative archive resolves from its intent text");
    expect(resolveOcioConfig(relative, *externalRevision.revision()).outcome() ==
               OcioBuiltInRegistryOutcome::Changed,
           "archive: the other kind's revision is Changed");

    // Not an archive at all, and a damaged one.
    const auto plain = root / "plain.ocioz";
    write(plain, lutConfig("grade.spi1d"));
    const OcioConfigLocation plainLocation{
        OcioConfigLocatorKind::ExternalArchive, plain.string(), {}};
    expect(resolveOcioConfig(plainLocation, core::Sha256Digest{}).outcome() ==
               OcioBuiltInRegistryOutcome::Invalid,
           "archive: a config file under an archive name is Invalid, never read as a file");
    const auto damaged = root / "damaged.ocioz";
    {
        std::ofstream file(damaged, std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size() / 2));
    }
    const OcioConfigLocation damagedLocation{
        OcioConfigLocatorKind::ExternalArchive, damaged.string(), {}};
    const auto damagedRevision =
        computeOcioContentRevisionV1(OcioContentLocatorKind::ExternalArchive, read(damaged));
    const auto damagedResult = resolveOcioConfig(damagedLocation, *damagedRevision.revision());
    expect(damagedResult.outcome() == OcioBuiltInRegistryOutcome::Invalid &&
               damagedResult.invalidReason() == OcioBuiltInInvalidReason::ParseFailed,
           "archive: a truncated archive is Invalid with a parse failure");
}

// The project's context variables select what the config reads, and so its revision.
void testContextVariables(const std::filesystem::path& root) {
    const auto folder = root / "shots";
    write(folder / "config.ocio", lutConfig("${SHOT}.spi1d", "environment:\n  SHOT: a\n"));
    write(folder / "luts" / "a.spi1d", curve(0.2F));
    write(folder / "luts" / "b.spi1d", curve(0.7F));
    const auto plain = loose(folder);
    auto chosen = plain;
    chosen.context = {{"SHOT", "b"}};

    expect(same(helperGraded(plain), referenceGraded(folder, "SHOT", "a")),
           "context: the config's declared default selects its file");
    expect(same(helperGraded(chosen), referenceGraded(folder, "SHOT", "b")) &&
               !same(helperGraded(chosen), helperGraded(plain)),
           "context: a project variable overrides the default");
    expect(revisionOf(plain).has_value() && revisionOf(chosen).has_value() &&
               revisionOf(plain) != revisionOf(chosen),
           "context: each consumed set has its own revision");
    // Only the file a context reads is in its revision.
    const auto before = revisionOf(plain);
    write(folder / "luts" / "b.spi1d", curve(0.75F));
    expect(revisionOf(plain) == before && revisionOf(chosen).has_value(),
           "context: a file another context reads does not change this revision");

    auto missing = plain;
    missing.context = {{"SHOT", "nowhere"}};
    expect(resolveOcioConfig(missing, core::Sha256Digest{}).outcome() ==
               OcioBuiltInRegistryOutcome::MissingResource,
           "context: a variable that names no file is MissingResource");

    const auto text = ocioConfigLocationText(chosen);
    expect(parseOcioConfigLocation(text) == chosen, "context: the intent text round-trips");
    OcioConfigLocation awkward{OcioConfigLocatorKind::ExternalLooseConfig,
                               (folder / "config.ocio").string(),
                               {{"A", "50% of\nboth = lines"}, {"B", ""}}};
    expect(parseOcioConfigLocation(ocioConfigLocationText(awkward)) == awkward,
           "context: values with line breaks, percent and equals signs round-trip");
    const auto display = resolveOcioConfig(text, *revisionOf(chosen)).takeResolved();
    if (display) {
        auto handle = buildCpuDisplayProcessorForView(*display, "sRGB", "Graded").takeHandle();
        const auto view = handle ? handle->identity().borrowedView() : std::nullopt;
        const std::string_view name = "SHOT";
        const auto bytes = view ? view->canonicalBytes() : std::span<const std::byte>{};
        const auto named = std::ranges::search(bytes, std::as_bytes(std::span(name)));
        expect(!named.empty(), "context: the display identity records the project's variables");
    } else {
        expect(false, "context: the chosen context resolves from its intent text");
    }
}

// Inputs that must be refused, each with its own state.
void testRefusals(const std::filesystem::path& root) {
    const auto outcome = [](const OcioConfigLocation& location) {
        return resolveOcioConfig(location, core::Sha256Digest{}).outcome();
    };
    expect(outcome(loose(root / "absent")) == OcioBuiltInRegistryOutcome::Missing,
           "refusal: an absent config is Missing");
    expect(outcome({OcioConfigLocatorKind::ExternalLooseConfig, "relative/config.ocio", {}}) ==
                   OcioBuiltInRegistryOutcome::Missing &&
               outcome({OcioConfigLocatorKind::ExternalLooseConfig,
                        "file://server/share/config.ocio",
                        {}}) == OcioBuiltInRegistryOutcome::Missing &&
               outcome({OcioConfigLocatorKind::ExternalArchive, (root / "graded").string(), {}}) ==
                   OcioBuiltInRegistryOutcome::Missing,
           "refusal: a relative path, a remote authority and a wrong file name are Missing");

    write(root / "garbage" / "config.ocio", "this: is: not: [a config\n");
    const auto garbage = resolveOcioConfig(loose(root / "garbage"), core::Sha256Digest{});
    expect(garbage.outcome() == OcioBuiltInRegistryOutcome::Invalid &&
               garbage.invalidReason() == OcioBuiltInInvalidReason::ParseFailed &&
               garbage.recomputedRevision().has_value(),
           "refusal: unparseable text is Invalid with a parse failure and still has a revision");

    write(root / "real" / "config.ocio", lutConfig("grade.spi1d"));
    std::filesystem::create_directories(root / "linked");
    std::error_code linked;
    std::filesystem::create_symlink(root / "real" / "config.ocio", root / "linked" / "config.ocio",
                                    linked);
    expect(!linked && outcome(loose(root / "linked")) == OcioBuiltInRegistryOutcome::Missing,
           "refusal: a config that is a symbolic link is Missing");

    const auto crowded = root / "crowded";
    write(crowded / "config.ocio", lutConfig("grade.spi1d"));
    for (std::size_t index = 0; index < kOcioLooseMaximumEntryCount; ++index)
        write(crowded / "luts" / (std::to_string(index) + ".txt"), "x");
    const auto tooMany = resolveOcioConfig(loose(crowded), core::Sha256Digest{});
    expect(tooMany.outcome() == OcioBuiltInRegistryOutcome::Invalid &&
               tooMany.invalidReason() == OcioBuiltInInvalidReason::ResourceTableLimitExceeded,
           "refusal: a root with more candidate files than one table carries is Invalid");

    // Records from the other side of the boundary are bounded and checked in full.
    detail::ConfigOpenReply reply;
    reply.outcome = detail::ConfigOpenOutcome::Ready;
    reply.consumedKeys = {"config.ocio", "luts/a.spi1d"};
    reply.description.processColorSpaceId = "lin";
    reply.description.displays = {{"sRGB", "Graded", "graded", true, true}};
    const auto encoded = detail::encodeConfigOpenReply(reply);
    const auto decoded = detail::decodeConfigOpenReply(encoded);
    bool truncatedRefused = true;
    for (std::size_t size = 0; size < encoded.size(); ++size)
        truncatedRefused =
            truncatedRefused && !detail::decodeConfigOpenReply(std::span(encoded).first(size));
    auto trailing = encoded;
    trailing.push_back(std::byte{0});
    auto badOutcome = encoded;
    badOutcome[4] = std::byte{9};
    expect(decoded && decoded->consumedKeys == reply.consumedKeys &&
               decoded->description.displays == reply.description.displays && truncatedRefused &&
               !detail::decodeConfigOpenReply(trailing) &&
               !detail::decodeConfigOpenReply(badOutcome),
           "refusal: a reply record round-trips; truncated, padded and out-of-range ones do not");
    expect(detail::canonicalConfigResourceKey("./luts//a.spi1d") == "luts/a.spi1d" &&
               detail::canonicalConfigResourceKey("../a").empty() &&
               detail::canonicalConfigResourceKey("/etc/passwd").empty() &&
               detail::canonicalConfigResourceKey("luts/../../a").empty(),
           "refusal: a resource key never leaves the root");
}

// A slow parse blocks nobody else, and a resolution can be cancelled. The slow one is held in its
// cancellation callback, which the helper wait polls: the callback does not return until the
// other config has resolved, so the other config resolving at all proves no lock is held across
// the first one's parse.
void testConcurrentAndCancelled(const std::filesystem::path& root) {
    const auto slowLocation = loose(root / "graded");
    const auto fastLocation = loose(root / "aces");
    const std::string slowWorking = "lin";
    const std::string fastWorking(kAcesCgV1SceneLinearColorSpaceId);
    const auto slowRevision = revisionOf(slowLocation, slowWorking);
    const auto fastRevision = revisionOf(fastLocation, fastWorking);
    expect(slowRevision && fastRevision, "concurrent: both configs have a revision");
    if (!slowRevision || !fastRevision)
        return;
    releaseExternalOcioConfigs();

    std::atomic<int> calls{0};
    std::atomic<bool> fastDone{false};
    std::atomic<bool> slowHeld{false};
    std::atomic<bool> slowFinished{false};
    std::atomic<bool> slowReady{false};
    const auto slowCancellation = [&] {
        // Calls 1 and 2 are the resolution's own checks; the third is inside the helper wait.
        if (++calls >= 3) {
            slowHeld = true;
            const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(60);
            while (!fastDone && std::chrono::steady_clock::now() < limit)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    };
    std::thread slow([&] {
        slowReady = resolveOcioConfig(slowLocation, *slowRevision, slowWorking, slowCancellation)
                        .ready();
        slowFinished = true;
    });
    const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (!slowHeld && std::chrono::steady_clock::now() < limit)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    expect(slowHeld, "concurrent: the slow resolution reached the helper wait");

    // A second request for the slow config waits for it and can be cancelled while it waits.
    std::atomic<int> waits{0};
    const auto waiter = resolveOcioConfig(slowLocation, *slowRevision, slowWorking,
                                          [&] { return ++waits >= 3; });
    expect(waiter.outcome() == OcioBuiltInRegistryOutcome::LocatorKindRequiresHelper &&
               waiter.helperError() == OcioHelperError::HelperCancelled && !slowFinished,
           "concurrent: a request waiting for the same config is cancellable");

    // A different config resolves while the slow one is still parsing.
    const auto fast = resolveOcioConfig(fastLocation, *fastRevision, fastWorking);
    expect(fast.ready() && !slowFinished,
           "concurrent: another config resolves while a slow parse is in progress");
    fastDone = true;
    slow.join();
    expect(slowReady, "concurrent: the slow resolution completes after it was released");

    // The released one is kept: a second request answers without a callback being polled.
    std::atomic<int> polled{0};
    const auto again = resolveOcioConfig(slowLocation, *slowRevision, slowWorking, [&] {
        ++polled;
        return false;
    });
    expect(again.ready() && polled <= 1, "concurrent: the finished config is kept");

    // Cancellation reaches the helper wait and leaves nothing kept.
    releaseExternalOcioConfigs();
    const auto before = resolveOcioConfig(slowLocation, *slowRevision, slowWorking,
                                          [] { return true; });
    expect(before.outcome() == OcioBuiltInRegistryOutcome::LocatorKindRequiresHelper &&
               before.helperError() == OcioHelperError::HelperCancelled,
           "cancel: a resolution cancelled before it starts is HelperCancelled");
    std::atomic<int> during{0};
    const auto cancelled = resolveOcioConfig(slowLocation, *slowRevision, slowWorking,
                                             [&] { return ++during >= 3; });
    expect(cancelled.outcome() == OcioBuiltInRegistryOutcome::LocatorKindRequiresHelper &&
               cancelled.helperError() == OcioHelperError::HelperCancelled && during >= 3,
           "cancel: a resolution cancelled in the helper wait is HelperCancelled");
    expect(resolveOcioConfig(slowLocation, *slowRevision, slowWorking).ready(),
           "cancel: a cancelled resolution leaves the config resolvable");
}

// The session itself: a hostile resource, a helper that dies, and tokens that die with it.
void testSession(const std::filesystem::path& root) {
    detail::ConfigParameters parameters;
    parameters.kind = OcioConfigLocatorKind::ExternalLooseConfig;
    const auto sealed = [](const std::span<const std::byte> bytes) {
        const int fd = detail::sharedFile();
        if (fd >= 0 && (!detail::writeFd(fd, bytes) || !detail::sealInput(fd))) {
            ::close(fd);
            return -1;
        }
        return fd;
    };

    // Bytes that are not a resource table end the helper as a protocol violation.
    const std::string noise = "definitely not a resource table";
    const detail::LutFd bad(sealed(std::as_bytes(std::span(noise))));
    const auto refused = detail::ConfigSession::open(parameters, bad.get(), noise.size());
    expect(!refused.session && refused.helperError == OcioHelperError::HelperProtocolViolation,
           "session: a malformed resource table is a protocol violation");

    // An unsealed resource is never sent.
    const detail::LutFd open(detail::sharedFile());
    expect(open.get() >= 0 && detail::writeFd(open.get(), std::as_bytes(std::span(noise))),
           "session: the unsealed fixture is written");
    const auto unsealed = detail::ConfigSession::open(parameters, open.get(), noise.size());
    expect(!unsealed.session && unsealed.helperError != OcioHelperError::None,
           "session: an unsealed resource is refused before the helper reads it");

    // A real table: one config and its LUT.
    const auto config = lutConfig("grade.spi1d");
    const auto lut = curve(0.3F);
    detail::ConfigWriter table;
    table.u32(detail::kConfigRecordVersion);
    table.u32(2);
    detail::appendConfigResource(table, "config.ocio", std::as_bytes(std::span(config)));
    detail::appendConfigResource(table, "luts/grade.spi1d", std::as_bytes(std::span(lut)));
    const auto bytes = std::move(table).take();
    const detail::LutFd good(sealed(bytes));
    auto opened = detail::ConfigSession::open(parameters, good.get(), bytes.size());
    expect(opened.session && opened.reply &&
               opened.reply->outcome == detail::ConfigOpenOutcome::Ready &&
               opened.reply->consumedKeys ==
                   std::vector<std::string>{"config.ocio", "luts/grade.spi1d"},
           "session: the helper reports exactly the resources the config reads");
    if (!opened.session)
        return;
    const auto session = opened.session;
    const auto built = session->buildProcessor(
        static_cast<std::uint32_t>(detail::ConfigProcessorKind::ColorSpace), "lin", "graded");
    expect(built.error == 0 && built.processor, "session: a processor token is handed out");
    std::array<float, 8> pixels{0.5F, 0.5F, 0.5F, 0.25F, 0.0F, 1.0F, 0.5F, 1.0F};
    expect(built.processor && built.processor->apply(pixels, false) && pixels[3] == 0.25F &&
               pixels[7] == 1.0F && pixels[0] != 0.5F,
           "session: a slab is transformed and alpha is left exactly as it was");

    // A non-finite pixel fails that call only; the helper and its tokens keep serving.
    std::array<float, 4> poisoned{std::nanf(""), 0.0F, 0.0F, 1.0F};
    std::array<float, 4> fine{0.5F, 0.5F, 0.5F, 1.0F};
    expect(built.processor && !built.processor->apply(poisoned, false) &&
               built.processor->apply(fine, false) && session->failure() == OcioHelperError::None,
           "session: a non-finite pixel fails one call and leaves the helper serving");

    // An unknown space is a typed answer, not a dead helper.
    const auto unknown = session->buildProcessor(
        static_cast<std::uint32_t>(detail::ConfigProcessorKind::ColorSpace), "lin", "nowhere");
    expect(unknown.error ==
                   static_cast<std::uint32_t>(detail::ConfigProcessorError::MissingDestination) &&
               !unknown.processor && session->failure() == OcioHelperError::None,
           "session: an unknown color space is answered without ending the helper");

    // The helper is killed from outside: every token is dead and says why.
    const auto pid = session->processId();
    expect(pid > 0 && ::kill(static_cast<pid_t>(pid), SIGKILL) == 0,
           "session: the helper process is killed");
    std::array<float, 4> after{0.5F, 0.5F, 0.5F, 1.0F};
    expect(built.processor && !built.processor->apply(after, false) && after[0] == 0.5F &&
               session->failure() != OcioHelperError::None,
           "session: a dead helper fails its tokens and publishes no pixels");
    const auto late = session->buildProcessor(
        static_cast<std::uint32_t>(detail::ConfigProcessorKind::ColorSpace), "lin", "graded");
    expect(late.error == detail::kRemoteHelperFailed && !late.processor,
           "session: a dead helper hands out no further token");

    // A helper that stops answering is ended at the slab deadline, and the session says so.
    {
        const detail::LutFd again(sealed(bytes));
        const auto stalled = detail::ConfigSession::open(parameters, again.get(), bytes.size());
        const auto token =
            stalled.session
                ? stalled.session->buildProcessor(
                      static_cast<std::uint32_t>(detail::ConfigProcessorKind::ColorSpace), "lin",
                      "graded")
                : detail::RemoteProcessorBuild{};
        const auto stalledPid = stalled.session ? stalled.session->processId() : 0;
        expect(token.processor && stalledPid > 0 &&
                   ::kill(static_cast<pid_t>(stalledPid), SIGSTOP) == 0,
               "session: a second helper is stopped in place");
        std::array<float, 4> waiting{0.5F, 0.5F, 0.5F, 1.0F};
        const auto started = std::chrono::steady_clock::now();
        const bool applied = token.processor && token.processor->apply(waiting, false);
        const auto waited = std::chrono::steady_clock::now() - started;
        expect(!applied && waiting[0] == 0.5F && stalled.session &&
                   stalled.session->failure() == OcioHelperError::HelperDeadline &&
                   waited >= std::chrono::seconds(9) && waited < std::chrono::seconds(30),
               "session: a silent helper fails at the 10 second slab deadline as HelperDeadline");
        expect(stalledPid > 0 && ::kill(static_cast<pid_t>(stalledPid), 0) != 0,
               "session: the helper that missed its deadline was terminated and reaped");
    }

    // The resolver replaces a kept helper that died.
    const auto folder = root / "graded";
    const auto location = loose(folder);
    const auto revision = revisionOf(location);
    expect(revision.has_value() && same(helperGraded(location), referenceGraded(folder)),
           "session: the resolver serves the config before its helper is lost");
    releaseExternalOcioConfigs();
    expect(revision.has_value() && same(helperGraded(location), referenceGraded(folder)),
           "session: after its helpers were released the resolver starts a new one");
}
#endif

} // namespace

int main() {
#ifndef __linux__
    // No confinement is qualified here: an external config is refused, never parsed in-process.
    const auto refused = resolveOcioConfig(
        OcioConfigLocation{OcioConfigLocatorKind::ExternalLooseConfig, "/nowhere/config.ocio", {}},
        core::Sha256Digest{});
    expect(refused.outcome() == OcioBuiltInRegistryOutcome::LocatorKindRequiresHelper &&
               refused.helperError() == OcioHelperError::HelperUnavailable,
           "an external config is HelperUnavailable on a target without a confined helper");
    return failures == 0 ? 0 : 1;
#else
    const auto root = std::filesystem::temp_directory_path() /
                      ("bloom-ocio-config-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    testLooseCopyMatchesBuiltIn(root);
    testLooseWithLut(root);
    testArchives(root);
    testContextVariables(root);
    testRefusals(root);
    testConcurrentAndCancelled(root);
    testSession(root);
    releaseExternalOcioConfigs();
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    if (failures == 0)
        std::cout << "ocio config resolution tests passed\n";
    return failures == 0 ? 0 : 1;
#endif
}
