// #627 Number list, through commands, the real compiler and the scripted read: authoring a list
// and undoing it, what a list connects to and what is refused (with the reason), that no curve
// carries a list, a list read from a node nothing consumes, and Write to a Number List.

#include <bloom/commands/animation_operations.hpp>
#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/number_list_write_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/value_nodes.hpp>
#include <bloom/scripting/node_path_query.hpp>

#include <algorithm>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace bloom;
using document::NumberListValue;
using document::SocketValueKind;

int failures = 0;
void expect(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

struct Fixture final {
    document::NewProject initial = document::makeNewProject(
        "Number lists", "Main", core::RationalTime::fromInteger(4),
        *document::CompositionFormat::create(64, 64, core::PixelAspectRatio::square(),
                                             document::FrameRate::framesPerSecond24()));
    document::Document document{std::move(initial.project)};
    commands::CommandStack commands{document};
    document::CompositionId composition = initial.initialCompositionId;

    template <typename Operation, typename... Arguments>
    commands::CommandResult attempt(Arguments&&... arguments) {
        commands::Transaction transaction("Number lists", document.snapshot().revision());
        transaction.emplace<Operation>(composition, std::forward<Arguments>(arguments)...);
        return commands.execute(std::move(transaction));
    }
    document::NodeId add(const std::string_view type) {
        const auto result = attempt<commands::AddNode>(std::string(type), document::Vec2d{});
        const auto id = result.outputId<document::NodeId>(commands::kAddNodeOutput);
        if (!id)
            throw std::runtime_error("fixture node refused: " + std::string(type));
        return *id;
    }
    [[nodiscard]] document::ParameterId parameter(const document::NodeId id,
                                                  const std::string_view role) const {
        const auto snapshot = document.snapshot();
        const auto* node = snapshot.project().findComposition(composition)->graph().findNode(id);
        if (node != nullptr)
            for (const auto& binding : node->parameters)
                if (binding.role == role)
                    return binding.parameterId;
        throw std::runtime_error("fixture role missing: " + std::string(role));
    }
    [[nodiscard]] std::optional<NumberListValue> list(const document::NodeId id) const {
        const auto snapshot = document.snapshot();
        const auto* record = snapshot.project().findComposition(composition)->parameters().find(
            parameter(id, document::kValueParameterRole));
        const auto* constant =
            record ? std::get_if<document::ConstantValueSource>(&record->source) : nullptr;
        const auto* held = constant ? std::get_if<NumberListValue>(&constant->value) : nullptr;
        return held ? std::optional(*held) : std::nullopt;
    }
    commands::CommandResult set(const document::NodeId id, document::ParameterValue value) {
        return attempt<commands::SetParameterSource>(
            parameter(id, document::kValueParameterRole),
            document::ConstantValueSource{std::move(value)});
    }
    [[nodiscard]] std::size_t nodeCount() const {
        return document.snapshot().project().findComposition(composition)->graph().nodes().size();
    }
};

[[nodiscard]] std::string reasonOf(const commands::CommandResult& result) {
    return result.operationFailures.empty() ? std::string{}
                                            : result.operationFailures.front().issue.message;
}

void theValueAndItsRules() {
    expect(NumberListValue{}.isValid(), "the empty list is a value");
    NumberListValue full;
    full.values.assign(document::kMaximumNumberListItems, 1.0);
    expect(full.isValid(), "a list of the maximum length is a value");
    full.values.push_back(1.0);
    expect(!full.isValid(), "a list one item past the maximum is not");
    expect(!NumberListValue{{1.0, std::numeric_limits<double>::infinity()}}.isValid() &&
               !NumberListValue{{std::numeric_limits<double>::quiet_NaN()}}.isValid(),
           "a list holds finite numbers only");
    expect(document::socketValueKindName(SocketValueKind::NumberList) == "number-list" &&
               document::parameterValueKindName(document::ParameterValueKind::NumberList) ==
                   "number-list",
           "the kind is named number-list on sockets and parameters");

    // What promotes to a list, and that nothing promotes out of one.
    for (const auto source : {SocketValueKind::Scalar, SocketValueKind::Integer,
                              SocketValueKind::Boolean, SocketValueKind::Vector2,
                              SocketValueKind::Vector3, SocketValueKind::Color})
        expect(document::isPromotedSocketConnection(source, SocketValueKind::NumberList),
               std::string(document::socketValueKindName(source)) + " promotes to a list");
    for (const auto source : {SocketValueKind::String, SocketValueKind::Image,
                              SocketValueKind::Audio, SocketValueKind::Mask,
                              SocketValueKind::ChannelSet, SocketValueKind::ScalarField,
                              SocketValueKind::Vector2Field, SocketValueKind::ColorField,
                              SocketValueKind::Path})
        expect(!document::isAcceptedSocketConnection(source, SocketValueKind::NumberList),
               std::string(document::socketValueKindName(source)) + " does not become a list");
    expect(document::isAcceptedSocketConnection(SocketValueKind::NumberList,
                                                SocketValueKind::NumberList),
           "a list connects to a list input");
    for (const auto destination :
         {SocketValueKind::Scalar, SocketValueKind::Integer, SocketValueKind::Boolean,
          SocketValueKind::Vector2, SocketValueKind::Vector3, SocketValueKind::Color,
          SocketValueKind::String, SocketValueKind::Image, SocketValueKind::Audio,
          SocketValueKind::Mask, SocketValueKind::ChannelSet, SocketValueKind::ScalarField,
          SocketValueKind::Vector2Field, SocketValueKind::ColorField, SocketValueKind::Path})
        expect(!document::isAcceptedSocketConnection(SocketValueKind::NumberList, destination),
               "a list does not connect to " +
                   std::string(document::socketValueKindName(destination)));
}

void authoringUndoAndRefusals() {
    Fixture fixture;
    const auto node = fixture.add(document::kNumberListValueNodeType);
    expect(fixture.list(node) == NumberListValue{}, "a new Number List node holds the empty list");

    const NumberListValue authored{{3.0, -1.5, 0.0, 1e6}};
    expect(fixture.set(node, authored).changed() && fixture.list(node) == authored,
           "a list is authored as one constant");
    expect(fixture.commands.undo().succeeded() && fixture.list(node) == NumberListValue{},
           "one undo restores the list before");
    expect(fixture.commands.redo().succeeded() && fixture.list(node) == authored,
           "and one redo restores the edit");

    NumberListValue tooLong;
    tooLong.values.assign(document::kMaximumNumberListItems + 1, 0.0);
    expect(!fixture.set(node, tooLong).succeeded() && fixture.list(node) == authored,
           "a list past the maximum length is refused and nothing changes");
    expect(!fixture.set(node, NumberListValue{{std::numeric_limits<double>::quiet_NaN()}})
                .succeeded(),
           "a list with a number that is not finite is refused");
    expect(!fixture.set(node, 2.0).succeeded() && fixture.list(node) == authored,
           "a list parameter does not take a number");
    const auto scalar = fixture.add(document::kScalarValueNodeType);
    expect(!fixture.set(scalar, authored).succeeded(), "a number parameter does not take a list");

    // No curve kind carries a list.
    expect(!fixture
                .attempt<commands::CreateAnimationForParameter>(
                    fixture.parameter(node, document::kValueParameterRole),
                    core::RationalTime::fromInteger(0))
                .succeeded() &&
               fixture.list(node) == authored,
           "a list cannot be keyed");

    // A list output connects to no number input, and the refusal says why.
    const auto math = fixture.add(document::kScalarMathNodeType);
    const auto refused = fixture.attempt<commands::ConnectPorts>(
        document::OutputPortRef{node, std::string(document::kValuePortName)},
        document::NodeInputRef{math, std::string(document::kFirstOperandPortName)});
    expect(!refused.succeeded() &&
               reasonOf(refused).find("a number list connects only to a Number List input") !=
                   std::string::npos,
           "a list into a number input is refused with the rule: " + reasonOf(refused));
}

void aListIsReadFromANodeNothingConsumes() {
    Fixture fixture;
    const auto node = fixture.add(document::kNumberListValueNodeType);
    const NumberListValue authored{{0.5, 2.0, -4.0}};
    expect(fixture.set(node, authored).changed(), "the fixture list is authored");
    const auto time = core::RationalTime::fromInteger(1);

    const auto read = scripting::queryNodeNumberList(fixture.document.snapshot(),
                                                     fixture.composition, node, {}, time, {});
    expect(read.list == authored && read.diagnostic.empty(),
           "the node's list is read though nothing is connected to it: " + read.diagnostic);
    const auto named = scripting::queryNodeNumberList(
        fixture.document.snapshot(), fixture.composition, node, document::kValuePortName, time, {});
    expect(named.list == authored, "and by the output's name");

    const auto scalar = fixture.add(document::kScalarValueNodeType);
    const auto none = scripting::queryNodeNumberList(fixture.document.snapshot(),
                                                     fixture.composition, scalar, {}, time, {});
    expect(!none.list && none.diagnostic.find("has no Number List output") != std::string::npos,
           "a node without a list output says so: " + none.diagnostic);
    const auto wrongPort = scripting::queryNodeNumberList(
        fixture.document.snapshot(), fixture.composition, node, "other", time, {});
    expect(!wrongPort.list &&
               wrongPort.diagnostic.find("has no Number List output named \"other\"") !=
                   std::string::npos,
           "an unknown output name is refused by name: " + wrongPort.diagnostic);
    const auto missing = scripting::queryNodeNumberList(
        fixture.document.snapshot(), fixture.composition, document::NodeId::fromRaw(9999), {},
        time, {});
    expect(!missing.list && missing.diagnostic.find("does not exist") != std::string::npos,
           "a missing node is refused");

    // The edit is what the next read answers: nothing stale is kept between snapshots.
    const NumberListValue edited{{0.5, 2.0, -4.0, 8.0}};
    expect(fixture.set(node, edited).changed(), "the list is edited");
    expect(scripting::queryNodeNumberList(fixture.document.snapshot(), fixture.composition, node,
                                          {}, time, {})
                   .list == edited,
           "a read after an edit gives the edited list");
}

void writeToANumberListIsOneUndoStep() {
    Fixture fixture;
    const auto source = fixture.add(document::kNumberListValueNodeType);
    const NumberListValue authored{{1.0, 2.0, 3.0}};
    expect(fixture.set(source, authored).changed(), "the fixture list is authored");
    const auto read = scripting::queryNodeNumberList(
        fixture.document.snapshot(), fixture.composition, source, {},
        core::RationalTime::fromInteger(0), {});
    if (!read.list) {
        expect(false, "the source list is read");
        return;
    }
    const auto nodesBefore = fixture.nodeCount();
    const auto before = fixture.commands.size();
    const auto result = fixture.attempt<commands::WriteValueToNumberList>(
        std::nullopt, *read.list, document::Vec2d{300.0, 0.0});
    const auto written =
        result.outputId<document::NodeId>(commands::kWriteToNumberListNodeOutput);
    expect(written.has_value() && fixture.commands.size() == before + 1 &&
               fixture.nodeCount() == nodesBefore + 1,
           "one command adds one Number List node and names it");
    if (!written)
        return;
    expect(fixture.list(*written) == authored, "the new node holds the list that was read");
    const auto snapshot = fixture.document.snapshot();
    const auto& graph = snapshot.project().findComposition(fixture.composition)->graph();
    expect(std::ranges::none_of(graph.edges(),
                                [&](const auto& edge) { return edge.source.nodeId == *written; }),
           "the new node is wired to nothing");
    expect(fixture.commands.undo().succeeded() && fixture.nodeCount() == nodesBefore,
           "one undo removes it");
    expect(fixture.commands.redo().succeeded() && fixture.list(*written) == authored,
           "and one redo brings it back");

    // Into an existing Number List node: its value is replaced, and undo restores it.
    const NumberListValue other{{9.0}};
    expect(fixture.attempt<commands::WriteValueToNumberList>(*written, other, document::Vec2d{})
                   .changed() &&
               fixture.list(*written) == other && fixture.nodeCount() == nodesBefore + 1,
           "a target node's list is replaced and no node is added");
    expect(fixture.commands.undo().succeeded() && fixture.list(*written) == authored,
           "undo restores the target's list");
    const auto scalar = fixture.add(document::kScalarValueNodeType);
    const auto wrongTarget =
        fixture.attempt<commands::WriteValueToNumberList>(scalar, other, document::Vec2d{});
    expect(!wrongTarget.succeeded() &&
               reasonOf(wrongTarget).find("is not a Number List node") != std::string::npos,
           "a target that is not a Number List node is refused: " + reasonOf(wrongTarget));
}

} // namespace

int main() {
    try {
        theValueAndItsRules();
        authoringUndoAndRefusals();
        aListIsReadFromANodeNothingConsumes();
        writeToANumberListIsOneUndoStep();
    } catch (const std::exception& error) {
        std::cerr << "number list fixture failure: " << error.what() << '\n';
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
