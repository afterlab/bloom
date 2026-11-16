// #733: every reader of a cached display frame reads both four-byte layouts, and a 10:10:10:2
// frame is presented by one rule (render/display_pixel_packing.hpp, "Presentation").
//
// The first part settles that rule against Qt itself, for every one of the 1024 codes: a frame's
// words borrowed as a 30-bit QImage (ui/display_frame_image.hpp) and drawn at their own size on an
// 8-bit target show render::presentedCode8, the top 8 bits, and on a deeper target keep their
// codes; a smooth-scaled draw stays within one 8-bit code; the worker-side 8-bit copy is the same
// rule; a transparent word leaves what is behind it. This is why the rule is the top 8 bits:
// the painter gives no other value without a converted copy of the frame on the interface thread.
//
// The second part feeds real frames in both layouts, from the CPU reference display, through the
// readers: the display view and its presented pixel, the copy a cache keeps, the Viewer's channel
// pictures, the comparison of two frames' storage, and the image a thumbnail or preview copies.
// No reader may take a 10:10:10:2 cell's bytes for channels.

#include <bloom/core/color.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/render/display_pixel_packing.hpp>
#include <bloom/render/image_types.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/evaluation.hpp>
#include <bloom/runtime/prepared_preview_frame.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/runtime/view_adjust.hpp>
#include <bloom/ui/display_frame_image.hpp>
#include <bloom/ui/viewer_channel_display.hpp>

#include <QColor>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <source_location>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace document = bloom::document;
namespace render = bloom::render;
namespace runtime = bloom::runtime;
namespace ui = bloom::ui;
using bloom::core::Color4d;
using render::DisplayPixelLayout;
using render::Rgba8;
using runtime::PreparedPreviewFrame;

int failures = 0;

void expect(const bool condition, const std::string_view message,
            const std::source_location location = std::source_location::current()) {
    if (condition) {
        return;
    }
    ++failures;
    std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
}

// ---- The rule, against Qt -----------------------------------------------------------------------

// Every 10-bit code, each channel on its own ramp so a swapped channel cannot pass, and the word 0.
[[nodiscard]] std::vector<Rgba8> everyCode() {
    std::vector<Rgba8> cells;
    cells.reserve(1025);
    for (std::uint32_t code = 0; code < 1024; ++code) {
        cells.push_back(render::rgb10A2Cell(
            {code, (code * 7U) & 1023U, 1023U - code, render::kDisplayAlpha2Opaque}));
    }
    cells.push_back(Rgba8{0, 0, 0, 0});
    return cells;
}

[[nodiscard]] Rgba8 pixelOf(const QImage& image, const int x, const int y) {
    const QColor color = image.pixelColor(x, y);
    return Rgba8{static_cast<std::uint8_t>(color.red()), static_cast<std::uint8_t>(color.green()),
                 static_cast<std::uint8_t>(color.blue()), static_cast<std::uint8_t>(color.alpha())};
}

