// #724 Scopes, section 8b: MCP's `query` kind `scopes` on the shared core without a transport. The
// tables an agent receives are the read's own (scripting/scope_read.hpp) cell for cell, a summary
// and a picture are answered, a range gives one result per frame, and every refusal carries its
// remedy. The project is one white-ish solid layer at quarter resolution, headless, no device.

#include <bloom/document/project.hpp>
#include <bloom/mcp/server_core.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/scripting/scope_read.hpp>
#include <bloom/scripting/session.hpp>

#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
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

bool contains(const std::string& text, const std::string_view part) {
    return text.find(part) != std::string::npos;
}

struct Server final {
    std::unique_ptr<scripting::Session> session = [] {
        auto created = scripting::Session::createNew();
        if (!created)
            throw std::runtime_error("session");
        return std::move(created).takeSession();
    }();
    runtime::TaskScheduler scheduler;
    std::filesystem::path captures =
        std::filesystem::temp_directory_path() / "bloom-scope-query-test";
    std::unique_ptr<mcp::McpServerCore> core;
    std::unique_ptr<mcp::McpProtocolSession> protocol;
    int nextId = 1;

    Server() {
        std::error_code ignored;
        std::filesystem::remove_all(captures, ignored);
        core = std::make_unique<mcp::McpServerCore>(mcp::McpCoreDependencies{
            .session = std::ref(*session),
            .exports = {.run =
                            [](const mcp::McpExportAccess::Work& work, const std::stop_token&) {
                                work(nullptr, 1);
                                return true;
                            },
                        .callerIsOwner = true},
            .scheduler = scheduler,
            .gpuProvider = nullptr,
            .captureDirectory = captures,
            .serverVersion = "test",
            .limits = mcp::kMcpMessageLimits});
        protocol = core->openSession();
        static_cast<void>(send(R"({"jsonrpc":"2.0","id":0,"method":"initialize","params":{)"
                               R"("protocolVersion":"2025-11-25","capabilities":{},)"
                               R"("clientInfo":{"name":"core","version":"1"}}})"));
        static_cast<void>(send(R"({"jsonrpc":"2.0","method":"notifications/initialized"})"));
    }
    ~Server() {
        core.reset();
        scheduler.beginShutdown();
        std::error_code ignored;
        std::filesystem::remove_all(captures, ignored);
    }
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    std::string send(const std::string& message) {
        const mcp::McpRequestControl control{};
        return core->handle(*protocol, message, control).response.value_or(std::string{});
    }
    std::string call(const std::string& tool, const std::string& arguments) {
        return send(R"({"jsonrpc":"2.0","id":)" + std::to_string(++nextId) +
                    R"(,"method":"tools/call","params":{"name":")" + tool + R"(","arguments":)" +
                    arguments + "}}");
    }
};

// The parsed response; `content()` is its structured result.
struct Parsed final {
    yyjson_doc* document = nullptr;
    explicit Parsed(const std::string& text) : document(yyjson_read(text.data(), text.size(), 0)) {}
    ~Parsed() { yyjson_doc_free(document); }
    Parsed(const Parsed&) = delete;
    Parsed& operator=(const Parsed&) = delete;
    [[nodiscard]] yyjson_val* content() const {
        return document == nullptr
                   ? nullptr
                   : yyjson_obj_get(yyjson_obj_get(yyjson_doc_get_root(document), "result"),
                                    "structuredContent");
    }
};

std::vector<std::uint64_t> numbersOf(yyjson_val* array) {
    std::vector<std::uint64_t> values;
    std::size_t index = 0;
    std::size_t count = 0;
    yyjson_val* item = nullptr;
    yyjson_arr_foreach(array, index, count, item) values.push_back(yyjson_get_uint(item));
    return values;
}

