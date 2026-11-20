#include <bloom/host/project_session.hpp>
#include <bloom/host/publication_coordinator.hpp>
#include <bloom/host/session_open.hpp>
#include <bloom/host/session_save.hpp>
#include <bloom/platform/private_temporary_directory.hpp>
#include <bloom/platform/staged_artifact.hpp>

#include <bloom/commands/animation_operations.hpp>
#include <bloom/commands/command_stack.hpp>
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

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
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
#include <thread>
#include <utility>
#include <vector>

// bloom.host.project-capacity-off-thread (issue #748): the project capacity check never walks the
// whole document on the owner (UI) thread. The size an edit is measured from is known when the
// project is opened (measured on the opening thread) or created; an edit built on the owner thread
// counts only the records it changed, and that measure equals a whole one; a prepared (agent or
// script) commit is measured during its preparation, off the owner thread, and publication only
// reads the result -- a commit re-based on a newer revision is measured again there, and one too
// large is refused with nothing applied.
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
        if (auto created = bloom::platform::createPrivateTemporaryDirectory(
                "bloom-project-capacity-off-thread-"))
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

// Parameters 1..`curves` animated by two-component curves of `keysPerComponent` keys each, and
// `spare` constant position parameters after them for edits to animate (the shape of
// bloom.host.project-capacity's fixture).
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

[[nodiscard]] commands::CommandStack& stackOf(host::ProjectSession& session) {
    auto* stack = session.liveDocumentAndStack().second;
    if (stack == nullptr)
        throw std::logic_error("command stack");
    return *stack;
}

[[nodiscard]] project::CanonicalDocumentIndexResult
wholeIndex(const document::Snapshot& snapshot, const document::ColorSettings& colors) {
    return project::indexCanonicalDocument({.snapshot = &snapshot, .colorSettings = &colors});
}

// `count` keys at frames `first`, `first + 1`, ... for a position parameter; `expected` the
// revision the transaction expects (none lets a preparation be re-based).
[[nodiscard]] commands::Transaction addKeys(const document::CompositionId composition,
                                            const std::uint64_t parameter, const std::int64_t first,
                                            const std::int64_t count,
                                            const std::optional<document::Revision> expected) {
    std::vector<commands::ParameterKey> keys;
    keys.reserve(static_cast<std::size_t>(count));
    for (std::int64_t frame = first; frame < first + count; ++frame)
        keys.push_back({.time = *bloom::core::RationalTime::create(frame, 30),
                        .value = document::Vec2d{static_cast<double>(frame) * 0.125, 3.5},
                        .interpolation = document::KeyframeInterpolation::Linear});
    commands::Transaction transaction("Add keys", expected);
    transaction.emplace<commands::SetParameterKeys>(
        composition, document::ParameterId::fromRaw(parameter), std::move(keys), false);
    return transaction;
}

// Prepares `transaction` on another thread, as a live commit's client does.
[[nodiscard]] commands::PreparedTransaction prepareElsewhere(commands::CommandStack& stack,
                                                             const document::Snapshot& base,
                                                             commands::Transaction transaction) {
    const auto owner = std::this_thread::get_id();
    auto preparing = std::async(std::launch::async, [&, owner] {
        if (std::this_thread::get_id() == owner)
            throw std::logic_error("prepared on the owner thread");
        return commands::CommandStack::prepare(base, std::move(transaction), {}, {},
                                               stack.capacityCheck());
    });
    return preparing.get();
}

