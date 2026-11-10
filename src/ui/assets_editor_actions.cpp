#include "asset_input_picker_guard.hpp"
#include "assets_editor_internal.hpp"
#include "interpret_footage_dialog.hpp"
#include "project_change_check.hpp"
#include "project_item_delete.hpp"
#include "reveal_in_file_manager.hpp"
#include <QHBoxLayout>
#include <QPointer>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>
#include <algorithm>
#include <bloom/commands/asset_operations.hpp>
#include <bloom/commands/composition_settings_operations.hpp>
#include <bloom/commands/footage_interpretation_operations.hpp>
#include <bloom/commands/layer_drop_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/project_item_operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/document/project.hpp>
#include <bloom/ui/asset_controller.hpp>
#include <bloom/ui/assets_editor.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dialog.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <filesystem>
#include <memory>
#include <set>
#include <tuple>

namespace bloom::ui {
namespace {
// The Edit Tags rows. A scroll area's own minimum is two scroll bars' worth, which a layout
// honours even under a single tag; one tag row is the real minimum.
class TagRowsArea final : public QScrollArea {
  public:
    using QScrollArea::QScrollArea;
    [[nodiscard]] QSize minimumSizeHint() const override {
        return {0, kit::px(kit::Size::Control)};
    }
};
} // namespace
std::optional<document::AssetFolderId> AssetsEditor::selectedFolder() const {
    return assets::targetFolder(tree_->currentItem() && tree_->currentItem()->isSelected()
                                    ? tree_->currentItem()
                                    : nullptr);
}
std::vector<document::AssetId> AssetsEditor::selectedAssets() const {
    std::vector<document::AssetId> ids;
    for (QTreeWidgetItemIterator it(tree_, QTreeWidgetItemIterator::Selected); *it; ++it)
        if (const auto id = assets::assetId(*it); id.isValid())
            ids.push_back(id);
    return ids;
}
void AssetsEditor::createFolder() {
    const auto parent = selectedFolder();
    std::set<std::string> names;
    for (const auto& folder : session_.snapshot().project().assetFolders())
        if (folder.parent == parent)
            names.insert(folder.name);
    auto name = tr("New Folder").toStdString();
    for (std::size_t suffix = 2; names.contains(name); ++suffix)
        name = tr("New Folder %1").arg(suffix).toStdString();
    search_->clear();
    if (parent)
        collapsedFolders_.remove(parent->value());
    commands::Transaction transaction("New Folder", session_.snapshot().revision());
    transaction.emplace<commands::CreateAssetFolder>(name, parent);
    const auto result = session_.executeTransaction(std::move(transaction));
    const auto id = result.outputId<document::AssetFolderId>(commands::kCreateAssetFolderOutput);
    if (!id)
        return;
    for (QTreeWidgetItemIterator it(tree_); *it; ++it)
        if (assets::folderId(*it) == id) {
            tree_->clearSelection();
            tree_->setCurrentItem(*it);
            beginRename(*it);
            break;
        }
}
void AssetsEditor::editTags(document::AssetId id) {
    const auto* asset = session_.snapshot().project().findAsset(id);
    if (!asset)
        return;
    std::vector<document::AssetId> ids;
    for (const auto* item : tree_->selectedItems()) {
        const auto selected = assets::assetId(item);
        if (selected.isValid())
            ids.push_back(selected);
    }
    if (std::ranges::find(ids, id) == ids.end())
        ids = {id};
    const auto tags = asset->tags;
    auto check = std::make_shared<ProjectChangeCheck>(session_);
    auto* dialog = new kit::KDialog(tr("Edit Tags"), QStringLiteral("assetsTagsDialog"), *this);
    auto* layout = dialog->contentLayout();
    auto* label =
        new kit::KLabel(ids.size() > 1 ? tr("Replace tags on %1 selected assets").arg(ids.size())
                                       : tr("One tag per field"),
                        dialog);
    layout->addWidget(label);
    auto* scroll = new TagRowsArea(dialog);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    // The rows' own height, scrolling only past a diagnostic-sized list: no empty band under a
    // single tag.
    scroll->setSizeAdjustPolicy(QAbstractScrollArea::AdjustToContents);
    scroll->setMaximumHeight(kit::px(kit::Size::DiagnosticHeight));
    // The tag rows sit on the dialog's own surface, not in a darker well.
    scroll->viewport()->setAutoFillBackground(false);
    auto* fields = new QWidget(scroll);
    auto* rows = new QVBoxLayout(fields);
    rows->setContentsMargins(0, 0, 0, 0);
    rows->setSpacing(kit::px(kit::Spacing::XS));
    rows->addStretch();
    scroll->setWidget(fields);
    fields->setAutoFillBackground(false);
    layout->addWidget(scroll);
    auto* add = new kit::KButton(tr("Add Tag"), dialog);
    add->setObjectName(QStringLiteral("assetsAddTagButton"));
    layout->addWidget(add, 0, Qt::AlignLeft);
    auto editors = std::make_shared<std::vector<QPointer<kit::KLineEdit>>>();
    const auto addTag = [dialog, scroll, fields, rows, add, editors](const QString& text) {
        if (editors->size() >= 64)
            return;
        auto* row = new QWidget(fields);
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(kit::px(kit::Spacing::XS));
        auto* edit = new kit::KLineEdit(text, row);
        edit->setObjectName(QStringLiteral("assetsTagField"));
        edit->setAccessibleName(tr("Tag"));
        edit->setPlaceholderText(tr("Tag"));
        auto* remove = new kit::KIconButton(row);
        remove->setIcon(kit::icon(kit::IconId::Close, kit::IconRole::Chrome));
        remove->setToolTip(tr("Remove Tag"));
        remove->setAccessibleName(tr("Remove Tag"));
        rowLayout->addWidget(edit, 1);
        rowLayout->addWidget(remove);
        rows->insertWidget(rows->count() - 1, row);
        scroll->updateGeometry();
        editors->emplace_back(edit);
        connect(remove, &kit::KIconButton::clicked, dialog, [editors, add, row, edit, scroll] {
            std::erase(*editors, QPointer<kit::KLineEdit>(edit));
            row->hide();
            row->deleteLater();
            scroll->updateGeometry();
            add->setEnabled(true);
        });
        add->setEnabled(editors->size() < 64);
        edit->setFocus();
    };
    for (const auto& tag : tags)
        addTag(QString::fromStdString(tag));
    if (tags.empty())
        addTag({});
    connect(add, &kit::KButton::clicked, dialog, [addTag] { addTag({}); });
    dialog->addButton(kit::KDialog::Role::Accept, tr("Apply"))
        ->setObjectName(QStringLiteral("assetsApplyTagsButton"));
    dialog->addButton(kit::KDialog::Role::Reject, tr("Cancel"));
    // Accept-time (and live) check: tags read from a project that has since changed are not
    // written over it.
    dialog->setValidator([check]() -> std::optional<kit::KRefusal> {
        if (check->projectChanged())
            return kit::KRefusal{nullptr, tr("The project changed while editing tags. Cancel and "
                                             "open Edit Tags again.")};
        return std::nullopt;
    });
    connect(&session_, &CompositionSession::snapshotChanged, dialog, &kit::KDialog::revalidate);
    if (!editors->empty())
        dialog->setInitialFocus(editors->front());
    QPointer<CompositionSession> session(&session_);
    dialog->open([session, check, ids = std::move(ids), editors](const kit::KDialog::Role role) {
        if (role != kit::KDialog::Role::Accept || !session || check->projectChanged())
            return;
        std::vector<std::string> edited;
        for (const auto& field : *editors) {
            const auto tag = field ? field->text().trimmed() : QString{};
            if (!tag.isEmpty())
                edited.push_back(tag.toStdString());
        }
        commands::Transaction transaction("Edit Asset Tags", check->snapshot().revision());
        transaction.emplace<commands::SetAssetTags>(ids, std::move(edited));
        (void)session->executeTransaction(std::move(transaction));
    });
}
std::optional<document::AssetInterpretation>& assets::copiedInterpretation() {
    static std::optional<document::AssetInterpretation> copied;
    return copied;
}
void AssetsEditor::copyInterpretation(const document::AssetId id) {
    if (const auto* asset = session_.snapshot().project().findAsset(id);
        asset && commands::isInterpretableFootage(asset->kind))
        assets::copiedInterpretation() = asset->interpretation;
}
void AssetsEditor::pasteInterpretation() {
    const auto& copied = assets::copiedInterpretation();
    auto targets = selectedAssets();
    if (!copied || targets.empty())
        return;
    commands::Transaction transaction("Paste Interpretation", session_.snapshot().revision());
    transaction.emplace<commands::SetAssetInterpretation>(
        std::move(targets), commands::AssetInterpretationChange::everything(*copied));
    (void)session_.executeTransaction(std::move(transaction));
}
void AssetsEditor::interpretFootage(const document::AssetId id) {
    const auto snapshot = session_.snapshot();
    const auto* asset = snapshot.project().findAsset(id);
    auto* controller = session_.assetController();
    if (!asset || !commands::isInterpretableFootage(asset->kind) ||
        (controller != nullptr && !controller->acceptsEdits()))
        return;
    // The clicked asset first, then the rest of the selected footage.
    std::vector<document::AssetId> targets{id};
    std::vector<document::AssetKind> kinds{asset->kind};
    for (const auto selected : selectedAssets())
        if (const auto* record = snapshot.project().findAsset(selected);
            selected != id && record && commands::isInterpretableFootage(record->kind)) {
            targets.push_back(selected);
            kinds.push_back(record->kind);
        }
    // What each target's interpretation was when the dialog opened: an edit elsewhere meanwhile
    // makes the dialog stale rather than overwriting it.
    std::vector<std::pair<document::AssetId, document::AssetInterpretation>> initial;
    for (const auto target : targets)
        initial.emplace_back(target, snapshot.project().findAsset(target)->interpretation);
    const auto colorChoices = [](const AssetController* assets) {
        std::vector<InterpretColorChoice> choices;
        if (assets == nullptr || assets->inputColorSpaces() == nullptr)
            return choices;
        auto ordered = *assets->inputColorSpaces();
        std::ranges::sort(ordered, [](const auto& left, const auto& right) {
            return std::tie(left.family, left.id) < std::tie(right.family, right.id);
        });
        for (const auto& space : ordered) {
            const auto name = QString::fromStdString(space.id);
            const auto family = QString::fromStdString(space.family);
            choices.push_back({family.isEmpty() ? name : family + QStringLiteral(" / ") + name,
                               QStringLiteral("id:") + name});
        }
        return choices;
    };
    const auto colorStatus = [](const AssetController* assets) {
        if (assets != nullptr && assets->inputColorSpaces() != nullptr)
            return tr("Auto reads the file's color tags (or the visible assumption for untagged "
                      "video). Choose any input space from the project's OCIO configuration.");
        if (assets != nullptr && !assets->inputColorCatalogDiagnostic().isEmpty())
            return assets->inputColorCatalogDiagnostic();
        return tr("Preparing OCIO color spaces…");
    };
    auto* dialog = new InterpretFootageDialog(
        *asset, targets.size(), kinds, colorChoices(controller),
        controller != nullptr ? controller->inputColorSpaceName(id) : QString{}, *this);
    dialog->setColorChoices(colorChoices(controller), colorStatus(controller));
    QPointer<AssetController> guardedController(controller);
    QPointer<InterpretFootageDialog> guardedDialog(dialog);
    if (controller != nullptr) {
        connect(controller, &AssetController::changed, dialog,
                [guardedController, guardedDialog, colorChoices, colorStatus] {
                    if (guardedController && guardedDialog)
                        guardedDialog->setColorChoices(colorChoices(guardedController),
                                                       colorStatus(guardedController));
                });
        controller->requestInputColorSpaces();
    }
    auto check = std::make_shared<ProjectChangeCheck>(session_);
    QPointer<CompositionSession> session(&session_);
    const auto stale = [session, check, initial] {
        if (!session || check->documentRebound())
            return true;
        return std::ranges::any_of(initial, [&](const auto& entry) {
            return !detail::assetInputPickerTargetMatches(check->snapshot().project().id(),
                                                          entry.first, entry.second,
                                                          session->snapshot().project());
        });
    };
    connect(&session_, &CompositionSession::snapshotChanged, dialog, &kit::KDialog::revalidate);
    dialog->open([session, stale, guardedDialog, guardedController,
                  targets](const kit::KDialog::Role role) {
        if (role != kit::KDialog::Role::Accept || !session || !guardedDialog)
            return;
        if (stale()) {
            emit session->commandRejected(
                tr("The footage changed while Interpret Footage was open. Open it again."));
            return;
        }
        auto change = guardedDialog->change();
        if (!change || change->empty())
            return;
        if (change->inputColor && !change->inputColor->second.empty()) {
            const auto* spaces =
                guardedController ? guardedController->inputColorSpaces() : nullptr;
            if (!spaces || std::ranges::none_of(*spaces, [&](const auto& space) {
                    return space.id == change->inputColor->second;
                })) {
                emit session->commandRejected(tr("That OCIO color space is unavailable. Reopen "
                                                 "Interpret Footage once the configuration is "
                                                 "ready."));
                return;
            }
        }
        commands::Transaction transaction("Interpret Footage", session->snapshot().revision());
        transaction.emplace<commands::SetAssetInterpretation>(targets, std::move(*change));
        (void)session->executeTransaction(std::move(transaction));
    });
}
void AssetsEditor::removeSelected() {
    ProjectItemSelection selection;
    for (QTreeWidgetItemIterator it(tree_, QTreeWidgetItemIterator::Selected); *it; ++it) {
        // A selected folder already accounts for what it holds.
        bool insideSelectedFolder = false;
        for (auto* parent = (*it)->parent(); parent; parent = parent->parent())
            insideSelectedFolder = insideSelectedFolder || parent->isSelected();
        if (insideSelectedFolder)
            continue;
        if (const auto asset = assets::assetId(*it); asset.isValid())
            selection.assets.push_back(asset);
        else if (const auto composition = assets::compositionId(*it); composition.isValid())
            selection.compositions.push_back(composition);
        else if (const auto folder = assets::folderId(*it))
            selection.folders.push_back(*folder);
    }
    // The same path, and the same in-use warning, as Composition > Delete Composition.
    requestDeleteProjectItems(session_, *this, std::move(selection));
}
void AssetsEditor::revealInFileManager(const document::AssetId id) {
    const auto* controller = session_.assetController();
    const auto path = controller ? controller->sourcePath(id) : std::filesystem::path{};
    // Selects the file where the platform's file manager can, else opens its folder (#380).
    ui::revealInFileManager(path, this);
}
void AssetsEditor::newCompositionFromSelection() {
    const auto snapshot = session_.snapshot();
    const auto& project = snapshot.project();
    std::vector<document::AssetId> footage;
    for (const auto id : selectedAssets())
        if (const auto* asset = project.findAsset(id);
            asset && asset->kind != document::AssetKind::Lut)
            footage.push_back(id);
    if (footage.empty())
        return;
    // Sized from the first visual footage item, like a layer-based compositor; the active
    // composition (or the defaults) supplies what the footage does not state. The command is shared
    // with bloom.composition.add-from-assets. An imported SVG becomes its artwork (#646), which the
    // controller reads on its worker.
    const auto* current = session_.composition();
    const auto name = tr("Composition %1").arg(project.compositions().size() + 1).toStdString();
    const auto folder = assets::targetFolder(tree_->currentItem());
    const QPointer<AssetsEditor> self(this);
    const auto opened = [self](const commands::CommandResult& result) {
        if (self == nullptr)
            return;
        if (const auto id = result.outputId<document::CompositionId>(commands::kAddCompositionOutput))
            self->openComposition(*id);
    };
    const auto templateComposition = current != nullptr ? std::optional(session_.compositionId()) : std::nullopt;
    if (auto* controller = session_.assetController()) {
        controller->createCompositionFromAssets(std::move(footage), templateComposition, name, folder, opened);
        return;
    }
    commands::Transaction transaction("New Composition from Selection", snapshot.revision());
    transaction.emplace<commands::AddCompositionFromFootage>(footage, templateComposition, name, folder);
    const auto result = session_.executeTransaction(std::move(transaction));
    if (result.succeeded())
        opened(result);
}
void AssetsEditor::addSelectionToComposition() {
    auto* controller = session_.assetController();
    if (controller == nullptr || session_.composition() == nullptr)
        return;
    const auto& project = session_.snapshot().project();
    std::vector<commands::LayerDropItem> items;
    for (QTreeWidgetItemIterator it(tree_, QTreeWidgetItemIterator::Selected); *it; ++it) {
        if (const auto asset = assets::assetId(*it); asset.isValid()) {
            if (commands::droppableAsLayer(project, session_.compositionId(), asset))
                items.emplace_back(asset);
        } else if (const auto composition = assets::compositionId(*it); composition.isValid()) {
            if (commands::droppableAsLayer(project, session_.compositionId(), composition))
                items.emplace_back(composition);
        }
    }
    if (items.empty())
        return;
    // On top at the playhead, the same placement as a drop with no insertion point.
    const QPointer<CompositionSession> session(&session_);
    controller->addToComposition(std::move(items), {.stack = commands::LayerDropPlacement::Stack::Top,
                                                    .before = {},
                                                    .start = session_.currentTime(),
                                                    .sequence = false,
                                                    .beforeLayer = {}},
                                 [session](const commands::CommandResult& result) {
                                     if (session == nullptr)
                                         return;
                                     std::vector<document::LayerId> layers;
                                     for (const auto& output : result.outputs)
                                         if (output.output.name == "layer")
                                             if (const auto* id = std::get_if<document::LayerId>(&output.output.id))
                                                 layers.push_back(*id);
                                     for (auto it = layers.rbegin(); it != layers.rend(); ++it)
                                         session->selectLayer(*it, it != layers.rbegin());
                                 });
}
} // namespace bloom::ui
