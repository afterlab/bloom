// #642: `bloom.layer.add-svg` takes a `mode` ("layer", the default, or "shapes") and reads and
// parses the file where the operation is created (the client thread), never where it applies (the
// application's thread); `bloom.layer.break-apart-svg` turns the one-layer artwork into the
// editable shapes in place, as one undo step, from the copy the project holds.
#include <bloom/commands/svg_import_operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/document/project.hpp>
#include <bloom/document/vector_artwork_nodes.hpp>
#include <bloom/scripting/operation_registry.hpp>
#include <bloom/scripting/session.hpp>
#include <bloom/scripting/value.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stop_token>
#include <string>

namespace {

using bloom::scripting::Arguments;
using bloom::scripting::StableId;
using bloom::scripting::Value;

int failures = 0;
void expect(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

constexpr const char* kArtwork = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="100">
<defs><linearGradient id="g"><stop offset="0" stop-color="#fff"/><stop offset="1" stop-color="#000"/></linearGradient></defs>
<rect id="Box" width="40" height="30" fill="#f00"/>
<path id="Ring" fill="#0f0" fill-rule="evenodd" d="M60 10H100V50H60Z M70 20H90V40H70Z"/>
<circle id="Dot" cx="150" cy="50" r="20" fill="url(#g)"/></svg>)svg";

std::filesystem::path writeFile(const std::string& name) {
    const auto path = std::filesystem::temp_directory_path() / ("bloom-svg-modes-" + name + ".svg");
    std::ofstream(path, std::ios::binary) << kArtwork;
    return path;
}

std::size_t countNodes(const bloom::scripting::Session& session, const bloom::document::CompositionId composition,
                       const std::string_view type) {
    const auto snapshot = session.snapshot();
    const auto* found = snapshot.project().findComposition(composition);
    if (found == nullptr)
        return 0;
    return static_cast<std::size_t>(std::ranges::count_if(
        found->graph().nodes(), [&](const auto& node) { return node.typeId == type; }));
}

std::unique_ptr<bloom::scripting::Session> newSession() {
    auto created = bloom::scripting::Session::createNew();
    if (!created) {
        std::cerr << "session creation failed\n";
        std::exit(1);
    }
    return std::move(created).takeSession();
}

} // namespace

