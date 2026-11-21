#include "properties_subtitles.hpp"

#include "subtitles_editing.hpp"
#include "subtitles_inspector.hpp"

#include <bloom/document/subtitles_nodes.hpp>
#include <bloom/subtitles/subtitles.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dialog.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/file_chooser.hpp>
#include <bloom/ui/kit/row.hpp>
#include <bloom/ui/kit/switch_control.hpp>
#include <bloom/ui/kit/text_field.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/timeline_ruler.hpp>

#include <QAbstractTableModel>
#include <QAction>
#include <QClipboard>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QMenu>
#include <QPointer>
#include <QSortFilterProxyModel>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QVBoxLayout>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <numeric>
#include <set>
#include <variant>

namespace bloom::ui {
namespace {

namespace editing = subtitles_editing;

QString tr(const char* text) { return QCoreApplication::translate("subtitles", text); }

enum Column : int { Number, Start, End, Duration, Text, Speaker, Checks, ColumnCount };

// The cue list as a table. It holds the list's cue handles (shared with the document, so a copy is
// one pointer per cue) and reads every cell on demand: the view asks only for the rows it shows.
class CueListModel final : public QAbstractTableModel {
  public:
    CueListModel(CompositionSession& session, const document::NodeId node, QObject* parent)
        : QAbstractTableModel(parent), session_(session), node_(node) {}

    void refresh() {
        const auto source = editing::cueSource(session_, node_);
        std::vector<document::CueHandle> cues;
        if (source.list != nullptr)
            cues = source.list->cues;
        const bool editable = source.kind == editing::CueSourceKind::Typed;
        parameter_ = source.parameter;
        if (cues.size() != cues_.size()) {
            beginResetModel();
            cues_ = std::move(cues);
            editable_ = editable;
            endResetModel();
            return;
        }
        const bool same = std::ranges::equal(cues, cues_);
        cues_ = std::move(cues);
        if (!same || editable != editable_) {
            editable_ = editable;
            if (!cues_.empty())
                emit dataChanged(index(0, 0), index(rowCount() - 1, ColumnCount - 1));
        }
    }
    // The checks changed, or the time reading did.
    void refreshCells() {
        if (!cues_.empty())
            emit dataChanged(index(0, 0), index(rowCount() - 1, ColumnCount - 1));
    }
    void setChecks(std::map<std::uint64_t, QStringList> checks) {
        if (checks == checks_)
            return;
        checks_ = std::move(checks);
        if (!cues_.empty())
            emit dataChanged(index(0, Checks), index(rowCount() - 1, Checks));
    }

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : static_cast<int>(cues_.size());
    }
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : ColumnCount;
    }
    [[nodiscard]] const document::Cue* cue(const int row) const {
        return row >= 0 && row < rowCount() ? cues_[static_cast<std::size_t>(row)].get() : nullptr;
    }
    [[nodiscard]] int rowOf(const std::uint64_t id) const {
        for (std::size_t row = 0; row < cues_.size(); ++row)
            if (cues_[row]->id == id)
                return static_cast<int>(row);
        return -1;
    }

    [[nodiscard]] QVariant headerData(const int section, const Qt::Orientation orientation,
                                      const int role) const override {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
            return {};
        switch (section) {
        case Number:
            return tr("#");
        case Start:
            return tr("Start");
        case End:
            return tr("End");
        case Duration:
            return tr("Duration");
        case Text:
            return tr("Text");
        case Speaker:
            return tr("Speaker");
        case Checks:
            return tr("Checks");
        default:
            return {};
        }
    }

    [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override {
        auto flags = QAbstractTableModel::flags(index);
        const auto column = index.column();
        if (editable_ && (column == Start || column == End || column == Text || column == Speaker))
            flags |= Qt::ItemIsEditable;
        return flags;
    }

    [[nodiscard]] QVariant data(const QModelIndex& index, const int role) const override {
        const auto* item = cue(index.row());
        if (item == nullptr)
            return {};
        const auto checks = checks_.find(item->id);
        switch (role) {
        case Qt::DisplayRole:
            switch (index.column()) {
            case Number:
                return index.row() + 1;
            case Start:
                return editing::cueTimeLabel(session_, parameter_, item->start);
            case End:
                return editing::cueTimeLabel(session_, parameter_, item->end);
            case Duration:
                return editing::cueDurationLabel(session_, item->start, item->end);
            case Text:
                return editing::plainCueText(*item, true);
            case Speaker:
                return QString::fromStdString(item->speaker);
            case Checks:
                return checks == checks_.end() ? QString{}
                                               : checks->second.join(QStringLiteral("; "));
            default:
                return {};
            }
        case Qt::EditRole:
            switch (index.column()) {
            case Start:
                return editing::cueTimeLabel(session_, parameter_, item->start);
            case End:
                return editing::cueTimeLabel(session_, parameter_, item->end);
            case Text:
                return QString::fromStdString(item->text);
            case Speaker:
                return QString::fromStdString(item->speaker);
            default:
                return {};
            }
        case Qt::ToolTipRole:
            if (index.column() == Checks && checks != checks_.end())
                return checks->second.join(QLatin1Char('\n'));
            if (index.column() == Text)
                return editing::plainCueText(*item).replace(QStringLiteral(" / "),
                                                            QStringLiteral("\n"));
            return {};
        case Qt::ForegroundRole:
            if (index.column() == Checks)
                return kit::color(kit::Color::Warn);
            if (index.column() == Number || index.column() == Duration)
                return kit::color(kit::Color::Muted);
            return {};
        case Qt::TextAlignmentRole:
            if (index.column() == Number)
                return QVariant::fromValue(Qt::AlignRight | Qt::AlignVCenter);
            return {};
        default:
            return {};
        }
    }

    bool setData(const QModelIndex& index, const QVariant& value, const int role) override {
        const auto* item = cue(index.row());
        if (item == nullptr || role != Qt::EditRole || !editable_)
            return false;
        commands::CueEdit edit;
        QString label;
        const auto text = value.toString();
        switch (index.column()) {
        case Start:
        case End: {
            const auto time = editing::parseCueTimeLabel(session_, parameter_, text);
            if (!time) {
                emit session_.commandRejected(
                    tr("\"%1\" is not a frame number or a timecode.").arg(text));
                return false;
            }
            if (index.column() == Start) {
                if (*time == item->start)
                    return false;
                edit.start = *time;
                label = tr("Set Cue Start");
            } else {
                if (*time == item->end)
                    return false;
                edit.end = *time;
                label = tr("Set Cue End");
            }
            break;
        }
        case Text:
            if (text.toStdString() == item->text)
                return false;
            edit.text = text.toStdString();
            label = tr("Edit Cue Text");
            break;
        case Speaker:
            if (text.trimmed().toStdString() == item->speaker)
                return false;
            edit.speaker = text.trimmed().toStdString();
            label = tr("Set Cue Speaker");
            break;
        default:
            return false;
        }
        return editing::editCue(session_, node_, item->id, std::move(edit), label);
    }

  private:
    CompositionSession& session_;
    document::NodeId node_;
    document::ParameterId parameter_;
    std::vector<document::CueHandle> cues_;
    std::map<std::uint64_t, QStringList> checks_;
    bool editable_ = false;
};

// The filter matches a cue's text or speaker, ignoring case.
class CueFilter final : public QSortFilterProxyModel {
  public:
    using QSortFilterProxyModel::QSortFilterProxyModel;
    void setNeedle(const QString& needle) {
        beginFilterChange();
        needle_ = needle.trimmed();
        endFilterChange(QSortFilterProxyModel::Direction::Rows);
    }

