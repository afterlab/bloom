// #745: the MCP JSON layer. A parse or a build allocates what it holds under the 32 MiB cap (it
// used to zero-fill a fresh 32 MiB arena each time), the value and depth bounds count exactly as
// before and name what they refuse, and the node reference's values written straight into a
// response equal parsing the scripting text they replace.
#include "core_internal.hpp"
#include "json.hpp"

#include <bloom/document/node_definition_registry.hpp>
#include <bloom/scripting/node_reference.hpp>

#include <cmath>
#include <iostream>
#include <limits>
#include <string>
#include <vector>
#include <yyjson.h>

namespace {

using namespace bloom;

int failures = 0;

void expect(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAILED: " << message << '\n';
    }
}

// What parsing a JSON text and writing it again gives: the old projection path.
std::string reparsed(const std::string& text) {
    yyjson_doc* document = yyjson_read(text.data(), text.size(), 0);
    if (document == nullptr)
        return "<invalid: " + text + ">";
    std::size_t size = 0;
    char* written = yyjson_write(document, 0, &size);
    std::string result(written, size);
    std::free(written);
    yyjson_doc_free(document);
    return result;
}

// The value and depth the pre-#745 recursive validator counted.
void oldCount(yyjson_val* value, const std::size_t depth, std::size_t& count,
              std::size_t& deepest) {
    ++count;
    deepest = std::max(deepest, depth);
    if (yyjson_is_arr(value)) {
        yyjson_arr_iter iterator = yyjson_arr_iter_with(value);
        while (auto* child = yyjson_arr_iter_next(&iterator))
            oldCount(child, depth + 1, count, deepest);
    } else if (yyjson_is_obj(value)) {
        yyjson_obj_iter iterator = yyjson_obj_iter_with(value);
        while (auto* key = yyjson_obj_iter_next(&iterator))
            oldCount(yyjson_obj_iter_get_val(key), depth + 1, count, deepest);
    }
}

void budgets() {
    const std::string trivial =
        R"({"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"query",)"
        R"("arguments":{"kind":"context"}}})";
    const mcp::Parsed parsed(trivial);
    expect(parsed.bytesUsed() < 16 * 1024, "a trivial request holds what it reads, not an arena: " +
                                               std::to_string(parsed.bytesUsed()));
    mcp::Json out;
    auto* root = out.object();
    out.set(root, "ok", out.boolean(true));
    expect(out.encode(root) == R"({"ok":true})" && out.bytesUsed() < 16 * 1024,
           "a small response holds what it writes: " + std::to_string(out.bytesUsed()));

    // The cap still binds, named with its value and how much was asked for.
    mcp::Json capped(4096);
    try {
        auto* list = capped.array();
        for (int index = 0; index < 10000; ++index)
            capped.append(list, capped.number(static_cast<std::uint64_t>(index)));
        expect(false, "a build over its memory cap is refused");
    } catch (const mcp::LimitExceeded& exceeded) {
        expect(exceeded.limit == mcp::McpLimit::JsonMemory && exceeded.value == 4096 &&
                   exceeded.actual > 4096 &&
                   std::string(exceeded.what()).find("jsonMemoryBytes") != std::string::npos,
               std::string("the memory refusal names the cap: ") + exceeded.what());
    }
    // A response over its bound is refused by name with its size.
    mcp::Json big;
    auto* text = big.text(std::string(5000, 'x'));
    try {
        static_cast<void>(big.encode(text, 1000));
        expect(false, "a response over its bound is refused");
    } catch (const mcp::LimitExceeded& exceeded) {
        expect(exceeded.limit == mcp::McpLimit::ResponseBytes && exceeded.value == 1000 &&
                   exceeded.actual == 5002,
               std::string("the response refusal names its size: ") + exceeded.what());
    }
}

void counts() {
    for (const std::string text : {R"({"a":[1,2,{"b":[[],[3]]}],"c":null})", R"([[[[0]]]])",
                                   R"("x")", R"({})", R"([{"a":{"b":{"c":[1,2,3]}}},4,[5,[6]]])"}) {
        const mcp::Parsed parsed(text);
        std::size_t count = 0;
        std::size_t deepest = 0;
        oldCount(parsed.root(), 0, count, deepest);
        expect(parsed.values() == count && parsed.depth() == deepest,
               "counted as before for " + text + ": " + std::to_string(parsed.values()) + "/" +
                   std::to_string(parsed.depth()));
    }
    // At the bound passes; one over is refused with the exact count.
    std::string list = "[";
    for (int index = 0; index < 65535; ++index)
        list += index ? ",0" : "0";
    list += "]";
    expect(mcp::Parsed(list).values() == 65536, "65,536 values are accepted");
    try {
        static_cast<void>(mcp::Parsed(list.substr(0, list.size() - 1) + ",0]"));
        expect(false, "65,537 values are refused");
    } catch (const mcp::LimitExceeded& exceeded) {
        expect(exceeded.limit == mcp::McpLimit::JsonValues && exceeded.value == 65536 &&
                   exceeded.actual == 65537,
               std::string("the value refusal counts exactly: ") + exceeded.what());
    }
    const mcp::Parsed deferred(std::string(33, '[') + std::string(33, ']'), true);
    expect(!deferred.refusal() && deferred.depth() == 32,
           "32 levels below the root are the bound, and pass");
    const mcp::Parsed over(std::string(34, '[') + std::string(34, ']'), true);
    expect(over.refusal() && over.refusal()->limit == mcp::McpLimit::JsonDepth &&
               over.refusal()->actual == 33,
           "33 levels are refused with the depth");
    try {
        static_cast<void>(mcp::Parsed(R"({"a":1,"b":2,"a":3})"));
        expect(false, "a duplicate member is refused");
    } catch (const mcp::InvalidInput&) {
    }
}

