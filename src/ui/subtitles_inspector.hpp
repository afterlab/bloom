#pragma once

// #752: the one background inspection of a composition's Subtitles nodes, owned by the session
// (CompositionSession::subtitlesInspector()). It answers what the artist surfaces show beside the
// rendered pixels and never changes them: where each cue that shows at the playhead is drawn (the
// Viewer's outlines, click targets and the track-only overlay), the node's safe area, every cue's
// checks (the Properties cue list) and a linked file's state (the Source row and the node card).
//
// The work (compiling the composition, laying cues out, reading a linked file's stamp and digest)
// runs on the inspector's own worker, never the interface thread. A newer snapshot, time or
// request cancels the running inspection; a result is published only for the request it answers.

#include <bloom/core/rational_time.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/host/linked_subtitle_files.hpp>
#include <bloom/runtime/subtitles_layout.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/subtitles/subtitles.hpp>

#include <QObject>
#include <QPointer>
#include <QRectF>
#include <QString>
#include <QStringList>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

class QTimer;

namespace bloom::ui {
class CompositionSession;

class SubtitlesInspector final : public QObject {
    Q_OBJECT

  public:
    // One cue drawn at the playhead: its block in the node's author pixels (the composition frame,
    // y down) and its number in the list (1 first).
    struct Block final {
        std::uint64_t cue = 0;
        std::size_t index = 0;
        QRectF rect;
        QString text;
    };
    struct Node final {
        // Why there are no blocks (the node does not reach the output, its cues come from another
        // node, or its layout failed), empty when there are.
        QString reason;
        QRectF frame;
        QRectF safeArea;
        bool burnIn = true;
        std::vector<Block> blocks;
        // Each cue's check messages, by cue id; complete only when `checked`.
        std::map<std::uint64_t, QStringList> checks;
        bool checked = false;
        // A linked list's file: its state and, when it changed and cannot be read, the reason.
        bool linked = false;
        host::LinkedFileState fileState = host::LinkedFileState::InSync;
        QString fileMessage;
    };

    explicit SubtitlesInspector(CompositionSession& session, QObject* parent = nullptr);
    ~SubtitlesInspector() override;

    // The nodes whose checks a surface shows (Properties). Blocks and a linked file's state are
    // inspected for every Subtitles node of the composition. Each owner replaces its own set.
    void setDetailNodes(const void* owner, std::set<document::NodeId> nodes);
    void releaseDetailNodes(const void* owner);
    // The newest published result for `node`, or nullptr.
    [[nodiscard]] const Node* result(document::NodeId node) const;
    // Requests a fresh inspection now (the session's snapshot, time and composition signals do).
    void refresh();
    // Whether no inspection or read is running or waiting (tests).
    [[nodiscard]] bool idle() const noexcept { return !active_ && !pending_ && reads_.empty(); }

    // A subtitle file or text read off the interface thread (Load File, Reload, Edit as Text's
    // Apply): `work` runs on the inspector's worker, `done` on this thread unless `receiver` went
    // away first.
    struct CueRead final {
        std::optional<document::CueListValue> list;
        std::vector<subtitles::Report> reports;
        std::optional<subtitles::LoadFailure> failure;
    };
    void read(QString name, std::function<CueRead()> work, QObject* receiver,
              std::function<void(CueRead)> done);

  signals:
    void inspected();

  private:
    void ensureScheduler();
    void start();
    void poll();

    CompositionSession& session_;
    std::shared_ptr<runtime::TaskScheduler> scheduler_;
    std::shared_ptr<std::atomic_bool> retire_;
    struct Cache;
    std::shared_ptr<Cache> cache_;
    runtime::TaskHandle<std::shared_ptr<std::map<document::NodeId, Node>>> task_;
    QTimer* timer_ = nullptr;
    QTimer* debounce_ = nullptr;
    std::map<const void*, std::set<document::NodeId>> detail_;
    std::map<document::NodeId, Node> results_;
    struct PendingRead final {
        runtime::TaskHandle<std::shared_ptr<CueRead>> handle;
        QPointer<QObject> receiver;
        std::function<void(CueRead)> done;
    };
    std::vector<PendingRead> reads_;
    std::uint64_t generation_ = 0;
    std::uint64_t activeGeneration_ = 0;
    bool active_ = false;
    bool pending_ = false;
};

} // namespace bloom::ui
