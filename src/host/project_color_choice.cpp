#include <bloom/host/project_color_choice.hpp>

#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/color/ocio_config_resolution.hpp>
#include <bloom/core/file_uri.hpp>
#include <bloom/host/export_job_runner.hpp>

#include <algorithm>
#include <array>
#include <variant>

namespace bloom::host {
namespace {

constexpr std::array<std::string_view, 1> kNeutralSpaces{document::kProcessColorSpaceIdV1};
constexpr std::array<std::string_view, 5> kAcesSpaces{
    "ACEScg", "ACES2065-1", "Linear Rec.709 (sRGB)", "Linear P3-D65", "Linear Rec.2020"};

} // namespace

std::optional<ProjectColorConfig> parseProjectColorConfig(const std::string_view name) {
    if (name == "neutral" || name == "clear")
        return ProjectColorConfig::BloomNeutral;
    if (name == "aces")
        return ProjectColorConfig::AcesCg;
    if (name == "current")
        return ProjectColorConfig::Current;
    return std::nullopt;
}

std::string_view projectColorConfigName(const ProjectColorConfig config) noexcept {
    switch (config) {
    case ProjectColorConfig::BloomNeutral:
        return "neutral";
    case ProjectColorConfig::AcesCg:
        return "aces";
    case ProjectColorConfig::Current:
        break;
    }
    return "current";
}

ProjectColorConfig projectColorConfigOf(const document::ColorSettings& settings) {
    const auto* builtIn =
        std::get_if<document::BuiltInOcioConfigLocator>(&settings.ocioConfig.locator);
    if (builtIn != nullptr && builtIn->uri == color::kAcesCgV1ConfigUri)
        return ProjectColorConfig::AcesCg;
    if (builtIn != nullptr && builtIn->uri == color::kBloomNeutralV1ConfigUri)
        return ProjectColorConfig::BloomNeutral;
    return ProjectColorConfig::Current;
}

std::string_view workingColorSpaceLabel(const std::string_view id) noexcept {
    if (id == document::kProcessColorSpaceIdV1)
        return "Linear Rec.709 (sRGB)";
    return id;
}

std::span<const std::string_view> projectWorkingColorSpaces(const ProjectColorConfig config) {
    switch (config) {
    case ProjectColorConfig::BloomNeutral:
        return kNeutralSpaces;
    case ProjectColorConfig::AcesCg:
        return kAcesSpaces;
    case ProjectColorConfig::Current:
        break;
    }
    return {};
}

ProjectColorResolution resolveProjectColorSettings(const document::ColorSettings& original,
                                                   const ProjectColorConfig config,
                                                   std::optional<std::string> workingColorSpaceId) {
    const auto current = projectColorConfigOf(original);
    const bool keep = config == ProjectColorConfig::Current || config == current;
    const auto offered = projectWorkingColorSpaces(keep ? current : config);
    std::string working = workingColorSpaceId.value_or(
        keep ? original.processColorSpaceId
             : std::string(offered.empty() ? std::string_view{} : offered.front()));
    // The project's own working space stays valid while its configuration stays, even when the
    // dialog would not offer it (a project authored elsewhere).
    const bool own = keep && working == original.processColorSpaceId;
    if (!own && !offered.empty() && std::ranges::find(offered, working) == offered.end())
        return {std::nullopt, "The working color space \"" + working +
                                  "\" is not offered by that configuration"};
    if (keep) {
        auto settings = original;
        settings.processColorSpaceId = std::move(working);
        return {std::move(settings), {}};
    }
    if (config == ProjectColorConfig::AcesCg) {
        const auto revision = color::ocioBuiltInContentRevision(
            color::OcioConfigLocatorKind::BloomBuiltIn, color::kAcesCgV1ConfigUri);
        if (!revision.has_value())
            return {std::nullopt, "The built-in ACES configuration is unavailable in this build, "
                                  "so it cannot be selected."};
        return {document::ColorSettings{
                    .schemaVersion = document::kColorSettingsSchemaVersionV1,
                    .processColorSpaceId = std::move(working),
                    .ocioConfig = {.schemaVersion = document::kOcioConfigReferenceSchemaVersionV1,
                                   .locator = document::BuiltInOcioConfigLocator{std::string(
                                       color::kAcesCgV1ConfigUri)},
                                   .expectedRevision = {.algorithm =
                                                            document::OcioRevisionAlgorithm::Sha256,
                                                        .digest = *revision},
                                   .portability = document::OcioConfigPortability::BuiltIn,
                                   .contextVariables = {}}},
                {}};
    }
    auto settings = document::makeBloomNeutralColorSettingsV1(color::kBloomNeutralV1ConfigDigest);
    settings.processColorSpaceId = std::move(working);
    return {std::move(settings), {}};
}

namespace {

// What went wrong with a configuration, in words an artist can act on. `name` is the file's name.
[[nodiscard]] std::string plainFailure(const color::OcioBuiltInResolutionResult& result,
                                       const std::string& name) {
    using Outcome = color::OcioBuiltInRegistryOutcome;
    switch (result.outcome()) {
    case Outcome::Missing:
        return "Bloom can't find or read \"" + name +
               "\". Check that the file exists, that it is a regular file and not a link, and "
               "that you can read it.";
    case Outcome::MissingResource:
        return "\"" + name +
               "\" refers to a file (such as a LUT) that is not in its folder or archive. Put the "
               "referenced files next to it, or choose a configuration that includes them.";
    case Outcome::Invalid:
        switch (result.invalidReason()) {
        case color::OcioBuiltInInvalidReason::ResourceTableLimitExceeded:
            return "\"" + name +
                   "\" is too large to use: its folder or archive holds too many files or too "
                   "many bytes. Choose a smaller configuration.";
        case color::OcioBuiltInInvalidReason::ValidateFailed:
            return "OpenColorIO read \"" + name + "\" but found it is not a valid configuration.";
        default:
            return "OpenColorIO couldn't read \"" + name +
                   "\" as a color configuration. Check that it is a config.ocio file or an .ocioz "
                   "archive that works in other OpenColorIO applications.";
        }
    case Outcome::LocatorKindRequiresHelper:
        switch (result.helperError()) {
        case color::OcioHelperError::HelperCancelled:
            return "Reading \"" + name + "\" was cancelled.";
        case color::OcioHelperError::HelperDeadline:
            return "Reading \"" + name +
                   "\" took too long and was stopped. Try a smaller configuration.";
        case color::OcioHelperError::HelperMemoryLimit:
            return "\"" + name + "\" needs more memory than the color helper may use.";
        case color::OcioHelperError::HelperUnavailable:
            return "Bloom's color helper isn't available on this computer, so it can't read an "
                   "external color configuration.";
        default:
            return "Bloom's color helper stopped while reading \"" + name + "\". Try again.";
        }
    case Outcome::Changed:
        return "\"" + name + "\" changed while Bloom was reading it. Try again.";
    default:
        break;
    }
    return "Bloom can't use \"" + name + "\" as a color configuration.";
}

[[nodiscard]] std::string pathText(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

} // namespace

ProjectColorResolution resolveProjectColorConfigFile(
    const document::ColorSettings& original, const ProjectColorConfigFile& file,
    std::optional<std::string> workingColorSpaceId, const std::filesystem::path& projectDirectory,
    const std::function<bool()>& cancellation) {
    if (file.path.empty())
        return {std::nullopt, "Choose a color configuration file: a config.ocio or an .ocioz archive."};
    document::OcioConfigReference reference;
    reference.schemaVersion = document::kOcioConfigReferenceSchemaVersionV1;
    reference.contextVariables = file.contextVariables;
    std::ranges::sort(reference.contextVariables, {}, &document::OcioContextVariable::name);
    std::string name;
    if (file.projectRelative) {
        const auto relative = std::filesystem::path(
            std::u8string(reinterpret_cast<const char8_t*>(file.path.data()), file.path.size()));
        if (projectDirectory.empty())
            return {std::nullopt, "Save the project first: a project-relative configuration is "
                                  "found beside the project file."};
        if (relative.is_absolute() || relative.has_root_name() ||
            std::ranges::any_of(relative, [](const auto& part) { return part == ".."; }))
            return {std::nullopt, "A project-relative configuration must be a path below the "
                                  "project's folder, without \"..\"."};
        if (relative.extension() != ".ocioz")
            return {std::nullopt, "A project-relative configuration must be an .ocioz archive."};
        reference.locator = document::ProjectRelativeOciozLocator{relative.generic_string()};
        reference.portability = document::OcioConfigPortability::ProjectRelative;
        name = pathText(relative.filename());
    } else {
        std::filesystem::path path;
        if (file.path.rfind("file:", 0) == 0) {
            const auto resolved = core::absolutePathFromFileUri(file.path);
            if (!resolved)
                return {std::nullopt, "That file location isn't a file on this computer."};
            path = *resolved;
        } else {
            path = std::filesystem::path(std::u8string(
                reinterpret_cast<const char8_t*>(file.path.data()), file.path.size()));
        }
        if (!path.is_absolute())
            return {std::nullopt, "Give the full path of the file, or choose a project-relative "
                                  "archive."};
        const auto uri = core::fileUriFromAbsolutePath(path);
        if (!uri)
            return {std::nullopt, "That file location can't be stored in a project."};
        name = pathText(path.filename());
        if (name == "config.ocio")
            reference.locator = document::ExternalOcioConfigLocator{*uri};
        else if (path.extension() == ".ocioz")
            reference.locator = document::ExternalOciozLocator{*uri};
        else
            return {std::nullopt, "Choose a file named config.ocio, or an archive that ends in "
                                  ".ocioz."};
        reference.portability = document::OcioConfigPortability::External;
    }

    // The configuration is read in the supervised helper: once with no expected revision, which
    // reports the current one, then pinned to it.
    const auto text = ocioConfigIntentLocation(reference, projectDirectory);
    const std::string working = workingColorSpaceId.value_or(std::string{});
    const auto probe = color::resolveOcioConfig(text, core::Sha256Digest{}, working, cancellation);
    if (probe.outcome() != color::OcioBuiltInRegistryOutcome::Changed ||
        !probe.recomputedRevision().has_value())
        return {std::nullopt, plainFailure(probe, name)};
    const auto revision = *probe.recomputedRevision();
    auto resolution = color::resolveOcioConfig(text, revision, working, cancellation);
    if (!resolution.ready()) {
        if (working.empty() || resolution.outcome() != color::OcioBuiltInRegistryOutcome::Invalid)
            return {std::nullopt, plainFailure(resolution, name)};
        return {std::nullopt, "The working color space \"" + working + "\" is not a scene-linear "
                                  "space in \"" + name + "\"."};
    }
    const auto config = std::move(resolution).takeResolved();
    if (!config.has_value())
        return {std::nullopt, "Bloom can't use \"" + name + "\" as a color configuration."};
    reference.expectedRevision = {document::OcioRevisionAlgorithm::Sha256, revision};
    auto settings = original;
    settings.processColorSpaceId = std::string(config->processColorSpaceId());
    settings.ocioConfig = std::move(reference);
    if (const auto valid = settings.validate(); !valid.ok())
        return {std::nullopt, "\"" + name + "\" can't be stored in the project: " +
                                  std::string(valid.issues().front().message)};
    return {std::move(settings), {}};
}

ProjectColorConfigDescription describeProjectColorConfig(const document::ColorSettings& settings) {
    ProjectColorConfigDescription description;
    const auto& reference = settings.ocioConfig;
    const auto digest = reference.expectedRevision.digest.toLowercaseHex();
    description.revision.assign(digest.data(), digest.size());
    description.contextVariables = reference.contextVariables;
    const auto asPath = [](const std::string& uri) {
        const auto path = core::absolutePathFromFileUri(uri);
        return path ? pathText(*path) : uri;
    };
    if (const auto* builtIn = std::get_if<document::BuiltInOcioConfigLocator>(&reference.locator)) {
        description.kind = builtIn->uri == color::kAcesCgV1ConfigUri ? "aces" : "neutral";
        description.location = builtIn->uri;
    } else if (const auto* relative =
                   std::get_if<document::ProjectRelativeOciozLocator>(&reference.locator)) {
        description.kind = "project-relative-archive";
        description.location = relative->path;
    } else if (const auto* archive =
                   std::get_if<document::ExternalOciozLocator>(&reference.locator)) {
        description.kind = "external-archive";
        description.location = asPath(archive->uri);
    } else if (const auto* loose =
                   std::get_if<document::ExternalOcioConfigLocator>(&reference.locator)) {
        description.kind = "external-config";
        description.location = asPath(loose->uri);
    }
    return description;
}

std::optional<std::vector<std::string>>
keptConfigColorSpaceIds(const std::string_view ocioConfigLocation,
                        const core::Sha256Digest& revision,
                        const std::string_view workingColorSpaceId) {
    auto resolution =
        color::resolveOcioConfigIfKept(ocioConfigLocation, revision, workingColorSpaceId);
    if (!resolution.ready())
        return std::nullopt;
    const auto config = std::move(resolution).takeResolved();
    if (!config)
        return std::nullopt;
    std::vector<std::string> ids;
    ids.reserve(config->colorSpaces().size());
    for (const auto& space : config->colorSpaces())
        ids.push_back(space.id);
    return ids;
}

} // namespace bloom::host
