#pragma once

// #487: the channels and layers of the channel set feeding a node, for the Shuffle slot pickers and
// the channel/layer checklists of Channel Copy, Select Channels and the Composition Output. The
// read is the Viewer channel menu's own worker listing (runtime::listChannelInspectionChoices) of
// the upstream ChannelSet output; it runs on a worker and never touches project state.

#include <QObject>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/document.hpp>
#include <bloom/runtime/channel_inspection_listing.hpp>
#include <bloom/runtime/evaluation.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace bloom::ui {

class CompositionSession;
class TaskUiBridge;

class ChannelSetController final : public QObject {
    Q_OBJECT

  public:
    using ListingProvider = std::function<runtime::ChannelInspectionListing(
        const document::Snapshot&, document::CompositionId, document::NodeId, const std::string&,
        core::RationalTime, const runtime::EvaluationColorIntent&,
        const runtime::CancellationToken&)>;

    ChannelSetController(CompositionSession& session, runtime::TaskScheduler& scheduler,
                         TaskUiBridge& bridge, ListingProvider provider, QObject* parent = nullptr);
    ~ChannelSetController() override;

    // The input whose channel set a node's pickers list: Channel Copy's `from`, the `channels`
    // input of a Shuffle, Select Channels or Composition Output, a Z Defocus's or Depth Fog's
    // `depth`; nullopt for any other node.
    [[nodiscard]] static std::optional<std::string_view> inputPort(std::string_view typeId);

    // Starts (or keeps) the listing of `node`'s input channel set for the current snapshot. An
    // unconnected input lists nothing at once, with a diagnostic.
    void request(document::NodeId node);
    // The latest listing (kept while a newer revision is re-read, so an edit does not blank a
    // picker); nullptr until the first one arrives (changed() announces it).
    [[nodiscard]] const runtime::ChannelInspectionListing* listing(document::NodeId node) const;
    [[nodiscard]] bool listingCurrent(document::NodeId node) const;

    void cancel();

  signals:
    void changed();

  private:
    void poll();

    struct Listing final {
        document::Revision revision;
        runtime::ChannelInspectionListing value;
    };
    struct Pending final {
        document::Revision revision;
        runtime::TaskHandle<std::shared_ptr<const runtime::ChannelInspectionListing>> handle;
    };

    CompositionSession& session_;
    runtime::TaskScheduler& scheduler_;
    TaskUiBridge& bridge_;
    ListingProvider provider_;
    std::map<document::NodeId, Listing> listings_;
    std::map<document::NodeId, Pending> tasks_;
};

} // namespace bloom::ui
