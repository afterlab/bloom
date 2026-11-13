#pragma once

// #724 Scopes: what the Scopes panel's status line says. A pure function of the controller's
// reading and the panel's state, so every item and every warning can be tested without a widget.
//
// The line always states what was measured: the composition and frame time, the display and view
// (or the working space), the resolution, the measured rectangle when a region or the Viewer's
// region of interest applies, the route, and the pixels counted. Warnings name every Viewer setting
// that makes the picture differ from the export. "Behind" says the scopes shown are another frame's
// and names that frame; data of another frame is never shown without it.

#include <bloom/document/ids.hpp>
#include <bloom/runtime/scope_measure.hpp>
#include <bloom/runtime/view_adjust.hpp>
#include <bloom/ui/scopes_controller.hpp>

#include <QString>
#include <QStringList>

#include <functional>

namespace bloom::ui {

struct ScopesStatusInput final {
    // The panel has a controller to measure with.
    bool available = true;
    // A Viewer is being followed.
    bool hasViewer = false;
    // At least one section is open and the panel is shown.
    bool measuring = true;
    // Why nothing is measured: the panel is hidden, rather than every section collapsed.
    bool hidden = false;
    ScopesReading reading;
    runtime::ScopeMeasureSettings settings;
    // The exposure and gamma the followed Viewer applies.
    runtime::ViewAdjust viewAdjust;
    // The composition's name; empty falls back to a generic one.
    std::function<QString(document::ProjectId, document::CompositionId)> compositionName;
};

struct ScopesStatusText final {
    // The items, joined with a middle dot; the reason when nothing is measured.
    QString line;
    // One entry per Viewer setting that makes the picture differ from the export (warning color).
    QStringList warnings;
    // "Behind: ..." naming the measured frame; empty when the scopes are the shown frame's.
    QString behind;
    // True when `line` states a measurement rather than a reason.
    bool measured = false;
};

[[nodiscard]] ScopesStatusText describeScopesStatus(const ScopesStatusInput& input);

// "frame 12 (0.500 s)" of a source's request.
[[nodiscard]] QString scopesFrameText(const PreviewScopeSource& source);

} // namespace bloom::ui
