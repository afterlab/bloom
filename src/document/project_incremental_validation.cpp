// #357. Incremental publication validation: a draft is checked against the published project it was
// drafted from, so an edit costs what it changed rather than what the project holds.
#include <bloom/document/vector_artwork_nodes.hpp>
#include <bloom/document/node_group_nodes.hpp>
#include <bloom/document/persisted_text.hpp>
#include <bloom/document/project.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <iterator>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace bloom::document {
namespace {

std::atomic<std::uint64_t> incrementalValidations{0};
std::atomic<std::uint64_t> compositionsValidated{0};
std::atomic<std::uint64_t> compositionsSkipped{0};
std::atomic<std::uint64_t> rootSectionValidations{0};
std::atomic<std::uint64_t> notProvenCount{0};

void sortUnique(std::vector<std::uint64_t>& ids) {
    std::ranges::sort(ids);
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
}

[[nodiscard]] bool contains(const std::vector<std::uint64_t>& sorted, const std::uint64_t id) {
    return std::ranges::binary_search(sorted, id);
}

bool notProven() {
    notProvenCount.fetch_add(1, std::memory_order_relaxed);
    return false;
}

} // namespace

namespace {

using Ids = std::vector<std::uint64_t>;

template <typename Id> void addId(Ids& ids, const Id id) {
    if (id.isValid())
        ids.push_back(id.value());
}

[[nodiscard]] Ids layerSlotIds(const CanonicalGraph& graph) {
    Ids ids;
    for (const auto& stack : graph.merges())
        for (const auto& entry : stack.entries())
            addId(ids, entry.slotId);
    sortUnique(ids);
    return ids;
}

[[nodiscard]] Ids groupIds(const NodeGroups& groups) {
    Ids ids;
    for (const auto& [groupId, group] : groups) {
        static_cast<void>(group);
        addId(ids, groupId);
    }
    sortUnique(ids);
    return ids;
}

// The ids in `after` but not `before` go to `added`, the ids in `before` but not `after` to
// `removed`. Both inputs are sorted and free of duplicates.
void splitChanges(const Ids& before, const Ids& after, Ids& added, Ids& removed) {
    std::ranges::set_difference(after, before, std::back_inserter(added));
    std::ranges::set_difference(before, after, std::back_inserter(removed));
}

// The ids the added records declare that the removed ones did not, and the reverse.
template <typename Record, typename Declare>
void recordIdChanges(const RecordDiff<Record>& diff, Declare&& declare, Ids& added, Ids& removed) {
    Ids now;
    Ids was;
    for (const auto* record : diff.added)
        declare(*record, now);
    for (const auto* record : diff.removed)
        declare(*record, was);
    sortUnique(now);
    sortUnique(was);
    splitChanges(was, now, added, removed);
}

[[nodiscard]] std::shared_ptr<const Ids> applyChanges(const std::shared_ptr<const Ids>& previous,
                                                      const Ids& added, const Ids& removed) {
    if (added.empty() && removed.empty())
        return previous;
    Ids kept;
    kept.reserve(previous->size());
    std::ranges::set_difference(*previous, removed, std::back_inserter(kept));
    Ids merged;
    merged.reserve(kept.size() + added.size());
    std::ranges::set_union(kept, added, std::back_inserter(merged));
    return std::make_shared<const Ids>(std::move(merged));
}

[[nodiscard]] bool sameMerges(const CanonicalGraph& left, const CanonicalGraph& right) {
    return std::ranges::equal(left.merges(), right.merges(),
                              [](const LayerStack& a, const LayerStack& b) {
                                  return a.nodeId() == b.nodeId() && a.enabled() == b.enabled() &&
                                         std::ranges::equal(a.entries(), b.entries());
                              });
}

} // namespace

Composition::DeclaredIdIndex Composition::buildDeclaredIdIndex(const Content& authored) {
    DeclaredIdIndex index;
    const auto build = [](const auto records, const auto id) {
        Ids ids;
        ids.reserve(records.size());
        for (const auto& record : records)
            addId(ids, id(record));
        sortUnique(ids);
        return std::make_shared<const Ids>(std::move(ids));
    };
    index.nodes = build(authored.graph.nodes(), [](const NodeRecord& node) { return node.id; });
    index.edges = build(authored.graph.edges(), [](const EdgeRecord& edge) { return edge.id; });
    index.parameters = build(authored.parameters.records(),
                             [](const ParameterRecord& parameter) { return parameter.id; });
    index.curves = build(authored.animationCurves.records(),
                         [](const AnimationCurveRecord& curve) { return animationCurveId(curve); });
    index.layers = build(authored.graph.layerOutputs(),
                         [](const LayerOutputBoundary& layer) { return layer.layerId; });
    index.layerSlots = std::make_shared<const Ids>(layerSlotIds(authored.graph));
    index.groups = std::make_shared<const Ids>(groupIds(authored.nodeGroups));
    index.keyframes = authored.animationCurves.keyframeIds();
    return index;
}

