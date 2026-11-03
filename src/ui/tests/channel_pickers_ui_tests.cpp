// #487 pickers on a real document: a rootless multi-layer EXR (no root R/G/B, as a layered 3D
// renderer writes it) feeds a Shuffle, Select Channels, Channel Copy and the Composition Output. The
// Shuffle slots are dropdowns of the input's channels plus 0 and 1 whose default shows the first colour
// layer it reads; the channel patterns and the output layers are checklists with the free-text
// pattern behind a toggle; a data reader picks a block of its kind; the Video audio stream on the
// node card is a dropdown. Properties and the node cards show the same controls.

#include "channel_checklist_field.hpp"
#include "node_editor_items.hpp"

#include <bloom/commands/asset_operations.hpp>
#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/channel_nodes.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/value_utility_nodes.hpp>
#include <bloom/runtime/channel_inspection_listing.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/channel_set_controller.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/text_field.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/node_editor.hpp>
#include <bloom/ui/properties_editor.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <ImathBox.h>
#include <ImfChannelList.h>
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>
#include <ImfOutputFile.h>

#include <QApplication>
#include <QElapsedTimer>
#include <QTest>

#include <filesystem>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace bloom;

int failures = 0;
void expect(const bool value, const std::string& message) {
    if (!value) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

template <typename Predicate> bool waitUntil(Predicate predicate, const int milliseconds = 15000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < milliseconds)
        QTest::qWait(5);
    return predicate();
}

constexpr int kSide = 4;

void writeRootlessExr(const std::filesystem::path& path) {
    const Imath::Box2i window(Imath::V2i(0, 0), Imath::V2i(kSide - 1, kSide - 1));
    Imf::Header header(window, window);
    const std::vector<std::string> names{"ViewLayer.Combined.R", "ViewLayer.Combined.G",
                                         "ViewLayer.Combined.B", "ViewLayer.Combined.A",
                                         "ViewLayer.Depth.Z",    "ViewLayer.Normal.X",
                                         "ViewLayer.Normal.Y",   "ViewLayer.Normal.Z"};
    std::vector<std::vector<float>> planes(names.size(), std::vector<float>(kSide * kSide, 0.5F));
    Imf::FrameBuffer frameBuffer;
    for (std::size_t index = 0; index < names.size(); ++index) {
        header.channels().insert(names[index], Imf::Channel(Imf::FLOAT));
        frameBuffer.insert(names[index],
                           Imf::Slice(Imf::FLOAT, reinterpret_cast<char*>(planes[index].data()),
                                      sizeof(float), kSide * sizeof(float)));
    }
    Imf::OutputFile file(path.string().c_str(), header);
    file.setFrameBuffer(frameBuffer);
    file.writePixels(kSide);
}

document::DataBlockRecord block(const std::uint64_t id, const document::DataBlockKind kind,
                                std::string typeId) {
    document::DataBlockRecord result;
    result.id = document::DataBlockRecordId::fromRaw(id);
    result.kind = kind;
    result.typeId = std::move(typeId);
    result.schemaVersion = {1, 0};
    result.mediaType = "application/vnd.bloom.test-data-block";
    result.provenance.source = document::DataBlockProducer{"bloom.test", "1", {}};
    result.provenance.createdAt = "2026-11-04T00:00:00Z";
    return result;
}

document::NewProject seededProject() {
    auto project = document::makeNewProject(
        "Pickers", "Main", core::RationalTime::fromInteger(1),
        *document::CompositionFormat::create(kSide, kSide, core::PixelAspectRatio::square(),
                                             *document::FrameRate::create(24, 1)));
    auto first = block(1, document::DataBlockKind::Curve, "bloom.test.first-curve");
    first.payload = document::DataBlockCurve{0.0, 1.0, {0.0, 1.0}};
    auto table = block(2, document::DataBlockKind::Table, "bloom.test.table");
    table.payload =
        document::DataBlockTable{{{"value", document::DataBlockValueKind::Float64, {1.0}}}};
    auto second = block(3, document::DataBlockKind::Curve, "bloom.test.second-curve");
    second.payload = document::DataBlockCurve{0.0, 1.0, {2.0, 3.0}};
    for (auto* value : {&first, &table, &second})
        if (!project.project.addDataBlock(std::move(*value)))
            throw std::runtime_error("data block rejected");
    return project;
}