// After every kind of edit, the index advanced from the previous one equals a whole measure (bytes
// and values exactly, the encoder's own size too; the maxima never fall below the whole's), from
// one step back and across several (undo, redo).
void testAdvancedIndexEqualsWholeMeasure(Expectations& expectations) {
    host::ProjectSessionIdentitySource identities;
    auto built = buildProject(16, 12, 4);
    const auto composition = built.composition;
    auto session = makeSession(identities, std::move(built.project));
    const auto colors = [&] { return *session.colorSettings(); };
    auto previous = snapshotOf(session);
    auto start = wholeIndex(previous, colors());
    expectations.expect(static_cast<bool>(start), "the fixture is measured");
    if (!start)
        return;
    auto index = start.index;
    const auto same = [&](const std::string_view step) {
        const auto snapshot = snapshotOf(session);
        const auto settings = colors();
        const auto advanced = project::advanceCanonicalDocumentIndex(
            *index, previous, {.snapshot = &snapshot, .colorSettings = &settings});
        const auto whole = wholeIndex(snapshot, settings);
        std::vector<char> payload(1 << 20);
        const auto scratch = project::canonicalDocumentScratchRequirements(snapshot);
        std::vector<std::size_t> sort(scratch.sortEntries);
        const auto exact = project::canonicalDocumentSize({.snapshot = &snapshot,
                                                           .colorSettings = &settings,
                                                           .payloadScratch = payload,
                                                           .sortScratch = sort});
        const bool equal =
            advanced && whole && exact && exact.value() != nullptr &&
            advanced.index->measure.bytes == whole.index->measure.bytes &&
            advanced.index->measure.bytes == *exact.value() &&
            advanced.index->measure.values == whole.index->measure.values &&
            advanced.index->measure.largestContainerEntries >=
                whole.index->measure.largestContainerEntries &&
            advanced.index->measure.largestStringBytes >= whole.index->measure.largestStringBytes;
        expectations.expect(equal, std::string(step) + ": the advanced index equals a whole one");
        if (advanced) {
            index = advanced.index;
            previous = snapshot;
        }
        return advanced;
    };
    const auto run = [&](commands::Transaction transaction, const std::string_view step) {
        expectations.expect(session.execute(std::move(transaction)).changed(),
                            std::string(step) + " applies");
        return same(step);
    };
    // A changed curve counts its old and new records only, and the composition's own members.
    const auto changed =
        run(addKeys(composition, 3, 100, 5, revisionOf(session)), "keys added to a curve");
    expectations.expect(changed.recordsCounted == 2 && changed.compositionsCounted == 1,
                        "a one-curve edit counts the curve it replaced and its replacement");
    run(addKeys(composition, 17, 0, 3, revisionOf(session)), "a constant parameter animated");
    {
        commands::Transaction constant("Constant", revisionOf(session));
        constant.emplace<commands::ConvertAnimationToConstant>(
            composition, document::ParameterId::fromRaw(1), document::Vec2d{1.0, 2.0});
        run(std::move(constant), "the first curve removed");
    }
    {
        commands::Transaction rename("Rename", revisionOf(session));
        rename.emplace<commands::SetProjectName>("Capacity renamed, longer");
        const auto renamed = run(std::move(rename), "the project renamed");
        expectations.expect(renamed.recordsCounted == 0 && renamed.compositionsCounted == 0,
                            "a project rename counts no record and no composition");
    }
    {
        commands::Transaction rename("Rename composition", revisionOf(session));
        rename.emplace<commands::SetCompositionName>(composition, "Main, renamed");
        const auto renamed = run(std::move(rename), "the composition renamed");
        expectations.expect(renamed.recordsCounted == 0 && renamed.compositionsCounted == 1,
                            "a composition rename counts the composition's own members only");
    }
    std::optional<document::CompositionId> added;
    {
        commands::Transaction add("Add composition", revisionOf(session));
        add.emplace<commands::AddComposition>("Second", document::CompositionFormat{},
                                              bloom::core::RationalTime::fromInteger(2));
        const auto result = session.execute(std::move(add));
        expectations.expect(result.changed(), "a composition added applies");
        same("a composition added");
        for (const auto& candidate : snapshotOf(session).project().compositions())
            if (candidate.id() != composition)
                added = candidate.id();
    }
    expectations.expect(added.has_value(), "the added composition exists");
    if (added) {
        commands::Transaction remove("Delete composition", revisionOf(session));
        remove.emplace<commands::DeleteComposition>(*added);
        run(std::move(remove), "a composition deleted");
    }
    // Several steps at once: undo twice, then redo once, measured from the last measured state.
    expectations.expect(session.undo().changed() && session.undo().changed(), "undo applies");
    same("two undos");
    expectations.expect(session.redo().changed(), "redo applies");
    same("a redo");
    // New color settings change the document's own members.
    auto settings = colors();
    settings.processColorSpaceId =
        settings.processColorSpaceId == "lin_rec709_srgb" ? "lin_rec2020" : "lin_rec709_srgb";
    if (session.setColorSettings(settings) == host::ProjectSessionColorSettingsStatus::Updated)
        same("new color settings");
}