  protected:
    [[nodiscard]] bool filterAcceptsRow(const int row, const QModelIndex& parent) const override {
        if (needle_.isEmpty())
            return true;
        const auto* model = sourceModel();
        for (const int column : {static_cast<int>(Text), static_cast<int>(Speaker)})
            if (model->index(row, column, parent)
                    .data(Qt::EditRole)
                    .toString()
                    .contains(needle_, Qt::CaseInsensitive))
                return true;
        return false;
    }

  private:
    QString needle_;
};

// Cells edit in place with the kit fields: a line for times and the speaker, a few lines for the
// text. Return (Ctrl+Return in the text), Escape, Tab and focus loss all commit (Escape confirms
// everywhere); text that is not a time is refused by the model with its reason.
class CueDelegate final : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem&,
                          const QModelIndex& index) const override {
        if (index.column() == Text) {
            auto* area = new kit::KTextArea(parent);
            area->setObjectName(QStringLiteral("subtitlesCueTextEditor"));
            area->setAccessibleName(tr("Cue text"));
            area->setToolTip(tr("WebVTT cue text: <i>, <b>, <u>, <c.yellow>; Ctrl+Return commits"));
            return area;
        }
        auto* line = new kit::KLineEdit(parent);
        line->setObjectName(index.column() == Speaker ? QStringLiteral("subtitlesCueSpeakerEditor")
                                                      : QStringLiteral("subtitlesCueTimeEditor"));
        if (index.column() != Speaker)
            line->setToolTip(tr("A frame number or a timecode (HH:MM:SS:FF)"));
        return line;
    }
    void setEditorData(QWidget* editor, const QModelIndex& index) const override {
        const auto text = index.data(Qt::EditRole).toString();
        if (auto* area = qobject_cast<QPlainTextEdit*>(editor))
            area->setPlainText(text);
        else if (auto* line = qobject_cast<QLineEdit*>(editor))
            line->setText(text);
    }
    void setModelData(QWidget* editor, QAbstractItemModel* model,
                      const QModelIndex& index) const override {
        if (auto* area = qobject_cast<QPlainTextEdit*>(editor))
            model->setData(index, area->toPlainText(), Qt::EditRole);
        else if (auto* line = qobject_cast<QLineEdit*>(editor))
            model->setData(index, line->text(), Qt::EditRole);
    }
    void updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& option,
                              const QModelIndex& index) const override {
        auto rect = option.rect;
        if (index.column() == Text)
            rect.setHeight(std::max(rect.height(), kit::px(kit::Size::ListRow) * 4));
        editor->setGeometry(rect);
    }

  protected:
    bool eventFilter(QObject* object, QEvent* event) override {
        auto* editor = qobject_cast<QWidget*>(object);
        if (editor != nullptr && event->type() == QEvent::KeyPress) {
            const auto* key = static_cast<QKeyEvent*>(event);
            const bool area = qobject_cast<QPlainTextEdit*>(editor) != nullptr;
            const bool enter = key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter;
            if (key->key() == Qt::Key_Escape ||
                (enter && (!area || (key->modifiers() & Qt::ControlModifier)))) {
                emit commitData(editor);
                emit closeEditor(editor, QAbstractItemDelegate::NoHint);
                return true;
            }
        }
        return QStyledItemDelegate::eventFilter(object, event);
    }
};

kit::KButton* actionButton(const QString& text, const QString& name, const QString& tip,
                           QWidget* parent) {
    auto* button = new kit::KButton(text, parent);
    button->setObjectName(name);
    button->setAccessibleName(text);
    button->setToolTip(tip);
    button->setControlSize(kit::KButton::ControlSize::Compact);
    return button;
}

QString describeFailure(const subtitles::LoadFailure& failure) {
    QString text = QString::fromStdString(failure.message);
    if (failure.line > 0)
        text = tr("Line %1, column %2: %3").arg(failure.line).arg(failure.column).arg(text);
    return text;
}

QString describeReport(const subtitles::Report& report) {
    return QString::fromStdString(subtitles::describe(report));
}