void referenceParity() {
    const auto same = [](mcp::Json& out, yyjson_mut_val* value, const std::string& text,
                         const std::string& what) {
        const auto direct = out.encode(value, std::numeric_limits<std::size_t>::max());
        const auto parsed = reparsed(text);
        expect(direct == parsed, what + ": " + direct + " != " + parsed);
    };
    const auto& registry = document::builtInNodeDefinitions();
    std::size_t types = 0;
    for (const auto& definition : registry.definitions()) {
        for (const bool full : {false, true}) {
            mcp::Json out;
            same(out, mcp::referenceNodeType(out, definition, full),
                 scripting::nodeTypeJson(definition, full), definition.key.typeId);
        }
        for (const auto& parameter : definition.parameters) {
            mcp::Json out;
            same(out, mcp::referenceValue(out, parameter.defaultValue),
                 scripting::parameterValueJson(parameter.defaultValue),
                 definition.key.typeId + " " + parameter.role + " default");
            mcp::Json metadata;
            same(metadata,
                 mcp::referenceMetadata(metadata, parameter.schemaKey, parameter.valueKind,
                                        &parameter),
                 scripting::parameterMetadataJson(parameter.schemaKey, parameter.valueKind,
                                                  &parameter),
                 definition.key.typeId + " " + parameter.role + " metadata");
        }
        ++types;
    }
    expect(types > 50, "every built-in node type was compared: " + std::to_string(types));

    std::vector<document::ParameterValue> values{true,
                                                 std::int64_t{-42},
                                                 0.0,
                                                 -0.0,
                                                 1.0,
                                                 1.5,
                                                 -2.75,
                                                 1e15,
                                                 -1e15,
                                                 999999999999999.0,
                                                 1e14,
                                                 9223372036854775808.0,
                                                 -9223372036854775808.0,
                                                 1.8446744073709552e19,
                                                 1e19,
                                                 1e-7,
                                                 123456789.125,
                                                 6.02e23,
                                                 std::numeric_limits<double>::quiet_NaN(),
                                                 std::numeric_limits<double>::infinity(),
                                                 document::Vec2d{1.0, 0.5},
                                                 document::Vec3d{-1.0, 2.0, 1e20},
                                                 core::Color4d{0.25, 0.5, 0.75, 1.0},
                                                 std::string("quote \" and \\ and \n newline"),
                                                 core::RationalTime::fromInteger(3),
                                                 document::NumberListValue{{1.0, 2.5, -0.0}}};
    document::GradientValue gradient;
    gradient.stops = {{0.0, {1.0, 0.0, 0.0, 1.0}}, {0.5, {0.0, 1.0, 0.0, 0.5}}};
    values.emplace_back(gradient);
    gradient.modes.colorMode = document::GradientColorMode::Hsv;
    gradient.modes.hueDirection = document::GradientHueDirection::Far;
    values.emplace_back(gradient);
    document::PathContour square;
    square.closed = true;
    square.anchors = {
        {{0.0, 0.0}, std::nullopt, document::Vec2d{1.0, 0.0}, document::PathAnchorMode::Free},
        {{10.0, 0.5}, document::Vec2d{9.0, 0.5}, std::nullopt, document::PathAnchorMode::Corner}};
    values.emplace_back(document::PathValue::fromContour(square));
    document::PathValue compound;
    compound.contours = {square, square};
    compound.fillRule = document::PathFillRule::EvenOdd;
    compound.groups = {{"a", 0, 2, core::Color4d{1.0, 1.0, 1.0, 1.0}}, {"b", 1, 1, std::nullopt}};
    values.emplace_back(compound);
    for (const auto& value : values) {
        mcp::Json out;
        same(out, mcp::referenceValue(out, value), scripting::parameterValueJson(value),
             "value " + std::to_string(value.index()));
    }
    const auto rate = *document::FrameRate::create(30000, 1001);
    for (const auto& time :
         {core::RationalTime::fromInteger(2), *core::RationalTime::create(1001, 30000),
          *core::RationalTime::create(1, 7)}) {
        mcp::Json out;
        same(out, mcp::referenceTime(out, time, rate), scripting::timeJson(time, rate), "time");
    }
}

} // namespace

int main() {
    try {
        budgets();
        counts();
        referenceParity();
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    if (failures == 0)
        std::cout << "json values passed\n";
    return failures == 0 ? 0 : 1;
}
