#include <bloom/ui/project_view_memory.hpp>

#include <bloom/document/project.hpp>

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QSettings>

namespace bloom::ui {
namespace {

// A remembered pin names a composition and, for a node pin, a node and its port. Anything else
// is not a pin this build writes.
bool isPinObject(const QJsonObject& pin) {
    const auto mode = pin.value("mode").toString();
    return (mode == QStringLiteral("composition") || mode == QStringLiteral("node")) &&
           pin.value("composition").isString();
}

} // namespace

void ProjectViewMemory::beginProject(const std::optional<std::filesystem::path>& projectPath) {
    projectPath_ = projectPath;
    displayViews_.clear();
    composition_.reset();
    viewerPins_.clear();
}

void ProjectViewMemory::moveProject(const std::optional<std::filesystem::path>& projectPath) {
    if (projectPath_ == projectPath)
        return;
    // What the old file's settings held but this run never touched comes along too.
    const auto composition = recallComposition();
    auto pins = savedViewerPins();
    for (const auto& [areaId, pin] : viewerPins_) {
        if (pin.isEmpty())
            pins.remove(areaId);
        else
            pins.insert(areaId, pin);
    }
    viewerPins_.clear();
    for (auto it = pins.begin(); it != pins.end(); ++it)
        viewerPins_[it.key()] = it.value().toObject();
    composition_ = composition;
    projectPath_ = projectPath;
    for (const auto& [revision, displayView] : displayViews_)
        saveDisplayView(revision, displayView);
    saveComposition();
    saveViewerPins();
}

std::optional<QStringList>
ProjectViewMemory::recallDisplayView(const QString& configRevision) const {
    if (const auto found = displayViews_.find(configRevision); found != displayViews_.end())
        return found->second;
    if (!projectPath_)
        return std::nullopt;
    auto saved = QSettings()
                     .value(QStringLiteral("viewer/project-display-view/%1/%2")
                                .arg(projectKey(), configRevision))
                     .toStringList();
    if (saved.size() != 2)
        return std::nullopt;
    return saved;
}

void ProjectViewMemory::rememberDisplayView(const QString& configRevision,
                                            const QStringList& displayView) {
    if (displayView.size() != 2)
        return;
    displayViews_[configRevision] = displayView;
    saveDisplayView(configRevision, displayView);
}

std::optional<document::CompositionId> ProjectViewMemory::recallComposition() const {
    if (composition_)
        return composition_;
    if (!projectPath_)
        return std::nullopt;
    bool ok = false;
    const auto raw =
        QSettings()
            .value(QStringLiteral("workspace/project-view/%1/composition").arg(projectKey()))
            .toString()
            .toULongLong(&ok);
    if (!ok || raw == 0)
        return std::nullopt;
    return document::CompositionId::fromRaw(raw);
}

void ProjectViewMemory::rememberComposition(const document::CompositionId compositionId) {
    if (!compositionId.isValid() || composition_ == compositionId)
        return;
    composition_ = compositionId;
    saveComposition();
}

document::CompositionId
ProjectViewMemory::reopenedComposition(const document::Project& project,
                                       const document::CompositionId fallback) const {
    if (const auto remembered = recallComposition();
        remembered && project.findComposition(*remembered) != nullptr)
        return *remembered;
    return fallback;
}

std::optional<QJsonObject> ProjectViewMemory::recallViewerPin(const QString& areaId) const {
    if (areaId.isEmpty())
        return std::nullopt;
    QJsonObject pin;
    if (const auto found = viewerPins_.find(areaId); found != viewerPins_.end())
        pin = found->second;
    else
        pin = savedViewerPins().value(areaId).toObject();
    if (!isPinObject(pin))
        return std::nullopt;
    return pin;
}

void ProjectViewMemory::rememberViewerPin(const QString& areaId, const QJsonObject& pin) {
    if (areaId.isEmpty())
        return;
    const QJsonObject kept = isPinObject(pin) ? pin : QJsonObject{};
    if (const auto found = viewerPins_.find(areaId);
        found != viewerPins_.end() && found->second == kept)
        return;
    viewerPins_[areaId] = kept;
    saveViewerPins();
}

QString ProjectViewMemory::projectKey() const {
    // A digest, so a path's separators and characters never shape the settings key.
    const auto path = QString::fromStdU16String(projectPath_->lexically_normal().u16string());
    return QString::fromLatin1(
        QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha256).toHex());
}

void ProjectViewMemory::saveDisplayView(const QString& configRevision,
                                        const QStringList& displayView) const {
    if (projectPath_)
        QSettings().setValue(
            QStringLiteral("viewer/project-display-view/%1/%2").arg(projectKey(), configRevision),
            displayView);
}

void ProjectViewMemory::saveComposition() const {
    if (projectPath_ && composition_)
        QSettings().setValue(
            QStringLiteral("workspace/project-view/%1/composition").arg(projectKey()),
            QString::number(composition_->value()));
}

QJsonObject ProjectViewMemory::savedViewerPins() const {
    if (!projectPath_)
        return {};
    return QJsonDocument::fromJson(
               QSettings()
                   .value(QStringLiteral("workspace/project-view/%1/viewer-pins").arg(projectKey()))
                   .toByteArray())
        .object();
}

void ProjectViewMemory::saveViewerPins() const {
    if (!projectPath_)
        return;
    auto pins = savedViewerPins();
    for (const auto& [areaId, pin] : viewerPins_) {
        if (pin.isEmpty())
            pins.remove(areaId);
        else
            pins.insert(areaId, pin);
    }
    const auto key = QStringLiteral("workspace/project-view/%1/viewer-pins").arg(projectKey());
    QSettings settings;
    if (pins.isEmpty())
        settings.remove(key);
    else
        settings.setValue(key, QJsonDocument(pins).toJson(QJsonDocument::Compact));
}

} // namespace bloom::ui