class SubtitlesCuePanel final : public QWidget {
  public:
    SubtitlesCuePanel(CompositionSession& session, const document::NodeId node, QWidget* parent)
        : QWidget(parent), session_(session), node_(node) {
        setObjectName(QStringLiteral("subtitlesCuePanel"));
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(kit::px(kit::Spacing::XS));

        // Source.
        sourceState_ = new kit::KLabel(this);
        sourceState_->setObjectName(QStringLiteral("subtitlesSourceState"));
        sourceState_->setWordWrap(true);
        auto* sourceRow = new kit::KPropertyRow(kit::makePropertyRowLabel(tr("Source"), this),
                                                nullptr, {sourceState_}, this);
        sourceRow->setProperty("rowLabel", tr("Source"));
        layout->addWidget(sourceRow);
        auto* sourceButtons = new QHBoxLayout;
        sourceButtons->setSpacing(kit::px(kit::Spacing::XS));
        load_ = actionButton(tr("Load File…"), QStringLiteral("subtitlesLoadFileButton"),
                             tr("Read an SRT, WebVTT, TTML or ASS file into the cues"), this);
        reload_ = actionButton(tr("Reload"), QStringLiteral("subtitlesReloadButton"),
                               tr("Read the linked file again"), this);
        unlink_ = actionButton(tr("Unlink"), QStringLiteral("subtitlesUnlinkButton"),
                               tr("Keep the cues and stop following the file, so they can be "
                                  "edited here"),
                               this);
        sourceButtons->addWidget(load_);
        sourceButtons->addWidget(reload_);
        sourceButtons->addWidget(unlink_);
        sourceButtons->addStretch();
        layout->addLayout(sourceButtons);
        report_ = new kit::KLabel(this);
        report_->setObjectName(QStringLiteral("subtitlesLoadReport"));
        report_->setWordWrap(true);
        report_->setTone(kit::Color::Muted);
        layout->addWidget(report_);
        refusal_ = new kit::KLabel(this);
        refusal_->setObjectName(QStringLiteral("subtitlesEditRefusal"));
        refusal_->setWordWrap(true);
        refusal_->setTone(kit::Color::Muted);
        layout->addWidget(refusal_);

        // Actions.
        auto* actions = new QHBoxLayout;
        actions->setSpacing(kit::px(kit::Spacing::XS));
        add_ = actionButton(tr("Add"), QStringLiteral("subtitlesAddCueButton"),
                            tr("Add a cue at the playhead (Insert)"), this);
        delete_ = actionButton(tr("Delete"), QStringLiteral("subtitlesDeleteCueButton"),
                               tr("Delete the selected cues (Delete)"), this);
        split_ = actionButton(tr("Split"), QStringLiteral("subtitlesSplitCueButton"),
                              tr("Split the selected cue at the playhead (Ctrl+B)"), this);
        merge_ = actionButton(tr("Merge"), QStringLiteral("subtitlesMergeCueButton"),
                              tr("Merge the selected cue with the next one (Ctrl+J)"), this);
        for (auto* button : {add_, delete_, split_, merge_})
            actions->addWidget(button);
        actions->addStretch();
        layout->addLayout(actions);
        auto* timing = new QHBoxLayout;
        timing->setSpacing(kit::px(kit::Spacing::XS));
        setStart_ = actionButton(tr("Set Start"), QStringLiteral("subtitlesSetStartButton"),
                                 tr("Start the selected cue at the playhead ([)"), this);
        setEnd_ = actionButton(tr("Set End"), QStringLiteral("subtitlesSetEndButton"),
                               tr("End the selected cue at the playhead (])"), this);
        shift_ = actionButton(tr("Shift…"), QStringLiteral("subtitlesShiftButton"),
                              tr("Move cues earlier or later by a time"), this);
        scale_ = actionButton(tr("Scale…"), QStringLiteral("subtitlesScaleButton"),
                              tr("Convert cue timing between frame rates"), this);
        for (auto* button : {setStart_, setEnd_, shift_, scale_})
            timing->addWidget(button);
        timing->addStretch();
        layout->addLayout(timing);

        auto* tools = new QHBoxLayout;
        tools->setSpacing(kit::px(kit::Spacing::XS));
        filter_ = new kit::KSearchField(this);
        filter_->setObjectName(QStringLiteral("subtitlesCueFilter"));
        filter_->setPlaceholderText(tr("Filter cues"));
        filter_->setAccessibleName(tr("Filter cues by text or speaker"));
        editAsText_ = actionButton(tr("Edit as Text"), QStringLiteral("subtitlesEditAsTextButton"),
                                   tr("Show the cues as SRT or WebVTT text to edit or paste"),
                                   this);
        editAsText_->setCheckable(true);
        copy_ = actionButton(tr("Copy"), QStringLiteral("subtitlesCopyButton"),
                             tr("Copy the cues as SRT or WebVTT"), this);
        copySrt_ = new QAction(tr("Copy as SRT"), this);
        copySrt_->setObjectName(QStringLiteral("subtitlesCopySrtAction"));
        copyVtt_ = new QAction(tr("Copy as WebVTT"), this);
        copyVtt_->setObjectName(QStringLiteral("subtitlesCopyVttAction"));
        connect(copySrt_, &QAction::triggered, this, [this] { copyAs(subtitles::Format::Srt); });
        connect(copyVtt_, &QAction::triggered, this,
                [this] { copyAs(subtitles::Format::WebVtt); });
        tools->addWidget(filter_, 1);
        tools->addWidget(editAsText_);
        tools->addWidget(copy_);
        layout->addLayout(tools);

        // The list and the text editor share one place.
        pages_ = new QStackedWidget(this);
        model_ = new CueListModel(session_, node_, this);
        proxy_ = new CueFilter(this);
        proxy_->setSourceModel(model_);
        table_ = new QTableView(pages_);
        table_->setObjectName(QStringLiteral("subtitlesCueTable"));
        table_->setAccessibleName(tr("Cues"));
        table_->setModel(proxy_);
        table_->setItemDelegate(new CueDelegate(table_));
        table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
        table_->setEditTriggers(QAbstractItemView::DoubleClicked |
                                QAbstractItemView::EditKeyPressed);
        table_->setAlternatingRowColors(true);
        table_->setWordWrap(false);
        table_->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
        table_->verticalHeader()->setVisible(false);
        table_->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
        table_->verticalHeader()->setDefaultSectionSize(kit::px(kit::Size::ListRow));
        auto* header = table_->horizontalHeader();
        header->setHighlightSections(false);
        header->setStretchLastSection(false);
        header->setSectionResizeMode(QHeaderView::Interactive);
        header->setSectionResizeMode(Text, QHeaderView::Stretch);
        header->setMinimumSectionSize(kit::px(kit::Size::ListRow));
        // Ten rows show; the list scrolls inside its own frame, so a long list never makes the
        // Properties panel tall.
        table_->setFixedHeight(header->sizeHint().height() + kit::px(kit::Size::ListRow) * 10 +
                               table_->frameWidth() * 2);
        pages_->addWidget(table_);

        auto* textPage = new QWidget(pages_);
        auto* textLayout = new QVBoxLayout(textPage);
        textLayout->setContentsMargins(0, 0, 0, 0);
        textLayout->setSpacing(kit::px(kit::Spacing::XS));
        format_ = new kit::KDropdown(textPage);
        format_->setObjectName(QStringLiteral("subtitlesTextFormat"));
        format_->setAccessibleName(tr("Text format"));
        format_->addItem(tr("SubRip (SRT)"), static_cast<int>(subtitles::Format::Srt));
        format_->addItem(tr("WebVTT"), static_cast<int>(subtitles::Format::WebVtt));
        auto* formatRow = new kit::KPropertyRow(kit::makePropertyRowLabel(tr("Format"), textPage),
                                                nullptr, {format_}, textPage);
        formatRow->setProperty("rowLabel", tr("Format"));
        textLayout->addWidget(formatRow);
        text_ = new kit::KTextArea(textPage);
        text_->setObjectName(QStringLiteral("subtitlesTextEditor"));
        text_->setAccessibleName(tr("Cues as text"));
        text_->setLineWrapMode(QPlainTextEdit::NoWrap);
        text_->setMinimumHeight(kit::px(kit::Size::ListRow) * 10);
        textLayout->addWidget(text_, 1);
        errors_ = new kit::KLabel(textPage);
        errors_->setObjectName(QStringLiteral("subtitlesTextErrors"));
        errors_->setWordWrap(true);
        errors_->setTone(kit::Color::Error);
        errors_->hide();
        textLayout->addWidget(errors_);
        auto* textButtons = new QHBoxLayout;
        apply_ = actionButton(tr("Apply"), QStringLiteral("subtitlesTextApplyButton"),
                              tr("Read the text into the cues as one step"), textPage);
        apply_->setVariant(kit::KButton::Variant::Primary);
        auto* cancel = actionButton(tr("Cancel"), QStringLiteral("subtitlesTextCancelButton"),
                                    tr("Leave the text without changing the cues"), textPage);
        textButtons->addStretch();
        textButtons->addWidget(cancel);
        textButtons->addWidget(apply_);
        textLayout->addLayout(textButtons);
        pages_->addWidget(textPage);
        layout->addWidget(pages_);

        // Keys while the list holds focus (docs/ux/interaction-model.md, "Subtitles").
        const auto key = [this](const QString& name, const QKeySequence& sequence, auto handler) {
            auto* action = new QAction(table_);
            action->setObjectName(name);
            action->setShortcut(sequence);
            action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
            table_->addAction(action);
            connect(action, &QAction::triggered, this, handler);
        };
        key(QStringLiteral("subtitlesAddCueAction"), QKeySequence(Qt::Key_Insert),
            [this] { addCue(); });
        key(QStringLiteral("subtitlesDeleteCueAction"), QKeySequence(Qt::Key_Delete),
            [this] { deleteCues(); });
        key(QStringLiteral("subtitlesDeleteCueBackspaceAction"), QKeySequence(Qt::Key_Backspace),
            [this] { deleteCues(); });
        key(QStringLiteral("subtitlesSplitCueAction"), QKeySequence(Qt::CTRL | Qt::Key_B),
            [this] { onCurrent(editing::splitCueAtPlayhead); });
        key(QStringLiteral("subtitlesMergeCueAction"), QKeySequence(Qt::CTRL | Qt::Key_J),
            [this] { onCurrent(editing::mergeCueWithNext); });
        key(QStringLiteral("subtitlesSetStartAction"), QKeySequence(Qt::Key_BracketLeft),
            [this] { onCurrent(editing::setCueStartToPlayhead); });
        key(QStringLiteral("subtitlesSetEndAction"), QKeySequence(Qt::Key_BracketRight),
            [this] { onCurrent(editing::setCueEndToPlayhead); });

        connect(load_, &QAbstractButton::clicked, this, [this] { chooseFile(); });
        connect(reload_, &QAbstractButton::clicked, this, [this] { reloadFile(); });
        connect(unlink_, &QAbstractButton::clicked, this,
                [this] { (void)editing::unlinkCues(session_, node_); });
        connect(add_, &QAbstractButton::clicked, this, [this] { addCue(); });
        connect(delete_, &QAbstractButton::clicked, this, [this] { deleteCues(); });
        connect(split_, &QAbstractButton::clicked, this,
                [this] { onCurrent(editing::splitCueAtPlayhead); });
        connect(merge_, &QAbstractButton::clicked, this,
                [this] { onCurrent(editing::mergeCueWithNext); });
        connect(setStart_, &QAbstractButton::clicked, this,
                [this] { onCurrent(editing::setCueStartToPlayhead); });
        connect(setEnd_, &QAbstractButton::clicked, this,
                [this] { onCurrent(editing::setCueEndToPlayhead); });
        connect(shift_, &QAbstractButton::clicked, this, [this] { openShift(); });
        connect(scale_, &QAbstractButton::clicked, this, [this] { openScale(); });
        connect(copy_, &QAbstractButton::clicked, this, [this] { openCopyMenu(); });
        connect(editAsText_, &QAbstractButton::toggled, this, [this](bool on) { showText(on); });
        connect(format_, &kit::KDropdown::activated, this, [this](int) { fillText(); });
        connect(apply_, &QAbstractButton::clicked, this, [this] { applyText(); });
        connect(cancel, &QAbstractButton::clicked, this, [this] { editAsText_->setChecked(false); });
        connect(filter_, &QLineEdit::textChanged, this,
                [this](const QString& text) { proxy_->setNeedle(text); });
        connect(table_->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
                [this](const QModelIndex& current) {
                    if (syncing_)
                        return;
                    if (const auto* item = model_->cue(proxy_->mapToSource(current).row()))
                        session_.selectCue(node_, item->id);
                    updateActions();
                });
        connect(table_->selectionModel(), &QItemSelectionModel::selectionChanged, this,
                [this] { updateActions(); });

        connect(&session_, &CompositionSession::snapshotChanged, this, [this] { refresh(); });
        connect(&session_, &CompositionSession::currentTimeChanged, this,
                [this] { updateActions(); });
        connect(&session_, &CompositionSession::cueSelectionChanged, this,
                [this] { followCueSelection(); });
        auto& inspector = session_.subtitlesInspector();
        inspector.setDetailNodes(this, {node_});
        connect(&inspector, &SubtitlesInspector::inspected, this, [this] { applyInspection(); });
        refresh();
        applyInspection();
        followCueSelection();
    }