struct Scene final {
    std::filesystem::path folder;
    document::NewProject initial = seededProject();
    document::Document document{std::move(initial.project)};
    commands::CommandStack stack{document};
    ui::CompositionSession session{document, stack, initial.initialCompositionId};
    document::NodeId source;
    document::NodeId shuffle;
    document::NodeId remove;
    document::NodeId copy;
    document::NodeId output;

    template <typename Operation, typename... Arguments>
    commands::CommandResult run(Arguments&&... arguments) {
        commands::Transaction transaction("Pickers", session.snapshot().revision());
        transaction.emplace<Operation>(std::forward<Arguments>(arguments)...);
        auto result = session.executeTransaction(std::move(transaction));
        if (!result.succeeded())
            throw std::runtime_error("fixture command refused");
        return result;
    }
    document::NodeId add(const std::string_view type) {
        return *run<commands::AddNode>(session.compositionId(), std::string(type),
                                       document::Vec2d{})
                    .outputId<document::NodeId>(commands::kAddNodeOutput);
    }
    void connect(const document::NodeId from, const std::string_view port,
                 const document::NodeId to, const std::string_view input) {
        run<commands::ConnectPorts>(session.compositionId(),
                                    document::OutputPortRef{from, std::string(port)},
                                    document::NodeInputRef{to, std::string(input)});
    }
    [[nodiscard]] document::ParameterValue value(const document::NodeId node,
                                                 const std::string_view role) const {
        const auto* record = session.composition()->graph().findNode(node);
        for (const auto& binding : record->parameters)
            if (binding.role == role)
                return std::get<document::ConstantValueSource>(
                           session.composition()->parameters().find(binding.parameterId)->source)
                    .value;
        throw std::runtime_error("binding missing");
    }
    Scene() {
        std::mt19937_64 random{std::random_device{}()};
        folder = std::filesystem::temp_directory_path() /
                 ("bloom-channel-pickers-" + std::to_string(random()));
        std::filesystem::create_directories(folder);
        writeRootlessExr(folder / "render.exr");
        run<commands::ImportAssets>(std::vector{folder / "render.exr"}, folder);
        run<commands::AddImageLayer>(session.compositionId(),
                                     session.snapshot().project().assets().front().id);
        for (const auto& node : session.composition()->graph().nodes()) {
            if (node.typeId == document::kImageSourceNodeType)
                source = node.id;
            if (node.typeId == document::kCompositionOutputNodeType)
                output = node.id;
        }
        shuffle = add(document::kChannelShuffleNodeType);
        remove = add(document::kChannelRemoveNodeType);
        copy = add(document::kChannelCopyNodeType);
        const auto channels = document::kImageSourceChannelsOutputPort;
        connect(source, channels, shuffle, document::kChannelSetPort);
        connect(source, channels, remove, document::kChannelSetPort);
        connect(remove, document::kChannelSetPort, copy, document::kChannelSetPort);
        connect(source, channels, copy, document::kChannelCopyFromPort);
        connect(source, channels, output, document::kCompositionOutputChannelsInputPort);
    }
    ~Scene() {
        std::error_code error;
        std::filesystem::remove_all(folder, error);
    }
    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;
};

struct Pipeline final {
    runtime::NodeDefinitionRegistry definitions;
    std::unique_ptr<runtime::SnapshotCompiler> compiler;
    Pipeline() {
        if (!runtime::registerBuiltInNodeDefinitions(definitions))
            throw std::runtime_error("built-in definitions");
        definitions.freeze();
        compiler = std::make_unique<runtime::SnapshotCompiler>(definitions);
    }
};

