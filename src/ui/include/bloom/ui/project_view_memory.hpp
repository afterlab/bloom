#pragma once

#include <bloom/document/ids.hpp>

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <filesystem>
#include <map>
#include <optional>

namespace bloom::document {
class Project;
}

namespace bloom::ui {

// What the artist was looking at in a project: the Viewer's Display / View choice, the last active
// composition and each pinned Viewer's target. It is session state, never project truth
// (docs/architecture/color-management.md, "Viewer display/view and look selection";
// docs/architecture/project-session.md, "Recent Paths And Presentation State"): remembering
// changes no document, revision, history entry, saved file, cache key or export. Everything is
// remembered per project, so it survives composition switches, Viewer re-creation and, for a saved
// project, closing and reopening it or relaunching Bloom: a saved project's memory lives in the
// application's QSettings under a digest of its file path. An unsaved project keeps its memory in
// this object only until Save As gives it a file.
class ProjectViewMemory final {
  public:
    // New or Open installed another project: forget the in-memory state. `projectPath` is the
    // installed project's file, or nullopt for an unsaved one.
    void beginProject(const std::optional<std::filesystem::path>& projectPath);
    // Save As gave the same project a new file: keep its state and save it under that file.
    void moveProject(const std::optional<std::filesystem::path>& projectPath);
    [[nodiscard]] const std::optional<std::filesystem::path>& projectPath() const noexcept {
        return projectPath_;
    }

    // The remembered {display, view} for a config revision (lowercase hex), if any.
    [[nodiscard]] std::optional<QStringList> recallDisplayView(const QString& configRevision) const;
    void rememberDisplayView(const QString& configRevision, const QStringList& displayView);

    // #550: the composition that was active last, if one is remembered.
    [[nodiscard]] std::optional<document::CompositionId> recallComposition() const;
    void rememberComposition(document::CompositionId compositionId);
    // The composition a reopened `project` starts on: the remembered one while `project` still
    // has it, otherwise `fallback` (the lowest valid CompositionId).
    [[nodiscard]] document::CompositionId
    reopenedComposition(const document::Project& project, document::CompositionId fallback) const;

    // #550: the pin of the Viewer in workspace area `areaId` ({mode, composition, node?, port?}),
    // or nullopt when that Viewer follows the active composition or nothing is remembered. An
    // empty `pin` remembers that the Viewer follows.
    [[nodiscard]] std::optional<QJsonObject> recallViewerPin(const QString& areaId) const;
    void rememberViewerPin(const QString& areaId, const QJsonObject& pin);

  private:
    [[nodiscard]] QString projectKey() const;
    void saveDisplayView(const QString& configRevision, const QStringList& displayView) const;
    void saveComposition() const;
    void saveViewerPins() const;
    [[nodiscard]] QJsonObject savedViewerPins() const;

    std::optional<std::filesystem::path> projectPath_;
    std::map<QString, QStringList> displayViews_;
    std::optional<document::CompositionId> composition_;
    // Pins remembered in this run (an empty object = follows); other areas read the saved settings.
    std::map<QString, QJsonObject> viewerPins_;
};

} // namespace bloom::ui