    ~SubtitlesCuePanel() override {
        // The session outlives every Properties panel it feeds.
        session_.subtitlesInspector().releaseDetailNodes(this);
    }

  private:
    void refresh() {
        model_->refresh();
        const auto source = editing::cueSource(session_, node_);
        const auto count = source.list == nullptr ? 0 : source.list->cues.size();
        switch (source.kind) {
        case editing::CueSourceKind::Typed:
            sourceState_->setText(tr("Typed in Bloom · %1 cues").arg(count));
            sourceState_->setToolTip({});
            break;
        case editing::CueSourceKind::Linked: {
            const auto* file = std::get_if<document::CueFileProvenance>(&source.list->provenance);
            sourceState_->setToolTip(file == nullptr ? QString{}
                                                     : QString::fromStdString(file->path));
            break;
        }
        case editing::CueSourceKind::Connected:
            sourceState_->setText(source.refusal);
            sourceState_->setToolTip({});
            break;
        case editing::CueSourceKind::Missing:
            sourceState_->setText(tr("No cues"));
            break;
        }
        refusal_->setText(source.kind == editing::CueSourceKind::Linked ? source.refusal
                                                                         : QString{});
        refusal_->setVisible(source.kind == editing::CueSourceKind::Linked);
        const auto report = session_.subtitlesLoadReport(node_);
        report_->setText(report.isEmpty()
                             ? QString{}
                             : tr("Last load: %1").arg(report.join(QStringLiteral("\n"))));
        report_->setVisible(!report.isEmpty());
        load_->setEnabled(source.kind != editing::CueSourceKind::Connected &&
                          source.kind != editing::CueSourceKind::Missing);
        reload_->setEnabled(source.kind == editing::CueSourceKind::Linked);
        unlink_->setEnabled(source.kind == editing::CueSourceKind::Linked);
        reload_->setVisible(source.kind == editing::CueSourceKind::Linked);
        unlink_->setVisible(source.kind == editing::CueSourceKind::Linked);
        copy_->setEnabled(source.list != nullptr && !source.list->cues.empty());
        applyInspection();
        updateActions();
        followCueSelection();
    }

