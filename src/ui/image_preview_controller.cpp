#include <bloom/ui/image_preview_controller.hpp>

#include <bloom/document/project.hpp>
#include <bloom/render/image.hpp>
#include <bloom/runtime/prepared_preview_frame.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/task_ui_bridge.hpp>
#include <bloom/ui/viewer_channel_display.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace bloom::ui {
namespace {

using document::ImagePreviewDisplay;

// The smallest whole divisor of the composition that fits `edge` device pixels on its longest
// side: the same rounding the Viewer's reduced resolutions and the Assets thumbnails use, so a
// zoom only re-requests when the divisor actually changes, and never more than full resolution.
[[nodiscard]] std::optional<render::ImageExtent>
previewExtent(const document::CompositionFormat& format, const int edge) {
    const auto longest = std::max(format.width(), format.height());
    const auto wanted =
        static_cast<std::uint32_t>(std::max(edge, ImagePreviewController::kMinimumEdge));
    const auto divisor = std::max<std::uint32_t>(1, longest / std::max<std::uint32_t>(1, wanted));
    const auto extent = render::ImageExtent::create((format.width() + divisor - 1) / divisor,
                                                    (format.height() + divisor - 1) / divisor);
    return extent ? std::optional(*extent.value()) : std::nullopt;
}

[[nodiscard]] bool gpuProvider(const runtime::PreviewDisplayProvider provider) noexcept {
    return provider == runtime::PreviewDisplayProvider::GpuPreparedPacked ||
           provider == runtime::PreviewDisplayProvider::GpuResident ||
           provider == runtime::PreviewDisplayProvider::GpuNeutral;
}

[[nodiscard]] std::optional<ViewerChannel> viewerChannel(const ImagePreviewDisplay display) {
    switch (display) {
    case ImagePreviewDisplay::Rgba:
        return ViewerChannel::Rgba;
    case ImagePreviewDisplay::Rgb:
        return ViewerChannel::Rgb;
    case ImagePreviewDisplay::Alpha:
        return ViewerChannel::Alpha;
    case ImagePreviewDisplay::Red:
        return ViewerChannel::Red;
    case ImagePreviewDisplay::Green:
        return ViewerChannel::Green;
    case ImagePreviewDisplay::Blue:
        return ViewerChannel::Blue;
    case ImagePreviewDisplay::Normalized:
        break;
    }
    return std::nullopt;
}

[[nodiscard]] runtime::TaskDiagnostic unavailable(std::string summary) {
    runtime::TaskDiagnostic diagnostic;
    diagnostic.code = "bloom.ui.image-preview.unavailable";
    diagnostic.severity = runtime::DiagnosticSeverity::Warning;
    diagnostic.summary = std::move(summary);
    return diagnostic;
}

// The packed display pixels of `frame` as an RGBA8 image, through the Viewer's own channel
// presentation (RGB, a single channel as grey) when one is asked for. Worker-only.
[[nodiscard]] std::optional<QImage>
displayImage(const runtime::PreparedPreviewFrame& frame,
             const std::shared_ptr<const runtime::PreparedPreviewFrame>& handle,
             const ViewerChannel channel, runtime::TaskContext& context) {
    const auto view = frame.displayBufferView();
    if (!view)
        return std::nullopt;
    const auto window = view->dataWindow.value_or(view->displayWindow).extent();
    if (window.width() > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        window.height() > static_cast<std::uint32_t>(std::numeric_limits<int>::max()))
        return std::nullopt;
    std::span<const render::Rgba8> pixels = view->pixels;
    std::shared_ptr<const ViewerChannelDisplay> presented;
    if (channel != ViewerChannel::Rgba) {
        auto prepared = prepareViewerChannelDisplay(
            handle, channel, std::numeric_limits<std::size_t>::max(), context);
        const auto& value = prepared.value();
        if (prepared.state() != runtime::TaskState::Succeeded || !value || !*value)
            return std::nullopt;
        presented = *value;
        pixels = presented->pixels;
    }
    const QImage borrowed(reinterpret_cast<const uchar*>(pixels.data()),
                          static_cast<int>(window.width()), static_cast<int>(window.height()),
                          static_cast<qsizetype>(view->layout.rowStrideBytes),
                          QImage::Format_RGBA8888);
    // Copied out of the frame here, on the worker, so the interface thread only paints.
    return borrowed.copy();
}

// #490 Normalized: the scene-linear RGB of the visible image stretched from its finite minimum to
// its maximum, so a data pass far outside [0, 1] (depth, mist, a position) reads as black..white.
// One range for the three channels, so their relative values survive. Worker-only.
[[nodiscard]] std::optional<QImage> normalizedImage(const runtime::PreparedPreviewFrame& frame,
                                                    runtime::TaskContext& context) {
    if (!frame.hasProcessFrame())
        return std::nullopt;
    const auto& image = frame.processImage();
    const auto* descriptor = image.descriptor();
    if (descriptor == nullptr)
        return std::nullopt;
    const auto display = descriptor->displayWindow();
    const auto data = descriptor->dataWindow();
    const auto width = static_cast<int>(display.extent().width());
    const auto height = static_cast<int>(display.extent().height());
    if (width <= 0 || height <= 0)
        return std::nullopt;
    const auto pixels = image.pixels();
    float low = std::numeric_limits<float>::infinity();
    float high = -std::numeric_limits<float>::infinity();
    for (std::size_t index = 0; index < pixels.size(); ++index) {
        if (index % 4096 == 0 && context.isCancellationRequested())
            return std::nullopt;
        for (const float value : {pixels[index].red(), pixels[index].green(), pixels[index].blue()})
            if (std::isfinite(value)) {
                low = std::min(low, value);
                high = std::max(high, value);
            }
    }
    // Opaque black outside the data window.
    QImage out(width, height, QImage::Format_RGBA8888);
    out.fill(0U);
    for (int row = 0; row < height; ++row) {
        auto* bytes = out.scanLine(row);
        for (int column = 0; column < width; ++column)
            bytes[4 * column + 3] = 255;
    }
    const float range = high > low ? high - low : 0.0F;
    const auto dataWidth = static_cast<std::int64_t>(data.extent().width());
    for (std::size_t index = 0; index < pixels.size(); ++index) {
        if (index % 4096 == 0 && context.isCancellationRequested())
            return std::nullopt;
        const auto x =
            data.originX() + static_cast<std::int64_t>(index) % dataWidth - display.originX();
        const auto y =
            data.originY() + static_cast<std::int64_t>(index) / dataWidth - display.originY();
        if (x < 0 || y < 0 || x >= width || y >= height)
            continue;
        const auto encode = [&](const float value) {
            if (!std::isfinite(value) || !(range > 0.0F))
                return 0;
            return static_cast<int>(
                std::lround(std::clamp((value - low) / range, 0.0F, 1.0F) * 255.0F));
        };
        auto* bytes = out.scanLine(static_cast<int>(y)) + 4 * x;
        bytes[0] = static_cast<uchar>(encode(pixels[index].red()));
        bytes[1] = static_cast<uchar>(encode(pixels[index].green()));
        bytes[2] = static_cast<uchar>(encode(pixels[index].blue()));
        bytes[3] = 255;
    }
    return out;
}

[[nodiscard]] std::optional<std::int64_t> constantInteger(const document::Composition& composition,
                                                          const document::NodeRecord& node,
                                                          const std::string_view role) {
    const auto binding =
        std::ranges::find(node.parameters, role, &document::ParameterBinding::role);
    if (binding == node.parameters.end())
        return std::nullopt;
    const auto* parameter = composition.parameters().find(binding->parameterId);
    const auto* constant =
        parameter ? std::get_if<document::ConstantValueSource>(&parameter->source) : nullptr;
    const auto* value = constant ? std::get_if<std::int64_t>(&constant->value) : nullptr;
    return value ? std::optional(*value) : std::nullopt;
}

} // namespace