// An opened project's size is measured on the thread that opened it: the session knows it at once,
// without a walk of its own, and the first edit counts only the records it changed.
void testOpenedProjectArrivesMeasured(Expectations& expectations) {
    host::ProjectSessionIdentitySource identities;
    auto built = buildProject(200, 24, 2);
    const auto composition = built.composition;
    auto session = makeSession(identities, std::move(built.project));
    TempDirectory directory;
    auto coordinator = host::PublicationCoordinator::create();
    auto artifacts = bloom::platform::StagedArtifactCoordinator::create({});
    expectations.expect(directory.isValid() && coordinator.has_value() && artifacts.succeeded(),
                        "the save fixture is available");
    if (!directory.isValid() || !coordinator.has_value() || !artifacts.succeeded())
        return;
    auto artifactCoordinator = std::move(artifacts).takeCoordinator();
    const auto target = directory.path() / "measured.bloom";
    const auto saveAs = session.advancePathIntentForSaveAs();
    if (!saveAs)
        return;
    const auto saved = host::saveProjectSession(
        session, *coordinator, artifactCoordinator,
        {.targetPath = target,
         .overwritePolicy = bloom::platform::ArtifactOverwritePolicy::CreateOnly,
         .expectedTarget = std::nullopt,
         .limits = {},
         .intent = saveAs.capture()},
        makeOperation(project::kDefaultProjectIoOperationResidentBytes));
    expectations.expect(static_cast<bool>(saved), "the fixture saves");
    if (!saved)
        return;
    const auto bytes = readFile(target);
    const std::span<const std::byte> archive(reinterpret_cast<const std::byte*>(bytes.data()),
                                             bytes.size());

    // Opened on another thread, as the desktop's open worker does; installed on this one.
    const auto admission = session.admitOpenIntent();
    expectations.expect(static_cast<bool>(admission), "the open is admitted");
    if (!admission)
        return;
    auto opening = std::async(std::launch::async, [&] {
        return project::openProjectArchive(
            archive, {}, makeOperation(project::kDefaultProjectIoOperationResidentBytes));
    });
    auto opened = opening.get();
    expectations.expect(opened.outcome() == project::OpenArchiveOutcome::Opened &&
                            opened.opened() != nullptr && opened.opened()->sizeIndex != nullptr,
                        "the opening thread measured the document");
    const auto installed = host::installOpenedArchiveResult(
        session, admission.capture(), std::move(opened), host::ProjectDisplayPath::create(target));
    expectations.expect(static_cast<bool>(installed), "the opened project is installed");
    const auto ready = session.capacityWork();
    expectations.expect(ready.known && ready.wholeMeasures == 0,
                        "the session knows the size without measuring the document itself");

    // The first edit: one curve changed, out of 200.
    expectations.expect(
        session.execute(addKeys(composition, 7, 200, 1, revisionOf(session))).changed(),
        "a one-key edit applies");
    const auto first = session.capacityWork();
    expectations.expect(first.wholeMeasures == 0 && first.ownerMeasures == 1 &&
                            first.ownerRecordsCounted == 2 && first.ownerCompositionsCounted == 1 &&
                            first.unmeasuredEdits == 0 && first.adopted == 1,
                        "the first edit counts the curve it changed, not the document");
    std::cerr << "  first edit after open: records counted " << first.ownerRecordsCounted
              << ", whole measures " << first.wholeMeasures << '\n';

    // Content installed without a size has it measured in the background, never at an edit.
    {
        const auto again = session.admitOpenIntent();
        auto reopened = project::openProjectArchive(
            archive, {}, makeOperation(project::kDefaultProjectIoOperationResidentBytes));
        if (!again || reopened.outcome() != project::OpenArchiveOutcome::Opened)
            return;
        auto content = std::move(reopened).takeOpened();
        content.sizeIndex = nullptr;
        const auto status = session.installDecodedReplacement(
            again.capture(),
            host::DecodedReplacementContent(
                std::move(content.document), std::move(content.colorSettings),
                std::move(content.roundTrip), content.schemaMinor, std::move(content.requirements),
                host::DecodedProjectEditability::Editable, std::nullopt, std::nullopt));
        expectations.expect(status == host::SessionInstallStatus::Installed,
                            "content without a size installs");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!session.capacityWork().known && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        const auto background = session.capacityWork();
        expectations.expect(background.known && background.wholeMeasures == 1 &&
                                background.ownerMeasures == 0,
                            "it is measured in the background");
        expectations.expect(
            session.execute(addKeys(composition, 8, 200, 1, revisionOf(session))).changed(),
            "an edit after the background measure applies");
        const auto after = session.capacityWork();
        expectations.expect(after.ownerMeasures == 1 && after.ownerRecordsCounted == 2 &&
                                after.wholeMeasures == 1,
                            "and is measured from what it changed");
    }
}