    void applyInspection() {
        const auto source = editing::cueSource(session_, node_);
        const auto* inspected = session_.subtitlesInspector().result(node_);
        if (inspected != nullptr && inspected->checked)
            model_->setChecks(inspected->checks);
        if (source.kind != editing::CueSourceKind::Linked)
            return;
        const auto count = source.list->cues.size();
        QString state = tr("In sync");
        if (inspected != nullptr && inspected->linked)
            switch (inspected->fileState) {
            case host::LinkedFileState::InSync:
                break;
            case host::LinkedFileState::Changed:
                state = inspected->fileMessage.isEmpty()
                            ? tr("Changed")
                            : tr("Changed; it cannot be read: %1").arg(inspected->fileMessage);
                break;
            case host::LinkedFileState::Missing:
                state = tr("Missing; the cues saved in the project are kept");
                break;
            case host::LinkedFileState::Unreadable:
                state = tr("Cannot be read");
                break;
            }
        sourceState_->setText(tr("%1 · %2 · %3 cues").arg(source.fileName, state).arg(count));
    }

    [[nodiscard]] std::vector<std::uint64_t> selectedCues() const {
        std::vector<std::uint64_t> ids;
        for (const auto& row : table_->selectionModel()->selectedRows())
            if (const auto* item = model_->cue(proxy_->mapToSource(row).row()))
                ids.push_back(item->id);
        return ids;
    }
    [[nodiscard]] std::optional<std::uint64_t> currentCue() const {
        if (const auto selected = session_.selectedCue(); selected && selected->node == node_)
            return selected->cue;
        if (const auto* item = model_->cue(proxy_->mapToSource(table_->currentIndex()).row()))
            return item->id;
        return std::nullopt;
    }

    void updateActions() {
        const auto source = editing::cueSource(session_, node_);
        const bool typed = source.kind == editing::CueSourceKind::Typed;
        const auto current = currentCue();
        const auto* cue = current && source.list != nullptr ? source.list->findCue(*current)
                                                            : nullptr;
        const auto index = current && source.list != nullptr ? source.list->indexOf(*current)
                                                             : std::nullopt;
        const auto at = source.parameter.isValid() ? session_.keyTimeAtPlayhead(source.parameter)
                                                   : core::RationalTime{};
        add_->setEnabled(typed);
        delete_->setEnabled(typed && (!selectedCues().empty() || cue != nullptr));
        split_->setEnabled(typed && cue != nullptr && cue->start < at && at < cue->end);
        merge_->setEnabled(typed && index && *index + 1 < source.list->cues.size());
        setStart_->setEnabled(typed && cue != nullptr);
        setEnd_->setEnabled(typed && cue != nullptr);
        shift_->setEnabled(typed && source.list != nullptr && !source.list->cues.empty());
        scale_->setEnabled(shift_->isEnabled());
        editAsText_->setEnabled(source.list != nullptr);
        apply_->setEnabled(typed);
        table_->setToolTip(typed ? QString{} : source.refusal);
    }

