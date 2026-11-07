// #389: the project edits that read media or evaluate when they are created -- bloom.asset.reload,
// bloom.layer.set-anchor-preset and bloom.cryptomatte.pick -- through a headless session, as MCP's
// stdio host and the Script editor call them: each makes one undoable edit, refuses with a typed
// diagnostic, refuses without a project source, and stops when the caller's stop token fires.

#include "cryptomatte_fixture_support.hpp"

#include <bloom/commands/asset_operations.hpp>
#include <bloom/commands/layer_operations.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/channel_nodes.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/project.hpp>
#include <bloom/runtime/cryptomatte_matte_list.hpp>
#include <bloom/scripting/session.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <source_location>
#include <stop_token>
#include <string>
#include <vector>

namespace {

using namespace bloom;
using scripting::StableId;
using scripting::Value;
using scripting::ValueArray;

int failures = 0;

void expect(const bool condition, const std::string& message,
            const std::source_location location = std::source_location::current()) {
    if (!condition) {
        ++failures;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }
}

[[nodiscard]] std::string codeOf(const scripting::SessionCommandResult& result) {
    if (result.diagnostic)
        return result.diagnostic->code + ": " + result.diagnostic->message;
    if (result.command && !result.command->succeeded())
        return "rejected";
    return "ok";
}

[[nodiscard]] std::filesystem::path scratch(const std::string& name) {
    std::mt19937_64 random{std::random_device{}()};
    auto folder = std::filesystem::temp_directory_path() /
                  ("bloom-evaluated-ops-" + name + "-" + std::to_string(random()));
    std::filesystem::create_directories(folder);
    return folder;
}

struct Fixture final {
    std::unique_ptr<scripting::Session> session;
    document::CompositionId composition;

    explicit Fixture(const std::uint32_t width = 1000, const std::uint32_t height = 500) {
        auto created = scripting::Session::createNew(
            "Evaluated", "Main", core::RationalTime::fromInteger(10),
            *document::CompositionFormat::create(width, height, core::PixelAspectRatio::square(),
                                                 *document::FrameRate::create(24, 1)));
        if (!created)
            std::abort();
        session = std::move(created).takeSession();
        composition = session->snapshot().project().compositions().front().id();
    }

    template <typename Operation, typename... Arguments>
    scripting::SessionCommandResult run(Arguments&&... args) {
        commands::Transaction transaction("Fixture", session->snapshot().revision());
        transaction.emplace<Operation>(std::forward<Arguments>(args)...);
        auto result = session->execute(std::move(transaction));
        expect(result.succeeded(), "a fixture command applies");
        return result;
    }

    scripting::SessionCommandResult op(const std::string& id, scripting::Arguments arguments) {
        return session->executeJsonOperation(id, arguments);
    }

    [[nodiscard]] const document::Composition& current() const {
        static document::Snapshot held = session->snapshot();
        held = session->snapshot();
        return *held.project().findComposition(composition);
    }

    [[nodiscard]] std::optional<document::ParameterValue>
    constant(const document::ParameterId parameter) const {
        const auto snapshot = session->snapshot();
        const auto* record =
            snapshot.project().findComposition(composition)->parameters().find(parameter);
        const auto* value =
            record ? std::get_if<document::ConstantValueSource>(&record->source) : nullptr;
        return value ? std::optional(value->value) : std::nullopt;
    }

