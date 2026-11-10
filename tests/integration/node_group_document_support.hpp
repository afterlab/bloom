#pragma once

// Shared fixture for the Node Group proofs (#342): a real document built through commands, once as
// a flat graph and once with the same nodes inside a group definition.
//
// The tone chain is `image1 -> Exposure -> Image Mix (second image2, Mask mask1) -> image1`. In the
// flat document the nodes sit in the main composition; in the grouped one they sit in a group
// definition made by CreateGroupDefinition, and the main composition holds a Node Group instance
// whose pixel inputs take the plate, a second plate and the Mask a Luma Key cuts from it. The
// instance's `scalar1` is the exposure in stops (a constant, or a Scalar value node when the
// driver option is set) and `scalar2` is the mix factor.

#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/group_definition_operations.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/node_group_nodes.hpp>
#include <bloom/document/value_nodes.hpp>

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace bloom::test::node_group {

struct ToneOptions final {
    bool grouped = true;
    // The instance's exposure (flat: the Exposure node's own value) in stops.
    double stops = 0.75;
    double factor = 0.6;
    // Drive the exposure from a Scalar value node instead of a constant.
    bool drivenStops = false;
};

struct ToneParts final {
    document::CompositionId group;
    document::NodeId plate;     // the Solid whose Layer shows the result
    document::NodeId layer;     // the Layer Output
    document::NodeId other;     // the second plate
    document::NodeId key;       // Luma Key of the second plate
    document::NodeId driver;    // Scalar value node (drivenStops)
    document::NodeId instance;  // the Node Group (grouped)
    document::NodeId exposure;  // flat: in main; grouped: in the group
    document::NodeId mix;
    document::NodeId groupInput;
    document::NodeId groupOutput;
};

struct Fixture final {
    document::NewProject initial = document::makeNewProject(
        "Node groups", "Main", core::RationalTime::fromInteger(4),
        *document::CompositionFormat::create(96, 80, core::PixelAspectRatio::square(),
                                             *document::FrameRate::create(24, 1)));
    document::CompositionId main = initial.initialCompositionId;
    document::Document document{std::move(initial.project)};
    commands::CommandStack commands{document};
    ToneParts parts;

    template <typename Operation, typename... Arguments>
    commands::CommandResult run(Arguments&&... arguments) {
        commands::Transaction transaction("Node group fixture", document.snapshot().revision());
        transaction.emplace<Operation>(std::forward<Arguments>(arguments)...);
        auto result = commands.execute(std::move(transaction));
        if (!result.succeeded()) {
            std::string why = "node group fixture command refused";
            for (const auto& failure : result.operationFailures)
                why += ": " + failure.issue.message;
            throw std::runtime_error(why);
        }
        return result;
    }

    [[nodiscard]] const document::Composition& current(const document::CompositionId id) const {
        const auto snapshot = document.snapshot();
        const auto* found = snapshot.project().findComposition(id);
        if (found == nullptr)
            throw std::runtime_error("node group fixture composition is missing");
        // The snapshot shares its project with the document, which outlives this reference.
        return *found;
    }

    [[nodiscard]] document::ParameterId parameter(const document::CompositionId in,
                                                  const document::NodeId node,
                                                  const std::string_view role) const {
        const auto* record = current(in).graph().findNode(node);
        if (record != nullptr)
            for (const auto& binding : record->parameters)
                if (binding.role == role)
                    return binding.parameterId;
        throw std::runtime_error("node group fixture parameter is missing: " + std::string(role));
    }

    document::NodeId addNode(const document::CompositionId in, const std::string_view type) {
        const auto made = run<commands::AddNode>(in, std::string(type), document::Vec2d{0, 0});
        const auto id = made.outputId<document::NodeId>(commands::kAddNodeOutput);
        if (!id)
            throw std::runtime_error("node group fixture node was not added");
        return *id;
    }

    // Connects, taking an existing link into the input out first.
    void connect(const document::CompositionId in, const document::NodeId from,
                 const std::string_view output, const document::NodeId to,
                 const std::string_view input) {
        for (const auto& edge : current(in).graph().edges())
            if (const auto* target = std::get_if<document::NodeInputRef>(&edge.destination);
                target != nullptr && target->nodeId == to && target->port == input) {
                run<commands::DisconnectInput>(in,
                                               document::NodeInputRef{to, std::string(input)});
                break;
            }
        run<commands::ConnectPorts>(in, document::OutputPortRef{from, std::string(output)},
                                    document::NodeInputRef{to, std::string(input)});
    }

    void set(const document::CompositionId in, const document::NodeId node,
             const std::string_view role, document::ParameterValue value) {
        run<commands::SetParameterSource>(in, parameter(in, node, role),
                                          document::ConstantValueSource{std::move(value)});
    }

    void drive(const document::CompositionId in, const document::NodeId node,
               const std::string_view role, const document::NodeId source,
               const std::string_view port) {
        run<commands::SetParameterSource>(
            in, parameter(in, node, role),
            document::DriverBindingSource{source, std::string(port)});
    }

    // The pixel graph around the tone chain: a translucent HDR plate on a Layer, a second plate,
    // and the Mask a Luma Key cuts from it.
    void buildPlates() {
        const auto solid =
            run<commands::AddSolidLayer>(main, "Plate", core::Color4d{1.4, 0.5, 0.2, 0.6});
        parts.plate = *solid.outputId<document::NodeId>(commands::kAddSolidLayerSolidNodeOutput);
        parts.layer =
            *solid.outputId<document::NodeId>(commands::kAddSolidLayerLayerOutputNodeOutput);
        run<commands::DisconnectInput>(
            main, document::NodeInputRef{parts.layer,
                                         std::string(document::kLayerOutputContentInputPort)});
        parts.other = addNode(main, document::kSolidSourceNodeType);
        set(main, parts.other, document::kSolidColorParameterRole,
            core::Color4d{0.2, 0.7, 0.9, 1.0});
        parts.key = addNode(main, "bloom.luma-key");
        set(main, parts.key, "low", 0.05);
        set(main, parts.key, "high", 0.7);
        connect(main, parts.other, "image", parts.key, "input");
    }
};

[[nodiscard]] inline std::unique_ptr<Fixture> buildTone(const ToneOptions& options = {}) {
    auto fixture = std::make_unique<Fixture>();
    auto& f = *fixture;
    auto& p = f.parts;
    f.buildPlates();
    const auto content = std::string(document::kLayerOutputContentInputPort);
    if (options.drivenStops) {
        p.driver = f.addNode(f.main, document::kScalarValueNodeType);
        f.set(f.main, p.driver, document::kValueParameterRole, options.stops);
    }
    if (!options.grouped) {
        p.exposure = f.addNode(f.main, "bloom.exposure");
        p.mix = f.addNode(f.main, "bloom.image-mix");
        if (options.drivenStops)
            f.drive(f.main, p.exposure, "amount", p.driver, document::kValuePortName);
        else
            f.set(f.main, p.exposure, "amount", options.stops);
        f.set(f.main, p.mix, "factor", options.factor);
        f.connect(f.main, p.plate, "image", p.exposure, "input");
        f.connect(f.main, p.exposure, "image", p.mix, "input");
        f.connect(f.main, p.other, "image", p.mix, "second");
        f.connect(f.main, p.key, "mask", p.mix, "mask");
        f.connect(f.main, p.mix, "image", p.layer, content);
        return fixture;
    }

    p.instance = f.addNode(f.main, document::kNodeGroupNodeType);
    const auto made = f.run<commands::CreateGroupDefinition>(f.main, "Tone", p.instance);
    p.group = *made.outputId<document::CompositionId>(commands::kCreateGroupDefinitionOutput);
    p.groupInput = *made.outputId<document::NodeId>(commands::kCreateGroupDefinitionInputNodeOutput);
    p.groupOutput =
        *made.outputId<document::NodeId>(commands::kCreateGroupDefinitionOutputNodeOutput);
    p.exposure = f.addNode(p.group, "bloom.exposure");
    p.mix = f.addNode(p.group, "bloom.image-mix");
    f.connect(p.group, p.groupInput, "image1", p.exposure, "input");
    f.connect(p.group, p.exposure, "image", p.mix, "input");
    f.connect(p.group, p.groupInput, "image2", p.mix, "second");
    f.connect(p.group, p.groupInput, "mask1", p.mix, "mask");
    f.connect(p.group, p.mix, "image", p.groupOutput, "image1");
    f.drive(p.group, p.exposure, "amount", p.groupInput, "scalar1");
    f.drive(p.group, p.mix, "factor", p.groupInput, "scalar2");
    if (options.drivenStops)
        f.drive(f.main, p.instance, "scalar1", p.driver, document::kValuePortName);
    else
        f.set(f.main, p.instance, "scalar1", options.stops);
    f.set(f.main, p.instance, "scalar2", options.factor);
    f.connect(f.main, p.plate, "image", p.instance, "image1");
    f.connect(f.main, p.other, "image", p.instance, "image2");
    f.connect(f.main, p.key, "mask", p.instance, "mask1");
    f.connect(f.main, p.instance, "image1", p.layer, content);
    return fixture;
}

// The ChannelSet chain: `plate -> Image to Channel Set -> Channel Pass -> Layer`. Grouped, the
// Channel Pass sits inside a group definition and the set enters at `channels1` (leaving: the pack
// sits inside and the set leaves at `channels1` into a Channel Pass outside).
[[nodiscard]] inline std::unique_ptr<Fixture> buildChannelChain(const bool grouped,
                                                                const bool leaving = false) {
    auto fixture = std::make_unique<Fixture>();
    auto& f = *fixture;
    auto& p = f.parts;
    f.buildPlates();
    const auto content = std::string(document::kLayerOutputContentInputPort);
    const auto pack = [&](const document::CompositionId in) {
        const auto node = f.addNode(in, "bloom.image-to-channel-set");
        f.set(in, node, "layer", std::string("pass"));
        return node;
    };
    const auto pass = [&](const document::CompositionId in) {
        const auto node = f.addNode(in, "bloom.channel-pass");
        f.set(in, node, "pass", std::string("pass"));
        return node;
    };
    if (!grouped) {
        const auto packNode = pack(f.main);
        const auto passNode = pass(f.main);
        f.connect(f.main, p.plate, "image", packNode, "image");
        f.connect(f.main, packNode, "channels", passNode, "channels");
        f.connect(f.main, passNode, "image", p.layer, content);
        return fixture;
    }
    p.instance = f.addNode(f.main, document::kNodeGroupNodeType);
    const auto made = f.run<commands::CreateGroupDefinition>(f.main, "Channels", p.instance);
    p.group = *made.outputId<document::CompositionId>(commands::kCreateGroupDefinitionOutput);
    p.groupInput = *made.outputId<document::NodeId>(commands::kCreateGroupDefinitionInputNodeOutput);
    p.groupOutput =
        *made.outputId<document::NodeId>(commands::kCreateGroupDefinitionOutputNodeOutput);
    if (!leaving) {
        const auto packNode = pack(f.main);
        const auto passNode = pass(p.group);
        f.connect(f.main, p.plate, "image", packNode, "image");
        f.connect(f.main, packNode, "channels", p.instance, "channels1");
        f.connect(p.group, p.groupInput, "channels1", passNode, "channels");
        f.connect(p.group, passNode, "image", p.groupOutput, "image1");
        f.connect(f.main, p.instance, "image1", p.layer, content);
    } else {
        const auto packNode = pack(p.group);
        const auto passNode = pass(f.main);
        f.connect(p.group, p.groupInput, "image1", packNode, "image");
        f.connect(p.group, packNode, "channels", p.groupOutput, "channels1");
        f.connect(f.main, p.plate, "image", p.instance, "image1");
        f.connect(f.main, p.instance, "channels1", passNode, "channels");
        f.connect(f.main, passNode, "image", p.layer, content);
    }
    return fixture;
}

} // namespace bloom::test::node_group