    void followCueSelection() {
        const auto selected = session_.selectedCue();
        if (!selected || selected->node != node_)
            return;
        const auto row = model_->rowOf(selected->cue);
        if (row < 0)
            return;
        const auto index = proxy_->mapFromSource(model_->index(row, Text));
        if (!index.isValid() || table_->currentIndex().row() == index.row())
            return;
        syncing_ = true;
        table_->selectionModel()->setCurrentIndex(
            index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        table_->scrollTo(index);
        syncing_ = false;
        updateActions();
    }

    void addCue() {
        std::uint64_t added = 0;
        if (editing::addCueAtPlayhead(session_, node_, &added)) {
            followCueSelection();
            // The new cue's text is typed next.
            const auto row = model_->rowOf(added);
            const auto index = proxy_->mapFromSource(model_->index(row, Text));
            if (index.isValid())
                table_->edit(index);
        }
    }
    void deleteCues() {
        auto ids = selectedCues();
        if (ids.empty())
            if (const auto current = currentCue())
                ids.push_back(*current);
        (void)editing::removeCues(session_, node_, ids);
    }
    void onCurrent(bool (*action)(CompositionSession&, document::NodeId, std::uint64_t)) {
        if (const auto current = currentCue())
            (void)action(session_, node_, *current);
    }

    void openShift() {
        auto* dialog = new kit::KDialog(tr("Shift Cue Timing"), QStringLiteral("subtitlesShiftDialog"),
                                        *this);
        auto* form = dialog->addForm();
        auto* amount = new kit::KLineEdit(dialog);
        amount->setObjectName(QStringLiteral("subtitlesShiftAmount"));
        amount->setText(editing::timecodeReading() ? QStringLiteral("00:00:01:00")
                                                   : QStringLiteral("1"));
        amount->setToolTip(tr("A number of frames, or a timecode length (HH:MM:SS:FF)"));
        form->addRow(tr("By"), amount);
        auto* direction = new kit::KDropdown(dialog);
        direction->setObjectName(QStringLiteral("subtitlesShiftDirection"));
        direction->addItem(tr("Later"), 1);
        direction->addItem(tr("Earlier"), -1);
        form->addRow(tr("Direction"), direction);
        auto* which = scopeDropdown(*dialog);
        form->addRow(tr("Cues"), which);
        dialog->addButton(kit::KDialog::Role::Reject, tr("Cancel"));
        dialog->addButton(kit::KDialog::Role::Accept, tr("Shift"));
        const auto frames = [this, amount]() -> std::optional<std::int64_t> {
            const auto* composition = session_.composition();
            if (composition == nullptr)
                return std::nullopt;
            const auto rate = composition->format().frameRate();
            auto parsed = parseTimelineFrameLabel(amount->text().trimmed(), rate, false);
            if (!parsed)
                parsed = parseTimelineFrameLabel(amount->text().trimmed(), rate, true);
            if (!parsed || *parsed == 0)
                return std::nullopt;
            return static_cast<std::int64_t>(*parsed);
        };
        dialog->setValidator([amount, frames]() -> std::optional<kit::KRefusal> {
            if (!frames())
                return kit::KRefusal{amount, tr("Type a number of frames or a timecode length "
                                                "greater than zero")};
            return std::nullopt;
        });
        dialog->watch(amount);
        dialog->setInitialFocus(amount);
        const auto selected = selectedCues();
        connect(dialog, &QDialog::accepted, this, [this, frames, direction, which, selected] {
            const auto count = frames();
            const auto* composition = session_.composition();
            if (!count || composition == nullptr)
                return;
            const auto rate = composition->format().frameRate();
            const auto by = core::RationalTime::create(
                *count * direction->currentData().toInt() *
                    static_cast<std::int64_t>(rate.denominator()),
                static_cast<std::int64_t>(rate.numerator()));
            if (by)
                (void)editing::shiftCues(session_, node_,
                                         which->currentData().toInt() == 1 ? selected
                                                                           : std::vector<std::uint64_t>{},
                                         *by);
        });
        dialog->open();
    }

    kit::KDropdown* scopeDropdown(kit::KDialog& dialog) {
        auto* which = new kit::KDropdown(&dialog);
        which->setObjectName(QStringLiteral("subtitlesTimingScope"));
        const auto source = editing::cueSource(session_, node_);
        which->addItem(tr("All %1 cues").arg(source.list == nullptr ? 0 : source.list->cues.size()),
                       0);
        const auto selected = selectedCues();
        if (!selected.empty())
            which->addItem(tr("The %1 selected").arg(selected.size()), 1);
        return which;
    }

    void openScale() {
        auto* dialog = new kit::KDialog(tr("Scale Cue Timing"), QStringLiteral("subtitlesScaleDialog"),
                                        *this);
        auto* form = dialog->addForm();
        struct Rate final {
            const char* label;
            std::int64_t numerator;
            std::int64_t denominator;
        };
        static constexpr std::array<Rate, 8> rates{{{"23.976", 24000, 1001},
                                                    {"24", 24, 1},
                                                    {"25", 25, 1},
                                                    {"29.97", 30000, 1001},
                                                    {"30", 30, 1},
                                                    {"50", 50, 1},
                                                    {"59.94", 60000, 1001},
                                                    {"60", 60, 1}}};
        const auto rateDropdown = [dialog](const QString& name, const int current) {
            auto* dropdown = new kit::KDropdown(dialog);
            dropdown->setObjectName(name);
            for (int index = 0; index < static_cast<int>(rates.size()); ++index)
                dropdown->addItem(tr("%1 fps").arg(QString::fromLatin1(rates[static_cast<std::size_t>(index)].label)),
                                  index);
            dropdown->setCurrentIndex(current);
            return dropdown;
        };
        auto* from = rateDropdown(QStringLiteral("subtitlesScaleFrom"), 2);
        auto* to = rateDropdown(QStringLiteral("subtitlesScaleTo"), 0);
        form->addRow(tr("Timed for"), from);
        form->addRow(tr("Played at"), to);
        auto* which = scopeDropdown(*dialog);
        form->addRow(tr("Cues"), which);
        auto* note = new kit::KLabel(tr("Each time t becomes t × timed rate ÷ played rate, from "
                                        "the start of the list."),
                                     dialog);
        note->setWordWrap(true);
        note->setTone(kit::Color::Muted);
        dialog->contentLayout()->addWidget(note);
        dialog->addButton(kit::KDialog::Role::Reject, tr("Cancel"));
        dialog->addButton(kit::KDialog::Role::Accept, tr("Scale"));
        dialog->setValidator([from, to]() -> std::optional<kit::KRefusal> {
            if (from->currentIndex() == to->currentIndex())
                return kit::KRefusal{to, tr("Choose two different rates")};
            return std::nullopt;
        });
        dialog->watch(from);
        dialog->watch(to);
        const auto selected = selectedCues();
        connect(dialog, &QDialog::accepted, this, [this, from, to, which, selected] {
            const auto& a = rates[static_cast<std::size_t>(from->currentData().toInt())];
            const auto& b = rates[static_cast<std::size_t>(to->currentData().toInt())];
            // (a.n / a.d) / (b.n / b.d), reduced.
            auto numerator = a.numerator * b.denominator;
            auto denominator = a.denominator * b.numerator;
            const auto divisor = std::gcd(numerator, denominator);
            numerator /= divisor;
            denominator /= divisor;
            const auto factor = core::RationalTime::create(numerator, denominator);
            if (factor)
                (void)editing::scaleCues(session_, node_,
                                         which->currentData().toInt() == 1 ? selected
                                                                           : std::vector<std::uint64_t>{},
                                         *factor, core::RationalTime{});
        });
        dialog->open();
    }

    [[nodiscard]] std::optional<QString> listText(const subtitles::Format format) const {
        const auto source = editing::cueSource(session_, node_);
        if (source.list == nullptr)
            return std::nullopt;
        const auto written = subtitles::write(*source.list, format);
        if (!written)
            return std::nullopt;
        return QString::fromStdString(written->text);
    }

    void openCopyMenu() {
        auto* menu = kit::makeMenu(this);
        menu->setObjectName(QStringLiteral("subtitlesCopyMenu"));
        menu->setAttribute(Qt::WA_DeleteOnClose);
        menu->addAction(copySrt_);
        menu->addAction(copyVtt_);
        menu->popup(copy_->mapToGlobal(QPoint(0, copy_->height())));
    }

  public:
    void copyAs(const subtitles::Format format) {
        if (const auto text = listText(format))
            QGuiApplication::clipboard()->setText(*text);
    }

  private:
    void showText(const bool on) {
        pages_->setCurrentIndex(on ? 1 : 0);
        filter_->setEnabled(!on);
        errors_->hide();
        if (on)
            fillText();
    }
    void fillText() {
        const auto format = static_cast<subtitles::Format>(format_->currentData().toInt());
        text_->setPlainText(listText(format).value_or(QString{}));
        errors_->hide();
    }

    void applyText() {
        const auto format = static_cast<subtitles::Format>(format_->currentData().toInt());
        const auto text = text_->toPlainText().toStdString();
        apply_->setEnabled(false);
        QPointer<SubtitlesCuePanel> self(this);
        session_.subtitlesInspector().read(
            QStringLiteral("Read Subtitles Text"),
            [text, format]() {
                SubtitlesInspector::CueRead read;
                subtitles::LoadOptions options;
                options.format = format;
                std::vector<std::string> warnings;
                auto loaded = subtitles::loadText(text, options, warnings);
                if (auto* failure = std::get_if<subtitles::LoadFailure>(&loaded)) {
                    read.reports = failure->reports;
                    read.failure = std::move(*failure);
                } else {
                    auto& result = std::get<subtitles::LoadResult>(loaded);
                    read.reports = std::move(result.reports);
                    read.list = std::move(result.cues);
                }
                return read;
            },
            this,
            [self](SubtitlesInspector::CueRead read) {
                if (self)
                    self->finishApply(std::move(read));
            });
    }

    void finishApply(SubtitlesInspector::CueRead read) {
        updateActions();
        // A cue the text loses (skipped, or zero length) is an error: nothing is written until
        // every cue reads. Other rows (a dropped NOTE, an approximated style) are listed as notes.
        QStringList errors;
        QStringList notes;
        if (read.failure)
            errors.push_back(describeFailure(*read.failure));
        for (const auto& report : read.reports) {
            const bool loses = report.code == subtitles::codes::kSkippedCue ||
                               report.code == subtitles::codes::kZeroLength;
            (loses ? errors : notes).push_back(describeReport(report));
        }
        if (!errors.isEmpty() || !read.list) {
            errors_->setTone(kit::Color::Error);
            errors_->setText(errors.join(QLatin1Char('\n')));
            errors_->show();
            return;
        }
        if (!editing::replaceCues(session_, node_, std::move(*read.list), tr("Edit Cues as Text"))) {
            // Unchanged text is not an error; a refusal was reported by the session.
            editAsText_->setChecked(false);
            return;
        }
        session_.setSubtitlesLoadReport(node_, notes);
        editAsText_->setChecked(false);
        refresh();
    }

    void chooseFile() {
        struct Choice final {
            std::optional<subtitles::Format> format;
            QString encoding;
            bool link = true;
        };
        auto choice = std::make_shared<Choice>();
        kit::KFileOptions options;
        options.label = tr("Read as");
        options.build = [](QWidget& dialog) -> QWidget* {
            auto* pane = new QWidget(&dialog);
            auto* row = new QHBoxLayout(pane);
            row->setContentsMargins(0, 0, 0, 0);
            auto* format = new kit::KDropdown(pane);
            format->setObjectName(QStringLiteral("subtitlesLoadFormat"));
            format->setAccessibleName(tr("Format"));
            format->addItem(tr("Automatic format"), -1);
            format->addItem(tr("SubRip (SRT)"), static_cast<int>(subtitles::Format::Srt));
            format->addItem(tr("WebVTT"), static_cast<int>(subtitles::Format::WebVtt));
            format->addItem(tr("TTML / IMSC"), static_cast<int>(subtitles::Format::Ttml));
            format->addItem(tr("ASS / SSA"), static_cast<int>(subtitles::Format::Ass));
            auto* encoding = new kit::KDropdown(pane);
            encoding->setObjectName(QStringLiteral("subtitlesLoadEncoding"));
            encoding->setAccessibleName(tr("Encoding"));
            encoding->addItem(tr("Automatic encoding"), QString{});
            encoding->addItem(QStringLiteral("UTF-8"), QStringLiteral("utf-8"));
            encoding->addItem(QStringLiteral("UTF-16 LE"), QStringLiteral("utf-16le"));
            encoding->addItem(QStringLiteral("UTF-16 BE"), QStringLiteral("utf-16be"));
            for (const auto name : subtitles::singleByteEncodingNames())
                encoding->addItem(QString::fromLatin1(name.data(), static_cast<qsizetype>(name.size())),
                                  QString::fromLatin1(name.data(), static_cast<qsizetype>(name.size())));
            auto* link = new kit::KCheckBox(pane);
            link->setObjectName(QStringLiteral("subtitlesLoadLink"));
            link->setText(tr("Follow the file"));
            link->setToolTip(tr("Reload the cues when the file changes; edit them in the file"));
            link->setChecked(true);
            row->addWidget(format);
            row->addWidget(encoding);
            row->addWidget(link);
            return pane;
        };
        options.accepted = [choice](QWidget& pane) {
            if (auto* format = pane.findChild<kit::KDropdown*>(QStringLiteral("subtitlesLoadFormat")))
                if (const auto value = format->currentData().toInt(); value >= 0)
                    choice->format = static_cast<subtitles::Format>(value);
            if (auto* encoding =
                    pane.findChild<kit::KDropdown*>(QStringLiteral("subtitlesLoadEncoding")))
                choice->encoding = encoding->currentData().toString();
            if (auto* link = pane.findChild<kit::KCheckBox*>(QStringLiteral("subtitlesLoadLink")))
                choice->link = link->isChecked();
        };
        kit::KFileRequest request;
        request.mode = kit::KFileRequest::Mode::Open;
        request.title = tr("Load Subtitles");
        request.kind = QStringLiteral("subtitles");
        request.nameFilters =
            tr("Subtitles (*.srt *.vtt *.ttml *.dfxp *.xml *.ass *.ssa);;All files (*)");
        const auto paths = kit::chooseFiles(*this, request, options);
        if (paths.isEmpty())
            return;
        loadFile(std::filesystem::path(paths.front().toStdU16String()), choice->format,
                 choice->encoding.toStdString(), choice->link);
    }

  public:
    // Reads `path` on the inspector's worker, then stores it as one Load step.
    void loadFile(std::filesystem::path path, std::optional<subtitles::Format> format,
                  std::string encoding, const bool link, const bool reload = false) {
        subtitles::FileLoadRequest request;
        request.path = std::move(path);
        request.projectDirectory = session_.projectDirectory();
        request.options.format = format;
        request.options.encoding = std::move(encoding);
        request.link = link;
        QPointer<SubtitlesCuePanel> self(this);
        session_.subtitlesInspector().read(
            QStringLiteral("Load Subtitles"),
            [request]() {
                SubtitlesInspector::CueRead read;
                auto loaded = subtitles::loadFile(request);
                if (auto* failure = std::get_if<subtitles::LoadFailure>(&loaded)) {
                    read.reports = failure->reports;
                    read.failure = std::move(*failure);
                } else {
                    auto& file = std::get<subtitles::FileLoad>(loaded);
                    read.reports = std::move(file.loaded.reports);
                    read.list = std::move(file.loaded.cues);
                }
                return read;
            },
            this,
            [self, reload](SubtitlesInspector::CueRead read) {
                if (self)
                    self->finishLoad(std::move(read), reload);
            });
    }

  private:
    void finishLoad(SubtitlesInspector::CueRead read, const bool reload) {
        QStringList report;
        for (const auto& row : read.reports)
            report.push_back(describeReport(row));
        if (read.failure || !read.list) {
            const auto message = read.failure ? describeFailure(*read.failure)
                                              : tr("The file could not be read");
            emit session_.commandRejected(tr("Could not load subtitles: %1").arg(message));
            return;
        }
        if (reload)
            (void)editing::reloadCues(session_, node_, std::move(*read.list), report);
        else
            (void)editing::loadCues(session_, node_, std::move(*read.list), report);
        refresh();
    }

    void reloadFile() {
        const auto source = editing::cueSource(session_, node_);
        const auto* file = source.list == nullptr
                               ? nullptr
                               : std::get_if<document::CueFileProvenance>(&source.list->provenance);
        if (file == nullptr)
            return;
        loadFile(subtitles::linkedFilePath(*file, session_.projectDirectory()),
                 subtitles::formatFromName(file->format), file->encoding, true, true);
    }

    CompositionSession& session_;
    document::NodeId node_;
    kit::KLabel* sourceState_ = nullptr;
    kit::KLabel* report_ = nullptr;
    kit::KLabel* refusal_ = nullptr;
    kit::KButton* load_ = nullptr;
    kit::KButton* reload_ = nullptr;
    kit::KButton* unlink_ = nullptr;
    kit::KButton* add_ = nullptr;
    kit::KButton* delete_ = nullptr;
    kit::KButton* split_ = nullptr;
    kit::KButton* merge_ = nullptr;
    kit::KButton* setStart_ = nullptr;
    kit::KButton* setEnd_ = nullptr;
    kit::KButton* shift_ = nullptr;
    kit::KButton* scale_ = nullptr;
    kit::KButton* editAsText_ = nullptr;
    kit::KButton* copy_ = nullptr;
    QAction* copySrt_ = nullptr;
    QAction* copyVtt_ = nullptr;
    kit::KButton* apply_ = nullptr;
    kit::KSearchField* filter_ = nullptr;
    QStackedWidget* pages_ = nullptr;
    QTableView* table_ = nullptr;
    CueListModel* model_ = nullptr;
    CueFilter* proxy_ = nullptr;
    kit::KDropdown* format_ = nullptr;
    kit::KTextArea* text_ = nullptr;
    kit::KLabel* errors_ = nullptr;
    bool syncing_ = false;
};

} // namespace

QWidget* makeSubtitlesCuePanel(CompositionSession& session, const document::NodeId node,
                               QWidget* parent) {
    return new SubtitlesCuePanel(session, node, parent);
}

void loadSubtitlesFile(QWidget& panel, std::filesystem::path path,
                       std::optional<subtitles::Format> format, std::string encoding,
                       const bool link) {
    if (auto* cues = dynamic_cast<SubtitlesCuePanel*>(&panel))
        cues->loadFile(std::move(path), format, std::move(encoding), link);
}

} // namespace bloom::ui