ImagePreviewController::ImagePreviewController(CompositionSession& session,
                                               CompositionPreviewController& preview,
                                               runtime::TaskScheduler& scheduler,
                                               TaskUiBridge& bridge, QObject* parent)
    : QObject(parent), session_(session), preview_(preview), scheduler_(scheduler),
      bridge_(bridge) {
    connect(&bridge_, &TaskUiBridge::resultsAvailable, this, &ImagePreviewController::poll);
    // What a preview shows follows the document and the playhead; the cards and the background
    // redraw, and the paint asks for the new identity. Nothing is requested for a preview no
    // view draws.
    connect(&session_, &CompositionSession::snapshotChanged, this, [this] {
        // A removed background node stops being the background.
        if (backgroundNode_ && backgroundComposition_ == session_.compositionId() &&
            !backgroundNode()) {
            backgroundNode_.reset();
            backgroundComposition_.reset();
            emit backgroundChanged();
        }
        emit changed();
    });
    connect(&session_, &CompositionSession::compositionChanged, this, [this] {
        emit backgroundChanged();
        emit changed();
    });
    connect(&session_, &CompositionSession::currentTimeChanged, this,
            &ImagePreviewController::changed);
    // Same revision, different pixels (a colour-settings change, the qualified display becoming
    // ready): nothing kept is the current picture any more.
    connect(&session_, &CompositionSession::evaluationChanged, this, [this] {
        kept_.clear();
        for (auto& [node, shown] : shown_)
            shown.key.reset();
        emit changed();
    });
    session_.setImagePreviews(this);
}