void testPresentationRule() {
    const auto cells = everyCode();
    const int width = static_cast<int>(cells.size());
    const QImage borrowed =
        ui::borrowDisplayFrameImage(cells, width, 1, static_cast<qsizetype>(cells.size() * 4U),
                                    DisplayPixelLayout::Rgb10A2);
    expect(borrowed.format() == QImage::Format_A2BGR30_Premultiplied &&
               borrowed.constBits() == reinterpret_cast<const uchar*>(cells.data()),
           "a 10:10:10:2 frame is borrowed as a 30-bit image without a copy");
    expect(ui::displayFrameImageFormat(DisplayPixelLayout::Straight8888) ==
               QImage::Format_RGBA8888,
           "a straight frame is borrowed as RGBA8888");

    // Drawn at its own size on the 8-bit targets a window's backing store can be.
    const QColor behind(10, 200, 30);
    for (const auto format : {QImage::Format_RGB32, QImage::Format_ARGB32_Premultiplied,
                              QImage::Format_RGBA8888_Premultiplied}) {
        for (const bool smooth : {false, true}) {
            QImage target(width, 1, format);
            target.fill(behind);
            {
                QPainter painter(&target);
                painter.setRenderHint(QPainter::SmoothPixmapTransform, smooth);
                painter.drawImage(QRectF(0, 0, width, 1), borrowed, QRectF(borrowed.rect()));
            }
            bool rule = true;
            for (int x = 0; x < width; ++x) {
                const auto shown = render::presentedRgba8(DisplayPixelLayout::Rgb10A2,
                                                          cells[static_cast<std::size_t>(x)]);
                const Rgba8 expected =
                    shown.alpha == 0 ? Rgba8{10, 200, 30, 255} : shown;
                rule = rule && pixelOf(target, x, 0) == expected;
            }
            expect(rule, "an 8-bit target shows the top 8 bits of every code, and a transparent "
                         "word shows what is behind it");
        }
    }

    // A deeper target keeps every code.
    for (const auto format : {QImage::Format_RGB30, QImage::Format_BGR30,
                              QImage::Format_A2BGR30_Premultiplied,
                              QImage::Format_RGBA64_Premultiplied}) {
        QImage target(width, 1, format);
        target.fill(Qt::black);
        {
            QPainter painter(&target);
            painter.drawImage(0, 0, borrowed);
        }
        const QImage back = target.convertToFormat(QImage::Format_A2BGR30_Premultiplied);
        bool kept = true;
        for (std::size_t x = 0; x + 1 < cells.size(); ++x) {
            const auto* words = reinterpret_cast<const std::uint32_t*>(back.constBits());
            const auto codes = render::rgb10A2Codes(render::displayCell(words[x]));
            const auto stored = render::rgb10A2Codes(cells[x]);
            kept = kept && codes.red == stored.red && codes.green == stored.green &&
                   codes.blue == stored.blue;
        }
        expect(kept, "a deeper target keeps every 10-bit code");
    }

    // Smooth scaling interpolates; a flat field stays within one 8-bit code of the rule.
    {
        constexpr int kField = 8;
        std::vector<Rgba8> rows;
        rows.reserve(std::size_t{1024} * kField);
        for (std::uint32_t code = 0; code < 1024; ++code) {
            for (int x = 0; x < kField; ++x) {
                rows.push_back(
                    render::rgb10A2Cell({code, code, code, render::kDisplayAlpha2Opaque}));
            }
        }
        const QImage fields = ui::borrowDisplayFrameImage(rows, kField, 1024, kField * 4,
                                                          DisplayPixelLayout::Rgb10A2);
        for (const double scale : {1.625, 0.5}) {
            QImage target(13, 1024, QImage::Format_ARGB32_Premultiplied);
            target.fill(Qt::black);
            {
                QPainter painter(&target);
                painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
                painter.drawImage(QRectF(0, 0, kField * scale, 1024), fields,
                                  QRectF(0, 0, kField, 1024));
            }
            bool close = true;
            for (std::uint32_t code = 0; code < 1024; ++code) {
                const int shown = pixelOf(target, 2, static_cast<int>(code)).red;
                close = close && std::abs(shown - render::presentedCode8(code)) <= 1;
            }
            expect(close, "a smooth-scaled flat field is within one 8-bit code of the rule");
        }
    }

    // The worker-side copy is the same rule and is straight RGBA8.
    const QImage copied =
        ui::copyDisplayFrameImage8(cells, width, 1, static_cast<qsizetype>(cells.size() * 4U),
                                   DisplayPixelLayout::Rgb10A2);
    bool copy = copied.format() == QImage::Format_RGBA8888 && copied.width() == width;
    for (int x = 0; copy && x < width; ++x) {
        copy = pixelOf(copied, x, 0) ==
               render::presentedRgba8(DisplayPixelLayout::Rgb10A2,
                                      cells[static_cast<std::size_t>(x)]);
    }
    expect(copy, "the 8-bit copy of a 10:10:10:2 frame is the presented pixel of every code");

    const std::vector<Rgba8> straight{{1, 2, 3, 255}, {40, 50, 60, 128}, {0, 0, 0, 0}};
    const QImage straightCopy = ui::copyDisplayFrameImage8(straight, 3, 1, 12,
                                                           DisplayPixelLayout::Straight8888);
    expect(straightCopy.format() == QImage::Format_RGBA8888 &&
               pixelOf(straightCopy, 0, 0) == straight[0] &&
               pixelOf(straightCopy, 1, 0) == straight[1] &&
               pixelOf(straightCopy, 2, 0) == straight[2] &&
               straightCopy.constBits() != reinterpret_cast<const uchar*>(straight.data()),
           "the 8-bit copy of a straight frame is its own bytes, copied");
}

