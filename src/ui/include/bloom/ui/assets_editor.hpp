#pragma once

#include <bloom/document/ids.hpp>
#include <bloom/ui/editor_area.hpp>

#include <QJsonObject>
#include <QSet>
#include <QWidget>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

class QLineEdit;
class QMenu;
class QPoint;
class QTreeWidget;
class QTreeWidgetItem;

namespace bloom::ui {

namespace kit {
class KIconButton;
} // namespace kit
namespace assets {
class ColumnController;
class SearchBar;
class SummaryStrip;
} // namespace assets
class CompositionSession;
class ExportQueueAdapter;

// The project's assets, compositions and folders as one sortable tree with columns, modelled on
// a compositor's project panel (#378): a compact summary strip of the selection above it, and a
// search row that opens from the header's Search toggle or Ctrl+F.
class AssetsEditor final : public QWidget,
                           public EditorChromeProvider,
                           public EditorSessionStateProvider {
    Q_OBJECT

  public:
    [[nodiscard]] EditorChromeSpec& editorChrome() override { return chrome_; }
    // Work counter: complete row rebuilds since construction.
    [[nodiscard]] std::uint64_t rowRebuildCount() const noexcept { return rowRebuilds_; }
    explicit AssetsEditor(CompositionSession& session, QWidget* parent = nullptr);
    // ADR 0033: composition rows offer Add to Export Queue through this adapter. Null (the
    // default) offers it disabled with the reason. Borrowed; must outlive the editor.
    void setExportQueue(ExportQueueAdapter* adapter) noexcept { exportQueue_ = adapter; }
    ~AssetsEditor() override;
    // #692: the column widths the user dragged travel with the panel in the workspace layout.
    [[nodiscard]] QJsonObject saveEditorSessionState() const override;
    void restoreEditorSessionState(const QJsonObject& state) override;
    // Test seam (#689): takes the place of the Import dialog; called with the folder the import
    // would land in.
    void setImportRequestForTest(std::function<void(std::optional<document::AssetFolderId>)> handler) {
        importRequestForTest_ = std::move(handler);
    }

  private:
    EditorChromeSpec chrome_;
    void rebuild();
    // Rebuilds rows only when the asset/folder/composition records or their asset-status
    // projection changed; playback, freshness checks and unrelated edits keep the existing rows.
    void refreshRows();
    [[nodiscard]] QString rowSignature() const;
    void updateSelection();
    void selectionChanged();
    void applyFilter(const QString& text);
    void openComposition(document::CompositionId id);
    void commitRename(QTreeWidgetItem* item, int column);
    void showContextMenu(QPoint position);
    void addCommonActions(QMenu& menu);
    void showColumnMenu(QPoint globalPosition);
    void openSearch();
    void setInfoVisible(bool visible);
    void duplicateComposition(document::CompositionId id);
    void showNewCompositionDialog();
    void newCompositionFromSelection();
    // #646: the selected footage, SVG artwork and compositions become layers of the active
    // composition, on top at the playhead, as a drop on the Viewer does.
    void addSelectionToComposition();
    void buildHeaderMenus();
    void buildFooter();
    void refreshSelectionHeader();
    void createFolder();
    void beginRename(QTreeWidgetItem* item);
    void editTags(document::AssetId id);
    // Interpret Footage (#381) for the selected footage, showing `id`'s interpretation.
    void interpretFootage(document::AssetId id);
    void copyInterpretation(document::AssetId id);
    void pasteInterpretation();
    void removeSelected();
    void revealInFileManager(document::AssetId id);
    void requestImport();
    // File > Import's dialog, for an import that lands in `folder` (empty: the top level).
    void requestImportInto(std::optional<document::AssetFolderId> folder);
    // The folder new folders, compositions and imports land in for the current selection.
    [[nodiscard]] std::optional<document::AssetFolderId> selectedFolder() const;
    [[nodiscard]] std::vector<document::AssetId> selectedAssets() const;

    CompositionSession& session_;
    std::function<void(std::optional<document::AssetFolderId>)> importRequestForTest_;
    ExportQueueAdapter* exportQueue_ = nullptr;
    QLineEdit* search_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QWidget* headerMenuWidget_ = nullptr;
    QWidget* footerWidget_ = nullptr;
    assets::SummaryStrip* summary_ = nullptr;
    assets::SearchBar* searchBar_ = nullptr;
    assets::ColumnController* columns_ = nullptr;
    kit::KIconButton* searchButton_ = nullptr;
    QMenu* columnMenu_ = nullptr;
    bool rebuilding_ = false;
    QString rowSignature_;
    std::uint64_t rowRebuilds_ = 0;
    QSet<qulonglong> collapsedFolders_;
    bool dataCollapsed_ = false;
};

} // namespace bloom::ui