// A prepared commit is measured off the owner thread: publication reads the result. One that would
// take the project past the limit is refused at publication with nothing applied, and one re-based
// on a newer revision is measured again during its new preparation.
void testPreparedCommitsAreMeasuredOffTheOwnerThread(Expectations& expectations) {
    const auto colors = neutralColorSettings();
    const project::ProjectCapacityLimits limits;
    constexpr std::int64_t kKeysPerComponent = 240;
    std::uint64_t perCurve = 0;
    std::uint64_t empty = 0;
    {
        auto small = buildProject(0, kKeysPerComponent, 0);
        auto ten = buildProject(10, kKeysPerComponent, 0);
        document::Document a{std::move(small.project)};
        document::Document b{std::move(ten.project)};
        empty = wholeIndex(a.snapshot(), colors).index->measure.values;
        perCurve = (wholeIndex(b.snapshot(), colors).index->measure.values - empty) / 10;
    }
    // About six curves' worth of room under the limit.
    const auto curves = (limits.jsonValues - empty) / perCurve - 6;
    host::ProjectSessionIdentitySource identities;
    auto built = buildProject(curves, kKeysPerComponent, 6);
    const auto composition = built.composition;
    auto session = makeSession(identities, std::move(built.project));
    auto& stack = stackOf(session);
    const auto valuesNow = [&] {
        return wholeIndex(snapshotOf(session), colors).index->measure.values;
    };
    const auto created = session.capacityWork();
    expectations.expect(created.known && created.wholeMeasures == 1,
                        "a created project is measured when it is created");

    // A prepared commit that fits: measured in its preparation, adopted at publication.
    const auto start = valuesNow();
    {
        auto prepared = prepareElsewhere(stack, snapshotOf(session),
                                         addKeys(composition, curves + 1, 0, 24, std::nullopt));
        const auto result = stack.publish(std::move(prepared));
        expectations.expect(result.changed(), "a prepared commit that fits publishes");
    }
    const auto fitted = valuesNow();
    const auto perKey = (fitted - start) / 24;
    std::cerr << "  prepared-commit fixture: " << curves << " curves, " << fitted
              << " JSON values, " << perKey << " per key\n";
    auto work = session.capacityWork();
    expectations.expect(work.preparationMeasures == 1 && work.ownerMeasures == 0 &&
                            work.adopted == 1 && work.preparationWholeMeasures == 0,
                        "it is measured in its preparation and publication reads the result");
    expectations.expect(perKey > 0 && fitted < limits.jsonValues,
                        "the fixture sits just under the limit");
    if (perKey == 0 || fitted >= limits.jsonValues)
        return;
    const auto room = limits.jsonValues - fitted;

    // Too large: refused at publication, from its preparation's measure, with nothing applied.
    {
        const auto before = revisionOf(session);
        const auto history = stack.size();
        const auto keys = static_cast<std::int64_t>(2 * room / perKey) + 1;
        auto prepared = prepareElsewhere(stack, snapshotOf(session),
                                         addKeys(composition, curves + 2, 0, keys, before));
        const auto result = stack.publish(std::move(prepared));
        expectations.expect(result.status == commands::CommandStatus::ProjectTooLarge &&
                                result.capacityRefusal.has_value() &&
                                result.capacityRefusal->limit == "projectJsonValues" &&
                                result.capacityRefusal->actual > limits.jsonValues,
                            "a prepared commit past the limit is refused");
        expectations.expect(revisionOf(session) == before && stack.size() == history,
                            "and nothing is applied");
        work = session.capacityWork();
        expectations.expect(work.preparationMeasures == 2 && work.ownerMeasures == 0,
                            "the refusal was measured off the owner thread");
    }

    // Re-based: prepared when it fits; the artist's own edit then grows the project, so the commit
    // is prepared again on the live revision, measured again there, and refused.
    {
        const auto agentKeys = static_cast<std::int64_t>(room * 6 / 10 / perKey);
        const auto artistKeys = agentKeys;
        auto prepared =
            prepareElsewhere(stack, snapshotOf(session),
                             addKeys(composition, curves + 3, 0, agentKeys, std::nullopt));
        expectations.expect(session.capacityWork().preparationMeasures == 3,
                            "the agent's commit is measured in its preparation");
        expectations.expect(
            session.execute(addKeys(composition, curves + 4, 0, artistKeys, revisionOf(session)))
                .changed(),
            "the artist's edit, made meanwhile, applies");
        expectations.expect(stack.rebaseNeeded(prepared), "the agent's commit must be re-based");
        const auto before = revisionOf(session);
        const auto history = stack.size();
        auto transaction = std::move(prepared).takeTransaction();
        auto rebased = prepareElsewhere(stack, snapshotOf(session), std::move(transaction));
        const auto result = stack.publish(std::move(rebased));
        expectations.expect(result.status == commands::CommandStatus::ProjectTooLarge &&
                                result.capacityRefusal.has_value() &&
                                result.capacityRefusal->limit == "projectJsonValues",
                            "the re-based commit, measured again, is refused");
        expectations.expect(revisionOf(session) == before && stack.size() == history,
                            "and nothing is applied");
        work = session.capacityWork();
        expectations.expect(work.preparationMeasures == 4 && work.ownerMeasures == 1 &&
                                work.preparationWholeMeasures == 0,
                            "re-measured in its new preparation; the owner measured only the "
                            "artist's edit");
    }
}

} // namespace

int main() {
    Expectations expectations;
    try {
        testAdvancedIndexEqualsWholeMeasure(expectations);
        testOpenedProjectArrivesMeasured(expectations);
        testPreparedCommitsAreMeasuredOffTheOwnerThread(expectations);
    } catch (const std::exception& error) {
        std::cerr << "unexpected exception: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
