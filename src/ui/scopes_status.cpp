#include <bloom/ui/scopes_status.hpp>

#include <bloom/runtime/compiled_plan.hpp>

#include <QCoreApplication>

#include <algorithm>
#include <cmath>
#include <variant>

namespace bloom::ui {
namespace {

QString tr(const char* text) { return QCoreApplication::translate("ScopesStatus", text); }

[[nodiscard]] QString resolutionText(const PreviewScopeSource& source) {
    const auto* proxy = std::get_if<runtime::ProxyResolution>(&source.identity.resolution);
    if (proxy == nullptr || source.process.plan == nullptr)
        return tr("Full");
    const auto full = source.process.plan->format().width();
    const auto reduced = proxy->extent.width();
    if (reduced == 0 || reduced >= full)
        return tr("Full");
    const auto divisor = static_cast<int>(std::lround(static_cast<double>(full) / reduced));
    if (divisor == 2)
        return tr("Half");
    if (divisor == 4)
        return tr("Quarter");
    return QStringLiteral("1/%1").arg(divisor);
}

[[nodiscard]] QString compositionText(const PreviewScopeSource& source,
                                      const ScopesStatusInput& input) {
    QString name;
    if (input.compositionName)
        name = input.compositionName(source.identity.projectId, source.identity.compositionId);
    if (name.isEmpty())
        name = tr("Composition");
    return QStringLiteral("%1, %2").arg(name, scopesFrameText(source));
}

[[nodiscard]] QString rectText(const std::int64_t x, const std::int64_t y,
                               const std::uint64_t width, const std::uint64_t height) {
    return QStringLiteral("%1, %2, %3 x %4").arg(x).arg(y).arg(width).arg(height);
}

} // namespace

QString scopesFrameText(const PreviewScopeSource& source) {
    const double seconds = source.identity.time.toSeconds();
    QString frame;
    if (source.process.plan != nullptr) {
        const auto rate = source.process.plan->format().frameRate();
        const auto index = std::llround(seconds * rate.numerator() / rate.denominator());
        frame = tr("frame %1").arg(index);
    } else {
        frame = tr("time");
    }
    return QStringLiteral("%1 (%2 s)").arg(frame).arg(seconds, 0, 'f', 3);
}

ScopesStatusText describeScopesStatus(const ScopesStatusInput& input) {
    ScopesStatusText text;
    const auto& reading = input.reading;
    // What is on show was measured under its own settings, which may be older than the panel's.
    const auto& settings =
        reading.scopes != nullptr ? reading.scopes->identity.settings : input.settings;

    // Warnings: every Viewer setting that makes the picture differ from the export.
    const auto& origin = reading.shown ? reading.shown : reading.measured;
    if (origin) {
        const bool display = settings.signal == runtime::ScopeSignal::Display;
        // Exposure, gamma and the look act in the display transform; the Working signal is taken
        // before it.
        if (display && input.viewAdjust.exposure != 0.0)
            text.warnings << tr("Exposure %1 EV").arg(input.viewAdjust.exposure, 0, 'g', 5);
        if (display && input.viewAdjust.gamma != 1.0)
            text.warnings << tr("Gamma %1").arg(input.viewAdjust.gamma, 0, 'g', 5);
        if (display && !origin->identity.showLook)
            text.warnings << tr("Look off");
        if (origin->identity.channelInspection) {
            QStringList channels;
            for (const auto& channel : origin->identity.channelInspection->channels)
                channels << QString::fromStdString(channel);
            text.warnings << (channels.isEmpty()
                                  ? tr("Channel shown")
                                  : tr("Channel shown: %1").arg(channels.join(QLatin1Char('/'))));
        }
        if (!origin->identity.isolation.empty())
            text.warnings << tr("Isolation on");
    }

    if (!input.available) {
        text.line = tr("Scopes are not available in this session.");
        text.warnings.clear();
        return text;
    }
    if (!input.hasViewer) {
        text.line = tr("No Viewer to follow. Open a Viewer to measure its picture.");
        return text;
    }
    if (input.hidden) {
        text.line = tr("The panel is hidden, so nothing is measured.");
        return text;
    }
    if (!input.measuring) {
        text.line = tr("Every section is collapsed, so nothing is measured.");
        return text;
    }
    if (!reading.shown) {
        text.line = tr("The Viewer shows no frame yet.");
        return text;
    }
    if (reading.state == ScopesReading::State::Failed) {
        text.line = reading.message.empty() ? tr("The frame could not be measured.")
                                            : QString::fromStdString(reading.message);
        return text;
    }
    if (reading.scopes == nullptr || !reading.measured) {
        text.line = tr("Measuring...");
        return text;
    }

    const auto& sidecar = *reading.scopes;
    const auto& source = *reading.measured;
    if (sidecar.tables.empty()) {
        text.line = tr("The region lies outside the picture, so there is nothing to count.");
        return text;
    }
    text.measured = true;
    QStringList items;
    items << compositionText(source, input);
    const auto& color = reading.color;
    if (settings.signal == runtime::ScopeSignal::Display) {
        if (color != nullptr)
            items << tr("Display %1 / %2")
                         .arg(QString::fromStdString(color->display()),
                              QString::fromStdString(color->view()));
        else
            items << tr("Display %1 / %2")
                         .arg(QString::fromStdString(source.identity.displayName),
                              QString::fromStdString(source.identity.viewName));
    } else {
        items << tr("Working space %1")
                     .arg(color != nullptr ? QString::fromStdString(color->colorSpaceId())
                                           : tr("(scene-linear)"));
    }
    items << tr("Resolution %1").arg(resolutionText(source));
    const bool hasRegion = settings.region.has_value();
    const bool hasInterest = source.identity.roi.has_value();
    if (hasRegion || hasInterest) {
        const auto& area = sidecar.area;
        const QString which = hasRegion && hasInterest ? tr("Region within the region of interest")
                              : hasRegion              ? tr("Region")
                                                       : tr("Region of interest");
        items << tr("%1 %2").arg(which, rectText(area.x, area.y, area.width, area.height));
    }
    items << (sidecar.tables.route() == runtime::ScopeRoute::Gpu ? tr("GPU") : tr("CPU"));
    items << tr("%1 pixels counted").arg(sidecar.tables.counted());
    text.line = items.join(QStringLiteral(" · "));
    if (!sidecar.note.empty())
        text.line += QStringLiteral(" (%1)").arg(QString::fromStdString(sidecar.note));
    if (reading.behind)
        text.behind = tr("Behind: these scopes are of %1").arg(compositionText(source, input));
    return text;
}

} // namespace bloom::ui