    [[nodiscard]] document::ParameterId role(const document::NodeId node,
                                             const std::string_view name) const {
        const auto snapshot = session->snapshot();
        const auto* record = snapshot.project().findComposition(composition)->graph().findNode(node);
        for (const auto& binding : record->parameters)
            if (binding.role == name)
                return binding.parameterId;
        return {};
    }
};

// ---- bloom.layer.set-anchor-preset ------------------------------------------------------------

void anchorPreset() {
    Fixture fixture;
    const auto added = fixture.run<commands::AddSolidLayer>(
        fixture.composition, "Card", core::Color4d{1, 1, 1, 1}, document::Vec2d{500, 250});
    const auto layer =
        added.command->outputId<document::LayerId>(commands::kAddSolidLayerLayerOutput).value();
    const auto solid =
        added.command->outputId<document::NodeId>(commands::kAddSolidLayerSolidNodeOutput).value();
    fixture.run<commands::SetParameterSource>(
        fixture.composition, fixture.role(solid, document::kSolidWidthParameterRole),
        document::ConstantValueSource{200.0});
    fixture.run<commands::SetParameterSource>(
        fixture.composition, fixture.role(solid, document::kSolidHeightParameterRole),
        document::ConstantValueSource{100.0});
    const auto layerNode = fixture.current().graph().findLayer(layer)->nodeId;
    const auto anchor = fixture.role(layerNode, document::kAnchorParameterRole);
    const auto before = fixture.constant(anchor);

    auto result = fixture.op("bloom.layer.set-anchor-preset",
                             {{"layer", Value(StableId{layer.value()})},
                              {"anchor", Value(std::string("top-left"))}});
    expect(result.succeeded(), "top-left applies: " + codeOf(result));
    const auto value = fixture.constant(anchor);
    const auto* point = value ? std::get_if<document::Vec2d>(&*value) : nullptr;
    expect(point && point->x == -100.0 && point->y == -50.0,
           "the anchor is half the local bounds toward the top-left");
    result = fixture.op("bloom.layer.set-anchor-preset", {{"layer", Value(StableId{layer.value()})},
                                                          {"anchor", Value(std::string("right"))},
                                                          {"time", Value(std::int64_t{3})}});
    const auto right = fixture.constant(anchor);
    const auto* rightPoint = right ? std::get_if<document::Vec2d>(&*right) : nullptr;
    expect(result.succeeded() && rightPoint && rightPoint->x == 100.0 && rightPoint->y == 0.0,
           "right is the middle of the right edge, a frame time is accepted: " + codeOf(result));

    expect(fixture.session->undo().succeeded() && fixture.session->undo().succeeded() &&
               fixture.constant(anchor) == before,
           "each preset is one undo step");

    result = fixture.op("bloom.layer.set-anchor-preset", {{"layer", Value(StableId{layer.value()})},
                                                          {"anchor", Value(std::string("middle"))}});
    expect(codeOf(result).starts_with("bloom.scripting.invalid-argument") &&
               codeOf(result).find("top-left") != std::string::npos,
           "an unknown point names the choices: " + codeOf(result));
    result = fixture.op("bloom.layer.set-anchor-preset", {{"layer", Value(StableId{999})},
                                                          {"anchor", Value(std::string("top"))}});
    expect(codeOf(result).find("does not exist") != std::string::npos,
           "an unknown layer is refused: " + codeOf(result));
    result = fixture.op("bloom.layer.set-anchor-preset", {{"layer", Value(StableId{layer.value()})},
                                                          {"anchor", Value(std::string("top"))},
                                                          {"time", Value(std::int64_t{400})}});
    expect(codeOf(result).find("no visible bounds") != std::string::npos,
           "a time outside the layer is refused: " + codeOf(result));
    fixture.run<commands::SetLayerLocked>(fixture.composition, layer, true);
    result = fixture.op("bloom.layer.set-anchor-preset", {{"layer", Value(StableId{layer.value()})},
                                                          {"anchor", Value(std::string("top"))}});
    expect(codeOf(result).find("locked") != std::string::npos,
           "a locked layer is refused: " + codeOf(result));
    fixture.run<commands::SetLayerLocked>(fixture.composition, layer, false);

    // A client with no project source, and a caller that stopped waiting.
    const auto& registry = fixture.session->operations();
    const scripting::Arguments arguments{{"composition", Value(StableId{fixture.composition.value()})},
                                         {"layer", Value(StableId{layer.value()})},
                                         {"anchor", Value(std::string("top"))},
                                         {"time", Value(ValueArray{Value(std::int64_t{0}),
                                                                   Value(std::int64_t{1})})}};
    auto created = registry.create("bloom.layer.set-anchor-preset", arguments);
    expect(!created && created.diagnostic()->code == "bloom.scripting.source-unavailable",
           "without a project source the preset is refused");
    std::stop_source stop;
    stop.request_stop();
    auto context = fixture.session->operationContext();
    context.stop = stop.get_token();
    created = registry.create("bloom.layer.set-anchor-preset", arguments, context);
    expect(!created && created.diagnostic()->code == "bloom.scripting.cancelled",
           "a stopped caller cancels the measurement");
}

// ---- bloom.asset.reload -----------------------------------------------------------------------

// A 1x1 RGBA PNG.
constexpr std::array<unsigned char, 70> kPixel{
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48,
    0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00,
    0x00, 0x1f, 0x15, 0xc4, 0x89, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x44, 0x41, 0x54, 0x78,
    0xda, 0x63, 0x64, 0x60, 0xf8, 0x5f, 0x0f, 0x00, 0x02, 0x87, 0x01, 0x80, 0xeb, 0x47,
    0xba, 0x92, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};

[[nodiscard]] std::uint32_t crc32(const std::string& bytes) {
    std::uint32_t crc = 0xffffffffU;
    for (const auto byte : bytes) {
        crc ^= static_cast<unsigned char>(byte);
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1U) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}

// The same pixel with a tEXt chunk after IHDR, so the file's bytes (and digest) change.
void writePixel(const std::filesystem::path& path, const std::string& text = {}) {
    std::string bytes(reinterpret_cast<const char*>(kPixel.data()), kPixel.size());
    if (!text.empty()) {
        const std::string body = "tEXt" + std::string("Comment") + '\0' + text;
        std::string chunk;
        const auto length = static_cast<std::uint32_t>(body.size() - 4);
        for (const int shift : {24, 16, 8, 0})
            chunk += static_cast<char>((length >> shift) & 0xffU);
        chunk += body;
        const auto crc = crc32(body);
        for (const int shift : {24, 16, 8, 0})
            chunk += static_cast<char>((crc >> shift) & 0xffU);
        bytes.insert(33, chunk);
    }
    std::ofstream(path, std::ios::binary).write(bytes.data(), static_cast<long>(bytes.size()));
}

void assetReload() {
    const auto folder = scratch("reload");
    Fixture fixture;
    expect(fixture.session->saveAs(folder / "reload.bloom").succeeded(),
           "the project is saved beside its media");
    const auto file = folder / "plate.png";
    writePixel(file);
    fixture.run<commands::ImportAssets>(std::vector{file}, folder);
    const auto asset = fixture.session->snapshot().project().assets().front();
    const auto digest = asset.contentDigest;

    writePixel(file, "graded");
    auto result = fixture.op("bloom.asset.reload", {{"asset", Value(StableId{asset.id.value()})}});
    expect(result.succeeded(), "reload applies: " + codeOf(result));
    const auto* reloaded = fixture.session->snapshot().project().findAsset(asset.id);
    expect(reloaded && reloaded->contentDigest != digest && reloaded->locator == asset.locator,
           "the asset takes the file's new content and keeps its locator");
    expect(fixture.session->undo().succeeded() &&
               fixture.session->snapshot().project().findAsset(asset.id)->contentDigest == digest,
           "reload is one undo step");

    result = fixture.op("bloom.asset.reload", {{"asset", Value(StableId{999})}});
    expect(codeOf(result).find("does not exist") != std::string::npos,
           "an unknown asset is refused: " + codeOf(result));
    std::filesystem::remove(file);
    result = fixture.op("bloom.asset.reload", {{"asset", Value(StableId{asset.id.value()})}});
    expect(!result.succeeded() &&
               fixture.session->snapshot().project().findAsset(asset.id)->contentDigest == digest,
           "a missing file is refused and changes nothing: " + codeOf(result));

    std::stop_source stop;
    stop.request_stop();
    auto context = fixture.session->operationContext();
    context.stop = stop.get_token();
    const auto created = fixture.session->operations().create(
        "bloom.asset.reload", {{"asset", Value(StableId{asset.id.value()})}}, context);
    expect(!created && created.diagnostic()->code == "bloom.scripting.cancelled",
           "a stopped caller cancels the probe");
    std::error_code error;
    std::filesystem::remove_all(folder, error);
}

// #558: a replacement path relative to the project file's folder relinks like an absolute one,
// when the old file is gone, from whatever directory the process runs in, and after the project
// is saved and opened again from elsewhere; an unreadable path's refusal names the path tried.
void assetRelinkRelative() {
    const auto folder = scratch("relink-relative");
    const auto elsewhere = scratch("relink-elsewhere");
    const auto previous = std::filesystem::current_path();
    std::filesystem::create_directories(folder / "media");
    {
        Fixture fixture;
        expect(fixture.session->saveAs(folder / "shot.bloom").succeeded(), "the project is saved");
        const auto old = folder / "old.png";
        writePixel(old);
        fixture.run<commands::ImportAssets>(std::vector{old}, folder);
        const auto id = fixture.session->snapshot().project().assets().front().id;
        std::filesystem::remove(old);
        writePixel(folder / "media" / "new.png", "replacement");

        std::filesystem::current_path(elsewhere);
        const auto args = [&](const std::string& path) {
            return scripting::Arguments{{"asset", Value(StableId{id.value()})},
                                        {"path", Value(path)},
                                        {"projectDirectory", Value(std::string("."))}};
        };
        auto result = fixture.op("bloom.asset.relink", args("media/new.png"));
        expect(result.succeeded(), "a project-relative path relinks: " + codeOf(result));
        const auto* relinked = fixture.session->snapshot().project().findAsset(id);
        expect(relinked && relinked->locator.portability == "project-relative" &&
                   relinked->locator.path == "media/new.png",
               "the relinked locator is stored project-relative, like an import's");

        result = fixture.op("bloom.asset.relink", args("media/missing.png"));
        auto message = codeOf(result);
        if (result.command && !result.command->operationFailures.empty())
            message += ": " + result.command->operationFailures.front().issue.message;
        expect(!result.succeeded() && message.find("media/missing.png") != std::string::npos,
               "an unreadable path's refusal names the path tried: " + message);
        expect(fixture.session->save().succeeded(), "the relinked project is saved");
    }
    // Opened again with the process in another folder: the locator still finds the file.
    auto opened = scripting::Session::open(folder / "shot.bloom");
    expect(static_cast<bool>(opened), "the saved project opens");
    if (opened) {
        auto session = std::move(opened).takeSession();
        const auto id = session->snapshot().project().assets().front().id;
        expect(session->snapshot().project().assets().front().locator.path == "media/new.png",
               "the reopened asset keeps its project-relative locator");
        const auto result = session->executeJsonOperation(
            "bloom.asset.reload", {{"asset", Value(StableId{id.value()})}});
        expect(result.succeeded() || codeOf(result).find("rejected") == std::string::npos,
               "the reopened asset's file resolves: " + codeOf(result));
        writePixel(folder / "media" / "again.png", "again");
        const auto relink = session->executeJsonOperation(
            "bloom.asset.relink", {{"asset", Value(StableId{id.value()})},
                                   {"path", Value(std::string("media/again.png"))},
                                   {"projectDirectory", Value(std::string("."))}});
        expect(relink.succeeded() && session->snapshot().project().findAsset(id)->locator.path ==
                                         "media/again.png",
               "a relative relink works after reopening: " + codeOf(relink));
    }
    std::filesystem::current_path(previous);
    std::error_code error;
    std::filesystem::remove_all(folder, error);
    std::filesystem::remove_all(elsewhere, error);
}

// ---- bloom.cryptomatte.pick -------------------------------------------------------------------

void cryptomattePick() {
    namespace crypto = bloom::cryptomatte_fixture;
    const auto folder = scratch("cryptomatte");
    Fixture fixture(crypto::kWidth, crypto::kHeight);
    expect(fixture.session->saveAs(folder / "crypto.bloom").succeeded(), "the project is saved");
    const auto file = folder / "crypto.exr";
    crypto::writeCryptomatteExr(file, true);
    fixture.run<commands::ImportAssets>(std::vector{file}, folder);
    fixture.run<commands::AddImageLayer>(fixture.composition,
                                         fixture.session->snapshot().project().assets().front().id);
    document::NodeId source;
    document::NodeId layer;
    for (const auto& node : fixture.current().graph().nodes()) {
        if (node.typeId == document::kImageSourceNodeType)
            source = node.id;
        if (node.typeId == document::kLayerOutputNodeType)
            layer = node.id;
    }
    const auto node =
        *fixture
             .run<commands::AddNode>(fixture.composition,
                                     std::string(document::kCryptomatteNodeType), document::Vec2d{})
             .command->outputId<document::NodeId>(commands::kAddNodeOutput);

    // Not shown by any layer yet.
    auto result = fixture.op("bloom.cryptomatte.pick", {{"node", Value(StableId{node.value()})},
                                                        {"point", Value(document::Vec2d{0.5, 0.5})}});
    expect(codeOf(result).find("not shown by a layer") != std::string::npos,
           "an unconnected Cryptomatte is refused: " + codeOf(result));

    fixture.run<commands::ConnectPorts>(
        fixture.composition,
        document::OutputPortRef{source, std::string(document::kImageSourceChannelsOutputPort)},
        document::NodeInputRef{node, std::string(document::kChannelSetPort)});
    fixture.run<commands::ConnectPorts>(
        fixture.composition,
        document::OutputPortRef{node, std::string(document::kCryptomatteImageOutputPort)},
        document::NodeInputRef{layer, std::string(document::kLayerOutputContentInputPort)});
    const auto mattes = fixture.role(node, document::kCryptomatteMattesParameterRole);
    const auto list = [&] {
        const auto value = mattes.isValid() ? fixture.constant(mattes) : std::nullopt;
        const auto* text = value ? std::get_if<std::string>(&*value) : nullptr;
        return text ? runtime::splitCryptomatteMatteList(*text) : std::vector<std::string>{};
    };

    // Pixel (0, 0) is the sphere; pixel (0, 1) the sky; pixel (1, 1) is empty.
    result = fixture.op("bloom.cryptomatte.pick", {{"node", Value(StableId{node.value()})},
                                                   {"point", Value(document::Vec2d{0.5, 0.5})}});
    expect(result.succeeded() && list() == std::vector<std::string>{"sphere"},
           "the object under the point is added by name: " + codeOf(result));
    result = fixture.op("bloom.cryptomatte.pick", {{"node", Value(StableId{node.value()})},
                                                   {"point", Value(document::Vec2d{0.5, 1.5})}});
    expect(result.succeeded() && list() == std::vector<std::string>{"sphere", "sky"},
           "a second pick appends: " + codeOf(result));
    result = fixture.op("bloom.cryptomatte.pick", {{"node", Value(StableId{node.value()})},
                                                   {"point", Value(document::Vec2d{0.5, 0.5})},
                                                   {"mode", Value(std::string("remove"))}});
    expect(result.succeeded() && list() == std::vector<std::string>{"sky"},
           "remove takes the object out: " + codeOf(result));
    expect(fixture.session->undo().succeeded() &&
               list() == std::vector<std::string>{"sphere", "sky"},
           "each pick is one undo step");

    result = fixture.op("bloom.cryptomatte.pick", {{"node", Value(StableId{node.value()})},
                                                   {"point", Value(document::Vec2d{1.5, 1.5})}});
    expect(codeOf(result).find("No object") != std::string::npos,
           "an empty pixel is refused: " + codeOf(result));
    result = fixture.op("bloom.cryptomatte.pick", {{"node", Value(StableId{node.value()})},
                                                   {"point", Value(document::Vec2d{40, 40})}});
    expect(codeOf(result).find("No object") != std::string::npos,
           "a point outside the layer is refused: " + codeOf(result));
    result = fixture.op("bloom.cryptomatte.pick", {{"node", Value(StableId{source.value()})},
                                                   {"point", Value(document::Vec2d{0.5, 0.5})}});
    expect(codeOf(result).find("not a Cryptomatte node") != std::string::npos,
           "another node type is refused: " + codeOf(result));
    result = fixture.op("bloom.cryptomatte.pick", {{"node", Value(StableId{node.value()})},
                                                   {"point", Value(document::Vec2d{0.5, 0.5})},
                                                   {"mode", Value(std::string("toggle"))}});
    expect(codeOf(result).find("\"add\" or \"remove\"") != std::string::npos,
           "an unknown mode is refused: " + codeOf(result));

    std::stop_source stop;
    stop.request_stop();
    auto context = fixture.session->operationContext();
    context.stop = stop.get_token();
    const auto created = fixture.session->operations().create(
        "bloom.cryptomatte.pick",
        {{"node", Value(StableId{node.value()})}, {"point", Value(document::Vec2d{0.5, 0.5})}},
        context);
    expect(!created && created.diagnostic()->code == "bloom.scripting.cancelled",
           "a stopped caller cancels the read-back");
    std::error_code error;
    std::filesystem::remove_all(folder, error);
}

} // namespace

int main() {
    anchorPreset();
    assetReload();
    assetRelinkRelative();
    cryptomattePick();
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "evaluated operations: ok\n";
    return 0;
}
