// ADR 0026 on a real compiled document: Image source v2 part/layer selection, the channels output,
// Remove, Channel Copy and Shuffle compiled into one CompiledChannelShuffle and evaluated by the CPU
// reference.

#include <bloom/commands/asset_operations.hpp>
#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/channel_nodes.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/runtime/channel_inspection_listing.hpp>
#include <bloom/runtime/channel_layers.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>

#include <ImathBox.h>
#include <ImfChannelList.h>
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>
#include <ImfMultiPartOutputFile.h>
#include <ImfOutputPart.h>
#include <ImfPartType.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace bloom;
namespace {
int failures = 0;
void expect(const bool value, const std::string& message) {
    if (!value) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}
bool near(const float value, const float expected, const float tolerance = 1e-4F) {
    return std::fabs(value - expected) <= tolerance;
}

constexpr int kWidth = 3;
constexpr int kHeight = 2;
constexpr std::size_t kSamples = 6;

// Part "beauty": premultiplied root RGBA (alpha 0.5), diffuse colour, depth with a NaN. Part
// "data": depth only.
void writeLayeredExr(const std::filesystem::path& path) {
    const Imath::Box2i window(Imath::V2i(0, 0), Imath::V2i(kWidth - 1, kHeight - 1));
    std::vector<std::pair<std::string, std::vector<float>>> beauty;
    const auto plane = [](std::string name, auto&& sample) {
        std::vector<float> values;
        for (std::size_t index = 0; index < kSamples; ++index)
            values.push_back(sample(index));
        return std::pair{std::move(name), std::move(values)};
    };
    beauty.push_back(plane("R", [](std::size_t) { return 0.25F; }));
    beauty.push_back(plane("G", [](std::size_t) { return 0.125F; }));
    beauty.push_back(plane("B", [](std::size_t) { return 0.0F; }));
    beauty.push_back(plane("A", [](std::size_t) { return 0.5F; }));
    beauty.push_back(plane("diffuse.R", [](std::size_t i) { return 0.1F * static_cast<float>(i); }));
    beauty.push_back(plane("diffuse.G", [](std::size_t) { return 0.5F; }));
    beauty.push_back(plane("diffuse.B", [](std::size_t) { return 2.0F; }));
    beauty.push_back(plane("depth.Z", [](std::size_t i) {
        return i == 5 ? std::numeric_limits<float>::quiet_NaN()
                      : -0.5F + 0.25F * static_cast<float>(i);
    }));
    std::vector<std::pair<std::string, std::vector<float>>> data;
    data.push_back(plane("depth.Z", [](std::size_t i) { return 10.0F + static_cast<float>(i); }));

    Imf::Header first(window, window);
    first.setName("beauty");
    first.setType(Imf::SCANLINEIMAGE);
    for (const auto& [name, values] : beauty)
        first.channels().insert(name, Imf::Channel(Imf::FLOAT));
    Imf::Header second(window, window);
    second.setName("data");
    second.setType(Imf::SCANLINEIMAGE);
    for (const auto& [name, values] : data)
        second.channels().insert(name, Imf::Channel(Imf::FLOAT));
    const std::vector<Imf::Header> headers{first, second};
    Imf::MultiPartOutputFile file(path.string().c_str(), headers.data(), 2);
    for (int partIndex = 0; partIndex < 2; ++partIndex) {
        auto& planes = partIndex == 0 ? beauty : data;
        Imf::OutputPart part(file, partIndex);
        Imf::FrameBuffer frameBuffer;
        for (auto& [name, values] : planes)
            frameBuffer.insert(name, Imf::Slice(Imf::FLOAT, reinterpret_cast<char*>(values.data()),
                                                sizeof(float), kWidth * sizeof(float)));
        part.setFrameBuffer(frameBuffer);
        part.writePixels(kHeight);
    }
}

struct Fixture {
    std::filesystem::path folder;
    document::NewProject initial = document::makeNewProject(
        "Channels", "Main", core::RationalTime::fromInteger(1),
        *document::CompositionFormat::create(kWidth, kHeight, core::PixelAspectRatio::square(),
                                             *document::FrameRate::create(24, 1)));
    document::Document document{std::move(initial.project)};
    commands::CommandStack commands{document};
    document::CompositionId composition = initial.initialCompositionId;
    document::NodeId source;
    document::NodeId layer;

