#pragma once
#include <algorithm>
#include <bloom/document/document.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <string>
#include <vector>

// The one rule for which node outputs Properties reads out as a value row. The rows the Properties
// editor draws and the evaluation request CompositionSession sends to the resolver both ask this,
// so a node that shows a readout is always a node that is asked for it: there is no second list
// to forget a new node in.
namespace bloom::ui {
// A Node Group is replaced by its group before anything is evaluated, and a group's boundaries
// republish the values they were given, so neither has a result of its own to read.
[[nodiscard]] inline bool nodeShowsValueOutputs(const document::NodeDefinition& definition) {
    return definition.lowering != document::NodeLoweringKind::NodeGroup &&
           definition.lowering != document::NodeLoweringKind::GroupBoundary;
}
// Pixel and sound outputs carry no value to read out.
[[nodiscard]] inline bool isReadableValueOutput(const document::OutputPortDefinition& output) {
    switch (output.valueKind) {
    case document::SocketValueKind::Image:
    case document::SocketValueKind::Audio:
    case document::SocketValueKind::Mask:
    case document::SocketValueKind::ChannelSet:
        return false;
    default:
        return true;
    }
}
[[nodiscard]] inline std::vector<std::string>
readableValueOutputs(const document::NodeDefinition& definition) {
    std::vector<std::string> ports;
    if (!nodeShowsValueOutputs(definition))
        return ports;
    for (const auto& output : definition.outputs)
        if (isReadableValueOutput(output))
            ports.push_back(output.name);
    return ports;
}
} // namespace bloom::ui