void Composition::declaredIdChangesBetween(const Content& before, const Content& after,
                                           DeclaredIdChanges& added, DeclaredIdChanges& removed) {
    if (before.graph.nodeStorageIdentity() != after.graph.nodeStorageIdentity())
        recordIdChanges(
            diffRecords(before.graph.nodes(), after.graph.nodes()),
            [](const NodeRecord& node, Ids& ids) { addId(ids, node.id); }, added.nodes,
            removed.nodes);
    if (before.graph.edgeStorageIdentity() != after.graph.edgeStorageIdentity())
        recordIdChanges(
            diffRecords(before.graph.edges(), after.graph.edges()),
            [](const EdgeRecord& edge, Ids& ids) { addId(ids, edge.id); }, added.edges,
            removed.edges);
    if (before.graph.layerStorageIdentity() != after.graph.layerStorageIdentity())
        recordIdChanges(
            diffRecords(before.graph.layerOutputs(), after.graph.layerOutputs()),
            [](const LayerOutputBoundary& layer, Ids& ids) { addId(ids, layer.layerId); },
            added.layers, removed.layers);
    if (before.parameters.storageIdentity() != after.parameters.storageIdentity())
        recordIdChanges(
            diffRecords(before.parameters.records(), after.parameters.records()),
            [](const ParameterRecord& parameter, Ids& ids) { addId(ids, parameter.id); },
            added.parameters, removed.parameters);
    if (before.animationCurves.storageIdentity() != after.animationCurves.storageIdentity()) {
        const auto curves =
            diffRecords(before.animationCurves.records(), after.animationCurves.records());
        recordIdChanges(
            curves,
            [](const AnimationCurveRecord& curve, Ids& ids) {
                addId(ids, animationCurveId(curve));
            },
            added.curves, removed.curves);
        recordIdChanges(
            curves,
            [](const AnimationCurveRecord& curve, Ids& ids) {
                forEachAnimationKeyframeId(curve, [&](const KeyframeId id) { addId(ids, id); });
            },
            added.keyframes, removed.keyframes);
    }
    if (!sameMerges(before.graph, after.graph))
        splitChanges(layerSlotIds(before.graph), layerSlotIds(after.graph), added.layerSlots,
                     removed.layerSlots);
    if (before.nodeGroups != after.nodeGroups)
        splitChanges(groupIds(before.nodeGroups), groupIds(after.nodeGroups), added.groups,
                     removed.groups);
}

Composition::DeclaredIdIndex Composition::updateDeclaredIdIndex(const DeclaredIdIndex& previous,
                                                                const Content& before,
                                                                const Content& after) {
    DeclaredIdChanges added;
    DeclaredIdChanges removed;
    declaredIdChangesBetween(before, after, added, removed);
    DeclaredIdIndex index;
    index.nodes = applyChanges(previous.nodes, added.nodes, removed.nodes);
    index.edges = applyChanges(previous.edges, added.edges, removed.edges);
    index.parameters = applyChanges(previous.parameters, added.parameters, removed.parameters);
    index.curves = applyChanges(previous.curves, added.curves, removed.curves);
    index.layers = applyChanges(previous.layers, added.layers, removed.layers);
    index.layerSlots = applyChanges(previous.layerSlots, added.layerSlots, removed.layerSlots);
    index.groups = applyChanges(previous.groups, added.groups, removed.groups);
    // The store keeps its own chunked index in step with every edit; this shares its chunks.
    index.keyframes = after.animationCurves.keyframeIds();
    return index;
}