// ---- Frames in both layouts through the readers -------------------------------------------------

constexpr auto kProjectId = document::ProjectId::fromRaw(1);
constexpr auto kCompositionId = document::CompositionId::fromRaw(2);
constexpr auto kSolidNode = document::NodeId::fromRaw(10);
constexpr auto kLayerNode = document::NodeId::fromRaw(11);
constexpr auto kStackNode = document::NodeId::fromRaw(12);
constexpr auto kOutputNode = document::NodeId::fromRaw(13);
constexpr auto kLayer = document::LayerId::fromRaw(20);
constexpr auto kSlot = document::LayerSlotId::fromRaw(30);
constexpr std::uint32_t kWidth = 16;
constexpr std::uint32_t kHeight = 8;

// One solid of `color`, moved by whole pixels off the frame's center, so the frame has exactly
// transparent pixels beside its covered ones and no partly covered pixel.
[[nodiscard]] std::shared_ptr<const runtime::CompiledCompositionPlan>
oneSolidPlan(const Color4d color) {
    const auto format = document::CompositionFormat::create(kWidth, kHeight);
    if (!format.has_value()) {
        std::abort();
    }
    std::vector<runtime::CompiledOperation> operations;
    operations.emplace_back(
        runtime::CompiledSolid{kSolidNode,
                               {document::ParameterId::fromRaw(40), color},
                               {document::ParameterId::fromRaw(kSolidNode.value() * 100 + 1000),
                                static_cast<double>(format->width())},
                               {document::ParameterId::fromRaw(kSolidNode.value() * 100 + 1001),
                                static_cast<double>(format->height())}});
    operations.emplace_back(runtime::CompiledLayerOutput{
        kLayerNode, kLayer, runtime::OperationIndex::fromRaw(0),
        runtime::CompiledVec2Parameter{document::ParameterId::fromRaw(41),
                                       document::Vec2d{2.0, 1.0}},
        runtime::CompiledVec2Parameter{document::ParameterId::fromRaw(43),
                                       document::kDefaultAnchor},
        runtime::CompiledVec2Parameter{document::ParameterId::fromRaw(44), document::kDefaultScale},
        runtime::CompiledScalarParameter{document::ParameterId::fromRaw(45),
                                         document::kDefaultRotationDegrees},
        runtime::CompiledScalarParameter{document::ParameterId::fromRaw(42), 1.0},
        document::ParameterId::fromRaw(46), bloom::core::kDefaultBlendMode});
    operations.emplace_back(runtime::CompiledMerge{
        kStackNode,
        {runtime::CompiledMergeInput{kSlot, kLayer, runtime::OperationIndex::fromRaw(1)}}});
    operations.emplace_back(
        runtime::CompiledCompositionOutput{kOutputNode, runtime::OperationIndex::fromRaw(2)});
    return std::make_shared<const runtime::CompiledCompositionPlan>(
        runtime::CompiledCompositionPlanDefinition{
            document::Revision::fromRaw(7), kProjectId, kCompositionId, *format,
            std::move(operations), runtime::OperationIndex::fromRaw(3)});
}