ImagePreviewController::~ImagePreviewController() {
    cancel();
    session_.setImagePreviews(nullptr);
}

void ImagePreviewController::cancel() {
    for (auto& [node, pending] : pending_) {
        if (pending.listing)
            pending.listing->cancel();
        if (pending.frame)
            pending.frame->cancel();
        if (pending.convert)
            pending.convert->cancel();
    }
    pending_.clear();
    wanted_.clear();
}

void ImagePreviewController::resetForProject() {
    cancel();
    shown_.clear();
    kept_.clear();
    channels_.clear();
    backgroundNode_.reset();
    backgroundComposition_.reset();
    project_ = session_.snapshot().project().id();
}

document::ImagePreviewDisplay ImagePreviewController::display(const document::NodeId node) const {
    const auto* composition = session_.composition();
    const auto* record = composition ? composition->graph().findNode(node) : nullptr;
    const auto value =
        record ? constantInteger(*composition, *record, document::kImagePreviewDisplayParameterRole)
               : std::nullopt;
    if (!value || *value < 0 || *value > document::kMaximumImagePreviewDisplay)
        return ImagePreviewDisplay::Rgba;
    return static_cast<ImagePreviewDisplay>(*value);
}

std::optional<ImagePreviewController::Key>
ImagePreviewController::keyFor(const document::NodeId node, const QSize devicePixels) const {
    const auto* composition = session_.composition();
    if (composition == nullptr)
        return std::nullopt;
    const auto source = document::imagePreviewSource(composition->graph(), node);
    if (!source)
        return std::nullopt;
    const auto extent =
        previewExtent(composition->format(), std::max(devicePixels.width(), devicePixels.height()));
    if (!extent)
        return std::nullopt;
    const auto time = session_.currentTime();
    // The live snapshot, not the one retained for the time: an edit that leaves the composition's
    // output unaffected (adding or wiring a preview, editing what only a preview reads) keeps the
    // retained snapshot, which may not hold the inspected node at all.
    const auto& snapshot = session_.snapshot();
    const auto& settings = preview_.settings();
    return Key{.project = snapshot.project().id(),
               .composition = session_.compositionId(),
               .revision = snapshot.revision(),
               .time = time,
               .width = extent->width(),
               .height = extent->height(),
               .display = display(node),
               .source = source->output,
               .channelSet = source->kind == document::SocketValueKind::ChannelSet,
               .displayName = settings.displayName,
               .viewName = settings.viewName,
               .showLook = settings.showLook};
}