std::vector<CompositionId> Composition::buildNestedReferences(const Content& authored) {
    std::vector<CompositionId> references;
    for (const auto& node : authored.graph.nodes()) {
        // #342: a Node Group references the group it names the way a Composition Source references
        // a composition, so a group that contains itself is a nesting cycle.
        if (usesGroupReference(node.typeId)) {
            for (const auto group : nodeGroupReferences(node, authored.parameters))
                references.push_back(group);
            continue;
        }
        if (node.typeId != kCompositionSourceNodeType)
            continue;
        for (const auto& binding : node.parameters) {
            if (binding.role != "composition")
                continue;
            const auto* parameter = authored.parameters.find(binding.parameterId);
            const auto* constant =
                parameter ? std::get_if<ConstantValueSource>(&parameter->source) : nullptr;
            const auto* value = constant ? std::get_if<std::int64_t>(&constant->value) : nullptr;
            if (!value || *value <= 0)
                continue;
            references.push_back(CompositionId::fromRaw(static_cast<std::uint64_t>(*value)));
        }
    }
    return references;
}

const Composition::DeclaredIdIndex* Composition::frozenDeclaredIds() const noexcept {
    return !editableContent_ && frozenContent_ && frozenContent_->frozenDeclaredIds
               ? &*frozenContent_->frozenDeclaredIds
               : nullptr;
}

std::vector<CompositionId> Composition::nestedReferences() const {
    if (!editableContent_ && frozenContent_ && frozenContent_->frozenNestedReferences)
        return *frozenContent_->frozenNestedReferences;
    return buildNestedReferences(content());
}

// A new shell member belongs here AND in sameEvaluationInputs(Composition) (#399).
bool Composition::sameShell(const Composition& other) const noexcept {
    return id_ == other.id_ && name_ == other.name_ && duration_ == other.duration_ &&
           format_ == other.format_ && workArea_ == other.workArea_ &&
           workingColorSpaceId_ == other.workingColorSpaceId_ && safeAreas_ == other.safeAreas_ &&
           backgroundColor_ == other.backgroundColor_ && timecode_ == other.timecode_ &&
           folder_ == other.folder_ && pixelArt_ == other.pixelArt_;
}

bool Project::sameRootRecordStorage(const Project& other) const noexcept {
    return assets_.identity() == other.assets_.identity() &&
           assetFolders_.identity() == other.assetFolders_.identity() &&
           extensionRecords_.identity() == other.extensionRecords_.identity() &&
           dataBlocks_.identity() == other.dataBlocks_.identity() &&
           exportJobs_.identity() == other.exportJobs_.identity();
}

Project::IncrementalValidationWork Project::incrementalValidationWork() noexcept {
    return {.incrementalValidations = incrementalValidations.load(std::memory_order_relaxed),
            .compositionsValidated = compositionsValidated.load(std::memory_order_relaxed),
            .compositionsSkipped = compositionsSkipped.load(std::memory_order_relaxed),
            .rootSectionValidations = rootSectionValidations.load(std::memory_order_relaxed),
            .notProven = notProvenCount.load(std::memory_order_relaxed)};
}

std::atomic<std::uint64_t>& recordsValidatedCount() noexcept {
    static std::atomic<std::uint64_t> count{0};
    return count;
}

namespace {

// Whether the graph section's rules (graph validation, masks, entry layers) read anything of this
// parameter beyond its id, schema and whether it is a driver: a constant integer may name a
// composition or a group, a mask parameter's constant is checked against its mask domain, and a
// Node Group or Composition Source role must stay constant.
[[nodiscard]] bool readByGraphSection(const ParameterRecord& record) {
    if (std::holds_alternative<DriverBindingSource>(record.source))
        return true;
    if (record.schemaKey == kLayerMaskPathSchemaKey ||
        record.schemaKey == kLayerMaskFeatherSchemaKey ||
        record.schemaKey == kLayerMaskExpansionSchemaKey ||
        record.schemaKey == kLayerMaskOpacitySchemaKey ||
        record.schemaKey == kLayerMaskInvertedSchemaKey)
        return true;
    const auto* constant = std::get_if<ConstantValueSource>(&record.source);
    return constant != nullptr && std::holds_alternative<std::int64_t>(constant->value);
}

// Whether parameter edits leave the graph section's answer as it was: every changed record keeps
// its id and schema, and neither version is one that section reads (above).
[[nodiscard]] bool graphNeutral(const RecordDiff<ParameterRecord>& parameters) {
    if (parameters.removed.size() != parameters.added.size())
        return false;
    std::unordered_map<ParameterId, const ParameterRecord*> was;
    was.reserve(parameters.removed.size());
    for (const auto* record : parameters.removed) {
        if (readByGraphSection(*record) || !was.emplace(record->id, record).second)
            return false;
    }
    for (const auto* record : parameters.added) {
        const auto found = was.find(record->id);
        if (found == was.end() || found->second->schemaKey != record->schemaKey ||
            readByGraphSection(*record))
            return false;
    }
    return true;
}

} // namespace