[[nodiscard]] std::shared_ptr<const PreparedPreviewFrame>
referenceFrame(const Color4d color, const bool allowRgb10A2, const runtime::ViewAdjust adjust = {}) {
    const auto plan = oneSolidPlan(color);
    const runtime::CpuCompositionEvaluator evaluator;
    const auto evaluated = evaluator.evaluate(
        plan,
        runtime::EvaluationRequest{.time = bloom::core::RationalTime::fromInteger(0),
                                   .output = plan->output(),
                                   .resolution = runtime::CompositionFormatResolution{},
                                   .quality = runtime::EvaluationQuality::Reference,
                                   .colorIntent = runtime::EvaluationColorIntent::LinearRec709Scene,
                                   .pixelStorageByteLimit = std::size_t{1} << 22},
        {});
    if (evaluated.frame() == nullptr) {
        return nullptr;
    }
    const runtime::CpuReferenceDisplayPreparer preparer;
    const auto display =
        preparer.prepare(evaluated.frame(),
                         runtime::ReferenceDisplayPreparationRequest{
                             .intent = runtime::ReferenceDisplayIntent::LinearRec709SceneToSrgb,
                             .aggregatePixelStorageByteLimit = std::size_t{1} << 22,
                             .viewAdjust = adjust,
                             .displayName = {},
                             .viewName = {},
                             .showLook = true,
                             .allowRgb10A2 = allowRgb10A2},
                         {});
    if (display.frame() == nullptr) {
        return nullptr;
    }
    const auto prepared = PreparedPreviewFrame::create(1, display.frame());
    return prepared.has_value() ? std::make_shared<const PreparedPreviewFrame>(*prepared)
                                : nullptr;
}

[[nodiscard]] bool withinOne(const Rgba8 left, const Rgba8 right) {
    return std::abs(left.red - right.red) <= 1 && std::abs(left.green - right.green) <= 1 &&
           std::abs(left.blue - right.blue) <= 1 && left.alpha == right.alpha;
}

// A covered and a transparent pixel of the test frame, found in its straight RGBA8 form.
struct Landmarks final {
    std::size_t covered = 0;
    std::size_t transparent = 0;
    bool found = false;
};

[[nodiscard]] Landmarks landmarksOf(const std::span<const Rgba8> straight) {
    Landmarks marks;
    bool covered = false;
    bool transparent = false;
    for (std::size_t index = 0; index < straight.size(); ++index) {
        if (!covered && straight[index].alpha == 255) {
            marks.covered = index;
            covered = true;
        }
        if (!transparent && straight[index].alpha == 0) {
            marks.transparent = index;
            transparent = true;
        }
    }
    marks.found = covered && transparent;
    return marks;
}

