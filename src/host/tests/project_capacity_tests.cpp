#include <bloom/host/project_session.hpp>
#include <bloom/host/publication_coordinator.hpp>
#include <bloom/host/session_save.hpp>
#include <bloom/platform/private_temporary_directory.hpp>
#include <bloom/platform/staged_artifact.hpp>

#include <bloom/commands/animation_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/core/sha256.hpp>
#include <bloom/document/animation.hpp>
#include <bloom/document/color_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/project/canonical_document.hpp>
#include <bloom/project/open_archive.hpp>
#include <bloom/project/project_capacity.hpp>
#include <bloom/project/project_io_memory.hpp>
#include <bloom/project/save_archive.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <numeric>
#include <optional>
#include <source_location>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// bloom.host.project-capacity (issue #748): a project Bloom lets you build always saves. An edit
// that would take a project past what one project file can save and reopen is refused at the edit,
// with the limit it would pass, and never at the save; the project as it stands then saves and
// reopens. The edit measure is the canonical encoder's own counting pass, remembered per curve, and
// equals a whole measure after every kind of change.

namespace {

namespace commands = bloom::commands;
namespace document = bloom::document;
namespace host = bloom::host;
namespace project = bloom::project;

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message,
                const std::source_location location = std::source_location::current()) {
        if (condition)
            return;
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }
    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

