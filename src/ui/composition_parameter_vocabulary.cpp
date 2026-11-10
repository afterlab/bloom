#include "composition_editor_support.hpp"
#include "node_editor_items.hpp"
#include "properties_registry_row.hpp"
#include <QCoreApplication>
#include <QSignalBlocker>
#include <algorithm>
#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/commands/video_source_operations.hpp>
#include <bloom/core/scalar_primitives.hpp>
#include <bloom/document/channel_nodes.hpp>
#include <bloom/document/compositing_recipe_nodes.hpp>
#include <bloom/document/depth_effect_parameters.hpp>
#include <bloom/document/image_preview.hpp>
#include <bloom/document/layer_masks.hpp>
#include <bloom/document/motion_blur_parameters.hpp>
#include <bloom/document/parameter_enums.hpp>
#include <bloom/document/project.hpp>
#include <bloom/document/value_utility_nodes.hpp>
#include <bloom/host/project_color_choice.hpp>
#include <bloom/runtime/channel_inspection_listing.hpp>
#include <bloom/ui/asset_controller.hpp>
#include <bloom/ui/channel_set_controller.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/cryptomatte_controller.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/timeline_ruler.hpp>
#include <limits>
#include <optional>
#include <tuple>
namespace bloom::ui {
QString imageDimensionsText(const document::AssetRecord* asset) {
    return asset ? QObject::tr("%1 × %2").arg(asset->width).arg(asset->height)
                 : QObject::tr("Unavailable");
}
QString imageRangeText(const document::AssetRecord* asset) {
    if (asset && asset->kind == document::AssetKind::Video)
        return QObject::tr("%1 frames · %2 s")
            .arg(asset->frames)
            .arg(asset->duration.toSeconds(), 0, 'f', 2);
    if (!asset || asset->kind != document::AssetKind::Sequence)
        return {};
    return QObject::tr("%1 frames · %2–%3")
        .arg(asset->manifest.members.size())
        .arg(asset->manifest.first)
        .arg(asset->manifest.last);
}
QString videoFieldOrderText(const std::uint32_t fieldOrder) {
    switch (fieldOrder) {
    case 1:
        return QObject::tr("Progressive");
    case 2:
        return QObject::tr("Interlaced, top field first");
    case 3:
        return QObject::tr("Interlaced, bottom field first");
    case 4:
        return QObject::tr("Interlaced, top coded, bottom displayed first");
    case 5:
        return QObject::tr("Interlaced, bottom coded, top displayed first");
    default:
        return QObject::tr("Unknown");
    }
}
QString videoTimecodeText(const document::AssetVideoStream& stream) {
    if (stream.timecodeType == 0)
        return QObject::tr("None");
    return QObject::tr("%1 (%2)").arg(QString::fromStdString(stream.timecode),
                                      stream.timecodeType == 2 ? QObject::tr("drop-frame")
                                                               : QObject::tr("non-drop"));
}
QString footageTimecodeText(const document::AssetRecord& asset) {
    const auto* stream = [&]() -> const document::AssetVideoStream* {
        const auto found = std::ranges::find(asset.videoStreams, 1U, &document::AssetVideoStream::kind);
        return found == asset.videoStreams.end() ? nullptr : &*found;
    }();
    if (!asset.interpretation.startTimecode)
        return stream ? videoTimecodeText(*stream) : QObject::tr("None");
    const auto rate =
        document::footageFrameRate(asset).value_or(document::FrameRate::framesPerSecond24());
    const auto& start = *asset.interpretation.startTimecode;
    const auto frame = static_cast<std::uint64_t>(
        std::max<std::int64_t>(0, timecodeStartFrame(rate, {start.start, false})));
    return QObject::tr("%1 (Interpret Footage)")
        .arg(formatTimelineFrameLabel(frame, rate, true, {core::RationalTime{}, start.dropFrame}));
}
QString footageFieldOrderText(const document::AssetRecord& asset) {
    const auto text = videoFieldOrderText(document::footageFieldOrder(asset));
    if (asset.interpretation.fieldOrder == document::AssetFieldOrder::File &&
        !asset.interpretation.deinterlace)
        return text;
    return QObject::tr("%1 (Interpret Footage%2)")
        .arg(text, asset.interpretation.deinterlace ? QObject::tr(", deinterlaced") : QString{});
}
QString imageAssetDisplayName(const document::AssetRecord& asset) {
    return QString::fromStdString(asset.name);
}
QString mediaLayerDisplayName(const CompositionSession& session, document::LayerId id) {
    const auto* composition = session.composition();
    const auto* layer = composition ? composition->graph().findLayer(id) : nullptr;
    const auto* source = directSourceNode(session, id);
    if (layer && source &&
        (source->typeId == "bloom.image-source" || source->typeId == "bloom.video-source" ||
         source->typeId == document::kAudioSourceNodeType)) {
        for (const auto& binding : source->parameters) {
            if (binding.role != "asset")
                continue;
            const auto value = session.constantStringValue(binding.parameterId);
            const auto* asset = value ? session.snapshot().project().findAsset(
                                            document::AssetId::fromRaw(value->toULongLong()))
                                      : nullptr;
            if (!asset)
                break;
            const auto importedName =
                asset->kind == document::AssetKind::Sequence
                    ? asset->manifest.pattern
                    : asset->locator.path.substr(asset->locator.path.find_last_of('/') + 1);
            // Existing import commands store the locator-derived label on the Layer. Project
            // organization changes its displayed default; an authored Layer name stays independent.
            if (layer->name == importedName || layer->name == asset->locator.path)
                return imageAssetDisplayName(*asset);
        }
    }
    return composition ? layerName(*composition, id) : QString{};
}
void refreshAssetSelector(kit::KDropdown& selector, const CompositionSession& session,
                          const QString& stored, const bool audioOnly,
                          const bool videoOnly = false) {
    const QSignalBlocker blocker(&selector);
    selector.clearItems();
    selector.addItem(QObject::tr("Choose Asset"), QString{});
    for (const auto& asset : session.snapshot().project().assets()) {
        if (asset.kind == document::AssetKind::Lut)
            continue;
        if (videoOnly != (asset.kind == document::AssetKind::Video) ||
            audioOnly != (asset.kind == document::AssetKind::Audio))
            continue;
        const bool sequence = asset.kind == document::AssetKind::Sequence;
        const auto kind =
            videoOnly   ? QObject::tr("Video")
            : audioOnly ? QObject::tr("Audio · %1 s").arg(asset.duration.toSeconds(), 0, 'f', 2)
            : sequence  ? QObject::tr("Sequence [%1]").arg(asset.manifest.members.size())
                        : QObject::tr("Image");
        selector.addItem(kit::icon(audioOnly  ? kit::IconId::Audio
                                   : sequence ? kit::IconId::Images
                                              : kit::IconId::Image,
                                   kit::IconRole::Chrome, kit::Color::Muted),
                         imageAssetDisplayName(asset) + " · " + kind,
                         QString::number(asset.id.value()));
    }
    auto index = selector.findData(stored);
    const bool missing = index < 0 && !stored.isEmpty();
    if (missing) {
        index = selector.addItem(QObject::tr("Missing asset"), stored);
        selector.setItemToolTip(index, stored);
    }
    selector.setCurrentIndex(index < 0 ? 0 : index);
    selector.setMutedValue(missing);
    selector.setWidthFloor(kit::px(kit::Size::PropertiesDropdownWidth));
    selector.setToolTip(missing ? QObject::tr("Missing asset: %1").arg(stored)
                                : selector.currentText());
}
void refreshImageAssetSelector(kit::KDropdown& selector, const CompositionSession& session,
                               const QString& stored) {
    refreshAssetSelector(selector, session, stored, false);
}
void refreshVideoAssetSelector(kit::KDropdown& selector, const CompositionSession& session,
                               const QString& stored) {
    refreshAssetSelector(selector, session, stored, false, true);
}
void refreshAudioAssetSelector(kit::KDropdown& selector, const CompositionSession& session,
                               const QString& stored) {
    refreshAssetSelector(selector, session, stored, true);
}

namespace {
[[nodiscard]] QString boundNodeText(const CompositionSession& session,
                                    const document::NodeRecord& node, const std::string_view role) {
    for (const auto& binding : node.parameters)
        if (binding.role == role)
            return session.constantStringValue(binding.parameterId).value_or(QString{});
    return {};
}
// The probed parts of the node's asset, requesting them when unknown.
[[nodiscard]] const std::vector<media::ImagePartInfo>* probedParts(const CompositionSession& session,
                                                                   const document::NodeRecord& node) {
    auto* controller = session.assetController();
    const auto text = boundNodeText(session, node, "asset");
    if (controller == nullptr || text.isEmpty())
        return nullptr;
    const auto asset = document::AssetId::fromRaw(text.toULongLong());
    const auto* parts = controller->imageParts(asset);
    if (parts == nullptr)
        controller->requestImageParts(asset);
    return parts;
}
[[nodiscard]] const media::ImagePartInfo* selectedPart(const std::vector<media::ImagePartInfo>& parts,
                                                       const QString& name) {
    if (name.isEmpty())
        return parts.empty() ? nullptr : &parts.front();
    const auto found = std::ranges::find(parts, name.toStdString(), &media::ImagePartInfo::name);
    return found == parts.end() ? nullptr : &*found;
}
// Fills `selector` with (label, value) items and selects `current`, adding "(missing)" for a
// value the probe does not list.
bool fillProbeSelector(kit::KDropdown& selector, const QList<std::pair<QString, QString>>& items,
                       const QString& current, const bool known, const QString& pending) {
    const QSignalBlocker blocker(selector);
    selector.clearItems();
    int index = -1;
    for (const auto& [label, value] : items) {
        selector.addItem(label, value);
        if (value == current)
            index = selector.count() - 1;
    }
    const bool listed = index >= 0;
    if (!listed) {
        selector.addItem(known ? QObject::tr("%1 (missing)").arg(current)
                               : (current.isEmpty() ? pending : current),
                         current);
        index = selector.count() - 1;
    }
    selector.setCurrentIndex(index);
    selector.setEnabled(known && (items.size() > 1 || !listed));
    selector.setMutedValue(known && !listed);
    selector.setToolTip(known ? QString{} : QObject::tr("Reading the file…"));
    return listed;
}
} // namespace

bool refreshImagePartSelector(kit::KDropdown& selector, const CompositionSession& session,
                              const document::NodeRecord& node) {
    const auto* parts = probedParts(session, node);
    QList<std::pair<QString, QString>> items{{QObject::tr("First part"), QString{}}};
    if (parts != nullptr)
        for (std::size_t index = 1; index < parts->size(); ++index)
            if (!(*parts)[index].name.empty())
                items.push_back({QString::fromStdString((*parts)[index].name),
                                 QString::fromStdString((*parts)[index].name)});
    return fillProbeSelector(selector, items,
                             boundNodeText(session, node, document::kImagePartParameterRole),
                             parts != nullptr, QObject::tr("First part"));
}

bool refreshImageLayerSelector(kit::KDropdown& selector, const CompositionSession& session,
                               const document::NodeRecord& node) {
    const auto* parts = probedParts(session, node);
    const auto* part =
        parts == nullptr
            ? nullptr
            : selectedPart(*parts, boundNodeText(session, node, document::kImagePartParameterRole));
    QList<std::pair<QString, QString>> items;
    if (part != nullptr)
        for (const auto& layer : part->colourLayers)
            items.push_back({layer.empty() ? QObject::tr("Color (root)")
                                           : QString::fromStdString(layer),
                             QString::fromStdString(layer)});
    auto current = boundNodeText(session, node, document::kImageLayerParameterRole);
    if (part != nullptr)
        current = QString::fromStdString(part->resolveColourLayer(current.toStdString()));
    if (part != nullptr && items.empty()) {
        // A data-only part: nothing to choose; the note says what to use instead.
        const QSignalBlocker blocker(selector);
        selector.clearItems();
        selector.addItem(QObject::tr("No color layer"), current);
        selector.setCurrentIndex(0);
        selector.setEnabled(false);
        selector.setMutedValue(true);
        selector.setToolTip(QObject::tr("No color layer — use Channels"));
        return true;
    }
    return fillProbeSelector(selector, items, current, part != nullptr,
                             QObject::tr("Color (root)"));
}

bool refreshCryptomatteLayerSelector(kit::KDropdown& selector, const CompositionSession& session,
                                     const document::NodeRecord& node) {
    auto* controller = session.cryptomatteController();
    const auto* layers = controller != nullptr ? controller->layers(node.id) : nullptr;
    if (controller != nullptr && !controller->listingCurrent(node.id))
        controller->requestLayers(node.id);
    QList<std::pair<QString, QString>> items;
    if (layers != nullptr)
        for (const auto& layer : *layers)
            items.push_back({QString::fromStdString(layer), QString::fromStdString(layer)});
    auto current = boundNodeText(session, node, document::kCryptomatteLayerParameterRole);
    // #487: an empty (default) layer reads the first Cryptomatte layer present; show that one.
    if (current.isEmpty() && layers != nullptr && !layers->empty())
        current = QString::fromStdString(layers->front());
    const bool listed = fillProbeSelector(selector, items, current, layers != nullptr,
                                          current.isEmpty() ? QObject::tr("First layer") : current);
    if (!listed && current.isEmpty() && layers != nullptr)
        selector.setItemText(selector.count() - 1, QObject::tr("No Cryptomatte layer"));
    if (layers != nullptr && layers->empty())
        selector.setToolTip(QObject::tr("The channel set has no Cryptomatte layer"));
    return listed;
}

bool isShuffleSlotSchema(const std::string_view schemaKey) noexcept {
    return schemaKey == "bloom.channel-shuffle.red" || schemaKey == "bloom.channel-shuffle.green" ||
           schemaKey == "bloom.channel-shuffle.blue" || schemaKey == "bloom.channel-shuffle.alpha" ||
           schemaKey == document::kChannelPassParameterSchemaKey ||
           schemaKey == "bloom.z-defocus.depthChannel" || schemaKey == "bloom.depth-fog.depthChannel" ||
           schemaKey == "bloom.vector-motion-blur.depthChannel" ||
           schemaKey == "bloom.vector-motion-blur.vectorChannels";
}

namespace {
// #480: the Pass picker offers the input's layers and channels; empty reads the first data layer.
bool refreshPassSelector(kit::KDropdown& selector, const CompositionSession& session,
                         const document::NodeRecord& node,
                         const runtime::ChannelInspectionListing* listing) {
    QList<std::pair<QString, QString>> items{{QObject::tr("First data layer"), QString{}}};
    if (listing != nullptr) {
        for (const auto& layer : listing->layers)
            if (!layer.empty())
                items.push_back({QObject::tr("%1 (layer)").arg(QString::fromStdString(layer)),
                                 QString::fromStdString(layer)});
        for (const auto& channel : listing->channels)
            items.push_back({QString::fromStdString(channel), QString::fromStdString(channel)});
    }
    const bool listed =
        fillProbeSelector(selector, items,
                          boundNodeText(session, node, document::kChannelPassParameterRole),
                          listing != nullptr, QObject::tr("First data layer"));
    if (listing != nullptr && listing->channels.empty() && !listing->diagnostic.empty())
        selector.setToolTip(QString::fromStdString(listing->diagnostic));
    return listed;
}
// #483: a depth effect's depth channel: R, G, B or A of an Image or Mask, or any channel of a
// channel set by name; empty reads the first channel (never a mix of the colour channels).
bool refreshDepthChannelSelector(kit::KDropdown& selector, const CompositionSession& session,
                                 const document::NodeRecord& node) {
    QList<std::pair<QString, QString>> items{{QObject::tr("First channel"), QString{}}};
    const auto* composition = session.composition();
    const auto source = composition != nullptr
                            ? document::inspectedInputSource(composition->graph(), node.id,
                                                             document::kDepthEffectDepthPort)
                            : std::nullopt;
    const bool channelSet = source && source->kind == document::SocketValueKind::ChannelSet;
    const runtime::ChannelInspectionListing* listing = nullptr;
    if (channelSet) {
        auto* controller = session.channelSetController();
        if (controller != nullptr && !controller->listingCurrent(node.id, document::kDepthEffectDepthPort))
            controller->request(node.id, document::kDepthEffectDepthPort);
        listing = controller != nullptr ? controller->listing(node.id, document::kDepthEffectDepthPort)
                                        : nullptr;
        if (listing != nullptr)
            for (const auto& channel : listing->channels)
                items.push_back({QString::fromStdString(channel), QString::fromStdString(channel)});
    } else {
        for (const auto* name : {"R", "G", "B", "A"})
            items.push_back({QString::fromLatin1(name), QString::fromLatin1(name)});
    }
    const bool known = !channelSet || listing != nullptr;
    const bool listed = fillProbeSelector(selector, items, boundNodeText(session, node, "depthChannel"),
                                          known, QObject::tr("First channel"));
    if (listing != nullptr && listing->channels.empty() && !listing->diagnostic.empty())
        selector.setToolTip(QString::fromStdString(listing->diagnostic));
    else
        selector.setToolTip(QObject::tr("The one channel the depth is read from: R, G, B or A of "
                                        "an image, or a channel set's channel by name (Z, "
                                        "depth.Z, mist). Never a mix of the colour channels"));
    return listed;
}
// #484: Vector Motion Blur's vectors: a layer of its channel set; empty finds the convention's or a
// usual vector layer. An image's vectors are its own channels, so there is nothing to pick.
bool refreshVectorChannelsSelector(kit::KDropdown& selector, const CompositionSession& session,
                                   const document::NodeRecord& node) {
    QList<std::pair<QString, QString>> items{{QObject::tr("Find the vector layer"), QString{}}};
    const auto* composition = session.composition();
    const auto source = composition != nullptr
                            ? document::inspectedInputSource(composition->graph(), node.id,
                                                             document::kMotionVectorsPort)
                            : std::nullopt;
    const bool channelSet = source && source->kind == document::SocketValueKind::ChannelSet;
    const runtime::ChannelInspectionListing* listing = nullptr;
    if (channelSet) {
        auto* controller = session.channelSetController();
        if (controller != nullptr && !controller->listingCurrent(node.id, document::kMotionVectorsPort))
            controller->request(node.id, document::kMotionVectorsPort);
        listing = controller != nullptr ? controller->listing(node.id, document::kMotionVectorsPort)
                                        : nullptr;
        if (listing != nullptr)
            for (const auto& layer : listing->layers)
                if (!layer.empty())
                    items.push_back({QString::fromStdString(layer), QString::fromStdString(layer)});
    }
    const bool listed =
        fillProbeSelector(selector, items, boundNodeText(session, node, "vectorChannels"),
                          !channelSet || listing != nullptr, QObject::tr("Find the vector layer"));
    if (listing != nullptr && listing->channels.empty() && !listing->diagnostic.empty())
        selector.setToolTip(QString::fromStdString(listing->diagnostic));
    else
        selector.setToolTip(QObject::tr("The channel set's layer the vectors are read from"));
    return listed;
}
} // namespace

bool refreshShuffleSlotSelector(kit::KDropdown& selector, const CompositionSession& session,
                                const document::NodeRecord& node,
                                const std::string_view schemaKey) {
    const auto role = schemaKey.substr(schemaKey.rfind('.') + 1);
    if (role == "depthChannel")
        return refreshDepthChannelSelector(selector, session, node);
    if (role == "vectorChannels")
        return refreshVectorChannelsSelector(selector, session, node);
    const std::size_t slot = role == "red" ? 0 : role == "green" ? 1 : role == "blue" ? 2 : 3;
    auto* controller = session.channelSetController();
    if (controller != nullptr && !controller->listingCurrent(node.id))
        controller->request(node.id);
    const auto* listing = controller != nullptr ? controller->listing(node.id) : nullptr;
    if (schemaKey == document::kChannelPassParameterSchemaKey)
        return refreshPassSelector(selector, session, node, listing);
    QList<std::pair<QString, QString>> items;
    if (listing != nullptr)
        for (const auto& channel : listing->channels)
            items.push_back({QString::fromStdString(channel), QString::fromStdString(channel)});
    items.push_back({QObject::tr("0 (constant)"), QStringLiteral("0")});
    items.push_back({QObject::tr("1 (constant)"), QStringLiteral("1")});
    const auto stored = boundNodeText(session, node, role);
    auto current = stored;
    // An empty (default) slot reads the default colour layer's channel: show that channel.
    if (stored.isEmpty() && listing != nullptr)
        if (const auto channel = runtime::defaultShuffleSlotChannel(listing->channels, slot))
            current = QString::fromStdString(*channel);
    const bool listed =
        fillProbeSelector(selector, items, current, listing != nullptr,
                          current.isEmpty() ? QObject::tr("Default color") : current);
    if (!listed && current.isEmpty() && listing != nullptr)
        selector.setItemText(selector.count() - 1, QObject::tr("No color layer"));
    if (listing != nullptr && listing->channels.empty() && !listing->diagnostic.empty())
        selector.setToolTip(QString::fromStdString(listing->diagnostic));
    return listed;
}

bool refreshDataBlockSelector(kit::KDropdown& selector, const CompositionSession& session,
                              const document::NodeRecord& node) {
    const QSignalBlocker blocker(selector);
    selector.clearItems();
    const auto* descriptor = document::findValueUtilityDescriptor(node.typeId);
    const auto kind =
        descriptor != nullptr ? document::dataBlockReaderKind(descriptor->kernel) : std::nullopt;
    std::int64_t stored = 0;
    for (const auto& binding : node.parameters)
        if (binding.role == document::kDataBlockParameterRole)
            if (const auto* composition = session.composition())
                if (const auto* parameter = composition->parameters().find(binding.parameterId))
                    if (const auto* constant =
                            std::get_if<document::ConstantValueSource>(&parameter->source))
                        if (const auto* value = std::get_if<std::int64_t>(&constant->value))
                            stored = *value;
    const auto& project = session.snapshot().project();
    const auto kindName = kind ? document::dataBlockKindName(*kind) : std::string_view{};
    const auto kindText =
        QString::fromUtf8(kindName.data(), static_cast<qsizetype>(kindName.size()));
    if (kind)
        for (const auto& block : project.typedDataBlocks()) {
            const auto* id = std::get_if<document::DataBlockRecordId>(&block.id);
            if (id == nullptr || block.kind != *kind ||
                id->value() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
                continue;
            const auto index = selector.addItem(
                QStringLiteral("%1 · %2").arg(QString::fromStdString(block.typeId),
                                              QString::fromStdString(block.provenance.createdAt)),
                QVariant::fromValue(static_cast<std::int64_t>(id->value())));
            selector.setItemToolTip(index,
                                    QObject::tr("%1 data block %2").arg(kindText).arg(id->value()));
        }
    const auto* resolved =
        descriptor != nullptr
            ? document::resolveDataBlockReaderBlock(project, descriptor->kernel, stored)
            : nullptr;
    const bool none = selector.count() == 0;
    int index = -1;
    if (resolved != nullptr)
        if (const auto* id = std::get_if<document::DataBlockRecordId>(&resolved->id))
            index = selector.findData(QVariant::fromValue(static_cast<std::int64_t>(id->value())));
    const bool missing = index < 0 && stored > 0;
    if (index < 0)
        index = selector.addItem(missing ? QObject::tr("Missing data block")
                                         : QObject::tr("No %1 data in the project").arg(kindText),
                                 QVariant::fromValue(stored));
    selector.setCurrentIndex(index);
    selector.setMutedValue(index < 0 || missing || none);
    selector.setToolTip(
        none ? QObject::tr("Add a %1 data block to the project to read it").arg(kindText)
             : selector.currentText());
    return !missing && !none;
}

bool refreshPathSourceSelector(kit::KDropdown& selector, const CompositionSession& session,
                               const document::NodeRecord& node) {
    const QSignalBlocker blocker(selector);
    selector.clearItems();
    const auto* descriptor = document::findValueUtilityDescriptor(node.typeId);
    const auto* composition = session.composition();
    if (descriptor == nullptr || composition == nullptr ||
        !document::isPathSourceReader(descriptor->kernel))
        return false;
    const auto kernel = descriptor->kernel;
    const bool masks = kernel == document::ValueUtilityKernel::MaskPath;
    std::int64_t stored = 0;
    for (const auto& binding : node.parameters)
        if (binding.role == document::kPathSourceParameterRole)
            if (const auto* parameter = composition->parameters().find(binding.parameterId))
                if (const auto* constant =
                        std::get_if<document::ConstantValueSource>(&parameter->source))
                    if (const auto* value = std::get_if<std::int64_t>(&constant->value))
                        stored = *value;
    const auto& graph = composition->graph();
    for (const auto& candidate : graph.nodes()) {
        if (!document::isPathSourceCandidate(graph, kernel, candidate) ||
            candidate.id.value() >
                static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            continue;
        QString label;
        if (candidate.typeId == document::kTextSourceNodeType) {
            // A text is named by what it says (a driven text by its node id).
            const auto binding =
                std::ranges::find(candidate.parameters, document::kTextParameterRole,
                                  &document::ParameterBinding::role);
            const auto content = binding == candidate.parameters.end()
                                     ? std::nullopt
                                     : session.constantStringValue(binding->parameterId);
            auto text = content.value_or(QString{}).simplified();
            if (text.size() > 24)
                text = text.left(23) + QChar(0x2026);
            label = text.isEmpty() ? QObject::tr("Text %1").arg(candidate.id.value())
                                   : QObject::tr("Text \u201c%1\u201d").arg(text);
        } else if (candidate.typeId == document::kMaskNodeType) {
            label = QObject::tr("Mask node %1").arg(candidate.id.value());
        } else {
            label =
                QObject::tr("%1 masks").arg(node_editor::nodeDisplayName(*composition, candidate));
        }
        const auto index = selector.addItem(
            label, QVariant::fromValue(static_cast<std::int64_t>(candidate.id.value())));
        selector.setItemToolTip(index, label);
    }
    const bool none = selector.count() == 0;
    const auto* resolved = document::resolvePathSourceNode(graph, kernel, stored);
    int index = resolved == nullptr ? -1
                                    : selector.findData(QVariant::fromValue(
                                          static_cast<std::int64_t>(resolved->id.value())));
    const bool missing = index < 0 && stored > 0;
    if (index < 0)
        index = selector.addItem(missing ? QObject::tr("Missing source")
                                 : masks ? QObject::tr("No masked layer or Mask node")
                                         : QObject::tr("No text in the composition"),
                                 QVariant::fromValue(stored));
    selector.setCurrentIndex(index);
    selector.setMutedValue(missing || none);
    selector.setToolTip(none ? (masks ? QObject::tr("Add a mask to a layer, or a Mask node, to "
                                                    "read its outline")
                                      : QObject::tr("Add a text to read its glyph outlines"))
                             : selector.currentText());
    return !missing && !none;
}

void refreshAudioStreamSelector(kit::KDropdown& selector, const CompositionSession& session,
                                const document::NodeRecord& node) {
    const document::AssetRecord* asset = nullptr;
    std::uint32_t selected = 0;
    for (const auto& binding : node.parameters) {
        if (binding.role == "asset")
            if (const auto value = session.constantStringValue(binding.parameterId))
                asset = session.snapshot().project().findAsset(
                    document::AssetId::fromRaw(value->toULongLong()));
        if (binding.role == document::kVideoAudioStreamParameterRole)
            if (const auto value = session.constantValue(binding.parameterId))
                selected = static_cast<std::uint32_t>(*value);
    }
    const QSignalBlocker blocker(selector);
    selector.clearItems();
    std::uint32_t ordinal = 0;
    if (asset != nullptr)
        for (const auto& stream : asset->videoStreams) {
            if (stream.kind != 2)
                continue;
            selector.addItem(QObject::tr("Stream %1 — %2 ch, %3 Hz (%4)")
                                 .arg(ordinal + 1)
                                 .arg(stream.channelLayout.size())
                                 .arg(stream.sampleRate)
                                 .arg(QString::fromStdString(stream.codec)),
                             QVariant::fromValue(ordinal));
            ++ordinal;
        }
    if (ordinal == 0)
        selector.addItem(QObject::tr("No audio stream"), QVariant::fromValue(0U));
    selector.setCurrentIndex(static_cast<int>(std::min(selected, ordinal == 0 ? 0 : ordinal - 1)));
    const auto* composition = session.composition();
    selector.setEnabled(ordinal > 1 && composition != nullptr && !composition->nodeLocked(node.id));
}

bool commitAudioStream(CompositionSession& session, const document::NodeId node,
                       const std::uint32_t ordinal) {
    commands::Transaction transaction(QObject::tr("Choose Audio Stream").toStdString(),
                                      session.snapshot().revision());
    transaction.emplace<commands::SetVideoAudioStream>(session.compositionId(), node, ordinal);
    return session.executeTransaction(std::move(transaction)).succeeded();
}

QString imageSourceColourNote(const CompositionSession& session, const document::NodeRecord& node) {
    auto* controller = session.assetController();
    const auto text = boundNodeText(session, node, "asset");
    if (controller == nullptr || text.isEmpty())
        return {};
    const auto* parts = controller->imageParts(document::AssetId::fromRaw(text.toULongLong()));
    const auto* part =
        parts == nullptr
            ? nullptr
            : selectedPart(*parts, boundNodeText(session, node, document::kImagePartParameterRole));
    const auto layer =
        part == nullptr
            ? std::string{}
            : part->resolveColourLayer(
                  boundNodeText(session, node, document::kImageLayerParameterRole).toStdString());
    return part != nullptr && std::ranges::find(part->colourLayers, layer) == part->colourLayers.end()
               ? QObject::tr("No color layer — use Channels")
               : QString{};
}

PropertiesRowControl propertiesRowControl(std::string_view schemaKey) {
    if (schemaKey == document::kTextAlignmentParameterSchemaKey)
        return PropertiesRowControl::SegmentedEnum;
    if (schemaKey == document::kInnerGlowSourceSchemaKey)
        return PropertiesRowControl::Radio;
    if (schemaKey == document::kTextSizeParameterSchemaKey ||
        schemaKey == document::kTextLineHeightParameterSchemaKey ||
        schemaKey == document::kTextLetterSpacingParameterSchemaKey)
        return PropertiesRowControl::Stepper;
    return PropertiesRowControl::Automatic;
}

// The labels come from the document's one enum table (document::parameterEnumOptions), the same
// members scripting, MCP and the CLI accept by name; only the translation is applied here, at
// display time. Properties, the Nodes cards and the Timeline rows cannot offer a member the table
// does not have, nor spell one differently.
QList<std::pair<QString, std::int64_t>> propertiesSelectorItems(std::string_view schemaKey) {
    QList<std::pair<QString, std::int64_t>> items;
    for (const auto& option : document::parameterEnumOptions(schemaKey))
        items.append({QCoreApplication::translate("PropertiesSelector", option.label.c_str()),
                      option.value});
    return items;
}


bool isEffectColorSpaceSchema(const std::string_view schema) {
    return schema == "bloom.ocio-cst.from" || schema == "bloom.ocio-cst.to" ||
           schema == "bloom.ocio-file.process-space";
}

namespace {
// #487: the source input colour space control's two non-OCIO choices. Neither can be an OCIO id
// (a config's colour space names never start with a control character).
const QString kRawInputChoice = QStringLiteral("\u0001raw");
const QString kLegacyInputChoicePrefix = QStringLiteral("\u0001legacy:");
} // namespace

bool isSourceInputColorSpaceSchema(const std::string_view schema) noexcept {
    return schema == document::kImageInputColorSpaceIdParameterSchemaKey ||
           schema == document::kVideoInputColorSpaceIdParameterSchemaKey;
}

bool isInputColorSpaceSchema(const std::string_view schema) {
    return isEffectColorSpaceSchema(schema) ||
           schema == document::kImageInputColorSpaceIdParameterSchemaKey ||
           schema == document::kVideoInputColorSpaceIdParameterSchemaKey;
}

namespace {
[[nodiscard]] std::optional<color::ResolvedBloomNeutralConfig>
inputColorConfig(const CompositionSession& session) {
    const auto& settings = session.colorSettings();
    const auto* builtIn =
        std::get_if<document::BuiltInOcioConfigLocator>(&settings.ocioConfig.locator);
    if (builtIn == nullptr ||
        settings.ocioConfig.expectedRevision.algorithm != document::OcioRevisionAlgorithm::Sha256)
        return {};
    auto result = color::resolveOcioBuiltIn(
        color::OcioConfigLocatorKind::BloomBuiltIn, builtIn->uri,
        settings.ocioConfig.expectedRevision.digest, session.colorIntent().workingColorSpaceId);
    if (!result.ready())
        return {};
    return std::move(result).takeResolved();
}

[[nodiscard]] QString sourceAssetInputColorSpace(const CompositionSession& session,
                                                 const document::NodeRecord* node) {
    if (node == nullptr)
        return {};
    for (const auto& binding : node->parameters) {
        if (binding.role != "asset")
            continue;
        const auto stored = session.constantStringValue(binding.parameterId);
        if (!stored)
            return {};
        bool numeric = false;
        const auto raw = stored->toULongLong(&numeric);
        if (!numeric)
            return {};
        const auto* asset = session.snapshot().project().findAsset(
            document::AssetId::fromRaw(static_cast<std::uint64_t>(raw)));
        if (asset == nullptr)
            return {};
        if (const auto* controller = session.assetController())
            return controller->inputColorSpaceDisplay(asset->id);
        return asset->interpretation.inputColorSpaceId.empty()
                   ? QString{}
                   : QString::fromStdString(asset->interpretation.inputColorSpaceId);
    }
    return {};
}

[[nodiscard]] std::int64_t sourceLegacyInputColorSpace(const CompositionSession& session,
                                                       const document::NodeRecord* node) {
    const auto* composition = session.composition();
    if (!node || !composition)
        return 0;
    for (const auto& binding : node->parameters) {
        if (binding.role != "colorSpace")
            continue;
        const auto* parameter = composition->parameters().find(binding.parameterId);
        if (!parameter)
            return 0;
        const auto* constant = std::get_if<document::ConstantValueSource>(&parameter->source);
        if (!constant)
            return 0;
        const auto* value = std::get_if<std::int64_t>(&constant->value);
        return value ? *value : 0;
    }
    return 0;
}

} // namespace

void populateColorSpaceSelector(kit::KDropdown& selector, const CompositionSession& session,
                                const std::string_view schemaKey, const QString& current) {
    const QSignalBlocker blocker(selector);
    selector.setSearchable(true);
    selector.clearItems();
    const bool effect = isEffectColorSpaceSchema(schemaKey);
    const auto automatic = selector.addItem(effect ? QObject::tr("Working space")
                                                   : QObject::tr("Auto"),
                                            QString{});
    selector.setItemToolTip(automatic, effect ? QObject::tr("Use the composition working color space")
                                              : QObject::tr("Inherit the asset's automatic interpretation"));
    if (!effect) {
        // #487: the legacy Raw choice lives in this one control.
        const auto raw = selector.addItem(QObject::tr("Raw (no conversion)"), kRawInputChoice);
        selector.setItemToolTip(raw, QObject::tr("Use the file's values as they are, with no "
                                                 "color conversion"));
    }
    const auto config = inputColorConfig(session);
    if (!config) {
        const auto unavailable = selector.addItem(QObject::tr("OCIO config unavailable"), QString{});
        selector.setItemEnabled(unavailable, false);
    } else {
        auto spaces = config->colorSpaces();
        std::ranges::sort(spaces, [](const auto& left, const auto& right) {
            return std::tie(left.family, left.id) < std::tie(right.family, right.id);
        });
        for (const auto& space : spaces) {
            const auto family = QString::fromStdString(space.family);
            const auto name = QString::fromStdString(space.id);
            const auto label = family.isEmpty() ? name : family + QStringLiteral(" / ") + name;
            const auto index = selector.addItem(label, name);
            selector.setItemToolTip(index, QObject::tr("OCIO color space id: %1").arg(name));
        }
    }
    auto index = selector.findData(current);
    if (index < 0 && !current.isEmpty()) {
        index = selector.addItem(QObject::tr("Missing config space"), current);
        selector.setItemToolTip(index, current);
    }
    selector.setCurrentIndex(index < 0 ? automatic : index);
    selector.setWidthFloor(kit::px(kit::Size::PropertiesDropdownWidth));
}

void refreshColorSpaceSelector(kit::KDropdown& selector, const CompositionSession& session,
                               const document::NodeRecord* node, const std::string_view schemaKey,
                               const QString& stored) {
    const QSignalBlocker blocker(selector);
    const bool effect = isEffectColorSpaceSchema(schemaKey);
    const auto legacy = effect ? std::int64_t{0} : sourceLegacyInputColorSpace(session, node);
    auto automatic = effect ? QObject::tr("Working space") : QObject::tr("Auto");
    if (!effect && stored.isEmpty() && legacy == 0) {
        const auto resolved = sourceAssetInputColorSpace(session, node);
        if (!resolved.isEmpty())
            automatic = resolved;
    }
    selector.setItemText(0, automatic);
    selector.setItemToolTip(0, effect ? QObject::tr("Composition working color space: %1")
                                            .arg(QString::fromUtf8(host::workingColorSpaceLabel(
                                                session.colorIntent().workingColorSpaceId)))
                                      : QObject::tr("Inherit the asset's input interpretation"));
    auto index = selector.findData(stored);
    if (!effect && stored.isEmpty() && legacy == 3)
        index = selector.findData(kRawInputChoice);
    else if (!effect && stored.isEmpty() && legacy != 0) {
        // A legacy sRGB or Linear choice load could not migrate (its config's sRGB texture space
        // is unknown without reading the config): shown, never offered.
        const auto data = kLegacyInputChoicePrefix + QString::number(legacy);
        index = selector.findData(data);
        if (index < 0) {
            index = selector.addItem(legacy == 1 ? QObject::tr("sRGB (legacy)")
                                                 : QObject::tr("Working space (legacy)"),
                                     data);
            selector.setItemToolTip(index, QObject::tr("A legacy source Color Space choice; "
                                                       "choose a color space to replace it"));
        }
    }
    if (index >= 0)
        selector.setCurrentIndex(index);
}

bool commitSourceInputColorSpace(CompositionSession& session, const document::NodeId nodeId,
                                 const QString& choice) {
    const auto* composition = session.composition();
    const auto* node = composition != nullptr ? composition->graph().findNode(nodeId) : nullptr;
    if (node == nullptr || choice.startsWith(kLegacyInputChoicePrefix))
        return node != nullptr;
    const bool raw = choice == kRawInputChoice;
    const auto targetId = raw ? std::string{} : choice.toStdString();
    const std::int64_t targetLegacy = raw ? 3 : 0;
    commands::Transaction transaction(QObject::tr("Set Input Color Space").toStdString(),
                                      session.snapshot().revision());
    for (const auto& binding : node->parameters) {
        if (binding.role == "inputColorSpaceId" &&
            session.constantStringValue(binding.parameterId).value_or(QString{}).toStdString() !=
                targetId)
            transaction.emplace<commands::SetNodeParameterValue>(
                session.compositionId(), nodeId, binding.role, document::ParameterValue{targetId});
        if (binding.role == "colorSpace" &&
            sourceLegacyInputColorSpace(session, node) != targetLegacy)
            transaction.emplace<commands::SetNodeParameterValue>(
                session.compositionId(), nodeId, binding.role,
                document::ParameterValue{targetLegacy});
    }
    return transaction.empty() || session.executeTransaction(std::move(transaction)).succeeded();
}

void refreshLutSelector(kit::KDropdown& selector, const CompositionSession& session,
                        const std::int64_t current) {
    const QSignalBlocker blocker(selector);
    selector.clearItems();
    selector.addItem(QObject::tr("Choose LUT"), QVariant::fromValue(std::int64_t{0}));
    for (const auto& asset : session.snapshot().project().assets())
        if (asset.kind == document::AssetKind::Lut &&
            asset.id.value() <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            selector.addItem(QString::fromStdString(asset.name),
                             QVariant::fromValue(static_cast<std::int64_t>(asset.id.value())));
    if (current != 0 && selector.findData(QVariant::fromValue(current)) < 0)
        selector.addItem(QObject::tr("Missing LUT"), QVariant::fromValue(current));
    const auto index = selector.findData(QVariant::fromValue(current));
    if (index >= 0)
        selector.setCurrentIndex(index);
}

} // namespace bloom::ui