void testFrameLayouts() {
    const Color4d color{0.2, 0.5, 0.8, 1.0};
    const auto narrow = referenceFrame(color, false);
    const auto wide = referenceFrame(color, true);
    const auto soft = referenceFrame(Color4d{0.2, 0.5, 0.8, 0.5}, true);
    expect(narrow != nullptr && wide != nullptr && soft != nullptr, "the frames prepare");
    if (narrow == nullptr || wide == nullptr || soft == nullptr) {
        return;
    }
    const auto narrowView = narrow->displayBufferView();
    const auto wideView = wide->displayBufferView();
    const auto softView = soft->displayBufferView();
    expect(narrowView.has_value() && wideView.has_value() && softView.has_value(),
           "the frames have display pixels");
    if (!narrowView || !wideView || !softView) {
        return;
    }
    const auto marks = landmarksOf(narrowView->pixels);
    expect(marks.found, "the test frame has covered and exactly transparent pixels");
    if (!marks.found) {
        return;
    }
    const std::size_t kCovered = marks.covered;
    const std::size_t kTransparent = marks.transparent;
    expect(narrowView->pixelLayout == DisplayPixelLayout::Straight8888,
           "a frame that was not allowed 10 bits is straight RGBA8");
    expect(wideView->pixelLayout == DisplayPixelLayout::Rgb10A2,
           "an opaque and transparent frame that was allowed is 10:10:10:2");
    expect(softView->pixelLayout == DisplayPixelLayout::Straight8888,
           "a frame with partly transparent pixels stays straight RGBA8");
    expect(wideView->pixels.size() == narrowView->pixels.size() &&
               wideView->layout == narrowView->layout,
           "both layouts are four bytes a pixel over the same window");

    const auto stored = render::rgb10A2Codes(wideView->pixels[kCovered]);
    expect(stored.alpha == render::kDisplayAlpha2Opaque && stored.red > 0U &&
               stored.red < stored.green && stored.green < stored.blue &&
               wideView->pixels[kTransparent] == (Rgba8{0, 0, 0, 0}),
           "a covered pixel is an opaque word of three codes and a transparent one is the word 0");
    bool presented = true;
    for (std::size_t index = 0; index < wideView->pixels.size(); ++index) {
        presented = presented &&
                    withinOne(wideView->presentedPixel(index), narrowView->presentedPixel(index));
    }
    expect(presented && narrowView->presentedPixel(kCovered) == narrowView->pixels[kCovered],
           "the presented pixel of the 10-bit frame is within one code of the 8-bit frame's");

    // Exposure is applied before the pack in both layouts.
    const runtime::ViewAdjust brighter{.exposure = 0.5, .gamma = 1.0};
    const auto narrowAdjusted = referenceFrame(color, false, brighter);
    const auto wideAdjusted = referenceFrame(color, true, brighter);
    const auto narrowAdjustedView =
        narrowAdjusted ? narrowAdjusted->displayBufferView() : std::nullopt;
    const auto wideAdjustedView = wideAdjusted ? wideAdjusted->displayBufferView() : std::nullopt;
    expect(narrowAdjustedView.has_value() && wideAdjustedView.has_value() &&
               wideAdjustedView->pixelLayout == DisplayPixelLayout::Rgb10A2 &&
               withinOne(wideAdjustedView->presentedPixel(kCovered),
                         narrowAdjustedView->presentedPixel(kCovered)) &&
               render::rgb10A2Codes(wideAdjustedView->pixels[kCovered]).red > stored.red &&
               wideAdjustedView->pixels[kTransparent] == (Rgba8{0, 0, 0, 0}),
           "a Viewer adjustment packs 10-bit codes of the adjusted value");

    // The copy a cache keeps holds the same words in the same layout.
    auto kept = runtime::PreviewDisplayOnlyFrame::create(
        *wide, std::numeric_limits<std::size_t>::max());
    expect(kept.has_value(), "the display-only copy of a 10-bit frame builds");
    if (kept.has_value()) {
        const auto envelope = PreparedPreviewFrame::createDisplayOnly(
            2, std::make_shared<const runtime::PreviewDisplayOnlyFrame>(std::move(*kept)));
        const auto keptView = envelope ? envelope->displayBufferView() : std::nullopt;
        bool same = keptView.has_value() && keptView->pixelLayout == DisplayPixelLayout::Rgb10A2 &&
                    keptView->pixels.size() == wideView->pixels.size();
        for (std::size_t index = 0; same && index < keptView->pixels.size(); ++index) {
            same = keptView->pixels[index] == wideView->pixels[index];
        }
        expect(same, "the display-only copy keeps the layout and every word");
    }

    expect(ui::sameViewerPackedSource(wide, wide) && !ui::sameViewerPackedSource(wide, narrow),
           "two frames are the same packed source only over the same storage");

    // The image a thumbnail or a preview copies on its worker.
    const auto extent = wideView->displayWindow.extent();
    const QImage copied = ui::copyDisplayFrameImage8(
        wideView->pixels, static_cast<int>(extent.width()), static_cast<int>(extent.height()),
        static_cast<qsizetype>(wideView->layout.rowStrideBytes), wideView->pixelLayout);
    const auto at = [&extent](const std::size_t index) {
        return std::pair{static_cast<int>(index % extent.width()),
                         static_cast<int>(index / extent.width())};
    };
    expect(copied.format() == QImage::Format_RGBA8888 &&
               pixelOf(copied, at(kCovered).first, at(kCovered).second) ==
                   wideView->presentedPixel(kCovered) &&
               pixelOf(copied, at(kTransparent).first, at(kTransparent).second) ==
                   (Rgba8{0, 0, 0, 0}),
           "the worker's 8-bit copy of a 10-bit frame is its presented pixels");
}

[[nodiscard]] ui::ViewerChannelDisplayHandle
channelPicture(runtime::TaskScheduler& scheduler,
               const std::shared_ptr<const PreparedPreviewFrame>& frame,
               const ui::ViewerChannel channel) {
    using namespace std::chrono_literals;
    auto submission = scheduler.submit<ui::ViewerChannelDisplayHandle>(
        runtime::TaskRequest(
            "Channel picture",
            runtime::TaskOwner{.kind = runtime::TaskOwnerKind::Composition,
                               .id = runtime::TaskOwnerId::fromRaw(1)}),
        [frame, channel](runtime::TaskContext& context) {
            return ui::prepareViewerChannelDisplay(
                frame, channel, std::numeric_limits<std::size_t>::max(), context);
        });
    if (!submission.accepted()) {
        return nullptr;
    }
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto result = submission.handle.tryTakeResult()) {
            const auto& value = result->value();
            return result->state() == runtime::TaskState::Succeeded && value ? *value : nullptr;
        }
        std::this_thread::sleep_for(1ms);
    }
    return nullptr;
}