bool Composition::validatesAgainst(const Composition& base) const {
    if (base.id_ != id_)
        return false;
    ValidationResult result;
    validateShell(result);
    if (!result.ok())
        return false;
    const auto& before = base.content();
    const auto& after = content();
    if (&before == &after)
        return duration_ == base.duration_ || [&] {
            validateLayerRanges(result);
            return result.ok();
        }();

    const auto parameterDiff =
        before.parameters.storageIdentity() == after.parameters.storageIdentity()
            ? RecordDiff<ParameterRecord>{}
            : diffRecords(before.parameters.records(), after.parameters.records());
    const auto curveDiff =
        before.animationCurves.storageIdentity() == after.animationCurves.storageIdentity()
            ? RecordDiff<AnimationCurveRecord>{}
            : diffRecords(before.animationCurves.records(), after.animationCurves.records());

    // The graph section reads every node, edge, layer, Merge, the layout and the groups, and the
    // parameters' ids, schemas and driver bindings: it runs whole when any of those changed.
    const bool graphChanged =
        before.graph.nodeStorageIdentity() != after.graph.nodeStorageIdentity() ||
        before.graph.edgeStorageIdentity() != after.graph.edgeStorageIdentity() ||
        before.graph.layerStorageIdentity() != after.graph.layerStorageIdentity() ||
        !sameMerges(before.graph, after.graph) ||
        before.graph.compositionOutput() != after.graph.compositionOutput() ||
        before.nodeLayout != after.nodeLayout || before.nodeGroups != after.nodeGroups;
    if (graphChanged || duration_ != base.duration_)
        validateLayerRanges(result);
    if (graphChanged || !graphNeutral(parameterDiff))
        validateGraphSection(result);
    if (!result.ok())
        return false;

    // Parameters: each changed record, and that its id is still held once.
    for (const auto* record : parameterDiff.added) {
        recordsValidatedCount().fetch_add(1, std::memory_order_relaxed);
        ParameterStore::validateRecord(*record, result);
        if (!result.ok() || !after.parameters.holdsOnce(record->id))
            return false;
    }

    // Curves: each changed curve. Curve ids stay unique and ordered because the store inserts in
    // id order and refuses a held id; keyframe ids are checked against the base below.
    for (const auto* record : curveDiff.added) {
        recordsValidatedCount().fetch_add(1, std::memory_order_relaxed);
        AnimationCurveStore::validateRecord(*record, result);
        if (!result.ok())
            return false;
    }
    if (!curveDiff.empty()) {
        // A key id the changed curves hold must be held once among them and by no unchanged
        // curve: an unchanged curve holds exactly the base's ids less those the base's changed
        // curves held.
        std::vector<KeyframeId> now;
        for (const auto* record : curveDiff.added)
            forEachAnimationKeyframeId(*record, [&](const KeyframeId id) { now.push_back(id); });
        std::ranges::sort(now);
        if (std::ranges::adjacent_find(now) != now.end() ||
            std::ranges::any_of(now, [](const KeyframeId id) { return !id.isValid(); }))
            return false;
        std::vector<KeyframeId> was;
        for (const auto* record : curveDiff.removed)
            forEachAnimationKeyframeId(*record, [&](const KeyframeId id) { was.push_back(id); });
        std::ranges::sort(was);
        const auto& held = before.animationCurves.keyframeIds();
        for (const auto id : now)
            if (held.contains(id) && !std::ranges::binary_search(was, id))
                return false;
    }

    // Curve references: every parameter that names a changed curve, every changed parameter
    // that names a curve, and every curve that lost its owner or whose owner names it no more.
    if (!parameterDiff.empty() || !curveDiff.empty()) {
        std::unordered_map<AnimationCurveId, std::vector<const ParameterRecord*>> owners;
        for (const auto& parameter : after.parameters.records())
            if (const auto* source = std::get_if<AnimationCurveSource>(&parameter.source))
                owners[source->curveId].push_back(&parameter);
        const auto checkCurve = [&](const AnimationCurveId id) {
            const auto* curve = after.animationCurves.find(id);
            const auto owner = owners.find(id);
            const bool owned = owner != owners.end();
            if (curve == nullptr)
                return !owned;
            if (!owned || owner->second.size() != 1)
                return false;
            validateAnimationCurveReference(*owner->second.front(), *curve, result);
            return result.ok();
        };
        for (const auto* record : curveDiff.added)
            if (!checkCurve(animationCurveId(*record)))
                return false;
        for (const auto* record : curveDiff.removed)
            if (!checkCurve(animationCurveId(*record)))
                return false;
        const auto checkParameter = [&](const ParameterRecord& record) {
            const auto* source = std::get_if<AnimationCurveSource>(&record.source);
            return source == nullptr || checkCurve(source->curveId);
        };
        for (const auto* record : parameterDiff.added)
            if (!checkParameter(*record))
                return false;
        for (const auto* record : parameterDiff.removed)
            if (!checkParameter(*record))
                return false;
    }
    return result.ok();
}