ImagePreviewController::Preview ImagePreviewController::preview(const document::NodeId node,
                                                                const QSize devicePixels) {
    if (session_.snapshot().project().id() != project_)
        resetForProject();
    const auto key = keyFor(node, devicePixels);
    auto& shown = shown_[node];
    if (!key) {
        if (const auto pending = pending_.find(node); pending != pending_.end()) {
            pending->second.cancelled = true;
            if (pending->second.listing)
                pending->second.listing->cancel();
            if (pending->second.frame)
                pending->second.frame->cancel();
            if (pending->second.convert)
                pending->second.convert->cancel();
        }
        wanted_.erase(node);
        return {.image = {},
                .state = State::Unconnected,
                .cpu = false,
                .message = tr("Connect an image")};
    }
    if (shown.key == key)
        return {.image = shown.image, .state = State::Ready, .cpu = shown.cpu, .message = {}};
    if (const auto kept = std::ranges::find(kept_, *key, &Kept::key); kept != kept_.end()) {
        kept->used = ++uses_;
        shown.key = key;
        shown.image = kept->image;
        shown.cpu = kept->cpu;
        shown.failed.reset();
        // A cache hit makes any request still in flight for this node stale.
        if (const auto pending = pending_.find(node); pending != pending_.end())
            want(node, *key);
        return {.image = shown.image, .state = State::Ready, .cpu = shown.cpu, .message = {}};
    }
    if (shown.failed == key)
        return {.image = shown.image,
                .state = State::Failed,
                .cpu = shown.cpu,
                .message = shown.failure};
    want(node, *key);
    return {.image = shown.image,
            .state = State::Pending,
            .cpu = shown.cpu,
            .message = tr("Preparing preview…")};
}

void ImagePreviewController::want(const document::NodeId node, const Key& key) {
    if (const auto pending = pending_.find(node); pending != pending_.end()) {
        if (pending->second.key == key && !pending->second.cancelled) {
            wanted_.erase(node);
            return;
        }
        // One request in flight per node: the stale one is cancelled, and the newest identity is
        // submitted once its terminal result has been observed.
        if (!pending->second.cancelled) {
            pending->second.cancelled = true;
            ++cancellations_;
            if (pending->second.listing)
                pending->second.listing->cancel();
            if (pending->second.frame)
                pending->second.frame->cancel();
            if (pending->second.convert)
                pending->second.convert->cancel();
        }
        if (shown_[node].key != key)
            wanted_.insert_or_assign(node, key);
        else
            wanted_.erase(node);
        return;
    }
    submit(node, key);
}

void ImagePreviewController::submit(const document::NodeId node, Key key) {
    if (bridge_.isShuttingDown() || preview_.isShuttingDown())
        return;
    Pending pending;
    pending.key = std::move(key);
    const auto channels = std::ranges::find_if(channels_, [&](const Channels& known) {
        return known.revision == pending.key.revision && known.source == pending.key.source;
    });
    if (pending.key.channelSet && channels == channels_.end()) {
        // A ChannelSet is shown as its image: its default colour layer, read by the Viewer
        // channel menu's own worker listing before the frame is requested.
        auto submission =
            preview_.submitChannelListing(pending.key.source.nodeId, pending.key.source.port);
        if (!submission.accepted()) {
            fail(node, pending.key, tr("The channel set could not be listed"));
            return;
        }
        pending.listing = std::move(submission.handle);
        pending_.insert_or_assign(node, std::move(pending));
        return;
    }
    auto [inserted, added] = pending_.insert_or_assign(node, std::move(pending));
    static_cast<void>(added);
    if (!startFrame(node, inserted->second))
        pending_.erase(node);
}