void testChannelPictures() {
    const auto wide = referenceFrame(Color4d{0.2, 0.5, 0.8, 1.0}, true);
    const auto narrow = referenceFrame(Color4d{0.2, 0.5, 0.8, 1.0}, false);
    const auto wideView = wide ? wide->displayBufferView() : std::nullopt;
    const auto narrowView = narrow ? narrow->displayBufferView() : std::nullopt;
    expect(wideView.has_value() && narrowView.has_value(), "the channel frames prepare");
    if (!wideView || !narrowView) {
        return;
    }
    const auto marks = landmarksOf(narrowView->pixels);
    expect(marks.found && wideView->pixelLayout == DisplayPixelLayout::Rgb10A2,
           "the channel frame is 10:10:10:2 with covered and transparent pixels");
    if (!marks.found) {
        return;
    }
    const std::size_t kCovered = marks.covered;
    const std::size_t kTransparent = marks.transparent;
    const auto stored = render::rgb10A2Codes(wideView->pixels[kCovered]);
    constexpr auto opaque = render::kDisplayAlpha2Opaque;
    constexpr auto top = render::kDisplayCode10Maximum;
    runtime::TaskScheduler scheduler;
    struct Case final {
        ui::ViewerChannel channel;
        render::Rgb10A2Codes covered;
        render::Rgb10A2Codes transparent;
    };
    const Case cases[] = {
        {ui::ViewerChannel::Rgb, {stored.red, stored.green, stored.blue, opaque},
         {0U, 0U, 0U, opaque}},
        {ui::ViewerChannel::Red, {stored.red, stored.red, stored.red, opaque},
         {0U, 0U, 0U, opaque}},
        {ui::ViewerChannel::Green, {stored.green, stored.green, stored.green, opaque},
         {0U, 0U, 0U, opaque}},
        {ui::ViewerChannel::Blue, {stored.blue, stored.blue, stored.blue, opaque},
         {0U, 0U, 0U, opaque}},
        {ui::ViewerChannel::Alpha, {top, top, top, opaque}, {0U, 0U, 0U, opaque}},
    };
    for (const Case& item : cases) {
        const auto picture = channelPicture(scheduler, wide, item.channel);
        expect(picture != nullptr && picture->pixelLayout == DisplayPixelLayout::Rgb10A2 &&
                   picture->pixels.size() == wideView->pixels.size() &&
                   render::rgb10A2Codes(picture->pixels[kCovered]) == item.covered &&
                   render::rgb10A2Codes(picture->pixels[kTransparent]) == item.transparent,
               "a channel of a 10-bit frame is a 10-bit picture of that channel's codes");
        // The same channel of the 8-bit frame is the straight picture it always was, and the two
        // pictures are shown within one 8-bit code of each other.
        const auto straight = channelPicture(scheduler, narrow, item.channel);
        expect(straight != nullptr && straight->pixelLayout == DisplayPixelLayout::Straight8888 &&
                   picture != nullptr &&
                   withinOne(render::presentedRgba8(picture->pixelLayout,
                                                    picture->pixels[kCovered]),
                             straight->pixels[kCovered]) &&
                   render::presentedRgba8(picture->pixelLayout, picture->pixels[kTransparent]) ==
                       straight->pixels[kTransparent],
               "the channel pictures of both layouts show the same picture");
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!scheduler.isQuiescent() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

} // namespace

int main(int argc, char** argv) {
    const QGuiApplication application(argc, argv);
    testPresentationRule();
    testFrameLayouts();
    testChannelPictures();
    if (failures != 0) {
        std::cerr << failures << " presentation depth expectation(s) failed\n";
        return 1;
    }
    std::cout << "PASS: display frames are read and presented in both layouts\n";
    return 0;
}