class TempDirectory final {
  public:
    TempDirectory() {
        if (auto created =
                bloom::platform::createPrivateTemporaryDirectory("bloom-project-capacity-"))
            path_ = std::move(*created);
    }
    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;
    ~TempDirectory() {
        if (path_.empty())
            return;
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    [[nodiscard]] bool isValid() const noexcept { return !path_.empty(); }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
};

[[nodiscard]] project::ProjectIoOperationMemory makeOperation(const std::uint64_t limitBytes) {
    auto coordinator = project::ProjectIoMemoryCoordinator::create(limitBytes);
    if (!coordinator.has_value())
        std::abort();
    auto operation = coordinator->createOperation(limitBytes, limitBytes);
    if (!operation.has_value())
        std::abort();
    return std::move(*operation);
}

[[nodiscard]] document::ColorSettings neutralColorSettings() {
    std::array<std::uint8_t, 32> bytes{};
    std::iota(bytes.begin(), bytes.end(), std::uint8_t{0});
    return document::makeBloomNeutralColorSettingsV1(bloom::core::Sha256Digest::fromBytes(bytes));
}

[[nodiscard]] std::string readFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

[[nodiscard]] std::span<const std::byte> asBytes(const std::string& bytes) noexcept {
    return {reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()};
}

// Parameters 1..`curves` animated by two-component curves of `keysPerComponent` keys each (the
// field project's keyframe-dense shape), and `spare` constant position parameters after them for
// edits to animate.
struct Built final {
    document::Project project;
    document::CompositionId composition;
};
[[nodiscard]] Built buildProject(const std::uint64_t curves, const std::int64_t keysPerComponent,
                                 const std::uint64_t spare) {
    const auto duration = bloom::core::RationalTime::create(240, 24);
    auto created = document::makeNewProject("Capacity", "Main", *duration);
    auto* composition = created.project.findComposition(created.initialCompositionId);
    if (composition == nullptr)
        throw std::logic_error("fixture composition");
    std::uint64_t nextKey = 1;
    for (std::uint64_t curve = 1; curve <= curves; ++curve) {
        std::array<document::ComponentAnimationCurve, 2> components;
        for (std::int64_t frame = 0; frame < keysPerComponent; ++frame) {
            const auto at = bloom::core::RationalTime::create(frame, 30);
            for (auto& component : components)
                component.keyframes.push_back({document::KeyframeId::fromRaw(nextKey++), at.value(),
                                               360.0 + static_cast<double>(frame) * 0.25,
                                               document::KeyframeInterpolation::Linear});
        }
        if (!composition->parameters().insert(
                {document::ParameterId::fromRaw(curve),
                 std::string(document::kPositionParameterSchemaKey),
                 document::AnimationCurveSource{document::AnimationCurveId::fromRaw(curve)}}) ||
            !composition->animationCurves().insert(document::Vec2AnimationCurve{
                document::AnimationCurveId::fromRaw(curve), std::move(components)}))
            throw std::logic_error("fixture curve");
    }
    for (std::uint64_t index = 1; index <= spare; ++index)
        if (!composition->parameters().insert(
                {document::ParameterId::fromRaw(curves + index),
                 std::string(document::kPositionParameterSchemaKey),
                 document::ConstantValueSource{document::Vec2d{0.0, 0.0}}}))
            throw std::logic_error("fixture spare parameter");
    return {std::move(created.project), created.initialCompositionId};
}

[[nodiscard]] host::ProjectSession makeSession(host::ProjectSessionIdentitySource& identities,
                                               document::Project builtProject) {
    auto result = host::ProjectSession::createDecoded(
        identities, {.project = std::move(builtProject),
                     .colorSettings = neutralColorSettings(),
                     .editability = host::DecodedProjectEditability::Editable,
                     .displayPath = std::nullopt,
                     .persistedAllocatorHighWater = std::nullopt});
    if (!result)
        throw std::logic_error("session fixture");
    return std::move(result).takeSession();
}

[[nodiscard]] document::Snapshot snapshotOf(const host::ProjectSession& session) {
    auto decoded = session.decodedSnapshot();
    if (!decoded)
        throw std::logic_error("decoded snapshot");
    return decoded.snapshot();
}

[[nodiscard]] document::Revision revisionOf(const host::ProjectSession& session) {
    return *session.stateSnapshot().currentRevision;
}

[[nodiscard]] project::CanonicalDocumentMeasureResult
measure(const document::Snapshot& snapshot, const document::ColorSettings& colors,
        project::CanonicalCurveSizeMemo* memo = nullptr) {
    return project::measureCanonicalDocument({.snapshot = &snapshot, .colorSettings = &colors}, {},
                                             memo);
}

// `count` keys at frames `first`, `first + 1`, ... for a position parameter.
[[nodiscard]] commands::Transaction addKeys(const host::ProjectSession& session,
                                            const document::CompositionId composition,
                                            const std::uint64_t parameter, const std::int64_t first,
                                            const std::int64_t count) {
    std::vector<commands::ParameterKey> keys;
    keys.reserve(static_cast<std::size_t>(count));
    for (std::int64_t frame = first; frame < first + count; ++frame)
        keys.push_back({.time = *bloom::core::RationalTime::create(frame, 30),
                        .value = document::Vec2d{static_cast<double>(frame) * 0.125, 3.5},
                        .interpolation = document::KeyframeInterpolation::Linear});
    commands::Transaction transaction("Add keys", revisionOf(session));
    transaction.emplace<commands::SetParameterKeys>(
        composition, document::ParameterId::fromRaw(parameter), std::move(keys), false);
    return transaction;
}

// The memo makes a repeated measure walk only the curves that changed; after each kind of change
// it equals a whole measure (and the encoder's own exact size), and it forgets curves that left.
void testMemoMatchesWholeMeasure(Expectations& expectations) {
    host::ProjectSessionIdentitySource identities;
    auto built = buildProject(12, 24, 2);
    const auto composition = built.composition;
    auto session = makeSession(identities, std::move(built.project));
    const auto colors = neutralColorSettings();
    project::CanonicalCurveSizeMemo memo;
    const auto same = [&](const std::string_view step) {
        const auto snapshot = snapshotOf(session);
        const auto whole = measure(snapshot, colors);
        const auto remembered = measure(snapshot, colors, &memo);
        const auto again = measure(snapshot, colors, &memo);
        const auto scratch = project::canonicalDocumentScratchRequirements(snapshot);
        std::vector<char> payload(scratch.payloadBytes);
        std::vector<std::size_t> sort(scratch.sortEntries);
        const auto exact = project::canonicalDocumentSize({.snapshot = &snapshot,
                                                           .colorSettings = &colors,
                                                           .payloadScratch = payload,
                                                           .sortScratch = sort});
        const auto equal = [](const project::CanonicalDocumentMeasure& a,
                              const project::CanonicalDocumentMeasure& b) {
            return a.bytes == b.bytes && a.values == b.values &&
                   a.largestContainerEntries == b.largestContainerEntries &&
                   a.largestStringBytes == b.largestStringBytes;
        };
        expectations.expect(whole && remembered && again && exact &&
                                equal(whole.measure, remembered.measure) &&
                                equal(whole.measure, again.measure) && exact.value() != nullptr &&
                                whole.measure.bytes == *exact.value(),
                            std::string(step) + ": the remembered measure equals a whole one");
    };
    same("as built");
    expectations.expect(memo.size() == 12, "every curve is remembered");
    // A changed curve (a new record), a new curve, and the first curve leaving (the one after it
    // becomes its array's first element, one byte shorter).
    expectations.expect(session.execute(addKeys(session, composition, 5, 100, 7)).changed(),
                        "keys are added to a curve");
    same("a curve changed");
    expectations.expect(session.execute(addKeys(session, composition, 13, 0, 3)).changed(),
                        "a constant parameter is animated");
    same("a curve added");
    commands::Transaction constant("Constant", revisionOf(session));
    constant.emplace<commands::ConvertAnimationToConstant>(
        composition, document::ParameterId::fromRaw(1), document::Vec2d{1.0, 2.0});
    expectations.expect(session.execute(std::move(constant)).changed(),
                        "the first curve is removed");
    same("the first curve removed");
    expectations.expect(memo.size() == 12, "a curve that left is forgotten");
}

// The issue's own case, at full size. A keyframe-dense project just under the limit (about 6
// million JSON values, more than the field project's 4.5 million): an edit that would take it past
// the old 8,000,000-value read limit, and one that would take it just past the new limit, are both
// refused at the edit with the limit named and nothing applied; edits that fit still apply; and the
// project then saves within the default memory budget and reopens with the same content.
void testPastTheOldLimitIsRefusedAtTheEdit(Expectations& expectations) {
    const auto colors = neutralColorSettings();
    const project::ProjectCapacityLimits limits;
    constexpr std::int64_t kKeysPerComponent = 240;
    // The JSON values one fixture curve adds, measured.
    std::uint64_t perCurve = 0;
    std::uint64_t empty = 0;
    {
        auto small = buildProject(0, kKeysPerComponent, 0);
        auto ten = buildProject(10, kKeysPerComponent, 0);
        document::Document a{std::move(small.project)};
        document::Document b{std::move(ten.project)};
        const auto sa = a.snapshot();
        const auto sb = b.snapshot();
        empty = measure(sa, colors).measure.values;
        perCurve = (measure(sb, colors).measure.values - empty) / 10;
    }
    expectations.expect(perCurve > 0, "a fixture curve has a measure");
    if (perCurve == 0)
        return;
    // Two to three curves' worth below the limit.
    const auto curves = (limits.jsonValues - empty) / perCurve - 2;
    host::ProjectSessionIdentitySource identities;
    auto built = buildProject(curves, kKeysPerComponent, 3);
    const auto composition = built.composition;
    auto session = makeSession(identities, std::move(built.project));
    const auto start = measure(snapshotOf(session), colors);
    std::cerr << "  capacity fixture: " << curves << " curves, " << start.measure.values
              << " JSON values, " << start.measure.bytes << " bytes\n";
    expectations.expect(start && start.measure.values <= limits.jsonValues &&
                            start.measure.values > 4'480'445,
                        "the fixture sits just under the limit, past the field project's size");

    // An edit that fits applies. What it added, per key, sizes the edit just past the limit.
    constexpr std::int64_t kFittingKeys = 24;
    const auto fits = session.execute(addKeys(session, composition, curves + 1, 0, kFittingKeys));
    expectations.expect(fits.changed(), "an edit that fits applies");
    const auto fitted = measure(snapshotOf(session), colors);
    const auto perKey = (fitted.measure.values - start.measure.values) / kFittingKeys;
    expectations.expect(fitted && perKey > 0 && fitted.measure.values < limits.jsonValues,
                        "the edit that fits stays under the limit");
    if (perKey == 0 || fitted.measure.values >= limits.jsonValues)
        return;

    // Past the old 8,000,000 read limit in one edit: refused at the edit, nothing applied.
    {
        const auto before = revisionOf(session);
        const auto past = session.execute(addKeys(session, composition, curves + 2, 0, 150'000));
        const auto& command = past.command;
        expectations.expect(command.has_value() &&
                                command->status == commands::CommandStatus::ProjectTooLarge &&
                                command->capacityRefusal.has_value() &&
                                command->capacityRefusal->limit == "projectJsonValues" &&
                                command->capacityRefusal->value == limits.jsonValues &&
                                command->capacityRefusal->actual >= 8'000'000,
                            "an edit past the old limit is refused at the edit");
        if (command && command->capacityRefusal)
            std::cerr << "  " << command->capacityRefusal->message << '\n';
        expectations.expect(revisionOf(session) == before, "and nothing is applied");
    }
    // Just past the new limit: refused with what it would hold.
    {
        const auto before = revisionOf(session);
        // Twice the room left, in keys: past the limit, far short of the old one.
        const auto keys =
            static_cast<std::int64_t>(2 * (limits.jsonValues - fitted.measure.values) / perKey) + 1;
        const auto over = session.execute(addKeys(session, composition, curves + 3, 0, keys));
        const auto& command = over.command;
        expectations.expect(command.has_value() &&
                                command->status == commands::CommandStatus::ProjectTooLarge &&
                                command->capacityRefusal.has_value() &&
                                command->capacityRefusal->actual > limits.jsonValues &&
                                command->capacityRefusal->actual < 8'000'000 &&
                                command->capacityRefusal->unit == "values" &&
                                command->capacityRefusal->message.starts_with(
                                    "The edit was not applied: the project would hold "),
                            "an edit just past the limit is refused with what it would hold");
        if (command && command->capacityRefusal)
            std::cerr << "  " << command->capacityRefusal->message << '\n';
        expectations.expect(revisionOf(session) == before, "and nothing is applied");
    }
    // An edit that does not grow the project applies even this close to the limit.
    {
        commands::Transaction rename("Rename", revisionOf(session));
        rename.emplace<commands::SetProjectName>("Capacity at the limit");
        expectations.expect(session.execute(std::move(rename)).changed(),
                            "an edit that does not grow the project applies");
    }

    // The project as it stands saves and reopens, within the default memory budget.
    TempDirectory directory;
    auto coordinator = host::PublicationCoordinator::create();
    auto artifacts = bloom::platform::StagedArtifactCoordinator::create({});
    expectations.expect(directory.isValid() && coordinator.has_value() && artifacts.succeeded(),
                        "the save fixture is available");
    if (!directory.isValid() || !coordinator.has_value() || !artifacts.succeeded())
        return;
    auto artifactCoordinator = std::move(artifacts).takeCoordinator();
    const auto target = directory.path() / "capacity.bloom";
    const auto saveAs = session.advancePathIntentForSaveAs();
    expectations.expect(static_cast<bool>(saveAs), "Save As takes the path");
    if (!saveAs)
        return;
    const auto admitted = measure(snapshotOf(session), colors);
    auto saved = host::saveProjectSession(
        session, *coordinator, artifactCoordinator,
        {.targetPath = target,
         .overwritePolicy = bloom::platform::ArtifactOverwritePolicy::CreateOnly,
         .expectedTarget = std::nullopt,
         .limits = {},
         .intent = saveAs.capture()},
        makeOperation(project::kDefaultProjectIoOperationResidentBytes));
    expectations.expect(static_cast<bool>(saved), "the project at the limit saves");
    if (!saved)
        return;
    const auto bytes = readFile(target);
    auto reopened = project::openProjectArchive(
        asBytes(bytes), project::SaveArchiveLimits{},
        makeOperation(project::kDefaultProjectIoOperationResidentBytes));
    expectations.expect(reopened.outcome() == project::OpenArchiveOutcome::Opened, "and reopens");
    if (reopened.outcome() != project::OpenArchiveOutcome::Opened)
        return;
    const auto opened = std::move(reopened).takeOpened();
    const auto again = measure(opened.document->snapshot(), opened.colorSettings);
    expectations.expect(again && again.measure.values == admitted.measure.values &&
                            again.measure.bytes == admitted.measure.bytes,
                        "with the same content");
}

} // namespace

int main() {
    Expectations expectations;
    try {
        testMemoMatchesWholeMeasure(expectations);
        testPastTheOldLimitIsRefusedAtTheEdit(expectations);
    } catch (const std::exception& error) {
        std::cerr << "unexpected exception: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