void tablesAreTheReadsOwn(Server& server) {
    const auto revision = server.session->snapshot().revision().value();
    const auto added = server.call(
        "transact", R"({"expectedRevision":)" + std::to_string(revision) +
                        R"(,"label":"Card","operations":[{"op":"bloom.layer.add-solid","args":{)"
                        R"("composition":1,"name":"Card","color":[0.25,0.5,0.75,1]}}]})");
    expect(contains(added, R"("isError":false)"), "the fixture solid is added: " + added);

    const auto response = server.call(
        "query", R"({"kind":"scopes","composition":1,"frame":0,"signal":"working",)"
                 R"("axisMaximum":2,"resolution":"quarter","scopes":["histogram","vectorscope"]})");
    const Parsed parsed(response);
    auto* content = parsed.content();
    expect(content != nullptr && contains(response, R"("isError":false)"),
           "a tables read answers: " + response.substr(0, 400));
    if (content == nullptr)
        return;
    auto* frames = yyjson_obj_get(content, "frames");
    expect(yyjson_is_arr(frames) && yyjson_arr_size(frames) == 1, "frames is an array of one");
    auto* frame = yyjson_arr_get_first(frames);
    expect(std::string(yyjson_get_str(yyjson_obj_get(frame, "route"))) == "cpu" &&
               !yyjson_get_bool(yyjson_obj_get(frame, "fromCache")) &&
               !yyjson_get_bool(yyjson_obj_get(frame, "empty")),
           "the frame names the CPU route and that it was measured");
    const auto counted = yyjson_get_uint(yyjson_obj_get(frame, "counted"));
    expect(counted > 0, "pixels are counted");

    // The same read through the library: the tables are cell for cell the same.
    scripting::ScopeReadRequest request;
    request.composition = document::CompositionId::fromRaw(1);
    request.signal = runtime::ScopeSignal::Working;
    request.axisMaximum = 2;
    request.resolution = scripting::ScopeReadResolution::Quarter;
    request.scopes = render::kScopeTableHistogram | render::kScopeTableVectorscope;
    scripting::ScopeReadPorts ports;
    ports.scheduler = &server.scheduler;
    const auto read = scripting::readScopes(*server.session, request, ports);
    expect(read.ok() && read.frames.size() == 1, "the library read answers: " + read.error.message);
    if (!read.ok() || read.frames.empty())
        return;
    expect(read.frames[0].counted == counted, "the same pixels are counted");
    const auto data =
        scripting::describeScopeTables(read.frames[0].counts, scripting::ScopeReadEncoding::Dense);
    auto* tables = yyjson_obj_get(frame, "tables");
    const auto histogram = numbersOf(yyjson_obj_get(yyjson_obj_get(tables, "histogram"), "values"));
    const auto vectorscope =
        numbersOf(yyjson_obj_get(yyjson_obj_get(tables, "vectorscope"), "values"));
    expect(data.histogram && histogram.size() == data.histogram->values.size() &&
               std::equal(histogram.begin(), histogram.end(), data.histogram->values.begin()),
           "the histogram's numbers equal the read's");
    expect(data.vectorscope && vectorscope.size() == data.vectorscope->values.size() &&
               std::equal(vectorscope.begin(), vectorscope.end(), data.vectorscope->values.begin()),
           "the vectorscope's numbers equal the read's");
    expect(yyjson_obj_get(tables, "waveform") == nullptr, "an unrequested scope is not returned");
    expect(yyjson_obj_get(content, "luma") != nullptr &&
               yyjson_obj_get(content, "primaries") != nullptr &&
               yyjson_obj_get(content, "settings") != nullptr,
           "luma, primaries and the settings measured are reported");

    // Sparse: pairs of the non-zero cells.
    const auto sparse = server.call(
        "query", R"({"kind":"scopes","composition":1,"frame":0,"signal":"working","axisMaximum":2,)"
                 R"("resolution":"quarter","scopes":["histogram"],"encoding":"sparse"})");
    const Parsed sparseParsed(sparse);
    auto* sparseFrame =
        sparseParsed.content() == nullptr
            ? nullptr
            : yyjson_arr_get_first(yyjson_obj_get(sparseParsed.content(), "frames"));
    const auto pairs =
        sparseFrame == nullptr
            ? std::vector<std::uint64_t>{}
            : numbersOf(yyjson_obj_get(
                  yyjson_obj_get(yyjson_obj_get(sparseFrame, "tables"), "histogram"), "values"));
    expect(!pairs.empty() && pairs.size() % 2 == 0 && pairs.size() < histogram.size(),
           "the sparse encoding lists index and count pairs: " + sparse.substr(0, 300));
}

