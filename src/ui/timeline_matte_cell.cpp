#include "timeline_matte_cell.hpp"

#include "layer_mask_support.hpp"

#include <QCoreApplication>
#include <QVariant>
#include <bloom/commands/layer_mask_operations.hpp>
#include <bloom/document/merge_entries.hpp>
#include <bloom/document/project.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/dropdown.hpp>

namespace bloom::ui::matte_cell {
namespace {
QString text(const char* source) {
    return QCoreApplication::translate("TimelineMatteCell", source);
}
} // namespace

void populate(kit::KDropdown& dropdown, const CompositionSession& session,
              const document::LayerId layer) {
    dropdown.clearItems();
    dropdown.addItem(text("None"), QVariant::fromValue(0LL));
    const auto* composition = session.composition();
    const auto* boundary = composition ? composition->graph().findLayer(layer) : nullptr;
    if (boundary == nullptr)
        return;
    for (const auto id : masks::matteSourceCandidates(*composition, layer))
        if (const auto* other = composition->graph().findLayer(id))
            dropdown.addItem(QString::fromStdString(other->name),
                             QVariant::fromValue(static_cast<long long>(id.value())));
    const auto matte = boundary->trackMatte;
    // #383: node Mask outputs.
    long long current = matte ? static_cast<long long>(matte->source.value()) : 0LL;
    const auto nodes = masks::matteNodeCandidates(*composition, layer);
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        const auto code = kNodeMatteBase - static_cast<long long>(index);
        dropdown.addItem(text("Node: %1").arg(masks::matteNodeText(*composition, nodes[index])),
                         QVariant::fromValue(code));
        if (matte && matte->node == nodes[index])
            current = code;
    }
    const bool luma = matte && matte->channel == document::TrackMatteChannel::Luma;
    const auto action = [&](const QString& label, const long long code) {
        const int index = dropdown.addItem(label, QVariant::fromValue(code));
        dropdown.setItemEnabled(index, matte.has_value());
    };
    action(luma ? text("Use Alpha") : text("Alpha (current)"), kActionAlpha);
    action(luma ? text("Luma (current)") : text("Use Luma"), kActionLuma);
    action(matte && matte->inverted ? text("Inverted (on)") : text("Inverted"), kActionInverted);
    dropdown.setCurrentIndex(dropdown.findData(QVariant::fromValue(current)));
    dropdown.setToolTip(matte ? text("Track matte: %1%2")
                                    .arg(masks::channelText(matte->channel),
                                         matte->inverted ? text(", inverted") : QString())
                              : text("Use another layer as this layer's matte"));
}

bool activate(CompositionSession& session, const document::LayerId layer, const long long code) {
    const auto* composition = session.composition();
    const auto* boundary = composition ? composition->graph().findLayer(layer) : nullptr;
    if (boundary == nullptr)
        return false;
    auto matte = boundary->trackMatte;
    if (code <= kNodeMatteBase) {
        // #383: a node Mask output.
        const auto nodes = masks::matteNodeCandidates(*composition, layer);
        const auto index = static_cast<std::size_t>(kNodeMatteBase - code);
        if (index >= nodes.size())
            return false;
        matte = document::TrackMatte{document::LayerId{},
                                     matte ? matte->channel : document::TrackMatteChannel::Alpha,
                                     matte && matte->inverted, nodes[index]};
    } else if (code >= 0) {
        if (code == 0)
            matte.reset();
        else if (matte) {
            matte->source = document::LayerId::fromRaw(static_cast<std::uint64_t>(code));
            matte->node.reset();
        }
        else
            matte =
                document::TrackMatte{document::LayerId::fromRaw(static_cast<std::uint64_t>(code)),
                                     document::TrackMatteChannel::Alpha, false};
    } else {
        if (!matte)
            return false;
        if (code == kActionAlpha)
            matte->channel = document::TrackMatteChannel::Alpha;
        else if (code == kActionLuma)
            matte->channel = document::TrackMatteChannel::Luma;
        else if (code == kActionInverted)
            matte->inverted = !matte->inverted;
    }
    if (matte == boundary->trackMatte)
        return false;
    return session.setTrackMatte(layer, matte);
}

void populate(kit::KDropdown& dropdown, const CompositionSession& session,
              const LayerTarget& target) {
    if (target.layer.isValid()) {
        populate(dropdown, session, target.layer);
        return;
    }
    dropdown.clearItems();
    dropdown.addItem(text("None"), QVariant::fromValue(0LL));
    const auto* composition = session.composition();
    const auto* merge = composition ? composition->graph().merge(target.entry.merge) : nullptr;
    if (merge == nullptr)
        return;
    for (const auto& other : merge->entries()) {
        if (other.slotId == target.entry.slot)
            continue;
        if (const auto* layer =
                document::mergeEntryLayer(composition->graph(), target.entry.merge, other.slotId))
            dropdown.addItem(QString::fromStdString(layer->name),
                             QVariant::fromValue(static_cast<long long>(layer->layerId.value())));
    }
    // #383: node Mask outputs; the entry's layer would be its Merge's.
    const auto nodes = masks::matteNodeCandidatesFor(*composition, target.entry.merge);
    for (std::size_t index = 0; index < nodes.size(); ++index)
        dropdown.addItem(text("Node: %1").arg(masks::matteNodeText(*composition, nodes[index])),
                         QVariant::fromValue(kNodeMatteBase - static_cast<long long>(index)));
    for (const auto& [label, code] :
         {std::pair{text("Alpha (current)"), kActionAlpha}, std::pair{text("Use Luma"), kActionLuma},
          std::pair{text("Inverted"), kActionInverted}}) {
        const int index = dropdown.addItem(label, QVariant::fromValue(code));
        dropdown.setItemEnabled(index, false);
    }
    dropdown.setCurrentIndex(0);
    dropdown.setToolTip(text("Use another layer as this layer's matte"));
}

bool activate(CompositionSession& session, const LayerTarget& target, const long long code) {
    if (target.layer.isValid())
        return activate(session, target.layer, code);
    if (code == 0 || (code < 0 && code > kNodeMatteBase))
        return false;
    const auto compositionId = session.compositionId();
    document::TrackMatte matte{document::LayerId::fromRaw(static_cast<std::uint64_t>(code)),
                               document::TrackMatteChannel::Alpha, false};
    if (code <= kNodeMatteBase) {
        // #383: a node Mask output.
        const auto* composition = session.composition();
        const auto nodes = composition == nullptr
                               ? std::vector<document::OutputPortRef>{}
                               : masks::matteNodeCandidatesFor(*composition, target.entry.merge);
        const auto index = static_cast<std::size_t>(kNodeMatteBase - code);
        if (index >= nodes.size())
            return false;
        matte = document::TrackMatte{document::LayerId{}, document::TrackMatteChannel::Alpha, false,
                                     nodes[index]};
    }
    return session
        .editEntry(target.entry, text("Set Track Matte"),
                   [compositionId, matte](const document::LayerId layer) {
                       return std::make_unique<commands::SetTrackMatte>(compositionId, layer,
                                                                        matte);
                   })
        .changed();
}

} // namespace bloom::ui::matte_cell
