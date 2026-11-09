// The Viewer's missing-font overlay marks every layer built from a Text in a fallback face,
// however its source is wired, and only those layers.

#include <bloom/runtime/missing_font_layers.hpp>

#include <iostream>
#include <string>
#include <vector>

namespace {
using namespace bloom;
using namespace bloom::runtime;

int failures = 0;
void expect(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

OperationIndex at(const std::size_t index) { return OperationIndex::fromRaw(index); }

CompiledText text(const bool missing) {
    CompiledText result;
    result.fontMissing = missing;
    return result;
}
CompiledImageEffect effect(const std::size_t input) {
    CompiledImageEffect result{{}, at(input), {}};
    return result;
}
CompiledLayerOutput layer(const std::size_t input, const std::optional<std::size_t> parent = {}) {
    CompiledLayerOutput result{{}, {}, at(input), {}, {}, {}, {}, {}, {}, {}};
    if (parent)
        result.parent = at(*parent);
    return result;
}
CompiledMerge merge(const std::vector<std::size_t>& inputs) {
    CompiledMerge result;
    for (const auto input : inputs)
        result.entries.push_back({{}, {}, at(input), {}});
    return result;
}
} // namespace

int main() {
    std::vector<CompiledOperation> operations;
    operations.emplace_back(text(true));    // 0: a Text in a fallback face
    operations.emplace_back(layer(0));      // 1: the source feeds the Layer Output directly
    operations.emplace_back(effect(0));     // 2: an effect on the text
    operations.emplace_back(effect(2));     // 3: another effect
    operations.emplace_back(layer(3));      // 4: the effects feed the Layer Output
    operations.emplace_back(merge({0, 2})); // 5: a merge of the text and an effect
    operations.emplace_back(layer(5));      // 6: the merge feeds the Layer Output
    operations.emplace_back(text(false));   // 7: a Text whose font is available
    operations.emplace_back(effect(7));     // 8
    operations.emplace_back(layer(8));      // 9: not marked
    operations.emplace_back(layer(8, 0));   // 10: its transform parent is the missing text
    operations.emplace_back(merge({1, 4})); // 11: a stack of layers
    operations.emplace_back(effect(11));    // 12: an adjustment reading the stack
    operations.emplace_back(layer(12));     // 13: an adjustment layer is not a text layer

    const auto marked = layersUsingMissingFont(operations);
    expect(marked.size() == operations.size(), "one answer per operation");
    expect(marked[1], "a text feeding the Layer Output directly is marked");
    expect(marked[4], "a text behind two effects is marked");
    expect(marked[6], "a text behind a merge is marked");
    expect(!marked[9], "a text whose font is available is not marked");
    expect(!marked[10], "a layer whose transform parent uses the missing font is not marked");
    expect(!marked[13], "an adjustment reading a stack with a missing-font layer is not marked");
    expect(!marked[0] && !marked[2] && !marked[5] && !marked[11],
           "only Layer Output operations are marked");
    return failures == 0 ? 0 : 1;
}