void run() {
    Scene scene;
    Pipeline pipeline;
    runtime::TaskScheduler scheduler;
    ui::TaskUiBridge bridge(scheduler, nullptr, std::chrono::milliseconds{1});
    ui::ChannelSetController channels(
        scene.session, scheduler, bridge,
        [&pipeline, base = scene.folder](
            const document::Snapshot& snapshot, const document::CompositionId composition,
            const document::NodeId node, const std::string& port, const core::RationalTime time,
            const runtime::EvaluationColorIntent& colorIntent,
            const runtime::CancellationToken& cancellation) {
            return runtime::listChannelInspectionChoices(*pipeline.compiler, snapshot, composition,
                                                         node, port, base, time, colorIntent,
                                                         cancellation);
        });
    expect(scene.session.channelSetController() == &channels,
           "the channel-set controller registers with the session");
    QWidget window;
    auto* properties = new ui::PropertiesEditor(scene.session, &window);
    auto* editor = new ui::NodeGraphEditor(scene.session, &window);
    properties->setGeometry(0, 0, 420, 800);
    editor->setGeometry(420, 0, 1000, 800);
    window.resize(1420, 800);
    window.show();
    QApplication::processEvents();

    // Shuffle: every slot is a searchable dropdown of the input's channels plus 0 and 1; the
    // default (empty) slots show the first colour layer's channels they read.
    scene.session.selectNode(scene.shuffle);
    QList<ui::kit::KDropdown*> slotBoxes;
    expect(waitUntil([&] {
               slotBoxes = properties->findChildren<ui::kit::KDropdown*>("propertiesShuffleSlot");
               return slotBoxes.size() == 4 && slotBoxes.front()->isEnabled() &&
                      slotBoxes.front()->count() == 10;
           }),
           "the Shuffle slotBoxes fill once the worker listing arrives");
    if (slotBoxes.size() == 4) {
        expect(slotBoxes[0]->currentText() == "ViewLayer.Combined.R" &&
                   slotBoxes[1]->currentText() == "ViewLayer.Combined.G" &&
                   slotBoxes[2]->currentText() == "ViewLayer.Combined.B" &&
                   slotBoxes[3]->currentText() == "1 (constant)",
               "the default slotBoxes show the rootless render's beauty channels and opaque alpha");
        expect(scene.value(scene.shuffle, "red") == document::ParameterValue{std::string{}},
               "showing the default writes nothing");
        expect(slotBoxes[0]->findData(QStringLiteral("ViewLayer.Depth.Z")) >= 0 &&
                   slotBoxes[0]->findData(QStringLiteral("0")) >= 0,
               "a slot offers every channel and the constants");
        slotBoxes[0]->chooseIndex(slotBoxes[0]->findData(QStringLiteral("ViewLayer.Depth.Z")));
        expect(scene.value(scene.shuffle, "red") ==
                   document::ParameterValue{std::string("ViewLayer.Depth.Z")},
               "choosing a channel writes its name");
    }
    const auto cardControl = [&](const document::NodeId node, const char* role) -> QWidget* {
        const auto controls =
            editor->graphScene()->nodeControlsForRoleForTest(node, QString::fromLatin1(role));
        return controls.isEmpty() ? nullptr : controls.front();
    };
    auto* cardGreen = qobject_cast<ui::kit::KDropdown*>(cardControl(scene.shuffle, "green"));
    expect(waitUntil([&] {
               return cardGreen != nullptr && cardGreen->currentText() == "ViewLayer.Combined.G";
           }),
           "the card's slot is the same dropdown with the same default");
    auto* cardRed = qobject_cast<ui::kit::KDropdown*>(cardControl(scene.shuffle, "red"));
    expect(cardRed != nullptr && cardRed->currentText() == "ViewLayer.Depth.Z",
           "a Properties choice projects to the card");

    // Select Channels: a checklist of the input's layers, empty by default.
    scene.session.selectNode(scene.remove);
    ui::ChannelChecklistField* checklist = nullptr;
    expect(waitUntil([&] {
               checklist =
                   properties->findChild<ui::ChannelChecklistField*>("propertiesChannelChecklist");
               return checklist != nullptr && checklist->entries().size() == 3;
           }),
           "Select Channels lists the input's three layers");
    if (checklist != nullptr && checklist->entries().size() == 3) {
        expect(checklist->entries()[1].token == "ViewLayer.Depth" &&
                   checklist->summary()->text() == "None" && checklist->summary()->isVisible() &&
                   !checklist->patternField()->isVisible(),
               "the default selects nothing and shows the checklist, not the pattern");
        checklist->setEntryChecked(1, true);
        expect(scene.value(scene.remove, "select") ==
                   document::ParameterValue{std::string("ViewLayer.Depth")},
               "checking a layer writes its name as the pattern");
        checklist->openChecklist();
        expect(checklist->checklist() != nullptr &&
                   checklist->checklist()->findChildren<QWidget*>("channelChecklistEntry").size() ==
                       3,
               "the checklist popup holds one checkbox per layer");
        checklist->patternToggle()->click();
        expect(checklist->patternField()->isVisible() &&
                   checklist->patternField()->text() == "ViewLayer.Depth",
               "the advanced toggle shows the pattern");
        checklist->patternToggle()->click();
        (void)scene.session.setNodeParameterValue(
            scene.remove, "select", document::ParameterValue{std::string("@depth")}, "Pattern");
        QApplication::processEvents();
        expect(checklist->patternField()->isVisible() && !checklist->patternToggle()->isEnabled(),
               "a pattern the checklist cannot show keeps the pattern field open");
    }

    // Channel Copy: every layer of `from` by default (`*`); unchecking lists the rest.
    scene.session.selectNode(scene.copy);
    expect(waitUntil([&] {
               checklist =
                   properties->findChild<ui::ChannelChecklistField*>("propertiesChannelChecklist");
               return checklist != nullptr && checklist->entries().size() == 3;
           }),
           "Channel Copy lists the layers of its from input");
    if (checklist != nullptr && checklist->entries().size() == 3) {
        expect(checklist->summary()->text() == "All", "Channel Copy copies every layer by default");
        checklist->setEntryChecked(0, false);
        expect(scene.value(scene.copy, "select") ==
                   document::ParameterValue{std::string("ViewLayer.Depth,ViewLayer.Normal")},
               "unchecking one layer writes the others");
        checklist->setEntryChecked(0, true);
        expect(scene.value(scene.copy, "select") == document::ParameterValue{std::string("*")},
               "checking every layer writes every channel again");
    }
    auto* cardCopy = qobject_cast<ui::ChannelChecklistField*>(cardControl(scene.copy, "select"));
    expect(cardCopy != nullptr, "the card shows the same checklist, not a text field");

    // Composition Output: the layers written beside RGBA, every layer by default; the last checked
    // layer cannot be unchecked (an empty list means every layer).
    scene.session.selectNode(scene.output);
    ui::ChannelChecklistField* layers = nullptr;
    expect(waitUntil([&] {
               layers = properties->findChild<ui::ChannelChecklistField*>(
                   "propertiesOutputChannelLayers");
               return layers != nullptr && layers->entries().size() == 3;
           }),
           "the output layer checklist lists the connected set's layers");
    if (layers != nullptr && layers->entries().size() == 3) {
        expect(layers->summary()->text() == "Every layer", "every layer is written by default");
        layers->setEntryChecked(2, false);
        layers->setEntryChecked(1, false);
        expect(scene.value(scene.output, document::kCompositionOutputLayersParameterRole) ==
                   document::ParameterValue{std::string("ViewLayer.Combined")},
               "unchecking layers writes the remaining one");
        layers->setEntryChecked(0, false);
        expect(scene.value(scene.output, document::kCompositionOutputLayersParameterRole) ==
                   document::ParameterValue{std::string("ViewLayer.Combined")},
               "the last layer stays checked");
    }

    // Data Sample: the block is a dropdown of the project's Curve blocks; unset reads the first.
    const auto sample = scene.add(document::kDataSampleNodeType);
    scene.session.selectNode(sample);
    QApplication::processEvents();
    auto* blocks = properties->findChild<ui::kit::KDropdown*>("propertiesDataBlock");
    expect(blocks != nullptr && blocks->count() == 2 &&
               blocks->currentText().startsWith("bloom.test.first-curve"),
           "an unset reader shows the first block of its kind, and only that kind");
    if (blocks != nullptr && blocks->count() == 2) {
        blocks->chooseIndex(1);
        expect(scene.value(sample, document::kDataBlockParameterRole) ==
                   document::ParameterValue{std::int64_t{3}},
               "choosing a block writes its id");
    }
    auto* cardBlocks = qobject_cast<ui::kit::KDropdown*>(
        cardControl(sample, document::kDataBlockParameterRole.data()));
    expect(cardBlocks != nullptr && cardBlocks->currentText().startsWith("bloom.test.second-curve"),
           "the card's block is the same dropdown");
    const auto lookup = scene.add(document::kTableLookupNodeType);
    auto* lookupBlocks = qobject_cast<ui::kit::KDropdown*>(
        cardControl(lookup, document::kDataBlockParameterRole.data()));
    expect(lookupBlocks != nullptr && lookupBlocks->count() == 1 &&
               lookupBlocks->currentText().startsWith("bloom.test.table"),
           "a Table Lookup offers the Table block");

    // Video: the card's audio stream is the Properties dropdown, not a number field.
    const auto video = scene.add("bloom.video-source");
    auto* stream = qobject_cast<ui::kit::KDropdown*>(
        cardControl(video, document::kVideoAudioStreamParameterRole.data()));
    expect(stream != nullptr && stream->count() == 1 && !stream->isEnabled() &&
               stream->currentText() == "No audio stream",
           "a Video card without an asset offers no stream choice");

    bridge.beginShutdown();
    scheduler.beginShutdown();
    QElapsedTimer timer;
    timer.start();
    while (!scheduler.isQuiescent() && timer.elapsed() < 4000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    ui::kit::installKinetikTheme(app);
    try {
        run();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
