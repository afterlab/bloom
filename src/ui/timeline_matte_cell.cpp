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
    const bool luma = matte && matte->channel == document::TrackMatteChannel::Luma;
    const auto action = [&](const QString& label, const long long code) {
        const int index = dropdown.addItem(label, QVariant::fromValue(code));
        dropdown.setItemEnabled(index, matte.has_value());
    };
    action(luma ? text("Use Alpha") : text("Alpha (current)"), kActionAlpha);
    action(luma ? text("Luma (current)") : text("Use Luma"), kActionLuma);
    action(matte && matte->inverted ? text("Inverted (on)") : text("Inverted"), kActionInverted);
    dropdown.setCurrentIndex(dropdown.findData(
        QVariant::fromValue(matte ? static_cast<long long>(matte->source.value()) : 0LL)));
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
    if (code >= 0) {
        if (code == 0)
            matte.reset();
        else if (matte)
            matte->source = document::LayerId::fromRaw(static_cast<std::uint64_t>(code));
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
    if (code <= 0)
        return false;
    const auto compositionId = session.compositionId();
    const document::TrackMatte matte{document::LayerId::fromRaw(static_cast<std::uint64_t>(code)),
                                     document::TrackMatteChannel::Alpha, false};
    return session
        .editEntry(target.entry, text("Set Track Matte"),
                   [compositionId, matte](const document::LayerId layer) {
                       return std::make_unique<commands::SetTrackMatte>(compositionId, layer,
                                                                        matte);
                   })
        .changed();
}

} // namespace bloom::ui::matte_cell