bool ImagePreviewController::startFrame(const document::NodeId node, Pending& pending) {
    const auto& key = pending.key;
    runtime::ChannelInspection inspection{key.source.nodeId, key.source.port, {}};
    if (key.channelSet) {
        const auto channels = std::ranges::find_if(channels_, [&](const Channels& known) {
            return known.revision == key.revision && known.source == key.source;
        });
        if (channels == channels_.end() || channels->names.size() != 4 ||
            channels->names[0].empty()) {
            fail(node, key, tr("The channel set has no color layer"));
            return false;
        }
        // The Alpha view of a channel set shows its alpha channel itself; every other view shows
        // its colour layer (a raw inspection, alpha one).
        const auto& names = channels->names;
        if (key.display == ImagePreviewDisplay::Alpha && !names[3].empty())
            inspection.channels = {names[3]};
        else
            inspection.channels = {names[0], names[1], names[2]};
    }
    const auto& settings = preview_.settings();
    runtime::PreviewRequestIdentity identity;
    identity.projectId = key.project;
    identity.compositionId = key.composition;
    identity.sourceRevision = key.revision;
    identity.requestGeneration = submissions_ + 1;
    identity.time = key.time;
    const auto extent = render::ImageExtent::create(key.width, key.height);
    if (!extent) {
        fail(node, key, tr("The preview size is not valid"));
        return false;
    }
    identity.resolution = runtime::ProxyResolution{*extent.value()};
    identity.quality = settings.quality;
    identity.colorIntent = settings.colorIntent;
    identity.displayName = key.displayName;
    identity.viewName = key.viewName;
    identity.showLook = key.showLook;
    identity.channelInspection = std::move(inspection);
    auto submission =
        preview_.submitNodePreview(identity, key.display == ImagePreviewDisplay::Normalized);
    if (!submission.accepted()) {
        fail(node, key,
             submission.diagnostic && !submission.diagnostic->summary.empty()
                 ? QString::fromStdString(submission.diagnostic->summary)
                 : tr("The preview could not start"));
        return false;
    }
    ++submissions_;
    pending.frame = std::move(submission.handle);
    return true;
}

bool ImagePreviewController::startConvert(const document::NodeId node, Pending& pending,
                                          const PreviewPreparationResultHandle& result) {
    const auto* prepared = result ? result->frame().get() : nullptr;
    if (prepared == nullptr) {
        fail(node, pending.key, tr("The preview could not be prepared"));
        return false;
    }
    const bool normalized = pending.key.display == ImagePreviewDisplay::Normalized;
    // Normalized reads the analysis route's CPU reference frame; any other view is GPU exactly
    // when the Viewer's display route prepared it there.
    pending.cpu = normalized || !gpuProvider(prepared->provenance().provider);
    runtime::TaskRequest request("Draw image preview",
                                 {.kind = runtime::TaskOwnerKind::PanelRequest,
                                  .id = runtime::TaskOwnerId::fromRaw(node.value())},
                                 runtime::TaskPriority::Background);
    const auto channel = viewerChannel(pending.key.display).value_or(ViewerChannel::Rgba);
    auto submission = scheduler_.submit<ConvertedHandle>(
        std::move(request),
        [frame = result->frame(), normalized, channel](runtime::TaskContext& context) {
            using Result = runtime::TaskResult<ConvertedHandle>;
            if (context.isCancellationRequested())
                return Result::cancelled();
            auto image = normalized ? normalizedImage(*frame, context)
                                    : displayImage(*frame, frame, channel, context);
            if (context.isCancellationRequested())
                return Result::cancelled();
            if (!image)
                return Result::failed(unavailable("The preview pixels could not be read"));
            return Result::succeeded(std::make_shared<const Converted>(
                Converted{std::move(*image), std::this_thread::get_id()}));
        });
    if (!submission.accepted()) {
        fail(node, pending.key, tr("The preview could not be drawn"));
        return false;
    }
    pending.convert = std::move(submission.handle);
    bridge_.wake();
    return true;
}

void ImagePreviewController::fail(const document::NodeId node, const Key& key, QString message) {
    auto& shown = shown_[node];
    shown.failed = key;
    shown.failure = std::move(message);
    emit changed();
}