    template <typename Operation, typename... Arguments>
    commands::CommandResult run(Arguments&&... arguments) {
        commands::Transaction transaction("Channels", document.snapshot().revision());
        transaction.emplace<Operation>(std::forward<Arguments>(arguments)...);
        auto result = commands.execute(std::move(transaction));
        if (!result.succeeded()) {
            for (const auto& failure : result.operationFailures)
                std::cerr << failure.operationType << ": " << failure.issue.message << '\n';
            throw std::runtime_error("fixture command refused");
        }
        return result;
    }
    explicit Fixture(const std::string& name) {
        std::mt19937_64 random{std::random_device{}()};
        folder = std::filesystem::temp_directory_path() /
                 ("bloom-channel-" + name + "-" + std::to_string(random()));
        std::filesystem::create_directories(folder);
        writeLayeredExr(folder / "layers.exr");
        run<commands::ImportAssets>(std::vector{folder / "layers.exr"}, folder);
        const auto asset = document.snapshot().project().assets().front().id;
        run<commands::AddImageLayer>(composition, asset);
        const auto& graph = document.snapshot().project().findComposition(composition)->graph();
        for (const auto& node : graph.nodes()) {
            if (node.typeId == document::kImageSourceNodeType)
                source = node.id;
            if (node.typeId == document::kLayerOutputNodeType)
                layer = node.id;
        }
    }
    ~Fixture() {
        std::error_code error;
        std::filesystem::remove_all(folder, error);
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    document::NodeId add(const std::string_view type) {
        return *run<commands::AddNode>(composition, std::string(type), document::Vec2d{})
                    .outputId<document::NodeId>(commands::kAddNodeOutput);
    }
    document::ParameterId parameter(const document::NodeId node, const std::string_view role) {
        const auto* record =
            document.snapshot().project().findComposition(composition)->graph().findNode(node);
        for (const auto& binding : record->parameters)
            if (binding.role == role)
                return binding.parameterId;
        throw std::runtime_error("binding missing");
    }
    template <typename Value>
    void set(const document::NodeId node, const std::string_view role, Value value) {
        run<commands::SetParameterSource>(
            composition, parameter(node, role),
            document::ConstantValueSource{document::ParameterValue{std::move(value)}});
    }
    void connect(const document::NodeId from, const std::string_view output,
                 const document::NodeId to, const std::string_view input) {
        run<commands::ConnectPorts>(composition, document::OutputPortRef{from, std::string(output)},
                                    document::NodeInputRef{to, std::string(input)});
    }
    std::shared_ptr<const runtime::CompiledCompositionPlan>
    compile(std::optional<runtime::ChannelInspection> inspection = std::nullopt) {
        runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
        const auto compiled =
            compiler.compile({document.snapshot(), composition, {}, std::move(inspection)}, {});
        if (!compiled.plan) {
            for (const auto& diagnostic : compiled.diagnostics)
                std::cerr << diagnostic.summary << ": " << diagnostic.detail << '\n';
            throw std::runtime_error("channel compilation failed");
        }
        return compiled.plan;
    }
    struct Frame {
        std::vector<render::Rgba32f> pixels;
        std::vector<std::string> warnings;
        [[nodiscard]] bool warned(const std::string_view text) const {
            return std::ranges::any_of(warnings, [&](const std::string& warning) {
                return warning.find(text) != std::string::npos;
            });
        }
    };
    Frame frame(std::optional<runtime::ChannelInspection> inspection = std::nullopt) {
        const auto plan = compile(std::move(inspection));
        runtime::CpuCompositionEvaluator evaluator;
        evaluator.setAssetBaseDirectory(folder);
        const auto result = evaluator.evaluate(
            plan,
            {{}, plan->output(), runtime::CompositionFormatResolution{},
             runtime::EvaluationQuality::Reference,
             runtime::EvaluationColorIntent::LinearRec709Scene, 1U << 24U},
            {});
        if (!result.frame()) {
            for (const auto& diagnostic : result.diagnostics())
                std::cerr << diagnostic.summary << ": " << diagnostic.detail << '\n';
            throw std::runtime_error("channel evaluation failed");
        }
        Frame value;
        const auto span = result.frame()->processImage().pixels();
        value.pixels.assign(span.begin(), span.end());
        for (const auto& diagnostic : result.diagnostics())
            value.warnings.push_back(diagnostic.summary + " " + diagnostic.detail);
        return value;
    }
};

void imageLayerSelection() {
    Fixture fixture("layer");
    const auto root = fixture.frame();
    expect(near(root.pixels[0].red(), 0.25F) && near(root.pixels[0].alpha(), 0.5F),
           "the default selection is the first part's root RGBA");
    fixture.set(fixture.source, document::kImageLayerParameterRole, std::string("diffuse"));
    const auto diffuse = fixture.frame();
    expect(near(diffuse.pixels[2].red(), 0.2F) && near(diffuse.pixels[2].blue(), 2.0F) &&
               near(diffuse.pixels[2].alpha(), 1.0F),
           "a named colour layer decodes opaque");
    fixture.set(fixture.source, document::kImagePartParameterRole, std::string("data"));
    fixture.set(fixture.source, document::kImageLayerParameterRole, std::string("depth"));
    const auto data = fixture.frame();
    expect(near(data.pixels[0].alpha(), 0.0F) && data.warned("No color layer — use Channels"),
           "a data-only layer has no colour image and reports why");
}

void shuffleChain() {
    Fixture fixture("shuffle");
    const auto remove = fixture.add(document::kChannelRemoveNodeType);
    const auto shuffle = fixture.add(document::kChannelShuffleNodeType);
    fixture.connect(fixture.source, document::kImageSourceChannelsOutputPort, remove,
                    document::kChannelSetPort);
    fixture.connect(remove, document::kChannelSetPort, shuffle, document::kChannelSetPort);
    fixture.connect(shuffle, document::kChannelShuffleImagePort, fixture.layer,
                    document::kLayerOutputContentInputPort);
    fixture.set(remove, "select", std::string("depth.Z"));
    fixture.set(shuffle, "red", std::string("diffuse.R"));
    fixture.set(shuffle, "green", std::string("diffuse.G"));
    fixture.set(shuffle, "blue", std::string("diffuse.B"));
    fixture.set(shuffle, "alpha", std::string("1"));

    const auto plan = fixture.compile();
    const auto shuffles = std::ranges::count_if(plan->operations(), [](const auto& operation) {
        return std::holds_alternative<runtime::CompiledChannelShuffle>(operation);
    });
    const auto images = std::ranges::count_if(plan->operations(), [](const auto& operation) {
        return std::holds_alternative<runtime::CompiledImageSource>(operation);
    });
    expect(shuffles == 1 && images == 0,
           "Remove and the channels output compile into one Shuffle; the unused image output "
           "compiles nothing");
    if (const auto it = std::ranges::find_if(plan->operations(),
                                             [](const auto& operation) {
                                                 return std::holds_alternative<
                                                     runtime::CompiledChannelShuffle>(operation);
                                             });
        it != plan->operations().end()) {
        const auto& compiled = std::get<runtime::CompiledChannelShuffle>(*it);
        expect(compiled.channels.sources.size() == 1 && compiled.channels.steps.size() == 2 &&
                   compiled.channels.steps[1].kind == runtime::CompiledChannelStepKind::Remove &&
                   compiled.mapping[0] == "diffuse.R",
               "the expression records the source, the Remove step and the slots");
    }
    const auto colour = fixture.frame();
    expect(near(colour.pixels[3].red(), 0.3F) && near(colour.pixels[3].green(), 0.5F) &&
               near(colour.pixels[3].blue(), 2.0F) && near(colour.pixels[3].alpha(), 1.0F),
           "a Colour shuffle of a scene-linear layer reproduces its values");

    fixture.set(shuffle, "red", std::string("depth.Z"));
    const auto removed = fixture.frame();
    expect(removed.warned("not in its input: depth.Z") && near(removed.pixels[0].alpha(), 0.0F),
           "a removed channel is a reported, transparent shuffle");

    fixture.set(remove, "select", std::string(""));
    fixture.set(shuffle, "green", std::string("depth.Z"));
    fixture.set(shuffle, "blue", std::string("0"));
    fixture.set(shuffle, "mode", document::kChannelShuffleRawMode);
    const auto raw = fixture.frame();
    expect(near(raw.pixels[0].red(), -0.5F) && near(raw.pixels[0].green(), -0.5F) &&
               near(raw.pixels[4].red(), 0.5F) && near(raw.pixels[0].blue(), 0.0F),
           "a Raw shuffle places data values in RGB unchanged, negatives included");
    expect(near(raw.pixels[5].red(), 0.0F) && raw.warned("repaired"),
           "a NaN data sample becomes zero and is reported");

    // Channel Copy brings depth from the second part into a set that removed it.
    const auto partSource = fixture.add(document::kImageSourceNodeType);
    fixture.set(partSource, "asset",
                std::to_string(fixture.document.snapshot().project().assets().front().id.value()));
    fixture.set(partSource, document::kImagePartParameterRole, std::string("data"));
    const auto copy = fixture.add(document::kChannelCopyNodeType);
    fixture.set(remove, "select", std::string("depth.Z"));
    fixture.connect(remove, document::kChannelSetPort, copy, document::kChannelSetPort);
    fixture.connect(partSource, document::kImageSourceChannelsOutputPort, copy,
                    document::kChannelCopyFromPort);
    fixture.set(copy, "select", std::string("@depth"));
    fixture.connect(copy, document::kChannelSetPort, shuffle, document::kChannelSetPort);
    const auto copied = fixture.frame();
    expect(near(copied.pixels[0].red(), 10.0F) && near(copied.pixels[5].green(), 15.0F) &&
               !copied.warned("repaired"),
           "Channel Copy replaces the removed depth with the data part's depth");

    // Shuffle's own channel output renames into a layer that a second Shuffle reads.
    const auto rename = fixture.add(document::kChannelShuffleNodeType);
    fixture.connect(copy, document::kChannelSetPort, rename, document::kChannelSetPort);
    fixture.set(rename, "red", std::string("diffuse.B"));
    fixture.set(rename, "layer", std::string("view"));
    fixture.connect(rename, document::kChannelSetPort, shuffle, document::kChannelSetPort);
    fixture.set(shuffle, "red", std::string("view.R"));
    fixture.set(shuffle, "green", std::string("view.A"));
    const auto renamed = fixture.frame();
    expect(near(renamed.pixels[1].red(), 2.0F) && near(renamed.pixels[1].green(), 1.0F),
           "a Shuffle channel output renames mapped channels into a layer");
}

// #487: an empty Shuffle slot (the default) reads the default colour layer's channel.
void defaultSlots() {
    using Names = std::vector<std::string>;
    const Names root{"R", "G", "B", "A", "diffuse.R", "diffuse.G", "diffuse.B"};
    const Names rootless{"ViewLayer.Depth.Z", "ViewLayer.Combined.R", "ViewLayer.Combined.G",
                         "ViewLayer.Combined.B", "ViewLayer.Combined.A"};
    const Names data{"depth.Z", "N.X", "N.Y"};
    expect(runtime::defaultShuffleSlotChannel(root, 0) == std::optional<std::string>("R") &&
               runtime::defaultShuffleSlotChannel(root, 3) == std::optional<std::string>("A"),
           "the root colour layer is the default when present");
    expect(runtime::defaultShuffleSlotChannel(rootless, 2) ==
                   std::optional<std::string>("ViewLayer.Combined.B") &&
               runtime::defaultShuffleSlotChannel(rootless, 3) ==
                   std::optional<std::string>("ViewLayer.Combined.A"),
           "a rootless render defaults to its first layer carrying R, G and B");
    expect(!runtime::defaultShuffleSlotChannel(data, 0) &&
               !runtime::defaultShuffleSlotChannel(Names{"R", "G", "B"}, 3),
           "a set without a colour layer (or alpha) has no default channel");

    Fixture fixture("default-slots");
    const auto shuffle = fixture.add(document::kChannelShuffleNodeType);
    fixture.connect(fixture.source, document::kImageSourceChannelsOutputPort, shuffle,
                    document::kChannelSetPort);
    fixture.connect(shuffle, document::kChannelShuffleImagePort, fixture.layer,
                    document::kLayerOutputContentInputPort);
    const auto defaulted = fixture.frame();
    expect(near(defaulted.pixels[0].red(), 0.25F) && near(defaulted.pixels[0].green(), 0.125F) &&
               near(defaulted.pixels[0].blue(), 0.0F) && near(defaulted.pixels[0].alpha(), 1.0F),
           "a new Shuffle's default slots read the root R, G and B");
    // A Shuffle inside a channel expression resolves its default slots the same way.
    const auto rename = fixture.add(document::kChannelShuffleNodeType);
    fixture.connect(fixture.source, document::kImageSourceChannelsOutputPort, rename,
                    document::kChannelSetPort);
    fixture.set(rename, "layer", std::string("view"));
    fixture.connect(rename, document::kChannelSetPort, shuffle, document::kChannelSetPort);
    fixture.set(shuffle, "red", std::string("view.G"));
    expect(near(fixture.frame().pixels[0].red(), 0.125F),
           "a renaming Shuffle's default slots carry the root colour channels");
}

void viewerInspection() {
    Fixture fixture("inspect");
    runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    const auto listing = runtime::listChannelInspectionChoices(
        compiler, fixture.document.snapshot(), fixture.composition, fixture.source,
        std::string(document::kImageSourceChannelsOutputPort), fixture.folder, {},
        runtime::EvaluationColorIntent::LinearRec709Scene, {});
    const auto has = [&](const std::string& label) {
        return std::ranges::any_of(listing.choices,
                                   [&](const auto& choice) { return choice.label == label; });
    };
    expect(listing.diagnostic.empty() && has("RGB (RGB)") && has("diffuse (RGB)") &&
               has("depth.Z") && has("A"),
           "the Viewer lists every channel and each RGB layer of a channel set");
    expect(listing.layers == std::vector<std::string>{"", "depth", "diffuse"} &&
               std::ranges::find(listing.channels, "depth.Z") != listing.channels.end() &&
               listing.channels.size() == 8,
           "the listing names every channel and layer for the pickers (#487)");

    const auto inspection = runtime::ChannelInspection{
        fixture.source, std::string(document::kImageSourceChannelsOutputPort), {"depth.Z"}};
    const auto inspected = fixture.frame(inspection);
    expect(near(inspected.pixels[0].red(), -0.5F) && near(inspected.pixels[0].green(), -0.5F) &&
               near(inspected.pixels[0].blue(), -0.5F) && near(inspected.pixels[0].alpha(), 1.0F),
           "one inspected channel shows raw as grey, alpha 1");
    const auto layer = fixture.frame(runtime::ChannelInspection{
        fixture.source, std::string(document::kImageSourceChannelsOutputPort),
        {"diffuse.R", "diffuse.G", "diffuse.B"}});
    expect(near(layer.pixels[1].red(), 0.1F) && near(layer.pixels[1].blue(), 2.0F),
           "an inspected layer shows its three channels as RGB");
    const auto plan = fixture.compile();
    expect(std::ranges::none_of(plan->operations(),
                                [](const auto& operation) {
                                    return std::holds_alternative<runtime::CompiledChannelShuffle>(
                                        operation);
                                }) &&
               near(fixture.frame().pixels[0].red(), 0.25F),
           "inspection never changes the composition's own plan or pixels");
}

void outputChannelLayers() {
    Fixture fixture("output");
    const auto& graph = fixture.document.snapshot().project().findComposition(fixture.composition)->graph();
    const auto output = graph.compositionOutput()->nodeId;
    fixture.connect(fixture.source, document::kImageSourceChannelsOutputPort, output,
                    document::kCompositionOutputChannelsInputPort);
    // A project's Composition Output predates the optional layer list: the first edit binds it.
    fixture.run<commands::SetNodeParameterValue>(
        fixture.composition, output, std::string(document::kCompositionOutputLayersParameterRole),
        document::ParameterValue{std::string("depth")});
    const auto plan = fixture.compile();
    const auto created = render::ImageWindow::create(0, 0, kWidth, kHeight);
    const auto window = *created.value();
    const auto assembled = runtime::assembleOutputChannelLayers(
        *plan, {}, fixture.folder, runtime::EvaluationColorIntent::LinearRec709Scene, window,
        window, 1U << 24U, {});
    expect(assembled.ok() && assembled.layers && assembled.layers->channels().size() == 1 &&
               assembled.layers->channels()[0].name == "depth.Z" &&
               assembled.layers->channels()[0].sampleKind == render::ChannelSampleKind::Float &&
               near(assembled.layers->plane(0)[0], -0.5F) &&
               std::isnan(assembled.layers->plane(0)[5]),
           "the Composition Output's selected layers assemble as raw source channels");
    fixture.run<commands::SetNodeParameterValue>(
        fixture.composition, output, std::string(document::kCompositionOutputLayersParameterRole),
        document::ParameterValue{std::string("")});
    const auto all = runtime::assembleOutputChannelLayers(
        *fixture.compile(), {}, fixture.folder, runtime::EvaluationColorIntent::LinearRec709Scene,
        window, window, 1U << 24U, {});
    expect(all.ok() && all.layers && all.layers->channels().size() == 4 &&
               !all.layers->find("R") && all.layers->find("diffuse.G"),
           "an empty selection writes every layer except the RGBA root");
    expect(near(fixture.frame().pixels[0].red(), 0.25F),
           "channel layers never change the composited image");
}


// #480: Auto reads data raw, Colour on a data channel is refused, non-finite samples take the
// node's substitute, and zero alpha follows the premultiplied image.
void dataSafeShuffle() {
    Fixture fixture("data-safe");
    const auto shuffle = fixture.add(document::kChannelShuffleNodeType);
    fixture.connect(fixture.source, document::kImageSourceChannelsOutputPort, shuffle,
                    document::kChannelSetPort);
    fixture.connect(shuffle, document::kChannelShuffleImagePort, fixture.layer,
                    document::kLayerOutputContentInputPort);
    for (const auto* slot : {"red", "green", "blue"})
        fixture.set(shuffle, slot, std::string("depth.Z"));
    fixture.set(shuffle, "alpha", std::string("1"));
    const auto automatic = fixture.frame();
    expect(near(automatic.pixels[0].red(), -0.5F) && near(automatic.pixels[4].green(), 0.5F) &&
               near(automatic.pixels[0].alpha(), 1.0F) && !automatic.warned("color-convert"),
           "a new Shuffle defaults to Auto and reads a data channel raw");
    expect(near(automatic.pixels[5].red(), 0.0F) && automatic.warned("repaired"),
           "a NaN data sample becomes the default non-finite value 0 and is reported");

    fixture.run<commands::SetNodeParameterValue>(
        fixture.composition, shuffle, std::string(document::kChannelNonFiniteParameterRole),
        document::ParameterValue{1000.0});
    const auto far = fixture.frame();
    expect(near(far.pixels[5].red(), 1000.0F) && near(far.pixels[5].blue(), 1000.0F) &&
               near(far.pixels[0].red(), -0.5F) && far.warned("repaired"),
           "a NaN data sample takes the node's finite substitute (a far depth)");

    // A project whose Shuffle stores Color mode over a data channel opens with a diagnostic and
    // never colour-converts the data.
    fixture.set(shuffle, "mode", document::kChannelShuffleColourMode);
    const auto refused = fixture.frame();
    expect(refused.warned("Shuffle Color mode would color-convert the data channel depth.Z") &&
               near(refused.pixels[0].red(), 0.0F) && near(refused.pixels[0].alpha(), 0.0F),
           "Color mode on a data channel is refused with a diagnostic");
    fixture.set(shuffle, "mode", document::kChannelShuffleAutoMode);
    fixture.set(shuffle, "red", std::string("diffuse.R"));
    fixture.set(shuffle, "green", std::string("diffuse.G"));
    fixture.set(shuffle, "blue", std::string("diffuse.B"));
    const auto colour = fixture.frame();
    expect(near(colour.pixels[3].red(), 0.3F) && near(colour.pixels[3].blue(), 2.0F) &&
               !colour.warned("color-convert"),
           "Auto reads a colour layer in Color mode");

    // Zero alpha: the image is premultiplied, so a straight slot at alpha 0 stores no colour.
    for (const auto* slot : {"red", "green", "blue"})
        fixture.set(shuffle, slot, std::string("depth.Z"));
    fixture.set(shuffle, "alpha", std::string("0"));
    fixture.set(shuffle, "associated", false);
    const auto transparent = fixture.frame();
    expect(near(transparent.pixels[4].red(), 0.0F) && near(transparent.pixels[4].alpha(), 0.0F),
           "a data slot under zero alpha is zero in the premultiplied result");
    fixture.set(shuffle, "associated", true);
    const auto associated = fixture.frame();
    expect(near(associated.pixels[4].red(), 0.0F) && associated.warned("repaired"),
           "associated data under zero alpha is dropped and reported");
}

// #480 Pass: a picked layer or channel as a grey or three-channel image, alpha 1, associated off.
void passPicker() {
    Fixture fixture("pass");
    const auto pass = fixture.add(document::kChannelPassNodeType);
    fixture.connect(fixture.source, document::kImageSourceChannelsOutputPort, pass,
                    document::kChannelSetPort);
    fixture.connect(pass, document::kChannelPassImagePort, fixture.layer,
                    document::kLayerOutputContentInputPort);
    const auto first = fixture.frame();
    expect(near(first.pixels[0].red(), -0.5F) && near(first.pixels[0].blue(), -0.5F) &&
               near(first.pixels[0].alpha(), 1.0F) && near(first.pixels[5].red(), 0.0F),
           "an empty pass reads the first data layer as grey, alpha 1");
    fixture.set(pass, document::kChannelPassParameterRole, std::string("diffuse"));
    const auto diffuse = fixture.frame();
    expect(near(diffuse.pixels[3].red(), 0.3F) && near(diffuse.pixels[3].green(), 0.5F) &&
               near(diffuse.pixels[3].blue(), 2.0F) && near(diffuse.pixels[3].alpha(), 1.0F),
           "a layer pass reads its R, G and B");
    fixture.set(pass, document::kChannelPassParameterRole, std::string("depth.Z"));
    fixture.set(pass, document::kChannelNonFiniteParameterRole, 1.0e4);
    const auto depth = fixture.frame();
    expect(near(depth.pixels[1].green(), -0.25F) && near(depth.pixels[5].red(), 1.0e4F),
           "a channel pass is grey, with its non-finite substitute");
    fixture.set(pass, document::kChannelPassParameterRole, std::string("motion"));
    const auto missing = fixture.frame();
    expect(missing.warned("The pass is not in the channel set: motion") &&
               near(missing.pixels[0].alpha(), 0.0F),
           "a pass the set lacks is reported");
    fixture.set(pass, document::kChannelPassParameterRole, std::string("depth"));
    fixture.set(pass, "mode", document::kChannelShuffleColourMode);
    expect(fixture.frame().warned("would color-convert the data channel depth.Z"),
           "a Pass in Color mode refuses a data pass");
}

// #480 Image to Channel Set: an image packed as a named layer, read back exactly by a Pass, joined
// with file channels only through image effects, and written as a layered OpenEXR layer.
void imageToChannelSet() {
    Fixture fixture("image-set");
    const auto pack = fixture.add(document::kImageToChannelSetNodeType);
    const auto pass = fixture.add(document::kChannelPassNodeType);
    fixture.connect(fixture.source, document::kImageSourceImageOutputPort, pack,
                    document::kImageToChannelSetImagePort);
    fixture.connect(pack, document::kChannelSetPort, pass, document::kChannelSetPort);
    fixture.connect(pass, document::kChannelPassImagePort, fixture.layer,
                    document::kLayerOutputContentInputPort);
    fixture.set(pass, document::kChannelPassParameterRole, std::string("pass"));
    const auto plan = fixture.compile();
    expect(std::ranges::none_of(plan->operations(),
                                [](const auto& operation) {
                                    return std::holds_alternative<runtime::CompiledChannelShuffle>(
                                        operation);
                                }) &&
               std::ranges::any_of(plan->operations(),
                                   [](const auto& operation) {
                                       const auto* effect =
                                           std::get_if<runtime::CompiledImageEffect>(&operation);
                                       return effect != nullptr &&
                                              std::holds_alternative<runtime::ChannelSelectKernel>(
                                                  effect->kernel);
                                   }),
           "a Pass over a packed image compiles to a Channel Select, not a file gather");
    const auto packed = fixture.frame();
    expect(near(packed.pixels[0].red(), 0.25F) && near(packed.pixels[0].green(), 0.125F) &&
               near(packed.pixels[0].alpha(), 1.0F),
           "a packed layer reads back the image's stored channels");
    fixture.set(pass, document::kChannelPassParameterRole, std::string("pass.A"));
    expect(near(fixture.frame().pixels[2].red(), 0.5F), "a packed channel reads as grey");

    runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    const auto listing = runtime::listChannelInspectionChoices(
        compiler, fixture.document.snapshot(), fixture.composition, pack,
        std::string(document::kChannelSetPort), fixture.folder, {},
        runtime::EvaluationColorIntent::LinearRec709Scene, {});
    expect(listing.diagnostic.empty() &&
               listing.channels ==
                   std::vector<std::string>{"pass.R", "pass.G", "pass.B", "pass.A"} &&
               listing.layers == std::vector<std::string>{"pass"},
           "the pickers list a packed layer's channels");
    const auto inspected = fixture.frame(runtime::ChannelInspection{
        pack, std::string(document::kChannelSetPort), {"pass.G"}});
    expect(near(inspected.pixels[0].red(), 0.125F), "the Viewer inspects a packed channel");

    // Mixing packed and file channels in one gather is refused with a diagnostic.
    const auto copy = fixture.add(document::kChannelCopyNodeType);
    fixture.connect(fixture.source, document::kImageSourceChannelsOutputPort, copy,
                    document::kChannelSetPort);
    fixture.connect(pack, document::kChannelSetPort, copy, document::kChannelCopyFromPort);
    fixture.set(copy, "select", std::string("pass"));
    const auto shuffle = fixture.add(document::kChannelShuffleNodeType);
    fixture.connect(copy, document::kChannelSetPort, shuffle, document::kChannelSetPort);
    fixture.connect(shuffle, document::kChannelShuffleImagePort, fixture.layer,
                    document::kLayerOutputContentInputPort);
    fixture.set(shuffle, "red", std::string("pass.R"));
    fixture.set(shuffle, "green", std::string("depth.Z"));
    fixture.set(shuffle, "blue", std::string("0"));
    fixture.set(shuffle, "alpha", std::string("1"));
    expect(fixture.frame().warned("Image to Channel Set channel pass.R"),
           "a Shuffle mixing packed and file channels is refused");

    // A packed layer reaches a layered OpenEXR beside the file's own layers.
    const auto& graph =
        fixture.document.snapshot().project().findComposition(fixture.composition)->graph();
    const auto output = graph.compositionOutput()->nodeId;
    fixture.connect(copy, document::kChannelSetPort, output,
                    document::kCompositionOutputChannelsInputPort);
    fixture.run<commands::SetNodeParameterValue>(
        fixture.composition, output, std::string(document::kCompositionOutputLayersParameterRole),
        document::ParameterValue{std::string("pass, depth")});
    const auto layered = fixture.compile();
    const auto created = render::ImageWindow::create(0, 0, kWidth, kHeight);
    const auto window = *created.value();
    const auto unrouted = runtime::assembleOutputChannelLayers(
        *layered, {}, fixture.folder, runtime::EvaluationColorIntent::LinearRec709Scene, window,
        window, 1U << 24U, {});
    expect(!unrouted.ok() && !unrouted.diagnostic.empty(),
           "a packed layer without an evaluation route is refused, never written as zeros");
    runtime::CpuCompositionEvaluator evaluator;
    evaluator.setAssetBaseDirectory(fixture.folder);
    const auto images = [&](const std::shared_ptr<const runtime::CompiledCompositionPlan>& layerPlan) {
        runtime::ChannelLayerImage layer;
        if (!layerPlan) {
            layer.failed = true;
            return layer;
        }
        const auto result = evaluator.evaluate(
            layerPlan,
            {{}, layerPlan->output(), runtime::CompositionFormatResolution{},
             runtime::EvaluationQuality::Reference,
             runtime::EvaluationColorIntent::LinearRec709Scene, 1U << 24U},
            {});
        if (!result.frame()) {
            layer.failed = true;
            return layer;
        }
        layer.image = std::shared_ptr<const render::Rgba32fImage>(result.frame(),
                                                                  &result.frame()->processImage());
        return layer;
    };
    const auto assembled = runtime::assembleOutputChannelLayers(
        *layered, {}, fixture.folder, runtime::EvaluationColorIntent::LinearRec709Scene, window,
        window, 1U << 24U, {}, images);
    const auto red = assembled.layers ? assembled.layers->find("pass.R") : std::nullopt;
    const auto alpha = assembled.layers ? assembled.layers->find("pass.A") : std::nullopt;
    const auto depth = assembled.layers ? assembled.layers->find("depth.Z") : std::nullopt;
    expect(assembled.ok() && red && alpha && depth &&
               near(assembled.layers->plane(*red)[0], 0.25F) &&
               near(assembled.layers->plane(*alpha)[3], 0.5F) &&
               near(assembled.layers->plane(*depth)[0], -0.5F),
           "a packed layer is written beside the file's layers in a layered OpenEXR");
}

// #480: a Video source's channels output is its decoded frame's R, G, B and A, read exactly.
void videoChannels() {
    Fixture fixture("video");
    const auto video = fixture.add(document::kVideoSourceNodeType);
    const auto pass = fixture.add(document::kChannelPassNodeType);
    fixture.connect(video, document::kVideoSourceChannelsOutputPort, pass,
                    document::kChannelSetPort);
    fixture.connect(pass, document::kChannelPassImagePort, fixture.layer,
                    document::kLayerOutputContentInputPort);
    fixture.set(pass, document::kChannelPassParameterRole, std::string("B"));
    const auto plan = fixture.compile();
    const auto& operations = plan->operations();
    const auto select = std::ranges::find_if(operations, [](const auto& operation) {
        const auto* effect = std::get_if<runtime::CompiledImageEffect>(&operation);
        return effect != nullptr && std::holds_alternative<runtime::ChannelSelectKernel>(effect->kernel);
    });
    const auto* effect = select != operations.end()
                             ? std::get_if<runtime::CompiledImageEffect>(&*select)
                             : nullptr;
    const auto* kernel =
        effect != nullptr ? std::get_if<runtime::ChannelSelectKernel>(&effect->kernel) : nullptr;
    expect(kernel != nullptr &&
               std::holds_alternative<runtime::CompiledVideoSource>(
                   operations[effect->input.value()]) &&
               kernel->sources[0] == runtime::ImageChannelSource::Blue &&
               kernel->sources[3] == runtime::ImageChannelSource::One,
           "a Pass over a Video source's channels selects the frame's stored channel");
}
} // namespace

int main() {
    try {
        imageLayerSelection();
        shuffleChain();
        defaultSlots();
        viewerInspection();
        outputChannelLayers();
        dataSafeShuffle();
        passPicker();
        imageToChannelSet();
        videoChannels();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