bool Project::validatesAgainst(const Project& base) const {
    incrementalValidations.fetch_add(1, std::memory_order_relaxed);
    if (id_ != base.id_)
        return notProven();

    // Root records and the project name: revalidated only when their storage changed.
    const bool rootChanged = !sameRootRecordStorage(base) || name_ != base.name_;

    // Which compositions changed. Unchanged ones still share their frozen content with `base`.
    std::unordered_map<CompositionId, const Composition*> before;
    before.reserve(base.compositions_.size());
    for (const auto& composition : base.compositions_)
        before.emplace(composition.id(), &composition);
    struct Changed final {
        std::size_t ordinal = 0;
        const Composition* base = nullptr;
        // The ids this composition declares that its base did not, and the reverse (#744).
        Composition::DeclaredIdChanges added;
        Composition::DeclaredIdChanges removed;
        // For a composition new to the project: every id it declares.
        std::optional<Composition::DeclaredIdIndex> fresh;
    };
    std::vector<Changed> changed;
    std::vector<std::size_t> unchanged;
    std::unordered_set<CompositionId> ids;
    for (std::size_t ordinal = 0; ordinal < compositions_.size(); ++ordinal) {
        const auto& composition = compositions_[ordinal];
        if (!composition.id().isValid() || !ids.insert(composition.id()).second)
            return notProven();
        const auto found = before.find(composition.id());
        if (found != before.end() &&
            found->second->contentIdentity() == composition.contentIdentity() &&
            found->second->sameShell(composition) && composition.frozenDeclaredIds() != nullptr)
            unchanged.push_back(ordinal);
        else
            changed.push_back(
                {ordinal, found == before.end() ? nullptr : found->second, {}, {}, std::nullopt});
    }
    const bool compositionSetChanged =
        !changed.empty() || base.compositions_.size() != ids.size() ||
        std::ranges::any_of(base.compositions_,
                            [&](const auto& c) { return !ids.contains(c.id()); });
    compositionsSkipped.fetch_add(unchanged.size(), std::memory_order_relaxed);

    if (rootChanged) {
        rootSectionValidations.fetch_add(1, std::memory_order_relaxed);
        ValidationResult root;
        validateRootRecords(root);
        if (!root.ok())
            return notProven();
    }
    // ADR 0033: an export job's composition must exist, so a changed composition set re-checks the
    // queue even when the queue itself is unchanged.
    if (!rootChanged && compositionSetChanged && !exportJobs_.get().empty()) {
        ValidationResult queue;
        validateExportQueue(queue);
        if (!queue.ok())
            return notProven();
    }
    // #378: a composition's folder must exist (folders are root records; a composition shell
    // carries the reference).
    if (rootChanged || !changed.empty())
        for (const auto& composition : compositions_)
            if (composition.folder() && !findAssetFolder(*composition.folder()))
                return notProven();

    // Changed compositions: their own validation -- #744: only the records a composition that
    // existed in `base` does not share with it -- then project-wide ID uniqueness of the ids each
    // newly declares against every other composition.
    for (auto& item : changed) {
        compositionsValidated.fetch_add(1, std::memory_order_relaxed);
        const auto& composition = compositions_[item.ordinal];
        const bool frozenBase = item.base != nullptr && item.base->frozenDeclaredIds() != nullptr;
        if (frozenBase ? !composition.validatesAgainst(*item.base) : !composition.validate().ok())
            return notProven();
        // #642: the SVG data block a changed composition's Vector Artwork names must exist.
        if (!validateVectorArtworkReferences(*this, composition).ok())
            return notProven();
        if (frozenBase)
            Composition::declaredIdChangesBetween(item.base->content(), composition.content(),
                                                  item.added, item.removed);
        else
            item.fresh = Composition::buildDeclaredIdIndex(composition.content());
    }
    using Field = std::vector<std::uint64_t> Composition::DeclaredIdChanges::*;
    using IndexField = Composition::DeclaredIdIndex::Ids Composition::DeclaredIdIndex::*;
    struct Kind final {
        Field changes;
        IndexField index;
    };
    const auto declaredBy = [&](const Composition& composition, const Changed* edit,
                                const Kind* kind, const std::uint64_t id) {
        // Keyframes are the one kind held in the chunked index rather than a sorted list.
        const auto held = [&](const Composition::DeclaredIdIndex& index) {
            return kind == nullptr ? index.keyframes.contains(KeyframeId::fromRaw(id))
                                   : contains(*(index.*(kind->index)), id);
        };
        if (edit == nullptr)
            return held(*composition.frozenDeclaredIds());
        if (edit->fresh)
            return held(*edit->fresh);
        const auto& added = kind == nullptr ? edit->added.keyframes : edit->added.*(kind->changes);
        const auto& removed =
            kind == nullptr ? edit->removed.keyframes : edit->removed.*(kind->changes);
        return contains(added, id) ||
               (held(*edit->base->frozenDeclaredIds()) && !contains(removed, id));
    };
    const auto declaredElsewhere = [&](const std::size_t ordinal, const Kind* kind,
                                       const std::uint64_t id) {
        for (const auto other : unchanged)
            if (declaredBy(compositions_[other], nullptr, kind, id))
                return true;
        for (const auto& other : changed)
            if (other.ordinal != ordinal &&
                declaredBy(compositions_[other.ordinal], &other, kind, id))
                return true;
        return false;
    };
    const std::array<Kind, 7> kinds{{
        {&Composition::DeclaredIdChanges::nodes, &Composition::DeclaredIdIndex::nodes},
        {&Composition::DeclaredIdChanges::edges, &Composition::DeclaredIdIndex::edges},
        {&Composition::DeclaredIdChanges::parameters, &Composition::DeclaredIdIndex::parameters},
        {&Composition::DeclaredIdChanges::curves, &Composition::DeclaredIdIndex::curves},
        {&Composition::DeclaredIdChanges::layers, &Composition::DeclaredIdIndex::layers},
        {&Composition::DeclaredIdChanges::layerSlots, &Composition::DeclaredIdIndex::layerSlots},
        {&Composition::DeclaredIdChanges::groups, &Composition::DeclaredIdIndex::groups},
    }};
    if (compositions_.size() > 1) {
        for (const auto& item : changed) {
            for (const auto& kind : kinds) {
                const auto& declared =
                    item.fresh ? *((*item.fresh).*(kind.index)) : item.added.*(kind.changes);
                for (const auto id : declared)
                    if (declaredElsewhere(item.ordinal, &kind, id))
                        return notProven();
            }
            const auto checkKeyframe = [&](const std::uint64_t id) {
                return !declaredElsewhere(item.ordinal, nullptr, id);
            };
            if (item.fresh) {
                bool unique = true;
                item.fresh->keyframes.forEach(
                    [&](const KeyframeId id) { unique = unique && checkKeyframe(id.value()); });
                if (!unique)
                    return notProven();
            } else if (!std::ranges::all_of(item.added.keyframes, checkKeyframe)) {
                return notProven();
            }
        }
    }

    if (compositionSetChanged && !validateCompositionNesting().ok())
        return notProven();

    // Extension records and data blocks resolve targets through project truth. Their storage
    // change, or a composition change while any of them can name composition content, reruns
    // them whole.
    bool crossRecordsAffected = rootChanged;
    if (!crossRecordsAffected && compositionSetChanged) {
        // Any owner or subject may name a composition or its content.
        crossRecordsAffected = !extensionRecords_.get().empty() ||
                               std::ranges::any_of(dataBlocks_.get(), [](const auto& block) {
                                   return block.owner.has_value() || block.subject.has_value();
                               });
    }
    if (crossRecordsAffected) {
        rootSectionValidations.fetch_add(1, std::memory_order_relaxed);
        ValidationResult cross;
        validateCrossRecords(cross);
        if (!cross.ok())
            return notProven();
    }
    return true;
}

} // namespace bloom::document