void ImagePreviewController::poll() {
    bool published = false;
    std::vector<document::NodeId> finished;
    for (auto& [node, pending] : pending_) {
        if (pending.listing) {
            auto result = pending.listing->tryTakeResult();
            if (!result)
                continue;
            pending.listing.reset();
            const auto& value = result->value();
            if (pending.cancelled || result->state() != runtime::TaskState::Succeeded || !value ||
                !*value) {
                if (!pending.cancelled)
                    fail(node, pending.key, tr("The channel set could not be listed"));
                finished.push_back(node);
                continue;
            }
            Channels channels{pending.key.revision, pending.key.source, {}};
            for (std::size_t slot = 0; slot < 4; ++slot)
                channels.names.push_back(
                    runtime::defaultShuffleSlotChannel((*value)->channels, slot).value_or(""));
            std::erase_if(channels_,
                          [&](const Channels& known) { return known.source == channels.source; });
            channels_.push_back(std::move(channels));
            if (!startFrame(node, pending))
                finished.push_back(node);
            continue;
        }
        if (pending.frame) {
            auto result = pending.frame->tryTakeResult();
            if (!result)
                continue;
            pending.frame.reset();
            const auto& value = result->value();
            if (pending.cancelled || result->state() != runtime::TaskState::Succeeded || !value ||
                !*value || !(*value)->frame()) {
                if (!pending.cancelled)
                    fail(node, pending.key,
                         result->diagnostics().empty()
                             ? tr("The preview could not be prepared")
                             : QString::fromStdString(result->diagnostics().front().summary));
                finished.push_back(node);
                continue;
            }
            if (!startConvert(node, pending, *value))
                finished.push_back(node);
            continue;
        }
        if (pending.convert) {
            auto result = pending.convert->tryTakeResult();
            if (!result)
                continue;
            pending.convert.reset();
            finished.push_back(node);
            const auto& value = result->value();
            if (pending.cancelled || result->state() != runtime::TaskState::Succeeded || !value ||
                !*value) {
                if (!pending.cancelled)
                    fail(node, pending.key, tr("The preview pixels could not be read"));
                continue;
            }
            lastWorkerThread_ = (*value)->thread;
            const auto existing = std::ranges::find(kept_, pending.key, &Kept::key);
            if (existing != kept_.end())
                *existing = Kept{pending.key, (*value)->image, pending.cpu, ++uses_};
            else
                kept_.push_back(Kept{pending.key, (*value)->image, pending.cpu, ++uses_});
            auto& shown = shown_[node];
            shown.key = pending.key;
            shown.image = (*value)->image;
            shown.cpu = pending.cpu;
            shown.failed.reset();
            published = true;
        }
    }
    for (const auto node : finished)
        pending_.erase(node);
    while (kept_.size() > kCapacity)
        kept_.erase(std::ranges::min_element(kept_, {}, &Kept::used));
    // The newest identity a node asked for while its stale request was finishing.
    for (auto wanted = wanted_.begin(); wanted != wanted_.end();) {
        if (pending_.contains(wanted->first)) {
            ++wanted;
            continue;
        }
        const auto node = wanted->first;
        auto key = std::move(wanted->second);
        wanted = wanted_.erase(wanted);
        submit(node, std::move(key));
    }
    if (!pending_.empty())
        bridge_.wake();
    if (published)
        emit changed();
}

std::optional<document::NodeId> ImagePreviewController::backgroundNode() const {
    if (!backgroundNode_ || backgroundComposition_ != session_.compositionId())
        return std::nullopt;
    const auto* composition = session_.composition();
    const auto* record = composition ? composition->graph().findNode(*backgroundNode_) : nullptr;
    if (record == nullptr || !document::isImagePreviewNodeType(record->typeId))
        return std::nullopt;
    return backgroundNode_;
}

void ImagePreviewController::setBackgroundNode(const std::optional<document::NodeId> node) {
    if (session_.snapshot().project().id() != project_)
        resetForProject();
    if (backgroundNode() == node)
        return;
    backgroundNode_ = node;
    backgroundComposition_ = node ? std::optional(session_.compositionId()) : std::nullopt;
    if (node)
        backgroundVisible_ = true;
    emit backgroundChanged();
}

void ImagePreviewController::setBackgroundVisible(const bool visible) {
    if (backgroundVisible_ == visible)
        return;
    backgroundVisible_ = visible;
    emit backgroundChanged();
}

} // namespace bloom::ui