int main() {
    using bloom::document::kPathValueNodeType;
    using bloom::document::kVectorArtworkNodeType;
    const auto path = writeFile("a");
    const std::string file = path.string();

    // Omitted: one layer. The graph gets a Vector Artwork node and none of the file's parts.
    {
        auto session = newSession();
        const auto composition = session->snapshot().project().compositions().front().id();
        const Value compositionValue{StableId{composition.value()}};
        const auto added = session->executeJsonOperation(
            "bloom.layer.add-svg", {{"composition", compositionValue}, {"path", Value{file}}});
        expect(added.succeeded(), "add-svg without a mode succeeds");
        expect(countNodes(*session, composition, kVectorArtworkNodeType) == 1 &&
                   countNodes(*session, composition, kPathValueNodeType) == 0,
               "an omitted mode imports as one layer");
        expect(added.command && added.command->outputId<bloom::document::NodeId>("node").has_value() &&
                   added.command->outputId<bloom::document::DataBlockRecordId>("dataBlock").has_value(),
               "the result names the node and the data block");

        // The same call with mode "shapes" in another session: the editable breakdown.
        auto editable = newSession();
        const auto editableComposition = editable->snapshot().project().compositions().front().id();
        const auto shapes = editable->executeJsonOperation(
            "bloom.layer.add-svg",
            {{"composition", Value{StableId{editableComposition.value()}}}, {"path", Value{file}}, {"mode", Value{"shapes"}}});
        expect(shapes.succeeded(), "add-svg with mode shapes succeeds");
        expect(countNodes(*editable, editableComposition, kVectorArtworkNodeType) == 0 &&
                   countNodes(*editable, editableComposition, kPathValueNodeType) > 0,
               "mode shapes imports the editable breakdown");

        // Break Apart: the layer's picture is the editable import's root group; the same graph.
        const auto layer = added.command->outputId<bloom::document::LayerId>("layer");
        expect(layer.has_value(), "the import names its layer");
        const auto broken = session->executeJsonOperation(
            "bloom.layer.break-apart-svg",
            {{"composition", compositionValue}, {"layer", Value{StableId{layer->value()}}}});
        expect(broken.succeeded(), "break-apart-svg succeeds");
        expect(countNodes(*session, composition, kVectorArtworkNodeType) == 0 &&
                   countNodes(*session, composition, kPathValueNodeType) ==
                       countNodes(*editable, editableComposition, kPathValueNodeType),
               "Break Apart leaves the editable import's Path nodes and no Vector Artwork");
        expect(broken.command && broken.command->outputId<bloom::document::NodeId>("merge").has_value(),
               "the result names the root group");
        // A second call has nothing to break apart, and says so.
        const auto again = session->executeJsonOperation(
            "bloom.layer.break-apart-svg",
            {{"composition", compositionValue}, {"layer", Value{StableId{layer->value()}}}});
        expect(!again.succeeded() && again.command && !again.command->operationFailures.empty() &&
                   again.command->operationFailures.front().issue.message.find("does not hold") != std::string::npos,
               "a layer that holds no Vector Artwork is refused by name");
        // One undo gives the layer back.
        const auto undone = session->undo();
        expect(undone.command && undone.command->changed() &&
                   countNodes(*session, composition, kVectorArtworkNodeType) == 1 &&
                   countNodes(*session, composition, kPathValueNodeType) == 0,
               "one undo restores the one-layer artwork");
    }

    // An invalid mode is named at creation, with the two spellings.
    {
        auto session = newSession();
        const auto composition = session->snapshot().project().compositions().front().id();
        const auto bad = session->executeJsonOperation(
            "bloom.layer.add-svg",
            {{"composition", Value{StableId{composition.value()}}}, {"path", Value{file}}, {"mode", Value{"flat"}}});
        expect(!bad.succeeded() && bad.diagnostic && bad.diagnostic->argument == "mode" &&
                   bad.diagnostic->message.find("\"layer\"") != std::string::npos &&
                   bad.diagnostic->message.find("\"shapes\"") != std::string::npos,
               "an invalid mode is refused, naming mode and both spellings");
    }

    // F7: the file is read and parsed when the operation is created. Deleting it before the
    // operation applies changes nothing, so applying reads no file (and enumerates no fonts).
    {
        auto session = newSession();
        const auto composition = session->snapshot().project().compositions().front().id();
        const auto fileBefore = writeFile("created");
        const auto registry = bloom::scripting::OperationRegistry::builtIn();
        auto created = registry.create("bloom.layer.add-svg",
                                       {{"composition", Value{StableId{composition.value()}}},
                                        {"path", Value{fileBefore.string()}}},
                                       session->operationContext());
        expect(static_cast<bool>(created), "the operation is created");
        std::filesystem::remove(fileBefore);
        if (created) {
            bloom::commands::Transaction transaction("Import SVG", session->snapshot().revision());
            expect(transaction.add(std::move(created).takeOperation()), "the operation joins a transaction");
            const auto applied = session->execute(std::move(transaction));
            expect(applied.succeeded() && countNodes(*session, composition, kVectorArtworkNodeType) == 1,
                   "applying after the file is gone imports it: the file was read at creation");
        }

        // A file that cannot be read still makes the operation; it rejects, naming the reason,
        // without reading anything when it applies.
        auto missing = registry.create("bloom.layer.add-svg",
                                       {{"composition", Value{StableId{composition.value()}}},
                                        {"path", Value{(std::filesystem::temp_directory_path() / "bloom-svg-modes-none.svg").string()}}},
                                       session->operationContext());
        expect(static_cast<bool>(missing), "an unreadable file still makes the operation");
        if (missing) {
            bloom::commands::Transaction transaction("Import SVG", session->snapshot().revision());
            (void)transaction.add(std::move(missing).takeOperation());
            const auto rejected = session->execute(std::move(transaction));
            expect(!rejected.succeeded() && rejected.command && !rejected.command->operationFailures.empty() &&
                       rejected.command->operationFailures.front().issue.message.find("SVG import failed") != std::string::npos,
                   "an unreadable file is rejected with its reason");
        }

        // A stop request at creation (the MCP client gave up) stops the read.
        std::stop_source stop;
        stop.request_stop();
        auto context = session->operationContext();
        context.stop = stop.get_token();
        const auto fileStopped = writeFile("stopped");
        auto stopped = registry.create("bloom.layer.add-svg",
                                       {{"composition", Value{StableId{composition.value()}}},
                                        {"path", Value{fileStopped.string()}}},
                                       context);
        expect(static_cast<bool>(stopped), "a stopped creation still returns an operation");
        if (stopped) {
            bloom::commands::Transaction transaction("Import SVG", session->snapshot().revision());
            (void)transaction.add(std::move(stopped).takeOperation());
            const auto result = session->execute(std::move(transaction));
            expect(!result.succeeded() && result.command && !result.command->operationFailures.empty() &&
                       result.command->operationFailures.front().issue.message.find("cancelled") != std::string::npos,
                   "a stop request cancels the read and the operation rejects");
        }
        std::filesystem::remove(fileStopped);
    }

    // Break Apart on a layer that holds no artwork is refused by name.
    {
        auto session = newSession();
        const auto composition = session->snapshot().project().compositions().front().id();
        const auto solid = session->executeJsonOperation(
            "bloom.layer.add-solid",
            {{"composition", Value{StableId{composition.value()}}},
             {"name", Value{"Red"}},
             {"color", Value{bloom::core::Color4d{1.0, 0.0, 0.0, 1.0}}}});
        const auto layer = solid.command ? solid.command->outputId<bloom::document::LayerId>("layer") : std::nullopt;
        if (!layer) {
            expect(false, "a solid layer is added");
        } else {
            const auto refused = session->executeJsonOperation(
                "bloom.layer.break-apart-svg",
                {{"composition", Value{StableId{composition.value()}}}, {"layer", Value{StableId{layer->value()}}}});
            expect(!refused.succeeded(), "a solid layer is not broken apart");
        }
    }
    std::filesystem::remove(path);
    return failures == 0 ? 0 : 1;
}
