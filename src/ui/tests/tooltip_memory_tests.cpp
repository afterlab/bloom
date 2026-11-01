// #418: hovering the widgets that carry tool tips keeps memory bounded.
//
// Outside Windows, Qt parents the visible tool tip (a QTipLabel window) to the widget it explains,
// so a widget that hands its tool tip to every descendant also reached the tip. A tip that carries
// a tool tip of its own shows it for itself when the pointer rests on it; Qt then makes the tip its
// own style-sheet parent, and QStyleSheetStyle's walk up the parents appends one style sheet per
// step and never ends (a desktop session reached a single 16 GiB block before it was killed).
//
// Each hover here does what a session does: show the tip for a widget, refresh the tool tips while
// it shows (the status bar does so every 250 ms), then rest the pointer on the tip itself. A memory
// ceiling ends the process if the walk ever starts again, so a regression fails here instead of
// exhausting the machine.
#include "window_status_bar_segments.hpp"

#include <bloom/commands/command_stack.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/runtime/memory_pressure_monitor.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/row.hpp>
#include <bloom/ui/kit/section.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/window_status_bar.hpp>

#include <QApplication>
#include <QHelpEvent>
#include <QLabel>
#include <QTest>
#include <QToolTip>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <optional>
#include <source_location>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace ui = bloom::ui;
namespace doc = bloom::document;

int gFailures = 0;
void expect(const bool condition, const std::string_view message,
            const std::source_location where = std::source_location::current()) {
    if (condition)
        return;
    ++gFailures;
    std::cerr << where.file_name() << ':' << where.line() << ": " << message << '\n';
}

constexpr std::size_t kMebibyte = std::size_t{1024} * 1024;
// Far above anything a few hundred tool tips need, far below what the unbounded walk reaches
// within a second.
constexpr std::size_t kCeilingGrowth = 1024 * kMebibyte;
// Growth allowed from the end of the first round to the end of the last one.
constexpr std::size_t kRoundGrowth = 16 * kMebibyte;
constexpr int kRounds = 6;

// Ends the process when the resident set grows past the ceiling. The unbounded walk runs inside a
// single event delivery on the UI thread, so only another thread can stop it.
class MemoryCeiling final {
  public:
    explicit MemoryCeiling(const std::size_t baseline)
        : limit_(baseline + kCeilingGrowth), watcher_([this] { watch(); }) {}
    MemoryCeiling(const MemoryCeiling&) = delete;
    MemoryCeiling& operator=(const MemoryCeiling&) = delete;
    ~MemoryCeiling() {
        stop_ = true;
        watcher_.join();
    }