void summaryAndPicture(Server& server) {
    const auto summary =
        server.call("query", R"({"kind":"scopes","composition":1,"frame":0,"signal":"working",)"
                             R"("resolution":"quarter","result":"summary"})");
    expect(contains(summary, R"("isError":false)") && contains(summary, R"("summary":{)") &&
               contains(summary, R"("channels":{)") && !contains(summary, R"("tables":{)"),
           "a summary read answers a summary and no tables: " + summary.substr(0, 400));

    const auto picture =
        server.call("query", R"({"kind":"scopes","composition":1,"frame":0,"resolution":"quarter",)"
                             R"("result":"image","image":"waveform","width":256,"height":192})");
    expect(contains(picture, R"("isError":false)") && contains(picture, R"("width":256)") &&
               contains(picture, R"("height":192)") && contains(picture, "image/png"),
           "an image read answers a PNG of the size asked for: " + picture.substr(0, 400));

    const auto range = server.call(
        "query", R"({"kind":"scopes","composition":1,"frame":0,"lastFrame":2,"signal":"working",)"
                 R"("resolution":"quarter","scopes":["histogram"],"encoding":"sparse"})");
    const Parsed parsed(range);
    auto* frames =
        parsed.content() == nullptr ? nullptr : yyjson_obj_get(parsed.content(), "frames");
    expect(frames != nullptr && yyjson_arr_size(frames) == 3,
           "a range answers one result per frame");
}

void refusals(Server& server) {
    auto text = server.call("query", R"({"kind":"scopes","composition":99,"frame":0})");
    expect(contains(text, "-32602") && contains(text, "compositions are"),
           "an unknown composition lists the valid ones: " + text);
    text = server.call("query", R"({"kind":"scopes","composition":1,"frame":99999})");
    expect(contains(text, "choose a frame"),
           "a frame outside the composition names the range: " + text);
    text =
        server.call("query", R"({"kind":"scopes","composition":1,"frame":0,"scopes":["scope"]})");
    expect(contains(text, "is not a scope") && contains(text, "chromaticity"),
           "a bad scope name lists the valid ones: " + text);
    text = server.call(
        "query", R"({"kind":"scopes","composition":1,"frame":0,"display":"No Such Display"})");
    expect(contains(text, "Valid choices"), "an unknown display lists the valid pairs: " + text);
    text = server.call(
        "query", R"({"kind":"scopes","composition":1,"frame":0,"result":"image","width":9999})");
    expect(contains(text, "128 to 2048"), "an image width out of bounds names the bounds: " + text);
    text = server.call(
        "query",
        R"({"kind":"scopes","composition":1,"frame":0,"signal":"working","axisMaximum":3})");
    expect(contains(text, "1, 2, 4, 8 or 16"), "a bad axis maximum lists the valid ones: " + text);
    text = server.call("query", R"({"kind":"scopes","composition":1,"frame":0,"time":0})");
    expect(contains(text, "-32602"), "frame and time together are refused: " + text);
    text = server.call(
        "query", R"({"kind":"scopes","composition":1,"frame":0,"lastFrame":50,"result":"image"})");
    expect(contains(text, "or fewer"), "a long image range is refused with the remedy: " + text);
}

} // namespace

int main() {
    try {
        Server server;
        tablesAreTheReadsOwn(server);
        summaryAndPicture(server);
        refusals(server);
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    if (failures == 0)
        std::cout << "scope query passed\n";
    return failures == 0 ? 0 : 1;
}