  private:
    void watch() const {
        while (!stop_) {
            if (const auto resident = bloom::runtime::processResidentBytes();
                resident && *resident > limit_) {
                std::cerr << "tool-tip hover grew the resident set to " << *resident / kMebibyte
                          << " MiB, past the " << limit_ / kMebibyte << " MiB ceiling\n";
                std::abort();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    std::size_t limit_;
    std::atomic<bool> stop_{false};
    std::thread watcher_;
};

QWidget* visibleTipLabel() {
    const auto widgets = QApplication::allWidgets();
    const auto found = std::find_if(widgets.begin(), widgets.end(), [](const QWidget* widget) {
        return widget->inherits("QTipLabel") && widget->isVisible();
    });
    return found == widgets.end() ? nullptr : *found;
}

// The event QApplication's tool-tip wake-up timer sends to the widget under a resting pointer.
void restPointerOn(QWidget* widget) {
    const QPoint local = widget->rect().center();
    QHelpEvent event(QEvent::ToolTip, local, widget->mapToGlobal(local));
    QApplication::sendEvent(widget, &event);
}

// A wheel turn anywhere closes the tip at once, as it does for the artist; the label is then
// deleted, so the next hover builds a new one parented to the widget it explains.
void dismissToolTip(QWidget* window) {
    const QPointF centre = QRectF(window->rect()).center();
    QWheelEvent wheel(centre, window->mapToGlobal(centre), QPoint(), QPoint(0, 120), Qt::NoButton,
                      Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(window, &wheel);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    expect(visibleTipLabel() == nullptr, "a wheel turn dismisses the tool tip");
}

struct Hovered final {
    QWidget* widget = nullptr;
    std::string name;
};

// Hovers every widget once: the tip shows for it, `refresh` re-applies the tool tips while the tip
// shows, and the pointer then rests on the tip. Even rounds dismiss the tip after every widget, so
// each widget becomes the parent of a new tip; odd rounds reuse one tip across the round, as when
// the pointer travels from cell to cell.
void hoverRound(const std::vector<Hovered>& widgets, const std::function<void(int)>& refresh,
                const int round) {
    int step = 0;
    for (const auto& hovered : widgets) {
        restPointerOn(hovered.widget);
        auto* tip = visibleTipLabel();
        expect(tip != nullptr && QToolTip::text() == hovered.widget->toolTip(),
               "the tool tip shows for " + hovered.name);
        if (tip == nullptr)
            continue;
        refresh(round * 1000 + step++);
        expect(visibleTipLabel() == tip, "the tool tip stays up through a refresh");
        if (!tip->toolTip().isEmpty()) {
            // Resting the pointer on this tip would never return; that is the defect itself.
            expect(false, "the tool tip label took a tool tip of its own while explaining " +
                              hovered.name);
            dismissToolTip(hovered.widget->window());
            continue;
        }
        restPointerOn(tip);
        expect(visibleTipLabel() == tip, "resting on the tool tip keeps it up");
        if (round % 2 == 0)
            dismissToolTip(hovered.widget->window());
    }
    if (!widgets.empty())
        dismissToolTip(widgets.front().widget->window());
}

// Hovers `widgets` for several rounds and expects the resident set to stop growing after the
// first: the tip, its style sheet and the caches are built once, then reused.
void hoverRepeatedly(const std::string_view label, const std::vector<Hovered>& widgets,
                     const std::function<void(int)>& refresh) {
    expect(!widgets.empty(), std::string(label) + ": there are widgets to hover");
    std::optional<std::size_t> afterFirst;
    std::optional<std::size_t> afterLast;
    for (int round = 0; round < kRounds; ++round) {
        hoverRound(widgets, refresh, round);
        const auto resident = bloom::runtime::processResidentBytes();
        if (round == 0)
            afterFirst = resident;
        afterLast = resident;
    }
    if (afterFirst && afterLast) {
        const auto growth = *afterLast > *afterFirst ? *afterLast - *afterFirst : 0;
        std::cout << label << ": " << widgets.size() << " widgets, " << kRounds
                  << " rounds, resident growth after the first round " << growth / 1024 << " KiB\n";
        expect(growth <= kRoundGrowth,
               std::string(label) + ": repeated hovering does not grow the resident set");
    }
}

std::vector<Hovered> toolTipBearers(const QWidget& root) {
    std::vector<Hovered> found;
    for (auto* widget : root.findChildren<QWidget*>())
        if (!widget->toolTip().isEmpty() && widget->isVisibleTo(root.window()))
            found.push_back({widget, widget->objectName().isEmpty()
                                         ? std::string(widget->metaObject()->className())
                                         : widget->objectName().toStdString()});
    return found;
}

// The status bar as the main window builds it: the Activity and Memory segments re-apply their
// tool tips to every cell while a tip shows.
void testStatusBarCells() {
    auto project = doc::makeNewProject("Tips", "Main", bloom::core::RationalTime::fromInteger(10));
    const auto compositionId = project.initialCompositionId;
    doc::Document document(std::move(project.project));
    bloom::commands::CommandStack stack(document);
    ui::CompositionSession session(document, stack, compositionId);

    QWidget window;
    auto* layout = new QVBoxLayout(&window);
    auto* bar = new ui::WindowStatusBar(session, nullptr, nullptr, nullptr, &window);
    layout->addWidget(bar);
    window.resize(1400, bar->sizeHint().height());
    window.show();
    expect(QTest::qWaitForWindowExposed(&window), "the status bar is exposed");

    const auto cells = toolTipBearers(*bar);
    hoverRepeatedly("status bar", cells, [bar](const int step) {
        bar->setPersistentMessage(step % 2 == 0 ? QStringLiteral("Saving…") : QString{});
        bar->pollMemoryPressureForTest(std::size_t{48} * 1024 * kMebibyte +
                                       static_cast<std::size_t>(step));
    });
}

// One segment on its own, every kind of cell it holds, re-explained on each hover.
void testSegmentCells() {
    QWidget window;
    auto* layout = new QVBoxLayout(&window);
    auto* segment = new ui::statusbar::Segment(QStringLiteral("tipsSegment"), &window);
    layout->addWidget(segment);
    segment->addLabel(QStringLiteral("Cache"));
    segment->add(new ui::statusbar::ValueLabel(QStringLiteral("tipsValue"),
                                               QStringLiteral("00.0/00.0 GB"),
                                               ui::kit::Color::Foreground, segment));
    segment->addGap();
    segment->add(new ui::statusbar::ClickableValue(
        QStringLiteral("tipsClickable"), QStringLiteral("MCP 0"), ui::kit::Color::Muted, segment));
    segment->setSegmentToolTip(QStringLiteral("The memory the caches hold."));
    window.show();
    expect(QTest::qWaitForWindowExposed(&window), "the segment is exposed");

    auto cells = toolTipBearers(window);
    cells.push_back({segment, "the segment itself"});
    hoverRepeatedly("segment", cells, [segment](const int step) {
        segment->setSegmentToolTip(QStringLiteral("The memory the caches hold: %1.").arg(step));
    });
}

// The kit's other labels that carry a static tool tip: an elided label, a property-row label and a
// section title. Their tool tips are re-applied while the tip shows, as a live value would.
void testKitLabels() {
    QWidget window;
    auto* layout = new QVBoxLayout(&window);
    auto* elided = new ui::kit::KLabel(&window);
    elided->setObjectName(QStringLiteral("tipsElided"));
    elided->setElidedText(QStringLiteral("A long value that elides in a narrow cell"));
    layout->addWidget(elided);
    auto* rowLabel = ui::kit::makePropertyRowLabel(QStringLiteral("Anchor point"), &window);
    rowLabel->setObjectName(QStringLiteral("tipsRowLabel"));
    layout->addWidget(rowLabel);
    auto* section = new ui::kit::KSection(QStringLiteral("Transform"), &window);
    layout->addWidget(section);
    window.show();
    expect(QTest::qWaitForWindowExposed(&window), "the kit labels are exposed");

    const auto labels = toolTipBearers(window);
    hoverRepeatedly("kit labels", labels, [elided, section](const int step) {
        elided->setElidedText(
            QStringLiteral("A long value that elides in a narrow cell %1").arg(step));
        section->setTitle(QStringLiteral("Transform %1").arg(step));
    });
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setOrganizationName("BloomTooltipMemoryTests");
    app.setApplicationName("TooltipMemory");
    // The application style sheet is what routes every tip through QStyleSheetStyle.
    ui::kit::installKinetikTheme(app);
    const auto baseline = bloom::runtime::processResidentBytes();
    std::optional<MemoryCeiling> ceiling;
    if (baseline)
        ceiling.emplace(*baseline);
    testSegmentCells();
    testStatusBarCells();
    testKitLabels();
    ceiling.reset();
    if (gFailures != 0)
        std::cerr << gFailures << " failure(s)\n";
    return gFailures == 0 ? 0 : 1;
}
