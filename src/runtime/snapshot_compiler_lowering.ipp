[[nodiscard]] std::optional<std::vector<document::NodeId>> makeTopologicalOrder() {
    std::unordered_map<document::NodeId, std::size_t> indegree;
    std::unordered_map<document::NodeId, std::vector<document::NodeId>> outgoing;
    for (const auto* node : reachableNodes_) {
        if (cancelled()) {
            return std::nullopt;
        }
        indegree.emplace(node->id, 0);
    }
    for (const auto* edge : reachableEdges_) {
        if (cancelled()) {
            return std::nullopt;
        }
        outgoing[edge->source.nodeId].push_back(destinationNode(edge->destination));
        ++indegree[destinationNode(edge->destination)];
    }
    // Driver bindings are edges for ordering purposes (task S7): a node whose parameter is driven
    // depends on the value node that drives it, so it has to come after it. Feeding them into THIS
    // map rather than ordering the value graph separately is what makes a cycle through a driver a
    // cycle the one existing check reports.
    for (const auto* node : reachableNodes_) {
        if (cancelled()) {
            return std::nullopt;
        }
        for (const auto& reference : driverReferences(*node)) {
            if (!indegree.contains(reference.source.nodeId)) {
                continue;
            }
            outgoing[reference.source.nodeId].push_back(node->id);
            ++indegree[node->id];
        }
    }

    // #502: an ordering edge that no document edge carries.
    const auto orderBefore = [&](const document::NodeId before, const document::NodeId after) {
        if (before == after || !indegree.contains(before) || !indegree.contains(after))
            return;
        outgoing[before].push_back(after);
        ++indegree[after];
    };

    for (const auto* node : reachableNodes_) {
        const auto boundary = layerOutputs_.find(node->id);
        if (boundary == layerOutputs_.end())
            continue;
        const auto parentId = boundary->second->parent;
        if (!parentId)
            continue;
        const auto* parent = composition_->graph().findLayer(*parentId);
        // #502: an entry layer parent is lowered on demand from its slot's content.
        if (parent && parent->entryOwned()) {
            std::vector<document::NodeId> dependencies;
            std::unordered_set<document::LayerId> seen;
            layerOperationDependencies(*parent, dependencies, seen);
            for (const auto dependency : dependencies)
                orderBefore(dependency, node->id);
        } else if (parent && indegree.contains(parent->nodeId)) {
            outgoing[parent->nodeId].push_back(node->id);
            ++indegree[node->id];
        }
    }

    // #383: a Layer node lowers its matte masks with its content, so after their nodes.
    for (const auto* node : reachableNodes_)
        if (const auto boundary = layerOutputs_.find(node->id); boundary != layerOutputs_.end())
            for (const auto& mask : boundary->second->masks)
                if (mask.matte)
                    orderBefore(mask.matte->nodeId, node->id);

    // #502: a Merge lowers its entries' own layers, parents and track mattes on demand, and an
    // Adjustment lowers the entries below its entry, so each comes after what those need.
    for (const auto* node : reachableNodes_) {
        if (const auto* stack = composition_->graph().merge(node->id);
            stack != nullptr && stack->enabled()) {
            for (const auto& entry : stack->entries())
                if (entryLowered(node->id, entry)) {
                    std::vector<document::NodeId> dependencies;
                    entryDependencies(node->id, entry, dependencies);
                    for (const auto dependency : dependencies)
                        orderBefore(dependency, node->id);
                }
        }
        if (node->typeId != document::kAdjustmentNodeType || isMuted(node->id))
            continue;
        const auto target = singleAdjustmentTarget(node->id);
        const auto* stack = target ? composition_->graph().merge(target->merge) : nullptr;
        if (stack == nullptr || !stack->enabled())
            continue;
        bool below = false;
        for (const auto& entry : stack->entries()) {
            if (below && entryLowered(target->merge, entry)) {
                std::vector<document::NodeId> dependencies;
                entryDependencies(target->merge, entry, dependencies);
                for (const auto dependency : dependencies)
                    orderBefore(dependency, node->id);
            }
            below = below || entry.slotId == target->slot;
        }
    }

    // #590: a function consumer lowers its group's body, so after the body's last node.
    for (const auto& [key, body] : expansion_.functionBodies)
        orderBefore(body.output, body.consumer);

    auto laterId = [](const document::NodeId left, const document::NodeId right) {
        return left.value() > right.value();
    };
    std::priority_queue<document::NodeId, std::vector<document::NodeId>, decltype(laterId)> ready(
        laterId);
    for (const auto* node : reachableNodes_) {
        if (cancelled()) {
            return std::nullopt;
        }
        if (indegree.at(node->id) == 0) {
            ready.push(node->id);
        }
    }

    std::vector<document::NodeId> order;
    order.reserve(reachableNodes_.size());
    while (!ready.empty()) {
        if (cancelled()) {
            return std::nullopt;
        }
        const auto nodeId = ready.top();
        ready.pop();
        order.push_back(nodeId);
        if (const auto targets = outgoing.find(nodeId); targets != outgoing.end()) {
            for (const auto target : targets->second) {
                if (cancelled()) {
                    return std::nullopt;
                }
                auto& degree = indegree.at(target);
                --degree;
                if (degree == 0) {
                    ready.push(target);
                }
            }
        }
    }
    if (order.size() != reachableNodes_.size()) {
        addFailure(runtime::CompileDiagnosticCode::TopologyInvariant, subject({}, "edges"),
                   "Reachable graph contains a cycle",
                   "The published graph violates its acyclic processing invariant.");
        return std::nullopt;
    }
    return order;
}

// #502: whether a Merge lowers this entry's image (its Merge's consumption rules).
[[nodiscard]] bool entryLowered(const document::NodeId merge,
                                const document::LayerStackEntry& entry) const {
    const auto* stack = composition_->graph().merge(merge);
    if (stack == nullptr || !stack->enabled() || stack->entries().empty())
        return false;
    if (isMuted(merge) && entry.slotId != stack->entries().front().slotId)
        return false;
    return !entryExcluded(merge, entry);
}

// #502: the nodes a layer's operation is lowered from: a Layer node's own node, or an entry
// layer's slot content and its parent's.
void layerOperationDependencies(const document::LayerOutputBoundary& layer,
                                std::vector<document::NodeId>& dependencies,
                                std::unordered_set<document::LayerId>& seen) const {
    if (!seen.insert(layer.layerId).second)
        return;
    if (!layer.entryOwned()) {
        dependencies.push_back(layer.nodeId);
        return;
    }
    if (const auto* edge = layerSlotInputEdge(layer.nodeId, *layer.entrySlot,
                                              document::kLayerStackContentInputRole))
        dependencies.push_back(edge->source.nodeId);
    // #383: an entry layer's matte masks are lowered with it.
    for (const auto& mask : layer.masks)
        if (mask.matte)
            dependencies.push_back(mask.matte->nodeId);
    if (layer.parent)
        if (const auto* parent = composition_->graph().findLayer(*layer.parent))
            layerOperationDependencies(*parent, dependencies, seen);
}

// #502: what lowering one Merge entry needs: its content, its own layer and its track matte.
void entryDependencies(const document::NodeId merge, const document::LayerStackEntry& entry,
                       std::vector<document::NodeId>& dependencies) const {
    if (const auto* edge =
            layerSlotInputEdge(merge, entry.slotId, document::kLayerStackContentInputRole))
        dependencies.push_back(edge->source.nodeId);
    const auto* layer = entryLayerOf(merge, entry.slotId);
    if (layer == nullptr)
        return;
    std::unordered_set<document::LayerId> seen;
    if (layer->entryOwned())
        layerOperationDependencies(*layer, dependencies, seen);
    else if (layer->parent)
        // An adjustment entry's coverage is placed with its Layer node's parent.
        if (const auto* parent = composition_->graph().findLayer(*layer->parent))
            layerOperationDependencies(*parent, dependencies, seen);
    if (layer->trackMatte && layer->trackMatte->node)
        dependencies.push_back(layer->trackMatte->node->nodeId); // #383: a node matte
    else if (layer->trackMatte)
        if (const auto* source = composition_->graph().findLayer(layer->trackMatte->source))
            layerOperationDependencies(*source, dependencies, seen);
}

[[nodiscard]] std::optional<CompiledCurveTables> compileReachableCurves() {
    std::unordered_set<document::AnimationCurveId> reachableCurveIds;
    const auto addParameterCurve = [&](const document::ParameterId id) {
        const auto* parameter = findParameter(id);
        const auto* source = parameter == nullptr
                                 ? nullptr
                                 : std::get_if<document::AnimationCurveSource>(&parameter->source);
        if (source != nullptr)
            reachableCurveIds.insert(source->curveId);
    };
    for (const auto* node : reachableNodes_) {
        if (cancelled()) {
            return std::nullopt;
        }
        if ((isMuted(node->id) || emptyImages_.contains(node->id)) &&
            !transformParents_.contains(node->id))
            continue;
        for (const auto& binding : node->parameters) {
            if (cancelled()) {
                return std::nullopt;
            }
            // #524, #514: a value the node does not read in its current mode is not lowered, so its
            // curve is not compiled either (a compiled curve nothing references refuses the plan).
            if (const auto* parameter = findParameter(binding.parameterId);
                parameter != nullptr &&
                document::isParameterInactive(*composition_, *node, parameter->schemaKey))
                continue;
            addParameterCurve(binding.parameterId);
        }
        // #509: a Text Outline samples its text's keyed values whether or not the text is drawn.
        if (const auto* source = pathSourceNodeOf(*node))
            for (const auto& binding : source->parameters)
                addParameterCurve(binding.parameterId);
        // ADR 0028: a reachable layer's masks own their parameters outside any node binding.
        if (const auto boundary = layerOutputs_.find(node->id); boundary != layerOutputs_.end())
            for (const auto& mask : boundary->second->masks)
                for (const auto& [id, schema] : document::layerMaskParameters(mask)) {
                    static_cast<void>(schema);
                    addParameterCurve(id);
                }
    }
    // #502: a reached entry layer owns its parameters and masks outside any node binding.
    for (const auto* layer : reachedEntryLayers_) {
        for (const auto& binding : layer->parameters)
            addParameterCurve(binding.parameterId);
        for (const auto& mask : layer->masks)
            for (const auto& [id, schema] : document::layerMaskParameters(mask)) {
                static_cast<void>(schema);
                addParameterCurve(id);
            }
    }
    // #436: the audio mix samples an audible layer's Time Remap whether or not the layer has a
    // picture. An audio-only Layer Output is an empty image, so the loop above skips it; its remap
    // is compiled here for the mix. An audible layer's source is reachable and unmuted, so the loop
    // above already compiled its level.
    for (const auto layerNodeId : audibleLayers_) {
        const auto* node = findNode(layerNodeId);
        if (const auto* binding =
                node ? findParameterBinding(*node, document::kTimeRemapParameterRole) : nullptr)
            addParameterCurve(binding->parameterId);
    }
    // Path curves are compiled inline into the mask kernel that samples them (ADR 0028), and
    // Gradient curves into the Colorize kernel that samples them (#541).
    std::erase_if(reachableCurveIds, [&](const document::AnimationCurveId id) {
        return composition_->animationCurves().findPath(id) != nullptr ||
               composition_->animationCurves().findGradient(id) != nullptr;
    });

    CompiledCurveTables tables;
    for (const auto& record : composition_->animationCurves().records()) {
        if (cancelled()) {
            return std::nullopt;
        }
        const auto curveId = document::animationCurveId(record);
        if (!reachableCurveIds.contains(curveId)) {
            continue;
        }
        // The document -> compiled per-curve conversion itself is the shared, pure
        // runtime::compileAnimationCurve() (issue #86, task E1;
        // bloom/runtime/curve_compilation.hpp)
        // -- this loop keeps only the per-curve index bookkeeping and cancellation checkpoints that
        // are specific to compiling a REACHABLE SET of curves.
        std::visit(
            [&](const auto& curve) {
                using Curve = std::decay_t<decltype(curve)>;
                if constexpr (std::is_same_v<Curve, document::ScalarAnimationCurve>) {
                    scalarCurveIndices_.emplace(
                        curve.id, runtime::ScalarCurveIndex::fromRaw(tables.scalar.size()));
                    tables.scalar.push_back(runtime::compileAnimationCurve(curve));
                } else if constexpr (std::is_same_v<Curve, document::Vec2AnimationCurve>) {
                    vec2CurveIndices_.emplace(curve.id,
                                              runtime::Vec2CurveIndex::fromRaw(tables.vec2.size()));
                    tables.vec2.push_back(runtime::compileAnimationCurve(curve));
                    for (const auto& parameter : composition_->parameters().records()) {
                        const auto* source =
                            std::get_if<document::AnimationCurveSource>(&parameter.source);
                        if (source != nullptr && source->curveId == curve.id &&
                            source->defaultValue.has_value()) {
                            if (const auto* value =
                                    std::get_if<document::Vec2d>(&*source->defaultValue))
                                tables.vec2.back().defaultValue = *value;
                        }
                    }
                } else if constexpr (std::is_same_v<Curve, document::Vec3AnimationCurve>) {
                    vec3CurveIndices_.emplace(curve.id,
                                              runtime::Vec3CurveIndex::fromRaw(tables.vec3.size()));
                    tables.vec3.push_back(runtime::compileAnimationCurve(curve));
                    for (const auto& parameter : composition_->parameters().records()) {
                        const auto* source =
                            std::get_if<document::AnimationCurveSource>(&parameter.source);
                        if (source != nullptr && source->curveId == curve.id &&
                            source->defaultValue.has_value()) {
                            if (const auto* value =
                                    std::get_if<document::Vec3d>(&*source->defaultValue))
                                tables.vec3.back().defaultValue = *value;
                        }
                    }
                } else if constexpr (std::is_same_v<Curve, document::PathAnimationCurve> ||
                                     std::is_same_v<Curve, document::GradientAnimationCurve>) {
                    // Unreachable: Path and Gradient curves were removed from the reachable set
                    // above.
                } else {
                    color4CurveIndices_.emplace(
                        curve.id, runtime::Color4CurveIndex::fromRaw(tables.color4.size()));
                    tables.color4.push_back(runtime::compileAnimationCurve(curve));
                    for (const auto& parameter : composition_->parameters().records()) {
                        const auto* source =
                            std::get_if<document::AnimationCurveSource>(&parameter.source);
                        if (source != nullptr && source->curveId == curve.id &&
                            source->defaultValue.has_value()) {
                            if (const auto* value =
                                    std::get_if<core::Color4d>(&*source->defaultValue))
                                tables.color4.back().defaultValue = *value;
                        }
                    }
                }
            },
            record);
        if (cancelled()) {
            return std::nullopt;
        }
    }
    if (scalarCurveIndices_.size() + vec2CurveIndices_.size() + vec3CurveIndices_.size() +
            color4CurveIndices_.size() !=
        reachableCurveIds.size()) {
        addTopologyFailure({}, "A reachable animation curve could not be lowered.");
        return std::nullopt;
    }
    return tables;
}

[[nodiscard]] std::shared_ptr<const runtime::CompiledCompositionPlan>
lower(const std::vector<document::NodeId>& order) {
    // Empty image values need no fake parameter IDs or evaluator operations. Propagate them
    // through bypasses and transparent-preserving boundaries before compiling curve tables.
    for (const auto id : order) {
        if (cancelled())
            return {};
        const auto* node = findNode(id);
        if (!node)
            continue;
        const auto* definition = registry_.find(node->typeId, node->schemaVersion);
        if (!definition || definition->lowering == runtime::NodeLoweringKind::LayerStack ||
            definition->lowering == runtime::NodeLoweringKind::CompositionOutput)
            continue;
        // A value node carries no pixels, so "empty image" is not a state it can be in -- and a
        // muted one must not be classified as one, or the parameter it drives would lose its
        // source.
        if (isValueNode(id))
            continue;
        // #525: a Fields node's outputs are fields and its own rasters; a muted one passes its
        // field through and draws nothing (lowerFieldNode()).
        if (definition->lowering == runtime::NodeLoweringKind::Field)
            continue;
        // #502: an Adjustment draws its target entry's stack below; with no single target, or
        // muted, it draws nothing. Several targets are a mistake the artist is told about.
        if (definition->lowering == runtime::NodeLoweringKind::Adjustment) {
            if (!isMuted(id) && adjustmentTargetsOf(id).size() > 1)
                addWarning(runtime::CompileDiagnosticCode::AdjustmentTargetAmbiguous,
                           subject(id, "image"),
                           "Adjustment reaches more than one Merge entry; it draws nothing",
                           "An Adjustment's image is the stack below the one Merge entry its "
                           "chain lands in. Connect its chain to a single entry.");
            if (isMuted(id) || !singleAdjustmentTarget(id))
                emptyImages_.insert(id);
            continue;
        }
        if (!isMuted(id) && definition->lowering != runtime::NodeLoweringKind::LayerOutput &&
            definition->lowering != runtime::NodeLoweringKind::ImageEffect)
            continue;
        auto input = firstImageInput(*node);
        // #479 Combine Channels' first CONNECTED input is its base, which owns every window: the
        // result is empty when that input is, or when no input is connected at all.
        if (node->typeId == document::kCombineChannelsNodeType && !isMuted(id)) {
            input.reset();
            for (const auto port : document::kImageChannelPortNames)
                if (fixedInputEdge(id, port) != nullptr) {
                    input = document::NodeInputRef{id, std::string(port)};
                    break;
                }
            if (!input) {
                emptyImages_.insert(id);
                continue;
            }
        }
        // #338 Luma Key reads an Image and publishes a Mask, so muted it carries nothing through
        // (an empty Mask), but an empty Image upstream makes its Mask empty too.
        if (!input && !isMuted(id))
            for (const auto& candidate : definition->inputs)
                if (candidate.valueKind == runtime::SocketValueKind::Image) {
                    input = document::NodeInputRef{id, candidate.name};
                    break;
                }
        const auto edge = input ? std::ranges::find_if(reachableEdges_,
                                                       [&](const auto* candidate) {
                                                           return candidate->destination == *input;
                                                       })
                                : reachableEdges_.end();
        // A Layer Output with nothing feeding its content port is empty whether it is muted or not
        // (task FIX1, item B). The artist wires a Layer node up by hand, so "added but not yet fed"
        // is an ordinary intermediate state; it draws nothing and says nothing, rather than failing
        // the whole compile on a required input.
        if ((edge == reachableEdges_.end() &&
             (isMuted(id) || definition->lowering == runtime::NodeLoweringKind::LayerOutput)) ||
            (edge != reachableEdges_.end() && emptyImages_.contains((*edge)->source.nodeId))) {
            emptyImages_.insert(id);
        }
    }
    collectAudibleLayers();
    auto curveTables = compileReachableCurves();
    if (!curveTables.has_value()) {
        return {};
    }
    // The value graph is compiled FIRST, and in the same order: every parameter the image pass
    // lowers may name one of its outputs, so the whole of it has to exist before a single image
    // operation is built. It shares no address space with the image chain -- a ValueOutputIndex and
    // an OperationIndex are different things -- which is exactly why the two passes can be
    // sequential rather than interleaved.
    if (!compileValueGraph(order)) {
        return {};
    }
    std::vector<runtime::CompiledOperation> operations;
    operations.reserve(order.size());
    std::unordered_map<document::NodeId, runtime::OperationIndex> indices;
    loweringOperations_ = &operations;

    for (const auto nodeId : order) {
        if (cancelled()) {
            return {};
        }
        const auto* node = findNode(nodeId);
        const auto definition = definitions_.find(nodeId);
        if (node == nullptr || definition == definitions_.end()) {
            addTopologyFailure(nodeId, "A reachable node lost its registered definition.");
            return {};
        }
        // Audio sources are compiled into the sibling audio mix below. They must remain in the
        // reachable/topological set so audio edges participate in graph validation, but never enter
        // the image operation variant or alter image evaluation semantics.
        if (definition->second->lowering == runtime::NodeLoweringKind::AudioSource ||
            definition->second->lowering == runtime::NodeLoweringKind::AudioEffect)
            continue;
        if (definition->second->lowering == runtime::NodeLoweringKind::CompositionSource &&
            std::ranges::none_of(reachableEdges_, [&](const auto* edge) {
                return edge->source.nodeId == nodeId && edge->source.port == "image";
            }))
            continue;
        // ADR 0026: an Image source whose colour `image` output feeds nothing is reachable only
        // through its `channels` output, which compiles into a Shuffle's channel expression.
        if (definition->second->lowering == runtime::NodeLoweringKind::ImageSource &&
            !hasReachableOutput(nodeId, document::kImageSourceImageOutputPort))
            continue;
        if (emptyImages_.contains(nodeId) && !transformParents_.contains(nodeId))
            continue;
        if (isValueNode(nodeId))
            continue;
        // #525: a Fields node's rasters; its field outputs compile into whoever reads them.
        if (definition->second->lowering == runtime::NodeLoweringKind::Field) {
            if (!lowerFieldNode(*node, operations, indices))
                return {};
            continue;
        }
        if (!lowerFieldImageInputs(*node, *definition->second, operations, indices))
            return {};
        // #502: an Adjustment's operation is the stack below its entry.
        if (definition->second->lowering == runtime::NodeLoweringKind::Adjustment) {
            const auto below = lowerStackBelowAdjustment(nodeId, indices);
            if (!below)
                return {};
            indices.emplace(nodeId, *below);
            continue;
        }
        // Channel algebra (Remove, Channel Copy, a Shuffle's channel output) touches no pixels and
        // compiles into the expression of the Shuffle whose image output reads it.
        if (definition->second->lowering == runtime::NodeLoweringKind::Channel) {
            // #480: a Pass is a Shuffle whose slots follow its picked pass.
            const bool shuffleImage =
                node->typeId == document::kChannelShuffleNodeType &&
                hasReachableOutput(nodeId, document::kChannelShuffleImagePort);
            const bool passImage = node->typeId == document::kChannelPassNodeType &&
                                   hasReachableOutput(nodeId, document::kChannelPassImagePort);
            if (!shuffleImage && !passImage)
                continue;
            const auto shuffled = lowerChannelShuffle(*node, operations, indices);
            if (!shuffled)
                return {};
            indices.emplace(nodeId, *shuffled);
            continue;
        }
        // #587 Test Card: a canvas, one field raster and the color space transform of its signal.
        if (node->typeId == document::kTestCardNodeType) {
            if (!lowerTestCard(*node, operations, indices))
                return {};
            continue;
        }
        // Four-Color Gradient: a canvas and one field raster of four weighted colors.
        if (node->typeId == document::kFourColorGradientNodeType) {
            if (!lowerFourColorGradient(*node, operations, indices))
                return {};
            continue;
        }
        // #486 Lens Distortion: the warped image and the ST map, each its own field raster. Muting
        // is the identity warp, so it is handled here and not as a bypass of the image alone.
        if (node->typeId == document::kLensDistortionNodeType) {
            if (!lowerLensDistortion(*node, operations, indices))
                return {};
            continue;
        }
        // #486 Chromatic Aberration: one field raster; muting is the identity, so it is handled here.
        if (node->typeId == document::kChromaticAberrationNodeType) {
            if (!lowerChromaticAberration(*node, operations, indices))
                return {};
            continue;
        }
        // #508 Light Rays: one field raster; muting is the identity, so it is handled here.
        if (node->typeId == document::kLightRaysNodeType) {
            if (!lowerLightRays(*node, operations, indices))
                return {};
            continue;
        }
        // #568 Rolling Shutter: one field raster; muting is the identity, so it is handled here.
        if (node->typeId == document::kRollingShutterNodeType) {
            if (!lowerRollingShutter(*node, operations, indices))
                return {};
            continue;
        }
        // #348 Corner Pin, ST Map and Displace: one field raster each; muting is the input itself.
        if (node->typeId == document::kCornerPinNodeType) {
            if (!lowerCornerPin(*node, operations, indices))
                return {};
            continue;
        }
        if (node->typeId == document::kStabilizeNodeType) {
            if (!lowerStabilize(*node, operations, indices))
                return {};
            continue;
        }
        if (node->typeId == document::kTileMirrorNodeType) {
            if (!lowerTileMirror(*node, operations, indices))
                return {};
            continue;
        }
        if (node->typeId == document::kProceduralWarpNodeType) {
            if (!lowerProceduralWarp(*node, operations, indices))
                return {};
            continue;
        }
        if (node->typeId == document::kStMapNodeType) {
            if (!lowerStMap(*node, operations, indices))
                return {};
            continue;
        }
        if (node->typeId == document::kDisplaceNodeType) {
            if (!lowerDisplace(*node, operations, indices))
                return {};
            continue;
        }
        // #364 Cryptomatte: the matte (Mask) and, with an input image, the image multiplied by
        // the matte (MultiplyAlpha, matte in alpha); without one the Image output is the matte.
        if (definition->second->lowering == runtime::NodeLoweringKind::Cryptomatte) {
            const bool inspected =
                request_.channelInspection && request_.channelInspection->node == nodeId;
            const bool previewInspected =
                inspected &&
                request_.channelInspection->port == document::kCryptomattePreviewInspectionPort;
            const bool maskUsed =
                hasReachableOutput(nodeId, document::kCryptomatteMaskOutputPort) ||
                (inspected && !previewInspected);
            const bool imageUsed =
                hasReachableOutput(nodeId, document::kCryptomatteImageOutputPort);
            const bool imageConnected =
                fixedInputEdge(nodeId, document::kCryptomatteImageInputPort) != nullptr;
            const auto imageInput =
                findInputOperation(nodeId, document::kCryptomatteImageInputPort, indices);
            const auto emptyImage = [&] {
                const auto empty = runtime::OperationIndex::fromRaw(operations.size());
                operations.emplace_back(runtime::CompiledMerge{nodeId, {}});
                return empty;
            };
            if (previewInspected) {
                // #364 Viewer preview: the ID colours mixed halfway with the input image (Image
                // Mix with the preview as its base, so a data-only or absent image -- a
                // transparent second input -- still shows the colours).
                const auto previewPort = std::string(document::kCryptomattePreviewInspectionPort);
                if (isMuted(nodeId)) {
                    portOperations_.insert_or_assign({nodeId, previewPort},
                                                     imageInput ? *imageInput : emptyImage());
                    continue;
                }
                auto lowered = lowerCryptomatte(*node);
                if (!lowered)
                    return {};
                std::get<runtime::CompiledCryptomatte>(*lowered).preview = true;
                const auto preview = runtime::OperationIndex::fromRaw(operations.size());
                operations.emplace_back(std::move(*lowered));
                indices.emplace(nodeId, preview);
                auto shown = preview;
                if (imageInput) {
                    shown = runtime::OperationIndex::fromRaw(operations.size());
                    operations.emplace_back(runtime::CompiledImageEffect{
                        nodeId, preview,
                        runtime::ImageCombineKernel{
                            runtime::ImageCombineMode::Mix,
                            runtime::CompiledScalarParameter{document::ParameterId{}, 0.5},
                            render::ImageCoverageChannel::Alpha},
                        false, false, *imageInput});
                }
                portOperations_.insert_or_assign({nodeId, previewPort}, shown);
                continue;
            }
            if (isMuted(nodeId)) {
                // A muted Cryptomatte isolates nothing: no matte, and the image passes through.
                if (maskUsed)
                    portOperations_.insert_or_assign(
                        {nodeId, std::string(document::kCryptomatteMaskOutputPort)}, emptyImage());
                if (imageUsed)
                    portOperations_.insert_or_assign(
                        {nodeId, std::string(document::kCryptomatteImageOutputPort)},
                        imageInput ? *imageInput : emptyImage());
                continue;
            }
            auto cryptomatte = lowerCryptomatte(*node);
            if (!cryptomatte)
                return {};
            const auto matte = runtime::OperationIndex::fromRaw(operations.size());
            operations.emplace_back(std::move(*cryptomatte));
            indices.emplace(nodeId, matte);
            portOperations_.insert_or_assign(
                {nodeId, std::string(document::kCryptomatteMaskOutputPort)}, matte);
            if (imageUsed) {
                auto image = matte;
                if (imageInput) {
                    image = runtime::OperationIndex::fromRaw(operations.size());
                    operations.emplace_back(runtime::CompiledImageEffect{
                        nodeId, *imageInput,
                        runtime::ImageCombineKernel{
                            runtime::ImageCombineMode::MultiplyAlpha,
                            runtime::CompiledScalarParameter{document::ParameterId{}, 1.0},
                            render::ImageCoverageChannel::Alpha, std::nullopt, false},
                        false, false, matte});
                } else if (imageConnected) {
                    image = emptyImage(); // an empty input image stays empty
                }
                portOperations_.insert_or_assign(
                    {nodeId, std::string(document::kCryptomatteImageOutputPort)}, image);
            }
            continue;
        }
        // An Image Reroute is ELIDED rather than compiled: its consumers read its input's operation
        // directly, so it costs nothing at evaluation -- the same treatment a muted node's bypass
        // already gets, and the generalisation of DissolveNode's single Image pair to a node that
        // exists only to tidy a wire.
        if (isImageReroute(nodeId)) {
            const auto* rerouteEdge = fixedInputEdge(nodeId, document::kValuePortName);
            const auto source = rerouteEdge == nullptr
                                    ? std::optional<runtime::OperationIndex>{}
                                    : sourceOperation(rerouteEdge->source, indices);
            if (!source) {
                addTopologyFailure(nodeId, "Image Reroute input was not lowered.");
                return {};
            }
            indices.emplace(nodeId, *source);
            continue;
        }
        if (isMuted(nodeId) && !transformParents_.contains(nodeId) &&
            definition->second->lowering != runtime::NodeLoweringKind::LayerStack &&
            definition->second->lowering != runtime::NodeLoweringKind::CompositionOutput) {
            const auto input = firstImageInput(*node);
            const auto edge =
                input ? std::ranges::find_if(
                            reachableEdges_,
                            [&](const auto* candidate) { return candidate->destination == *input; })
                      : reachableEdges_.end();
            if (edge != reachableEdges_.end()) {
                const auto source = sourceOperation((*edge)->source, indices);
                if (!source) {
                    addTopologyFailure(nodeId, "Muted node input was not lowered.");
                    return {};
                }
                indices.emplace(nodeId, *source);
            } else {
                addTopologyFailure(nodeId, "Muted empty image was not classified.");
                return {};
            }
            continue;
        }
        // #479 Separate Channels: one Channel Select per used output, each a grey image of its
        // channel (R = G = B = the stored channel, A = 1) over the input's data window.
        if (node->typeId == document::kSeparateChannelsNodeType) {
            const auto input =
                findInputOperation(nodeId, document::kSeparateChannelsInputPort, indices);
            if (!input) {
                addTopologyFailure(nodeId, "Separate Channels input could not be lowered.");
                return {};
            }
            std::optional<runtime::OperationIndex> first;
            for (std::size_t channel = 0; channel < document::kImageChannelCount; ++channel) {
                const auto port = document::kImageChannelPortNames[channel];
                const bool inspected = request_.channelInspection &&
                                       request_.channelInspection->node == nodeId &&
                                       request_.channelInspection->port == port;
                if (!hasReachableOutput(nodeId, port) && !inspected)
                    continue;
                const auto source = static_cast<runtime::ImageChannelSource>(channel);
                const auto index = runtime::OperationIndex::fromRaw(operations.size());
                operations.emplace_back(runtime::CompiledImageEffect{
                    nodeId, *input,
                    runtime::ChannelSelectKernel{
                        {source, source, source, runtime::ImageChannelSource::One}}});
                portOperations_.insert_or_assign({nodeId, std::string(port)}, index);
                if (!first)
                    first = index;
            }
            if (first)
                indices.emplace(nodeId, *first);
            continue;
        }
        // #479 Combine Channels: a Channel Select of the base, then a Channel Combine per further
        // connected input.
        if (node->typeId == document::kCombineChannelsNodeType) {
            const auto combined = lowerCombineChannels(*node, operations, indices);
            if (!combined)
                return {};
            indices.emplace(nodeId, *combined);
            continue;
        }
        // #339 Drop Shadow, Glow and Light Wrap: a chain of the existing primitives.
        if (isRecipeNodeType(node->typeId)) {
            const auto lowered = lowerRecipe(*node, operations, indices);
            if (!lowered)
                return {};
            indices.emplace(nodeId, *lowered);
            continue;
        }
        // #484 the motion-blur nodes: the temporal-sampling engine's kernels, then the effect
        // mask (Optical Flow has none).
        if (document::isMotionBlurNodeType(node->typeId)) {
            const auto lowered = lowerMotionNode(*node, operations, indices);
            if (!lowered)
                return {};
            const auto masked = applyEffectMask(*node, *lowered, indices);
            if (!masked)
                return {};
            indices.emplace(nodeId, *masked);
            continue;
        }
        // Gaussian Blur with a radius per channel: a chain of blurs and channel combines.
        if (node->typeId == "bloom.gaussian-blur") {
            const auto* perChannel =
                parameterConstant<bool>(findParameterBinding(*node, "perChannel"));
            if (perChannel != nullptr && *perChannel) {
                const auto lowered = lowerChannelBlur(*node, operations, indices);
                if (!lowered)
                    return {};
                indices.emplace(nodeId, *lowered);
                continue;
            }
        }
        // #514 Denoise & Smooth: one neighborhood pass, or the temporal mean, then the effect mask.
        if (node->typeId == document::kDenoiseNodeType) {
            const auto lowered = lowerDenoise(*node, operations, indices);
            if (!lowered)
                return {};
            const auto masked = applyEffectMask(*node, *lowered, indices);
            if (!masked)
                return {};
            indices.emplace(nodeId, *masked);
            continue;
        }
        // #486 Sharpen: its input's blur and the unsharp mask, then the effect mask.
        if (node->typeId == document::kSharpenNodeType) {
            const auto lowered = lowerSharpen(*node, operations, indices);
            if (!lowered)
                return {};
            const auto masked = applyEffectMask(*node, *lowered, indices);
            if (!masked)
                return {};
            indices.emplace(nodeId, *masked);
            continue;
        }
        // #563 Codec Degrade: its look-back chain, then the effect mask.
        if (node->typeId == document::kCodecDegradeNodeType) {
            const auto lowered = lowerCodecDegrade(*node, operations, indices);
            if (!lowered)
                return {};
            const auto masked = applyEffectMask(*node, *lowered, indices);
            if (!masked)
                return {};
            indices.emplace(nodeId, *masked);
            continue;
        }
        // #566 Auto Exposure: its look-back window of reductions, filter and gain, then the effect
        // mask.
        if (node->typeId == document::kAutoExposureNodeType) {
            const auto lowered = lowerAutoExposure(*node, operations, indices);
            if (!lowered)
                return {};
            const auto masked = applyEffectMask(*node, *lowered, indices);
            if (!masked)
                return {};
            indices.emplace(nodeId, *masked);
            continue;
        }
        // #347 Audio Waveform and Audio Spectrum: a canvas and the plot of the analysis over it.
        if (node->typeId == document::kAudioWaveformNodeType ||
            node->typeId == document::kAudioSpectrumNodeType) {
            if (!lowerAudioPlot(*node, operations, indices))
                return {};
            continue;
        }
        // #349 Histogram: the tile counts, their sum and the plot. It publishes a graph, not a
        // changed input, so it takes no effect mask.
        if (node->typeId == document::kHistogramNodeType) {
            const auto lowered = lowerHistogram(*node, operations, indices);
            if (!lowered)
                return {};
            indices.emplace(nodeId, *lowered);
            continue;
        }
        // #483 Z Defocus and Depth Fog: a depth map and its gather or fog mix, then the effect
        // mask.
        if (document::isDepthEffectNodeType(node->typeId)) {
            const auto lowered = lowerDepthEffect(*node, operations, indices);
            if (!lowered)
                return {};
            const auto masked = applyEffectMask(*node, *lowered, indices);
            if (!masked)
                return {};
            indices.emplace(nodeId, *masked);
            continue;
        }
        const auto* outputEdge = fixedInputEdge(nodeId, document::kCompositionOutputInputPort);
        // An unconnected composition output draws nothing, whether or not it is muted. Like a Layer
        // Output with no content ("added but not yet fed"), a disconnected output is an ordinary
        // edit state: the whole composition renders transparent rather than failing the compile on
        // a required input and leaving the previous frame on screen.
        if (definition->second->lowering == runtime::NodeLoweringKind::CompositionOutput &&
            (outputEdge == nullptr || emptyImages_.contains(outputEdge->source.nodeId))) {
            const auto empty = runtime::OperationIndex::fromRaw(operations.size());
            operations.emplace_back(runtime::CompiledMerge{nodeId, {}});
            indices.emplace(nodeId, runtime::OperationIndex::fromRaw(operations.size()));
            operations.emplace_back(runtime::CompiledCompositionOutput{nodeId, empty});
            continue;
        }
        // A combine's empty second image is still a meaningful transparent matte. Materialize
        // it through the existing empty Merge path rather than losing the connected input.
        if (node->typeId == "bloom.image-mix" || node->typeId == "bloom.set-alpha" ||
            node->typeId == "bloom.multiply-alpha") {
            const auto* second =
                fixedInputEdge(nodeId, node->typeId == "bloom.image-mix" ? "second" : "matte");
            // Image Mix's connected Mask may be empty too (a muted Mask node, or a Matte Blur
            // of one, #337): the same transparent image, so the mask is zero coverage.
            const auto* mask =
                node->typeId == "bloom.image-mix" ? fixedInputEdge(nodeId, "mask") : nullptr;
            for (const auto* edge : {second, mask})
                if (edge && emptyImages_.contains(edge->source.nodeId) &&
                    !indices.contains(edge->source.nodeId)) {
                    const auto empty = runtime::OperationIndex::fromRaw(operations.size());
                    operations.emplace_back(runtime::CompiledMerge{edge->source.nodeId, {}});
                    indices.emplace(edge->source.nodeId, empty);
                }
            // #361: Set Alpha's and Multiply Alpha's matte is optional. An unconnected matte is the
            // same transparent matte as a connected empty one (zero coverage, full when inverted),
            // owned by the combine node itself because there is no source node to name.
            if (second == nullptr && node->typeId != "bloom.image-mix") {
                const auto empty = runtime::OperationIndex::fromRaw(operations.size());
                operations.emplace_back(runtime::CompiledMerge{nodeId, {}});
                unconnectedMattes_.insert_or_assign(nodeId, empty);
            }
        }
        // An empty transform parent still owns authored transform values. Give it an empty
        // image input without adding document nodes or Merge membership.
        std::optional<runtime::OperationIndex> emptyParentInput;
        if (emptyImages_.contains(nodeId) && transformParents_.contains(nodeId)) {
            emptyParentInput = runtime::OperationIndex::fromRaw(operations.size());
            operations.emplace_back(runtime::CompiledMerge{nodeId, {}});
        }
        // ADR 0028 layer masks: a mask effect on the layer's content, in content space, before
        // the Layer Output transforms it.
        std::optional<runtime::OperationIndex> maskedContent;
        if (const auto boundary = layerOutputs_.find(nodeId);
            boundary != layerOutputs_.end() && !boundary->second->masks.empty() &&
            !emptyParentInput && !emptyImages_.contains(nodeId)) {
            if (const auto content =
                    findInputOperation(nodeId, document::kLayerOutputContentInputPort, indices)) {
                maskedContent = lowerMaskedContent(nodeId, *boundary->second, *content, indices);
                if (!maskedContent)
                    return {};
            }
        }
        // #383 Mask Combine and Image to Mask.
        if (definition->second->lowering == runtime::NodeLoweringKind::Matte) {
            const auto matte = lowerMatteNode(*node, indices);
            if (!matte)
                return {};
            indices.emplace(nodeId, *matte);
            continue;
        }
        // ADR 0028 Mask node: an opaque white canvas, masked.
        if (definition->second->lowering == runtime::NodeLoweringKind::Mask) {
            auto kernel = lowerMaskNode(*node);
            if (!kernel)
                return {};
            const auto canvas = runtime::OperationIndex::fromRaw(operations.size());
            const auto& format = composition_->format();
            operations.emplace_back(runtime::CompiledSolid{
                nodeId,
                runtime::CompiledColorParameter{document::ParameterId{},
                                                core::Color4d{1.0, 1.0, 1.0, 1.0}},
                runtime::CompiledScalarParameter{document::ParameterId{},
                                                 static_cast<double>(format.width())},
                runtime::CompiledScalarParameter{document::ParameterId{},
                                                 static_cast<double>(format.height())}});
            const auto index = runtime::OperationIndex::fromRaw(operations.size());
            operations.emplace_back(
                runtime::CompiledImageEffect{nodeId, canvas, std::move(*kernel)});
            indices.emplace(nodeId, index);
            continue;
        }
        auto operation = emptyParentInput ? lowerLayerOutput(*node, indices, emptyParentInput)
                                          : lowerNode(*node, *definition->second, indices);
        if (!operation.has_value()) {
            return {};
        }
        if (maskedContent)
            if (auto* layer = std::get_if<runtime::CompiledLayerOutput>(&*operation))
                layer->input = *maskedContent;
        // Output always publishes a full composition image. A graph wired directly to a
        // source or Layer gets a derived Normal Merge, including empty/cropped Layer images.
        // Its zero slot ID is synthetic plan metadata, never a durable document slot.
        if (auto* output = std::get_if<runtime::CompiledCompositionOutput>(&*operation);
            output &&
            !std::holds_alternative<runtime::CompiledMerge>(operations[output->input.value()])) {
            const auto normalized = runtime::OperationIndex::fromRaw(operations.size());
            operations.emplace_back(runtime::CompiledMerge{nodeId, {{{}, {}, output->input}}});
            output->input = normalized;
        }
        auto index = runtime::OperationIndex::fromRaw(operations.size());
        operations.push_back(*operation);
        // #383: an effect with a connected mask applies only where the mask covers it.
        if (definition->second->lowering == runtime::NodeLoweringKind::ImageEffect &&
            document::hasEffectMask(*definition->second)) {
            const auto masked = applyEffectMask(*node, index, indices);
            if (!masked)
                return {};
            index = *masked;
        }
        indices.emplace(nodeId, index);
    }

    if (!resolveBoundsReadouts(indices)) {
        return {};
    }

    const auto& compositionOutput = composition_->graph().compositionOutput();
    if (!compositionOutput.has_value()) {
        addTopologyFailure({}, "Composition output disappeared before lowering.");
        return {};
    }
    const auto outputNodeId = compositionOutput->nodeId;
    const auto output = indices.find(outputNodeId);
    if (output == indices.end() && !request_.channelInspection) {
        addTopologyFailure(outputNodeId, "Composition output was not lowered.");
        return {};
    }
    const auto audioMix = lowerAudioMix();
    if (!audioMix.has_value()) {
        return {};
    }
    auto planOutput =
        output == indices.end() ? runtime::OperationIndex::fromRaw(0) : output->second;
    if (request_.channelInspection && request_.channelInspection->channels.empty()) {
        // #364 Viewer matte preview: an inspection with no channels shows a pixel output port
        // (a Cryptomatte's Mask) directly.
        // #490: an Image Preview shows any node's Image or Mask output the same way.
        const auto& inspection = *request_.channelInspection;
        std::optional<runtime::OperationIndex> shown;
        if (const auto port = portOperations_.find({inspection.node, inspection.port});
            port != portOperations_.end())
            shown = port->second;
        else if (const auto* inspected = findNode(inspection.node);
                 inspected != nullptr && inspectsPixelOutput(*inspected))
            shown = sourceOperation(document::OutputPortRef{inspection.node, inspection.port},
                                    indices);
        if (!shown) {
            addTopologyFailure(inspection.node, "The inspected output has no pixels.");
            return {};
        }
        if (request_.statisticsTap) {
            // #349: the picture an Image Statistics node reads, reduced to its statistics: the
            // plan shows the two-pixel result.
            shown = lowerStatisticsReduction(*request_.statisticsTap, *shown, operations);
            if (!shown)
                return {};
        }
        const auto mergeIndex = runtime::OperationIndex::fromRaw(operations.size());
        operations.emplace_back(runtime::CompiledMerge{outputNodeId, {{{}, {}, *shown}}});
        planOutput = runtime::OperationIndex::fromRaw(operations.size());
        operations.emplace_back(runtime::CompiledCompositionOutput{outputNodeId, mergeIndex});
    } else if (request_.channelInspection) {
        // ADR 0026 Viewer inspection: a Raw Shuffle of the chosen channels (one as grey, three as
        // RGB, alpha 1) becomes this viewer-only plan's output.
        const auto& inspection = *request_.channelInspection;
        if (inspection.channels.size() != 1 && inspection.channels.size() != 3) {
            addTopologyFailure(inspection.node, "Viewer inspection shows one or three channels.");
            return {};
        }
        runtime::CompiledChannelShuffle shuffle;
        shuffle.sourceNodeId = inspection.node;
        std::map<std::pair<document::NodeId, std::string>, std::uint32_t> memo;
        const auto root =
            lowerChannelStep(document::OutputPortRef{inspection.node, inspection.port},
                             shuffle.channels, memo, 0, &indices);
        if (!root)
            return {};
        if (*root + 1 != shuffle.channels.steps.size()) {
            auto copy = shuffle.channels.steps[*root];
            shuffle.channels.steps.push_back(std::move(copy));
        }
        const auto& names = inspection.channels;
        shuffle.mapping = {names[0], names.size() == 3 ? names[1] : names[0],
                           names.size() == 3 ? names[2] : names[0], "1"};
        shuffle.mode = document::kChannelShuffleRawMode;
        shuffle.associated = true;
        // #480: Image to Channel Set channels are inspected through the same image effects.
        const auto mapping = shuffle.mapping;
        const auto shuffleIndex =
            emitChannelShuffle(std::move(shuffle), mapping, inspection.node, operations);
        // The Composition Output reads a Merge, as for any directly connected image.
        const auto mergeIndex = runtime::OperationIndex::fromRaw(operations.size());
        operations.emplace_back(runtime::CompiledMerge{outputNodeId, {{{}, {}, shuffleIndex}}});
        planOutput = runtime::OperationIndex::fromRaw(operations.size());
        operations.emplace_back(runtime::CompiledCompositionOutput{outputNodeId, mergeIndex});
    }
    runtime::CompiledCompositionPlanDefinition definition{
        request_.snapshot.revision(),
        request_.snapshot.project().id(),
        request_.compositionId,
        composition_->format(),
        std::move(operations),
        planOutput,
        std::move(curveTables->scalar),
        std::move(curveTables->vec2),
        std::move(curveTables->vec3),
        std::move(curveTables->color4),
        std::move(valueOperations_),
        valueOutputCount_,
        runtime::kCompiledCompositionPlanSemanticsVersion,
        runtime::kAnimationSamplingSemanticsVersion,
        !request_.parameterOverrides.empty(),
        *audioMix,
        std::move(nestedPlans_),
        composition_->duration()};
    // ADR 0024: place every operation on its layer time clock.
    if (const auto failure = runtime::detail::scopeLayerTime(definition)) {
        addFailure(runtime::CompileDiagnosticCode::TimeScopeLimit, subject({}, "layerTime"),
                   failure->summary, "Reduce the number of retimed layers sharing upstream nodes.");
        return {};
    }
    // #480: Image to Channel Set layers written beside RGBA evaluate as plans of their own.
    runtime::detail::separateChannelLayerImages(definition);
    return std::make_shared<const runtime::CompiledCompositionPlan>(std::move(definition));
}

[[nodiscard]] bool reachableEdge(const document::EdgeRecord& edge) const {
    return std::ranges::any_of(reachableEdges_,
                               [&](const auto* candidate) { return candidate->id == edge.id; });
}

// The edge into `port` of node `nodeId`, if any.
// #349: the plan of the picture an Image Statistics node reads, followed by the reduction. A node
// with no picture wired has none (and every statistic is 0).
struct StatisticsLowering final {
    bool ok = true;
    std::shared_ptr<const runtime::CompiledCompositionPlan> plan;
};
[[nodiscard]] StatisticsLowering lowerStatisticsPlan(const document::NodeRecord& node) {
    if (const auto found = statisticsPlans_.find(node.id); found != statisticsPlans_.end())
        return {true, found->second};
    const auto* edge = audioInputEdge(node.id, document::kImagePixelInputPort);
    if (edge == nullptr || isMuted(node.id))
        return {};
    if (std::ranges::find(request_.statisticsStack, node.id) != request_.statisticsStack.end()) {
        addTopologyFailure(node.id, "An Image Statistics node cannot read a picture that depends "
                                    "on its own result.");
        return {false, nullptr};
    }
    auto nested = request_;
    nested.channelInspection = runtime::ChannelInspection{edge->source.nodeId, edge->source.port, {}};
    // #645: a statistic measures the picture as exported, never the Viewer's dimmed one.
    nested.isolation.clear();
    nested.statisticsTap = node.id;
    nested.statisticsStack.push_back(node.id);
    auto compiled = CompilePass(registry_, nested, cancellation_, nullptr).run();
    if (compiled.status != runtime::SnapshotCompileStatus::Compiled || compiled.plan == nullptr) {
        addTopologyFailure(node.id, "The picture an Image Statistics node reads could not be "
                                    "compiled.");
        return {false, nullptr};
    }
    statisticsPlans_.emplace(node.id, compiled.plan);
    return {true, std::move(compiled.plan)};
}

[[nodiscard]] const document::EdgeRecord* audioInputEdge(const document::NodeId nodeId,
                                                         const std::string_view port) const {
    const auto& edges = composition_->graph().edges();
    const auto edge = std::ranges::find_if(edges, [&](const auto& candidate) {
        const auto* input = std::get_if<document::NodeInputRef>(&candidate.destination);
        return input != nullptr && input->nodeId == nodeId && input->port == port;
    });
    return edge == edges.end() ? nullptr : &*edge;
}

// #569: the node an audio chain ends in, walking up from `start` through the audio effects (a muted
// effect is bypassed). Nullopt when an unmuted effect's audio input is unwired, so the chain is
// silent.
[[nodiscard]] std::optional<document::NodeId> audioChainEnd(document::NodeId start) const {
    for (std::size_t step = 0; step <= composition_->graph().nodes().size(); ++step) {
        const auto* node = findNode(start);
        if (node == nullptr || !document::isAudioEffectNodeType(node->typeId))
            return start;
        const auto* upstream = audioInputEdge(node->id, document::kAudioEffectInputPort);
        if (upstream == nullptr)
            return std::nullopt;
        start = upstream->source.nodeId;
    }
    return std::nullopt;
}

[[nodiscard]] static std::optional<runtime::AudioEffectKind>
audioEffectKindOf(const std::string_view typeId) noexcept {
    using Kind = runtime::AudioEffectKind;
    if (typeId == document::kAudioEqualizerNodeType)
        return Kind::Equalizer;
    if (typeId == document::kAudioCompressorNodeType)
        return Kind::Compressor;
    if (typeId == document::kAudioAutoLevelNodeType)
        return Kind::AutoLevel;
    if (typeId == document::kAudioMonoNodeType)
        return Kind::Mono;
    if (typeId == document::kAudioHissNodeType)
        return Kind::Hiss;
    return std::nullopt;
}

[[nodiscard]] std::optional<runtime::CompiledAudioEffect>
compileAudioEffect(const document::NodeRecord& node) {
    const auto kind = audioEffectKindOf(node.typeId);
    runtime::CompiledAudioEffect effect{node.id, kind.value_or(runtime::AudioEffectKind::Mono), {}};
    bool ok = kind.has_value();
    for (const auto& spec : document::audioEffectParameters(node.typeId)) {
        const auto* binding = findParameterBinding(node, std::string(spec.role));
        if (spec.kind == document::AudioEffectParameterKind::Scalar) {
            const auto parameter = compiledScalarParameter(binding);
            if (!parameter) {
                ok = false;
                break;
            }
            effect.parameters.push_back(*parameter);
        } else {
            const auto* constant = parameterConstant<std::int64_t>(binding);
            if (constant == nullptr) {
                ok = false;
                break;
            }
            effect.parameters.push_back({binding->parameterId, static_cast<double>(*constant)});
        }
    }
    if (!ok) {
        addTopologyFailure(node.id, "Audio effect parameters could not be lowered.");
        return std::nullopt;
    }
    return effect;
}

// #569: resolves the audio chain feeding a layer or the output from `start`: the source node it
// ends in and the effects between, nearest the source first. `node` is null for a chain that ends
// unwired (silent); `ok` is false after a lowering failure.
struct AudioChain final {
    const document::NodeRecord* node = nullptr;
    std::vector<runtime::CompiledAudioEffect> effects;
    bool ok = true;
};

[[nodiscard]] AudioChain resolveAudioChain(document::NodeId start) {
    AudioChain chain;
    for (std::size_t step = 0; step <= composition_->graph().nodes().size(); ++step) {
        const auto* node = findNode(start);
        if (node == nullptr || !document::isAudioEffectNodeType(node->typeId)) {
            chain.node = node;
            std::ranges::reverse(chain.effects);
            return chain;
        }
        if (!isMuted(node->id)) {
            auto effect = compileAudioEffect(*node);
            if (!effect) {
                chain.ok = false;
                return chain;
            }
            chain.effects.push_back(std::move(*effect));
        }
        const auto* upstream = audioInputEdge(node->id, document::kAudioEffectInputPort);
        if (upstream == nullptr)
            return chain;
        start = upstream->source.nodeId;
    }
    addTopologyFailure(start, "Audio effects form a cycle.");
    chain.ok = false;
    return chain;
}

// #347: the audio an analysis node (Audio Amplitude, Waveform, Spectrum) reads: the Audio or Video
// source its `audio` input ends in, through any audio effects (an analysis reads the source's own
// sound, so the effects are not applied). `source` is null for an input that is unwired, muted or
// ends in an unwired effect: silence. `ok` is false after a lowering failure.
struct AudioAnalysisLowering final {
    bool ok = true;
    std::shared_ptr<const runtime::AudioAnalysisSource> source;
};

[[nodiscard]] AudioAnalysisLowering lowerAudioAnalysisSource(const document::NodeRecord& node) {
    const auto* edge = audioInputEdge(node.id, document::kAudioAnalysisInputPort);
    if (edge == nullptr)
        return {};
    const auto end = audioChainEnd(edge->source.nodeId);
    if (!end)
        return {};
    const auto* audioSource = findNode(*end);
    if (audioSource == nullptr)
        return {};
    if (audioSource->typeId != document::kAudioSourceNodeType &&
        audioSource->typeId != "bloom.video-source") {
        addTopologyFailure(node.id, "Audio analysis needs an Audio or Video source.");
        return {false, nullptr};
    }
    if (isMuted(audioSource->id))
        return {};
    const auto* assetText = parameterConstant<std::string>(findParameterBinding(*audioSource, "asset"));
    const auto* startFrame =
        parameterConstant<std::int64_t>(findParameterBinding(*audioSource, "startFrame"));
    std::uint64_t assetRaw = 0;
    if (assetText == nullptr || startFrame == nullptr || assetText->empty() ||
        std::from_chars(assetText->data(), assetText->data() + assetText->size(), assetRaw).ec !=
            std::errc{} ||
        !document::AssetId::fromRaw(assetRaw).isValid()) {
        addTopologyFailure(node.id, "The audio source of an analysis could not be lowered.");
        return {false, nullptr};
    }
    const auto* asset = request_.snapshot.project().findAsset(document::AssetId::fromRaw(assetRaw));
    if (asset == nullptr) {
        addTopologyFailure(node.id, "The audio source of an analysis names a missing asset.");
        return {false, nullptr};
    }
    auto source = std::make_shared<runtime::AudioAnalysisSource>();
    source->asset = *asset;
    source->startFrame = *startFrame;
    source->compositionRate = composition_->format().frameRate();
    if (audioSource->typeId == "bloom.video-source") {
        if (const auto* selected = parameterConstant<std::int64_t>(findParameterBinding(
                *audioSource, std::string(document::kVideoAudioStreamParameterRole)));
            selected != nullptr && *selected >= 0 && *selected < 256)
            source->audioStream = static_cast<std::uint32_t>(*selected);
        const auto* loop =
            parameterConstant<std::int64_t>(findParameterBinding(*audioSource, "loopMode"));
        if (loop != nullptr && (*loop == 1 || *loop == 2)) {
            source->loopExtent = runtime::resolveVideoStreamExtent(*asset);
            if (source->loopExtent)
                source->loopMode = *loop;
        }
        source->footage = runtime::footageAudioTiming(*asset);
    }
    return {true, std::move(source)};
}

// #436: the audio mix lowers only what is audible. A Layer is audible exactly when its audio
// reaches Composition Output through the edges the picture consumes -- so a disabled or muted
// Merge, and a disabled, muted or solo-excluded layer, are silent, as they are absent from the
// picture -- and its source is not muted. (A track matte source's slot is the one exception: it
// hides the layer's pixels, not its audio.) Every audible layer and its source are therefore
// reachable, so the curves and value graph the mix samples are compiled; compileReachableCurves()
// adds the Time Remap of an audible layer that has no picture.
void collectAudibleLayers() {
    audibleLayers_.clear();
    const auto& graph = composition_->graph();
    const auto& endpoint = graph.compositionOutput();
    if (!endpoint.has_value())
        return;
    const auto* outputEdge =
        audioInputEdge(endpoint->nodeId, document::kCompositionOutputAudioInputPort);
    if (outputEdge == nullptr || !reachableEdge(*outputEdge))
        return;
    const auto consider = [&](const document::NodeId layerNodeId) {
        const auto boundary = layerOutputs_.find(layerNodeId);
        if (boundary == layerOutputs_.end() || !boundary->second->enabled)
            return;
        // A muted Layer Output consumes only its image input, so its audio edge is unreached.
        const auto* audio = audioInputEdge(layerNodeId, document::kLayerOutputAudioInputPort);
        // #569: the source is what the chain of audio effects ends in; a muted effect is bypassed.
        if (audio != nullptr && reachableEdge(*audio)) {
            const auto end = audioChainEnd(audio->source.nodeId);
            if (end && !isMuted(*end))
                audibleLayers_.insert(layerNodeId);
        }
    };
    const auto feedNodeId = outputEdge->source.nodeId;
    const auto* stack = graph.merge(feedNodeId);
    const auto* stackNode = findNode(feedNodeId);
    if (stack == nullptr || stackNode == nullptr) {
        consider(feedNodeId);
        return;
    }
    for (const auto& entry : stack->entries()) {
        const auto edge = std::ranges::find_if(graph.edges(), [&](const auto& candidate) {
            const auto* input = std::get_if<document::LayerStackInputRef>(&candidate.destination);
            return input != nullptr && input->stackNodeId == stack->nodeId() &&
                   input->slotId == entry.slotId &&
                   input->role == document::kLayerStackAudioInputRole;
        });
        if (edge != graph.edges().end() && consumesEdge(*stackNode, *edge, false))
            consider(edge->source.nodeId);
    }
}

[[nodiscard]] std::optional<runtime::CompositionAudioMix> lowerAudioMix() {
    runtime::CompositionAudioMix mix;
    const auto& graph = composition_->graph();
    const auto& endpoint = graph.compositionOutput();
    if (!endpoint.has_value())
        return std::nullopt;
    mix.outputNodeId = endpoint->nodeId;
    const auto outputEdge = std::ranges::find_if(graph.edges(), [&](const auto& edge) {
        const auto* input = std::get_if<document::NodeInputRef>(&edge.destination);
        return input != nullptr && input->nodeId == endpoint->nodeId &&
               input->port == document::kCompositionOutputAudioInputPort;
    });
    if (outputEdge == graph.edges().end())
        return mix;
    // #436: a muted Composition Output consumes no audio.
    if (!reachableEdge(*outputEdge))
        return mix;
    // Lowers one audio source node into `mix.sources`, returning its index.
    const auto appendSource =
        [&](const document::NodeRecord& audioSource) -> std::optional<std::size_t> {
        const auto* assetBinding = findParameterBinding(audioSource, "asset");
        const auto* startBinding = findParameterBinding(audioSource, "startFrame");
        const auto* assetText = parameterConstant<std::string>(assetBinding);
        const auto* startFrame = parameterConstant<std::int64_t>(startBinding);
        const auto level =
            audioSource.typeId == "bloom.video-source" && assetBinding
                ? std::optional(runtime::CompiledScalarParameter{assetBinding->parameterId, 1.0})
                : compiledScalarParameter(findParameterBinding(audioSource, "level"));
        std::uint64_t assetRaw = 0;
        if (assetText == nullptr || startFrame == nullptr || !level || assetText->empty() ||
            std::from_chars(assetText->data(), assetText->data() + assetText->size(), assetRaw)
                    .ec != std::errc{} ||
            !document::AssetId::fromRaw(assetRaw).isValid() ||
            composition_->parameters().find(assetBinding->parameterId) == nullptr ||
            request_.snapshot.project().findAsset(document::AssetId::fromRaw(assetRaw)) ==
                nullptr) {
            addTopologyFailure(audioSource.id, "Audio source parameters could not be lowered.");
            return std::nullopt;
        }
        runtime::CompiledAudioSource compiled{audioSource.id, document::AssetId::fromRaw(assetRaw),
                                              *startFrame, *level};
        if (audioSource.typeId == "bloom.video-source") {
            // Document 1.25: the optional stream selection; absent plays the first audio stream.
            if (const auto* selected = parameterConstant<std::int64_t>(findParameterBinding(
                    audioSource, std::string(document::kVideoAudioStreamParameterRole)));
                selected != nullptr && *selected >= 0 && *selected < 256)
                compiled.audioStream = static_cast<std::uint32_t>(*selected);
            // #354: embedded audio follows its picture's loop mode over the picture extent.
            const auto* loop =
                parameterConstant<std::int64_t>(findParameterBinding(audioSource, "loopMode"));
            const auto& asset = *request_.snapshot.project().findAsset(compiled.assetId);
            if (loop != nullptr && (*loop == 1 || *loop == 2)) {
                compiled.loopExtent = runtime::resolveVideoStreamExtent(asset);
                if (compiled.loopExtent)
                    compiled.loopMode = *loop;
            }
            // #381: the audio follows a conformed rate and the Loop count.
            compiled.footage = runtime::footageAudioTiming(asset);
        }
        const auto sourceIndex = mix.sources.size();
        mix.sources.push_back(std::move(compiled));
        return sourceIndex;
    };
    // Lowers one Layer node as an audio layer: its boundary supplies enable, solo, and range; the
    // source on its audio input supplies the clip. A Layer whose audio input is unwired contributes
    // nothing rather than failing, so an image-only layer routed into the output stays valid.
    const auto appendLayer = [&](const document::NodeId layerNodeId) -> bool {
        const auto* layerNode = findNode(layerNodeId);
        const auto boundary =
            std::ranges::find_if(graph.layerOutputs(), [&](const auto& candidate) {
                return !candidate.entryOwned() && candidate.nodeId == layerNodeId;
            });
        if (layerNode == nullptr || layerNode->typeId != document::kLayerOutputNodeType ||
            boundary == graph.layerOutputs().end()) {
            addTopologyFailure(layerNodeId, "Validated audio layer topology could not be lowered.");
            return false;
        }
        // #431, #436: a layer that is not audible (collectAudibleLayers()) contributes nothing to
        // the mix. A disabled, muted or solo-excluded layer may be outside the reachable set, so
        // its Time Remap and level curves are not compiled and its timing must not be lowered.
        if (!audibleLayers_.contains(layerNodeId))
            return true;
        const auto layerAudioEdgeIterator =
            std::ranges::find_if(graph.edges(), [&](const auto& edge) {
                const auto* input = std::get_if<document::NodeInputRef>(&edge.destination);
                return input != nullptr && input->nodeId == layerNodeId &&
                       input->port == document::kLayerOutputAudioInputPort;
            });
        if (layerAudioEdgeIterator == graph.edges().end())
            return true;
        auto chain = resolveAudioChain(layerAudioEdgeIterator->source.nodeId);
        if (!chain.ok)
            return false;
        // An effect whose audio input is unwired plays nothing, like a Layer with no audio input.
        if (chain.node == nullptr)
            return true;
        const auto* audioSource = chain.node;
        if (audioSource && audioSource->typeId == document::kCompositionSourceNodeType) {
            if (!chain.effects.empty()) {
                addTopologyFailure(audioSource->id,
                                   "Audio effects need an Audio or Video source; a nested "
                                   "composition's sound has no effects of its own.");
                return false;
            }
            const auto source = lowerCompositionSource(*audioSource);
            if (!source)
                return false;
            const auto time = lowerLayerTime(*layerNode, *boundary);
            if (!time)
                return false;
            mix.nestedLayers.push_back({*source, true, boundary->solo, boundary->inPoint,
                                        boundary->endPoint(composition_->duration()), *time});
            return true;
        }
        if (audioSource == nullptr || (audioSource->typeId != document::kAudioSourceNodeType &&
                                       audioSource->typeId != "bloom.video-source")) {
            addTopologyFailure(layerAudioEdgeIterator->source.nodeId,
                               "Audio layer does not have an audio source.");
            return false;
        }
        const auto sourceIndex = appendSource(*audioSource);
        if (!sourceIndex)
            return false;
        const auto time = lowerLayerTime(*layerNode, *boundary);
        if (!time)
            return false;
        mix.layers.push_back({layerNodeId, boundary->layerId, *sourceIndex, true, boundary->solo,
                              boundary->inPoint, boundary->endPoint(composition_->duration()),
                              *time, std::move(chain.effects)});
        return true;
    };

    const auto feedNodeId = outputEdge->source.nodeId;
    if (const auto* stack = graph.merge(feedNodeId); stack != nullptr) {
        // The usual shape: the Merge sums the audio of every slot that carries an audio edge.
        for (const auto& entry : stack->entries()) {
            if (cancelled())
                return std::nullopt;
            const auto stackEdgeIterator =
                std::ranges::find_if(graph.edges(), [&](const auto& edge) {
                    const auto* input =
                        std::get_if<document::LayerStackInputRef>(&edge.destination);
                    return input != nullptr && input->stackNodeId == stack->nodeId() &&
                           input->slotId == entry.slotId &&
                           input->role == document::kLayerStackAudioInputRole;
                });
            if (stackEdgeIterator == graph.edges().end())
                continue;
            if (!appendLayer(stackEdgeIterator->source.nodeId))
                return std::nullopt;
        }
        return mix;
    }
    auto feedChain = resolveAudioChain(feedNodeId);
    if (!feedChain.ok)
        return std::nullopt;
    // An effect chain that ends unwired is silent; a missing node is a topology failure below.
    if (feedChain.node == nullptr && findNode(feedNodeId) != nullptr)
        return mix;
    const auto* feed = feedChain.node;
    if (!feedChain.effects.empty() && feed->typeId != document::kAudioSourceNodeType &&
        feed->typeId != "bloom.video-source") {
        addTopologyFailure(feed->id, "Audio effects need an Audio or Video source.");
        return std::nullopt;
    }
    if (feed != nullptr && feed->typeId == document::kLayerOutputNodeType) {
        // A Layer wired straight into the output is a one-layer mix.
        if (!appendLayer(feedNodeId))
            return std::nullopt;
        return mix;
    }
    // A muted source wired straight into the output is silent, as it is behind a Layer.
    if (feed != nullptr && isMuted(feed->id) &&
        (feed->typeId == document::kCompositionSourceNodeType ||
         feed->typeId == document::kAudioSourceNodeType || feed->typeId == "bloom.video-source"))
        return mix;
    if (feed && feed->typeId == document::kCompositionSourceNodeType) {
        const auto source = lowerCompositionSource(*feed);
        if (!source)
            return std::nullopt;
        mix.nestedLayers.push_back({*source, true, false, {}, composition_->duration()});
        return mix;
    }
    if (feed != nullptr &&
        (feed->typeId == document::kAudioSourceNodeType || feed->typeId == "bloom.video-source")) {
        // An audio source wired straight into the output plays whole, from the composition start.
        const auto sourceIndex = appendSource(*feed);
        if (!sourceIndex)
            return std::nullopt;
        mix.layers.push_back({feed->id, document::LayerId{}, *sourceIndex, true, false,
                              core::RationalTime{}, composition_->duration(), std::nullopt,
                              std::move(feedChain.effects)});
        return mix;
    }
    addTopologyFailure(feedNodeId,
                       "Composition audio is not fed by a Merge, a Layer, or an Audio source.");
    return std::nullopt;
}

[[nodiscard]] std::optional<runtime::CompiledOperation>
lowerNode(const document::NodeRecord& node, const runtime::NodeDefinition& definition,
          const std::unordered_map<document::NodeId, runtime::OperationIndex>& indices) {
    using runtime::NodeLoweringKind;
    switch (definition.lowering) {
    case NodeLoweringKind::Solid:
        return lowerSolid(node);
    case NodeLoweringKind::Checker:
        return lowerChecker(node);
    case NodeLoweringKind::Gradient:
        return lowerGradient(node);
    case NodeLoweringKind::CompositionSource:
        return lowerCompositionSource(node);
    case NodeLoweringKind::VideoSource:
        return lowerVideoSource(node);
    case NodeLoweringKind::ImageEffect:
        return lowerImageEffect(node, indices);
    case NodeLoweringKind::ImageSource:
        return lowerImageSource(node);
    case NodeLoweringKind::AudioSource:
    case NodeLoweringKind::AudioEffect:
        break;
    case NodeLoweringKind::Text:
        return lowerText(node);
    case NodeLoweringKind::LayerOutput:
        return lowerLayerOutput(node, indices);
    case NodeLoweringKind::LayerStack:
        return lowerLayerStack(node, definition, indices);
    case NodeLoweringKind::CompositionOutput:
        return lowerCompositionOutput(node, indices);
    case NodeLoweringKind::Shape:
        return lowerShape(node);
    case NodeLoweringKind::Mask: // lowered in the main loop: a canvas Solid then a mask effect
    case NodeLoweringKind::Matte: // #383, lowered in the main loop (lowerMatteNode())
    case NodeLoweringKind::Adjustment: // lowered in the main loop: the stack below its entry
    case NodeLoweringKind::TestCard: // #587, lowered in the main loop (lowerTestCard())
    case NodeLoweringKind::FourColorGradient: // lowered in the main loop
    // ADR 0026: channel nodes are lowered in the main loop (a Shuffle's image output is a
    // CompiledChannelShuffle; channel algebra compiles into its expression).
    case NodeLoweringKind::Channel:
    case NodeLoweringKind::Cryptomatte:
    // #490: an Image Preview has no output, so no plan reaches it.
    case NodeLoweringKind::ImagePreview:
    // #525: Fields nodes are lowered in the main loop (lowerFieldNode()).
    case NodeLoweringKind::Field:
    case NodeLoweringKind::Unsupported:
    // A value lowering never reaches here: compileValueGraph() compiled it into the plan's value
    // operations, and lower()'s own loop skips it. Reaching this arm means the two passes disagree
    // about which graph a node belongs to, which is a topology failure rather than a silent skip.
    case NodeLoweringKind::ValueConstant:
    case NodeLoweringKind::ValueTime:
    case NodeLoweringKind::ValueScalarMath:
    case NodeLoweringKind::ValueVectorMath:
    case NodeLoweringKind::ValueVectorReduce:
    case NodeLoweringKind::ValueMapRange:
    case NodeLoweringKind::ValueClamp:
    case NodeLoweringKind::ValueMix:
    case NodeLoweringKind::ValueColorMix:
    case NodeLoweringKind::ValueCompare:
    case NodeLoweringKind::ValueSwitch:
    case NodeLoweringKind::ValueSeparate:
    case NodeLoweringKind::ValueCombine:
    case NodeLoweringKind::ValueRandom:
    case NodeLoweringKind::ValueReroute:
    case NodeLoweringKind::ValueBoundsReadout:
    case NodeLoweringKind::ValueUtility:
    case NodeLoweringKind::ValueFieldSample:
    case NodeLoweringKind::ValueAudioAmplitude:
    case NodeLoweringKind::ValueImageStatistics:
    // #342: Group Input and Group Output are value-pass nodes. A Node Group the expansion could
    // not replace is reported by resolveDefinitions() and never reaches publication.
    case NodeLoweringKind::GroupBoundary:
    case NodeLoweringKind::ValueFunction:
    case NodeLoweringKind::NodeGroup:
    // #642: a Vector Artwork node is refused by resolveDefinitions() until it is expanded.
    case NodeLoweringKind::VectorArtwork:
        break;
    }
    addTopologyFailure(node.id, "Unsupported lowering reached plan publication.");
    return std::nullopt;
}

[[nodiscard]] std::optional<runtime::CompiledOperation>
lowerSolid(const document::NodeRecord& node) {
    using namespace document;
    const auto* binding = findParameterBinding(node, kSolidColorParameterRole);
    // Task S5: a solid colour is a typed operand now, lowered through exactly the same
    // constant-or-curve-index helper a transform operand uses, so an animated solid colour and an
    // animated position are resolved by one rule rather than two.
    const auto color = compiledColorParameter(binding);
    if (binding == nullptr || !color.has_value()) {
        addTopologyFailure(node.id, "Validated solid color could not be lowered.");
        return std::nullopt;
    }
    auto width = compiledScalarParameter(findParameterBinding(node, kSolidWidthParameterRole));
    auto height = compiledScalarParameter(findParameterBinding(node, kSolidHeightParameterRole));
    if (!width || !height) {
        addTopologyFailure(node.id, "Solid dimensions could not be lowered.");
        return std::nullopt;
    }
    return runtime::CompiledSolid{node.id, *color, *width, *height};
}

[[nodiscard]] std::optional<runtime::CompiledOperation>
lowerChecker(const document::NodeRecord& node) {
    const auto a = compiledColorParameter(findParameterBinding(node, "colorA"));
    const auto b = compiledColorParameter(findParameterBinding(node, "colorB"));
    const auto* cellWidth =
        parameterConstant<std::int64_t>(findParameterBinding(node, "cellWidth"));
    const auto* cellHeight =
        parameterConstant<std::int64_t>(findParameterBinding(node, "cellHeight"));
    const auto* width = parameterConstant<std::int64_t>(findParameterBinding(node, "width"));
    const auto* height = parameterConstant<std::int64_t>(findParameterBinding(node, "height"));
    if (!a || !b || !cellWidth || !cellHeight || !width || !height || *cellWidth < 1 ||
        *cellHeight < 1 || *width < 1 || *height < 1 || *cellWidth > 16777216 ||
        *cellHeight > 16777216 || *width > 16777216 || *height > 16777216) {
        addTopologyFailure(node.id, "Checker parameters could not be lowered.");
        return std::nullopt;
    }
    return runtime::CompiledChecker{node.id,
                                    *a,
                                    *b,
                                    static_cast<std::uint32_t>(*cellWidth),
                                    static_cast<std::uint32_t>(*cellHeight),
                                    static_cast<std::uint32_t>(*width),
                                    static_cast<std::uint32_t>(*height)};
}

[[nodiscard]] std::optional<runtime::CompiledOperation>
lowerGradient(const document::NodeRecord& node) {
    using namespace document;
    const auto* gradientBinding = findParameterBinding(node, "gradient");
    const auto stops =
        gradientBinding ? compiledGradientParameter(node, *gradientBinding) : std::nullopt;
    const auto start = compiledVec2Parameter(findParameterBinding(node, "start"));
    const auto end = compiledVec2Parameter(findParameterBinding(node, "end"));
    const auto width = compiledScalarParameter(findParameterBinding(node, "width"));
    const auto height = compiledScalarParameter(findParameterBinding(node, "height"));
    const auto* repeatBinding = findParameterBinding(node, "repeat");
    const auto* repeat = parameterConstant<std::int64_t>(repeatBinding);
    if (!stops || !start || !end || !width || !height || !repeatBinding ||
        !repeat || *repeat < 0 || *repeat > 2) {
        addTopologyFailure(node.id, "Gradient parameters could not be lowered.");
        return std::nullopt;
    }
    // #541: the colours are the gradient's stops; the Solid's own colour is unused.
    runtime::CompiledSolid source{
        node.id, runtime::CompiledColorParameter{{}, core::Color4d{0.0, 0.0, 0.0, 0.0}}, *width,
        *height};
    source.gradient = runtime::CompiledGradient{node.typeId == kRadialGradientNodeType,
                                                *stops,
                                                *start,
                                                *end,
                                                repeatBinding->parameterId,
                                                *repeat};
    return source;
}

[[nodiscard]] std::optional<runtime::CompiledCompositionSource>
lowerCompositionSource(const document::NodeRecord& node) {
    if (const auto found = compositionSources_.find(node.id); found != compositionSources_.end())
        return found->second;
    const auto* id = parameterConstant<std::int64_t>(findParameterBinding(node, "composition"));
    const auto* loop = parameterConstant<std::int64_t>(findParameterBinding(node, "loopMode"));
    const auto offset = compiledScalarParameter(findParameterBinding(node, "timeOffset"));
    const auto scale = compiledScalarParameter(findParameterBinding(node, "timeScale"));
    if (!id || !loop || !offset || !scale || *id < 0 || *loop < 0 || *loop > 2) {
        addTopologyFailure(node.id, "Composition source parameters could not be lowered.");
        return std::nullopt;
    }
    const auto nestedId = document::CompositionId::fromRaw(static_cast<std::uint64_t>(*id));
    if (!request_.snapshot.project().findComposition(nestedId)) {
        addFailure(runtime::CompileDiagnosticCode::CompositionNotFound, subject(node.id),
                   "Source composition is missing",
                   "Choose an existing composition for this source.");
        return std::nullopt;
    }
    // A channel layer plan (#480) shares its composition id but is not the composition's plan.
    std::size_t index = 0;
    while (index < nestedPlans_.size() && (channelLayerPlans_.contains(index) ||
                                           nestedPlans_[index]->compositionId() != nestedId))
        ++index;
    if (index == nestedPlans_.size()) {
        runtime::SnapshotCompileRequest nestedRequest{request_.snapshot, nestedId};
        nestedRequest.fontResolver = request_.fontResolver;
        nestedRequest.fontFetch = request_.fontFetch;
        auto nested =
            planCache_ ? planCache_->compile(runtime::SnapshotCompiler(registry_), nestedRequest,
                                             cancellation_)
                       : runtime::SnapshotCompiler(registry_).compile(nestedRequest, cancellation_);
        for (auto& diagnostic : nested.diagnostics)
            diagnostics_.emplace(diagnosticKey(diagnostic), std::move(diagnostic));
        if (!nested.plan) {
            hasFailure_ = true;
            return std::nullopt;
        }
        nestedPlans_.push_back(std::move(nested.plan));
    }
    runtime::CompiledCompositionSource source{node.id, index, {*offset, *scale, *loop}};
    compositionSources_.emplace(node.id, source);
    return source;
}

// #479 Combine Channels. The first connected input (red, then green, blue, alpha) is the base: a
// Channel Select of it fills its own slots (every input reading that same image folds in here),
// zero for an unconnected red, green or blue and one for an unconnected alpha. Each further
// connected input then sets its slot with a Channel Combine, alpha last; a connected input with no
// image reads zero. A Straight alpha mode premultiplies in the last operation only.
[[nodiscard]] std::optional<runtime::OperationIndex>
lowerCombineChannels(const document::NodeRecord& node,
                     std::vector<runtime::CompiledOperation>& operations,
                     std::unordered_map<document::NodeId, runtime::OperationIndex>& indices) {
    std::array<runtime::ImageChannelSource, document::kImageChannelCount> sources{};
    for (std::size_t slot = 0; slot < document::kImageChannelCount; ++slot) {
        const auto* value = parameterConstant<std::int64_t>(
            findParameterBinding(node, document::kCombineChannelsSourceRoles[slot]));
        if (value == nullptr || *value < 0 || *value >= document::kImageChannelSourceCount) {
            addTopologyFailure(node.id, "Combine Channels source channels could not be lowered.");
            return std::nullopt;
        }
        sources[slot] = static_cast<runtime::ImageChannelSource>(*value);
    }
    const auto* mode = parameterConstant<std::int64_t>(
        findParameterBinding(node, document::kCombineChannelsAlphaModeRole));
    if (mode == nullptr || (*mode != document::kCombineChannelsPremultiplied &&
                            *mode != document::kCombineChannelsStraight)) {
        addTopologyFailure(node.id, "Combine Channels alpha mode could not be lowered.");
        return std::nullopt;
    }
    const bool premultiply = *mode == document::kCombineChannelsStraight;
    std::array<std::optional<runtime::OperationIndex>, document::kImageChannelCount> inputs{};
    for (std::size_t slot = 0; slot < document::kImageChannelCount; ++slot) {
        const auto* edge = fixedInputEdge(node.id, document::kImageChannelPortNames[slot]);
        if (edge == nullptr)
            continue;
        // A connected input with no image is a transparent image, as for Image Mix's second.
        if (emptyImages_.contains(edge->source.nodeId) && !indices.contains(edge->source.nodeId)) {
            const auto empty = runtime::OperationIndex::fromRaw(operations.size());
            operations.emplace_back(runtime::CompiledMerge{edge->source.nodeId, {}});
            indices.emplace(edge->source.nodeId, empty);
        }
        inputs[slot] = sourceOperation(edge->source, indices);
        if (!inputs[slot]) {
            addTopologyFailure(node.id, "Combine Channels input could not be lowered.");
            return std::nullopt;
        }
    }
    const auto base =
        std::ranges::find_if(inputs, [](const auto& input) { return input.has_value(); });
    if (base == inputs.end()) {
        addTopologyFailure(node.id, "Combine Channels has no connected input.");
        return std::nullopt;
    }
    const auto baseInput = **base;
    runtime::ChannelSelectKernel select;
    std::vector<std::size_t> pending;
    for (std::size_t slot = 0; slot < document::kImageChannelCount; ++slot) {
        const bool alpha = slot + 1 == document::kImageChannelCount;
        if (inputs[slot] && *inputs[slot] == baseInput) {
            select.sources[slot] = sources[slot];
            continue;
        }
        // A slot a later operation sets keeps a nonzero alpha meanwhile, so no colour is lost.
        select.sources[slot] =
            alpha ? runtime::ImageChannelSource::One : runtime::ImageChannelSource::Zero;
        if (inputs[slot])
            pending.push_back(slot);
    }
    select.premultiply = premultiply && pending.empty();
    auto current = runtime::OperationIndex::fromRaw(operations.size());
    operations.emplace_back(runtime::CompiledImageEffect{node.id, baseInput, select});
    for (const auto slot : pending) {
        runtime::ChannelCombineKernel kernel{static_cast<std::uint8_t>(slot), sources[slot],
                                             premultiply && slot == pending.back()};
        const auto next = runtime::OperationIndex::fromRaw(operations.size());
        operations.emplace_back(
            runtime::CompiledImageEffect{node.id, current, kernel, false, false, *inputs[slot]});
        current = next;
    }
    return current;
}

[[nodiscard]] std::optional<runtime::CompiledOperation>
lowerImageEffect(const document::NodeRecord& node,
                 const std::unordered_map<document::NodeId, runtime::OperationIndex>& indices) {
    const auto input = findInputOperation(node.id, "input", indices);
    if (!input) {
        addTopologyFailure(node.id, "Image effect input could not be lowered.");
        return std::nullopt;
    }
    if (node.typeId == "bloom.image-mix" || node.typeId == "bloom.set-alpha" ||
        node.typeId == "bloom.multiply-alpha") {
        const bool mix = node.typeId == "bloom.image-mix";
        const bool multiplyAlpha = node.typeId == "bloom.multiply-alpha";
        auto second = findInputOperation(node.id, mix ? "second" : "matte", indices);
        if (!second && !mix) {
            if (const auto unconnected = unconnectedMattes_.find(node.id);
                unconnected != unconnectedMattes_.end())
                second = unconnected->second;
        }
        if (!second) {
            addTopologyFailure(node.id, "Image combine second input could not be lowered.");
            return std::nullopt;
        }
        if (multiplyAlpha) {
            const auto factor = compiledScalarParameter(findParameterBinding(node, "factor"));
            const auto* channelBinding = findParameterBinding(node, "channel");
            const auto* channel = parameterConstant<std::int64_t>(channelBinding);
            const auto channelDriver =
                drivenOutput(channelBinding, runtime::SocketValueKind::Integer);
            const auto selection =
                channel ? render::matteSelectionFromStoredValue(*channel) : std::nullopt;
            if (factor && (selection || channelDriver))
                return runtime::CompiledImageEffect{
                    node.id,
                    *input,
                    runtime::ImageCombineKernel{runtime::ImageCombineMode::MultiplyAlpha, *factor,
                                                selection ? selection->channel
                                                          : render::ImageCoverageChannel::Alpha,
                                                channelDriver, selection && selection->invert},
                    false,
                    false,
                    *second};
        } else if (mix) {
            if (const auto factor = compiledScalarParameter(findParameterBinding(node, "factor"))) {
                runtime::CompiledImageEffect effect{
                    node.id,
                    *input,
                    runtime::ImageCombineKernel{runtime::ImageCombineMode::Mix, *factor,
                                                render::ImageCoverageChannel::Alpha},
                    false,
                    false,
                    *second};
                // ADR 0028: the optional Mask scales the factor per pixel.
                if (fixedInputEdge(node.id, "mask") != nullptr) {
                    effect.maskInput = findInputOperation(node.id, "mask", indices);
                    if (!effect.maskInput) {
                        addTopologyFailure(node.id, "Image Mix mask input could not be lowered.");
                        return std::nullopt;
                    }
                }
                return effect;
            }
        } else {
            const auto* channelBinding = findParameterBinding(node, "channel");
            const auto* channel = parameterConstant<std::int64_t>(channelBinding);
            const auto channelDriver =
                drivenOutput(channelBinding, runtime::SocketValueKind::Integer);
            const auto selection =
                channel ? render::matteSelectionFromStoredValue(*channel) : std::nullopt;
            if (selection || channelDriver)
                return runtime::CompiledImageEffect{
                    node.id,
                    *input,
                    runtime::ImageCombineKernel{runtime::ImageCombineMode::SetAlpha,
                                                {},
                                                selection ? selection->channel
                                                          : render::ImageCoverageChannel::Alpha,
                                                channelDriver,
                                                selection && selection->invert},
                    false,
                    false,
                    *second};
        }
        addTopologyFailure(node.id, "Image combine parameters could not be lowered.");
        return std::nullopt;
    }
    if (node.typeId == "bloom.flip") {
        const auto* horizontal = parameterConstant<bool>(findParameterBinding(node, "horizontal"));
        const auto* vertical = parameterConstant<bool>(findParameterBinding(node, "vertical"));
        if (horizontal && vertical)
            return runtime::CompiledImageEffect{
                node.id, *input, runtime::DiscreteImageKernel{*horizontal, *vertical, 0}};
    }
    if (node.typeId == "bloom.rotate-90") {
        const auto* turns =
            parameterConstant<std::int64_t>(findParameterBinding(node, "quarterTurns"));
        if (turns)
            return runtime::CompiledImageEffect{
                node.id, *input,
                runtime::DiscreteImageKernel{false, false,
                                             static_cast<std::uint8_t>((*turns % 4 + 4) % 4)}};
    }
    if (node.typeId == "bloom.gaussian-blur" || node.typeId == "bloom.box-blur") {
        const auto radius = compiledScalarParameter(findParameterBinding(node, "radius"));
        const auto* edge = parameterConstant<std::int64_t>(findParameterBinding(node, "edge"));
        if (radius && edge && *edge >= 0 && *edge <= 1) {
            runtime::SeparableBlurKernel kernel{
                node.typeId == "bloom.gaussian-blur" ? runtime::SeparableBlurKind::Gaussian
                                                     : runtime::SeparableBlurKind::Box,
                *radius, static_cast<runtime::BlurEdgeMode>(*edge)};
            // Separate axes are a later addition (Gaussian Blur only): a node saved without them
            // blurs both axes by the radius.
            const auto* separate = parameterConstant<bool>(findParameterBinding(node, "separate"));
            if (separate != nullptr && *separate) {
                if (const auto radiusY =
                        compiledScalarParameter(findParameterBinding(node, "radiusY"))) {
                    kernel.separate = true;
                    kernel.radiusY = *radiusY;
                } else {
                    addTopologyFailure(node.id, "Blur parameters could not be lowered.");
                    return std::nullopt;
                }
            }
            return runtime::CompiledImageEffect{node.id, *input, std::move(kernel)};
        }
    }
    // #337 Matte Blur: the Gaussian kernel over the Mask's coverage alone. #338 Matte Erode and
    // Matte Dilate: the same separable neighborhood reduced by minimum or maximum.
    if (node.typeId == "bloom.matte-blur" || node.typeId == "bloom.matte-erode" ||
        node.typeId == "bloom.matte-dilate") {
        const auto radius = compiledScalarParameter(findParameterBinding(node, "radius"));
        const auto* edge = parameterConstant<std::int64_t>(findParameterBinding(node, "edge"));
        const auto kind = node.typeId == "bloom.matte-erode"    ? runtime::SeparableBlurKind::Erode
                          : node.typeId == "bloom.matte-dilate" ? runtime::SeparableBlurKind::Dilate
                                                                : runtime::SeparableBlurKind::Gaussian;
        if (radius && edge && *edge >= 0 && *edge <= 1)
            return runtime::CompiledImageEffect{
                node.id, *input,
                runtime::SeparableBlurKernel{kind, *radius,
                                             static_cast<runtime::BlurEdgeMode>(*edge),
                                             runtime::BlurChannels::Coverage}};
        addTopologyFailure(node.id, "Matte filter parameters could not be lowered.");
        return std::nullopt;
    }
    // #338 Luma Key: an Image to a Mask.
    if (node.typeId == "bloom.luma-key") {
        const auto low = compiledScalarParameter(findParameterBinding(node, "low"));
        const auto high = compiledScalarParameter(findParameterBinding(node, "high"));
        const auto* invert = parameterConstant<bool>(findParameterBinding(node, "invert"));
        if (low && high && invert)
            return runtime::CompiledImageEffect{node.id, *input,
                                                runtime::LumaKeyKernel{*low, *high, *invert}};
        addTopologyFailure(node.id, "Luma Key parameters could not be lowered.");
        return std::nullopt;
    }
    // #340 Levels, Curves and Hue/Saturation: every operand in the kernel's channel-major order.
    if (node.typeId == document::kLevelsNodeType) {
        runtime::LevelsKernel kernel;
        const auto* clamp = parameterConstant<bool>(findParameterBinding(node, "clamp"));
        bool lowered = clamp != nullptr;
        for (std::size_t channel = 0; lowered && channel < document::kGradeChannelCount;
             ++channel) {
            auto& target = kernel.channels[channel];
            const std::array<runtime::CompiledScalarParameter*, document::kLevelsFieldCount>
                fields{&target.inputBlack, &target.inputWhite, &target.gamma, &target.outputBlack,
                       &target.outputWhite};
            for (std::size_t field = 0; lowered && field < fields.size(); ++field) {
                const auto parameter = compiledScalarParameter(
                    findParameterBinding(node, document::levelsRole(channel, field)));
                lowered = parameter.has_value();
                if (parameter)
                    *fields[field] = *parameter;
            }
        }
        if (lowered) {
            kernel.clamp = *clamp;
            return runtime::CompiledImageEffect{node.id, *input, std::move(kernel)};
        }
        addTopologyFailure(node.id, "Levels parameters could not be lowered.");
        return std::nullopt;
    }
    if (node.typeId == document::kCurvesNodeType) {
        runtime::CurvesKernel kernel;
        const auto* clamp = parameterConstant<bool>(findParameterBinding(node, "clamp"));
        bool lowered = clamp != nullptr;
        // #493: each curve is a constant point list or a whole-value Path curve.
        for (std::size_t index = 0; lowered && index < document::kGradeCurveCount; ++index) {
            const auto* binding = findParameterBinding(
                node, document::gradeCurveRole(document::kGradeCurves[index]));
            const auto parameter =
                binding ? compiledPathParameter(binding->parameterId) : std::nullopt;
            lowered = parameter.has_value();
            if (parameter)
                kernel.curves[index] = *parameter;
        }
        if (lowered) {
            kernel.clamp = *clamp;
            return runtime::CompiledImageEffect{node.id, *input, std::move(kernel)};
        }
        addTopologyFailure(node.id, "Curves parameters could not be lowered.");
        return std::nullopt;
    }
    // #503 Alpha Association: the constant mode and white switch, and the background colour in
    // Remove Color Matting only; the other modes lower a constant black without an identity, so an
    // unused (even animated) colour neither claims nor invalidates anything.
    if (node.typeId == document::kAlphaAssociationNodeType) {
        const auto* mode = parameterConstant<std::int64_t>(findParameterBinding(node, "mode"));
        const auto* white = parameterConstant<bool>(findParameterBinding(node, "white"));
        const auto background = compiledColorParameter(findParameterBinding(node, "background"));
        if (mode && white && background && *mode >= document::kAlphaAssociationUnmult &&
            *mode <= document::kAlphaAssociationRemoveMatting) {
            runtime::AlphaAssociationKernel kernel;
            kernel.mode = static_cast<runtime::AlphaAssociationMode>(*mode);
            kernel.white = *white;
            if (kernel.mode == runtime::AlphaAssociationMode::RemoveMatting)
                kernel.background = *background;
            return runtime::CompiledImageEffect{node.id, *input, kernel};
        }
        addTopologyFailure(node.id, "Alpha Association parameters could not be lowered.");
        return std::nullopt;
    }
    // #504 Colorize: the constant mode, source and cycle, and the operands the mode reads; every
    // other operand keeps the kernel's constant default without an identity, so an unused (even
    // animated) value neither claims nor invalidates anything.
    if (node.typeId == document::kColorizeNodeType) {
        const auto* mode = parameterConstant<std::int64_t>(findParameterBinding(node, "mode"));
        const auto* source = parameterConstant<std::int64_t>(findParameterBinding(node, "source"));
        const auto* cycle = parameterConstant<std::int64_t>(findParameterBinding(node, "cycle"));
        const auto inRange = [](const std::int64_t* value, const std::string_view schemaKey) {
            const auto domain = document::colorizeIntegerDomain(schemaKey);
            return value && *value >= domain->minimum && *value <= domain->maximum;
        };
        bool lowered = inRange(mode, document::kColorizeModeSchemaKey) &&
                       inRange(source, document::kColorizeSourceSchemaKey) &&
                       inRange(cycle, document::kColorizeCycleSchemaKey);
        runtime::ColorizeKernel kernel;
        if (lowered) {
            kernel.mode = static_cast<runtime::ColorizeMode>(*mode);
            using Mode = runtime::ColorizeMode;
            const bool ramp = kernel.mode == Mode::Tritone || kernel.mode == Mode::GradientMap;
            const bool map = kernel.mode == Mode::GradientMap;
            if (map) {
                kernel.source = static_cast<runtime::ColorizeSource>(*source);
                kernel.cycle = static_cast<runtime::ColorizeCycle>(*cycle);
            }
            const auto scalar = [&](const std::string_view role,
                                    runtime::CompiledScalarParameter& target) {
                const auto parameter = compiledScalarParameter(findParameterBinding(node, role));
                lowered = lowered && parameter.has_value();
                if (parameter)
                    target = *parameter;
            };
            const auto color = [&](const std::string_view role,
                                   runtime::CompiledColorParameter& target) {
                const auto parameter = compiledColorParameter(findParameterBinding(node, role));
                lowered = lowered && parameter.has_value();
                if (parameter)
                    target = *parameter;
            };
            // #541: a Gradient Map whose node binds a gradient maps through it; one made before
            // the gradient existed keeps its shadows, midtones, highlights ramp.
            const auto* gradient = map ? findParameterBinding(node, "gradient") : nullptr;
            scalar("amount", kernel.amount);
            if (kernel.mode == Mode::Fill)
                color("fillColor", kernel.color);
            if ((kernel.mode == Mode::Tint || ramp) && gradient == nullptr) {
                color("shadows", kernel.shadows);
                color("highlights", kernel.highlights);
            }
            if (gradient != nullptr) {
                kernel.gradient = compiledGradientParameter(node, *gradient);
                lowered = lowered && kernel.gradient.has_value();
            } else if (ramp) {
                color("midtones", kernel.midtones);
                scalar("midpoint", kernel.midpoint);
            }
            if (map) {
                scalar("repetitions", kernel.repetitions);
                scalar("phase", kernel.phase);
            }
            if (kernel.mode == Mode::BlackAndWhite) {
                const std::array<std::string_view, 6> roles{"reds",  "yellows", "greens",
                                                            "cyans", "blues",   "magentas"};
                for (std::size_t index = 0; index < roles.size(); ++index)
                    scalar(roles[index], kernel.weights[index]);
            }
        }
        if (lowered)
            return runtime::CompiledImageEffect{node.id, *input, std::move(kernel)};
        addTopologyFailure(node.id, "Colorize parameters could not be lowered.");
        return std::nullopt;
    }
    // #507 Lens Flare: the constant counts and choices, every value, and the occlusion matte as the
    // one image the kernel reads beside its input. A Mask is read as its coverage; only the
    // selector's invert half applies to one.
    if (node.typeId == document::kLensFlareNodeType) {
        runtime::LensFlareKernel kernel;
        bool lowered = true;
        const auto integer = [&](const std::string_view role, std::int64_t& target) {
            const auto* value = parameterConstant<std::int64_t>(findParameterBinding(node, role));
            lowered = lowered && value != nullptr;
            if (value != nullptr)
                target = *value;
        };
        integer("lightMode", kernel.lightMode);
        integer("streakCount", kernel.streakCount);
        integer("ghostCount", kernel.ghostCount);
        integer("seed", kernel.seed);
        integer("irisCount", kernel.irisCount);
        integer("irisBlades", kernel.irisBlades);
        integer("occlusionChannel", kernel.occlusionChannel);
        for (const auto& spec : document::lensFlareParameters()) {
            if (spec.kind != document::LensFlareParameterKind::Scalar)
                continue;
            const auto value = compiledScalarParameter(findParameterBinding(node, spec.role));
            lowered = lowered && value.has_value();
            if (value)
                kernel.values[document::lensFlareScalarIndex(spec.role)] = *value;
        }
        const auto position = compiledVec2Parameter(findParameterBinding(node, "lightPosition"));
        const auto color = compiledColorParameter(findParameterBinding(node, "lightColor"));
        const auto anamorphic = compiledColorParameter(findParameterBinding(node, "anamorphicColor"));
        lowered = lowered && position && color && anamorphic;
        if (!lowered) {
            addTopologyFailure(node.id, "Lens Flare parameters could not be lowered.");
            return std::nullopt;
        }
        kernel.position = *position;
        kernel.color = *color;
        kernel.anamorphicColor = *anamorphic;
        runtime::CompiledImageEffect effect{node.id, *input, std::move(kernel)};
        if (const auto* edge = fixedInputEdge(node.id, document::kLensFlareOcclusionPort)) {
            const auto matte =
                matteInputOperation(node.id, document::kLensFlareOcclusionPort, indices);
            if (!matte)
                return std::nullopt;
            auto& flare = std::get<runtime::LensFlareKernel>(effect.kernel);
            if (composition_->graph().outputKind(edge->source, registry_) ==
                    runtime::SocketValueKind::Mask &&
                flare.occlusionChannel >= 0 && flare.occlusionChannel < 10)
                flare.occlusionChannel = flare.occlusionChannel >= 5 ? 5 : 0;
            effect.fieldImages.push_back(*matte);
        }
        return effect;
    }
    // #486 Film Grain: the constant animation, seed and Monochrome, and every value. A node with no
    // Monochrome or Mode binding (saved before them, on a path that does not migrate) lowers with
    // them off. #565: a mode lowers only the values it reads; the others keep their defaults with
    // no identity, so an unused (even animated) value neither claims nor invalidates anything.
    if (node.typeId == document::kFilmGrainNodeType) {
        runtime::FilmGrainKernel kernel;
        const auto* animation =
            parameterConstant<std::int64_t>(findParameterBinding(node, "animation"));
        const auto* seed = parameterConstant<std::int64_t>(findParameterBinding(node, "seed"));
        bool lowered = animation != nullptr && seed != nullptr;
        if (lowered) {
            kernel.animation = *animation;
            kernel.seed = *seed;
        }
        if (const auto* binding = findParameterBinding(node, "monochrome")) {
            const auto* monochrome = parameterConstant<bool>(binding);
            lowered = lowered && monochrome != nullptr;
            if (monochrome != nullptr)
                kernel.monochrome = *monochrome;
        }
        if (const auto* binding = findParameterBinding(node, "mode")) {
            const auto* mode = parameterConstant<std::int64_t>(binding);
            lowered = lowered && mode != nullptr && *mode >= document::kFilmGrainFilm &&
                      *mode <= document::kFilmGrainSensorNoise;
            if (mode != nullptr)
                kernel.mode = *mode;
        }
        const bool sensor = kernel.mode == document::kFilmGrainSensorNoise;
        for (const auto& spec : document::filmGrainParameters()) {
            if (spec.kind != document::FilmGrainParameterKind::Scalar)
                continue;
            const auto slot = document::filmGrainScalarIndex(spec.role);
            kernel.values[slot] = {{}, spec.value};
            const bool sensorOnly = spec.optional;
            const bool filmOnly = spec.role == "shadows" || spec.role == "midtones" ||
                                  spec.role == "highlights";
            if ((sensorOnly && !sensor) || (filmOnly && sensor))
                continue;
            const auto value = compiledScalarParameter(findParameterBinding(node, spec.role));
            // A node saved before a Sensor Noise value reads its default.
            if (!value && sensorOnly && findParameterBinding(node, spec.role) == nullptr)
                continue;
            lowered = lowered && value.has_value();
            if (value)
                kernel.values[slot] = *value;
        }
        if (lowered)
            return runtime::CompiledImageEffect{node.id, *input, std::move(kernel)};
        addTopologyFailure(node.id, "Film Grain parameters could not be lowered.");
        return std::nullopt;
    }
    // #505 Vignette: the constant shape and mode, the strength the mode reads and every shared
    // value; the other strength keeps the kernel's constant default without an identity, so an
    // unused (even animated) value neither claims nor invalidates anything.
    if (node.typeId == document::kVignetteNodeType) {
        const auto* shape = parameterConstant<std::int64_t>(findParameterBinding(node, "shape"));
        const auto* mode = parameterConstant<std::int64_t>(findParameterBinding(node, "mode"));
        const auto inRange = [](const std::int64_t* value, const std::string_view schemaKey) {
            const auto domain = document::vignetteIntegerDomain(schemaKey);
            return value && *value >= domain->minimum && *value <= domain->maximum;
        };
        bool lowered = inRange(shape, document::kVignetteShapeSchemaKey) &&
                       inRange(mode, document::kVignetteModeSchemaKey);
        runtime::VignetteKernel kernel;
        if (lowered) {
            kernel.shape = static_cast<runtime::VignetteShape>(*shape);
            kernel.mode = static_cast<runtime::VignetteMode>(*mode);
            const auto scalar = [&](const std::string_view role,
                                    runtime::CompiledScalarParameter& target) {
                const auto parameter = compiledScalarParameter(findParameterBinding(node, role));
                lowered = lowered && parameter.has_value();
                if (parameter)
                    target = *parameter;
            };
            if (kernel.mode == runtime::VignetteMode::Multiply)
                scalar("amount", kernel.amount);
            else
                scalar("exposure", kernel.exposure);
            scalar("midpoint", kernel.midpoint);
            scalar("roundness", kernel.roundness);
            scalar("feather", kernel.feather);
            scalar("highlightProtection", kernel.highlightProtection);
            const auto center = compiledVec2Parameter(findParameterBinding(node, "center"));
            lowered = lowered && center.has_value();
            if (center)
                kernel.center = *center;
        }
        if (lowered)
            return runtime::CompiledImageEffect{node.id, *input, std::move(kernel)};
        addTopologyFailure(node.id, "Vignette parameters could not be lowered.");
        return std::nullopt;
    }
    // #481 Image Math, Image Map Range and Image Clamp: the constant operation, channels and
    // premultiplication, and only the values the operation reads; an Image Math whose `second`
    // input is connected reads that image instead of its value, which is then neither lowered nor
    // part of the identity.
    if (document::isImagePixelNodeType(node.typeId)) {
        runtime::PixelMathKernel kernel;
        const auto integer = [&](const std::string_view role, const std::int64_t high,
                                 std::int64_t& target) {
            const auto* value = parameterConstant<std::int64_t>(findParameterBinding(node, role));
            if (value == nullptr || *value < 0 || *value > high)
                return false;
            target = *value;
            return true;
        };
        std::int64_t space = 0;
        bool lowered = node.typeId == document::kImageCompareNodeType ||
                       integer("space", document::kImagePixelStored, space);
        kernel.straight = space == document::kImagePixelStraight;
        const auto scalar = [&](const std::string_view role, const std::size_t slot) {
            const auto parameter = compiledScalarParameter(findParameterBinding(node, role));
            lowered = lowered && parameter.has_value();
            if (parameter)
                kernel.values[slot] = *parameter;
        };
        if (node.typeId == document::kImageCompareNodeType) {
            // #349: A against B, on the stored values; both images are required.
            kernel.operation = runtime::PixelMathOperation::Compare;
            kernel.straight = false;
            kernel.second = true;
            lowered = lowered && integer("mode", document::kImageCompareCheckerboard,
                                         kernel.compareMode);
            std::size_t slot = 0;
            for (const auto role : {"gain", "splitPosition", "angle", "cellSize"})
                scalar(role, slot++);
        } else if (node.typeId == document::kImageClampNodeType) {
            kernel.operation = runtime::PixelMathOperation::Clamp;
            kernel.channels = document::kImagePixelColorAndAlpha;
            std::size_t slot = 0;
            for (const auto role : {"minRed", "maxRed", "minGreen", "maxGreen", "minBlue", "maxBlue",
                                    "minAlpha", "maxAlpha"})
                scalar(role, slot++);
        } else if (node.typeId == document::kImageMapRangeNodeType) {
            kernel.operation = runtime::PixelMathOperation::MapRange;
            lowered = lowered && integer("channels", document::kImagePixelColorAndAlpha,
                                         kernel.channels);
            const auto* clamp = parameterConstant<bool>(findParameterBinding(node, "clamp"));
            lowered = lowered && clamp != nullptr;
            kernel.clamp = clamp != nullptr && *clamp;
            std::size_t slot = 0;
            for (const auto role : {"inputMin", "inputMax", "outputMin", "outputMax"})
                scalar(role, slot++);
        } else {
            std::int64_t operation = 0;
            lowered = lowered && integer("operation", document::kImageMathAbsolute, operation) &&
                      integer("channels", document::kImagePixelColorAndAlpha, kernel.channels);
            kernel.operation = static_cast<runtime::PixelMathOperation>(operation);
            kernel.second = fixedInputEdge(node.id, document::kImagePixelSecondPort) != nullptr;
            if (!kernel.second && kernel.operation != runtime::PixelMathOperation::Absolute)
                scalar("value", 0);
        }
        if (!lowered) {
            addTopologyFailure(node.id, "Image pixel math parameters could not be lowered.");
            return std::nullopt;
        }
        runtime::CompiledImageEffect effect{node.id, *input, std::move(kernel)};
        if (std::get<runtime::PixelMathKernel>(effect.kernel).second) {
            const auto second = findInputOperation(node.id, document::kImagePixelSecondPort, indices);
            if (!second) {
                addTopologyFailure(node.id, "Image pixel math second input could not be lowered.");
                return std::nullopt;
            }
            effect.secondaryInput = *second;
        }
        return effect;
    }
    // #527 Dither: every choice, the seed and Monochrome are constants; the amplitude is the one
    // value. The matrix size is read only by the Ordered pattern, so another pattern lowers it to
    // the blue-noise side whatever it holds and it never claims the identity.
    if (node.typeId == document::kDitherNodeType) {
        runtime::DitherKernel kernel;
        const auto constant = [&](const std::string_view role) {
            return parameterConstant<std::int64_t>(findParameterBinding(node, role));
        };
        const auto* pattern = constant("pattern");
        const auto* size = constant("matrixSize");
        const auto* depth = constant("bitDepth");
        const auto* domain = constant("domain");
        const auto* animation = constant("animation");
        const auto* seed = constant("seed");
        const auto* monochrome = parameterConstant<bool>(findParameterBinding(node, "monochrome"));
        bool lowered = pattern != nullptr && size != nullptr && depth != nullptr &&
                       domain != nullptr && animation != nullptr && seed != nullptr &&
                       monochrome != nullptr;
        if (lowered) {
            kernel.blueNoise = *pattern == document::kDitherBlueNoise;
            kernel.side = kernel.blueNoise ? core::dither::kBlueNoiseSide
                                           : document::ditherBayerSide(*size);
            kernel.bits = document::ditherBits(*depth);
            kernel.stored = *domain == document::kDitherStored;
            kernel.everyFrame = *animation == document::kDitherEveryFrame;
            kernel.seed = *seed;
            kernel.monochrome = *monochrome;
            lowered = (*pattern == document::kDitherOrdered || kernel.blueNoise) &&
                      kernel.side != 0 && kernel.bits != 0 &&
                      (*domain == document::kDitherDisplay || kernel.stored) &&
                      (*animation == document::kDitherEveryFrame ||
                       *animation == document::kDitherHeld) &&
                      *seed >= 0;
        }
        const auto amplitude = compiledScalarParameter(findParameterBinding(node, "amplitude"));
        lowered = lowered && amplitude.has_value();
        if (amplitude)
            kernel.amplitude = *amplitude;
        if (lowered)
            return runtime::CompiledImageEffect{node.id, *input, std::move(kernel)};
        addTopologyFailure(node.id, "Dither parameters could not be lowered.");
        return std::nullopt;
    }
    // #518 Tone Map: the constant method and gamut switch, then the scalars in the kernel's order.
    if (document::isColorCleanupNodeType(node.typeId)) {
        runtime::ColorToolKernel kernel;
        bool lowered = true;
        const auto choice = [&](const std::string_view role, const std::size_t slot,
                                const std::int64_t high) {
            const auto* value = parameterConstant<std::int64_t>(findParameterBinding(node, role));
            lowered = lowered && value != nullptr && *value >= 0 && *value <= high;
            if (value != nullptr)
                kernel.choices[slot] = *value;
        };
        const auto flag = [&](const std::string_view role, const std::size_t slot) {
            const auto* value = parameterConstant<bool>(findParameterBinding(node, role));
            lowered = lowered && value != nullptr;
            kernel.choices[slot] = value != nullptr && *value ? 1 : 0;
        };
        const auto scalars = [&](std::initializer_list<std::string_view> roles) {
            std::size_t slot = 0;
            for (const auto role : roles) {
                const auto parameter = compiledScalarParameter(findParameterBinding(node, role));
                lowered = lowered && parameter.has_value();
                if (parameter)
                    kernel.values[slot] = *parameter;
                ++slot;
            }
        };
        if (node.typeId == document::kToneMapNodeType) {
            kernel.operation = runtime::ColorToolOperation::ToneMap;
            choice("method", 0, document::kToneMapBrightestChannel);
            flag("gamutCompress", 1);
            scalars({"knee", "ceiling", "shadows", "shadowRange", "gamutThreshold", "gamutLimit"});
        } else if (node.typeId == document::kColorMatrixNodeType) {
            // #517 Color Matrix: the preset, size and two color models, then the sixteen elements.
            kernel.operation = runtime::ColorToolOperation::ColorMatrix;
            choice("preset", 0, document::kColorMatrixPresetCount - 1);
            choice("matrixSize", 1, document::kColorMatrixSize4);
            choice("inputModel", 2, document::kColorModelCount - 1);
            choice("outputModel", 3, document::kColorModelCount - 1);
            scalars({"rr", "rg", "rb", "ra", "gr", "gg", "gb", "ga", "br", "bg", "bb", "ba", "ar",
                     "ag", "ab", "aa"});
        } else if (node.typeId == document::kQuantizeNodeType) {
            // #519 Quantize: the mode, basis, invert and the two link switches are constants; a
            // scalar the mode does not read is a fixed constant, never a curve nothing references,
            // so a keyed value of the other mode neither varies the picture nor invalidates it.
            kernel.operation = runtime::ColorToolOperation::Quantize;
            choice("mode", 0, document::kQuantizePosterize);
            choice("basis", 1, document::kQuantizeBasisPerChannel);
            flag("invert", 2);
            const auto* link = parameterConstant<bool>(findParameterBinding(node, "linkLevels"));
            const auto* alpha =
                parameterConstant<bool>(findParameterBinding(node, "posterizeAlpha"));
            lowered = lowered && link != nullptr && alpha != nullptr;
            const bool linked = link != nullptr && *link;
            const bool alphaOn = alpha != nullptr && *alpha;
            kernel.choices[3] = (linked ? 1 : 0) | (alphaOn ? 2 : 0);
            const bool posterize = kernel.choices[0] == document::kQuantizePosterize;
            const auto fixed = runtime::CompiledScalarParameter{document::ParameterId{}, 0.0};
            const auto one = [&](const std::string_view role, const std::size_t slot, const bool read) {
                if (!read) {
                    kernel.values[slot] = fixed;
                    return;
                }
                const auto parameter = compiledScalarParameter(findParameterBinding(node, role));
                lowered = lowered && parameter.has_value();
                if (parameter)
                    kernel.values[slot] = *parameter;
            };
            one("level", 0, !posterize);
            one("softness", 1, !posterize);
            one("levels", 2, posterize && linked);
            one("redLevels", 3, posterize && !linked);
            one("greenLevels", 4, posterize && !linked);
            one("blueLevels", 5, posterize && !linked);
            one("alphaLevels", 6, posterize && alphaOn);
        } else if (node.typeId == document::kHslQualifierNodeType) {
            // #523 HSL Qualifier: an Image to a Mask.
            kernel.operation = runtime::ColorToolOperation::HslQualifier;
            flag("invert", 0);
            scalars({"hueCenter", "hueWidth", "hueSoftness", "satLow", "satHigh", "satSoftness",
                     "lumaLow", "lumaHigh", "lumaSoftness"});
        }
        if (!lowered) {
            addTopologyFailure(node.id, "Color tool parameters could not be lowered.");
            return std::nullopt;
        }
        return runtime::CompiledImageEffect{node.id, *input, std::move(kernel)};
    }
    if (node.typeId == document::kHueSaturationNodeType) {
        const auto hue = compiledScalarParameter(findParameterBinding(node, "hue"));
        const auto saturation = compiledScalarParameter(findParameterBinding(node, "saturation"));
        // Vibrance and Colorize are later additions: a node saved without them reads as off.
        runtime::HueSaturationKernel kernel{};
        bool optionalLowered = true;
        if (const auto* binding = findParameterBinding(node, "vibrance")) {
            const auto vibrance = compiledScalarParameter(binding);
            optionalLowered = vibrance.has_value();
            if (vibrance)
                kernel.vibrance = *vibrance;
        }
        if (const auto* binding = findParameterBinding(node, "colorize")) {
            const auto* colorize = parameterConstant<bool>(binding);
            optionalLowered = optionalLowered && colorize != nullptr;
            kernel.colorize = colorize != nullptr && *colorize;
        }
        if (hue && saturation && optionalLowered) {
            kernel.hue = *hue;
            kernel.saturation = *saturation;
            return runtime::CompiledImageEffect{node.id, *input, std::move(kernel)};
        }
        addTopologyFailure(node.id, "Hue/Saturation parameters could not be lowered.");
        return std::nullopt;
    }
    // #494 Grade: the wheels' values, then the primaries, in the kernel's order.
    if (node.typeId == document::kGradeNodeType) {
        runtime::GradeKernel kernel;
        std::size_t index = 0;
        bool lowered = true;
        const auto lower = [&](const std::string& role) {
            const auto parameter = compiledScalarParameter(findParameterBinding(node, role));
            lowered = lowered && parameter.has_value();
            if (parameter)
                kernel.values[index] = *parameter;
            ++index;
        };
        for (const auto wheel : document::kGradeWheels)
            for (std::size_t component = 0; component < document::kGradeWheelComponentCount;
                 ++component)
                lower(document::gradeWheelRole(wheel, component));
        for (const auto primary : document::kGradePrimaries)
            lower(std::string(document::gradePrimaryRole(primary)));
        if (lowered)
            return runtime::CompiledImageEffect{node.id, *input, std::move(kernel)};
        addTopologyFailure(node.id, "Grade parameters could not be lowered.");
        return std::nullopt;
    }
    const auto pointwise = [&]() -> std::optional<runtime::PointwiseImageEffect> {
        if (node.typeId == "bloom.exposure")
            return runtime::PointwiseImageEffect::Exposure;
        if (node.typeId == "bloom.offset")
            return runtime::PointwiseImageEffect::Offset;
        if (node.typeId == "bloom.multiply")
            return runtime::PointwiseImageEffect::Multiply;
        if (node.typeId == "bloom.invert")
            return runtime::PointwiseImageEffect::Invert;
        return std::nullopt;
    }();
    if (pointwise) {
        if (*pointwise == runtime::PointwiseImageEffect::Invert)
            return runtime::CompiledImageEffect{node.id, *input,
                                                runtime::PointwiseImageKernel{*pointwise, {}}};
        if (const auto amount = compiledScalarParameter(findParameterBinding(node, "amount")))
            return runtime::CompiledImageEffect{node.id, *input,
                                                runtime::PointwiseImageKernel{*pointwise, *amount}};
    }
    if (node.typeId == "bloom.crop" || node.typeId == "bloom.pad") {
        std::array<runtime::CompiledScalarParameter, 4> margins;
        for (std::size_t i = 0; i < margins.size(); ++i) {
            const auto parameter = compiledScalarParameter(
                findParameterBinding(node, std::array{"left", "top", "right", "bottom"}[i]));
            if (!parameter) {
                addTopologyFailure(node.id, "Canvas margin could not be lowered.");
                return std::nullopt;
            }
            margins[i] = *parameter;
        }
        return runtime::CompiledImageEffect{
            node.id, *input,
            runtime::CanvasImageKernel{node.typeId == "bloom.crop"
                                           ? runtime::CanvasImageEffect::Crop
                                           : runtime::CanvasImageEffect::Pad,
                                       margins}};
    }
    if (node.typeId == "bloom.transform-2d") {
        const auto translate = compiledVec2Parameter(findParameterBinding(node, "translate"));
        const auto pivot = compiledVec2Parameter(findParameterBinding(node, "pivot"));
        const auto scale = compiledVec2Parameter(findParameterBinding(node, "scale"));
        const auto rotation = compiledScalarParameter(findParameterBinding(node, "rotation"));
        const auto* extent = parameterConstant<std::int64_t>(findParameterBinding(node, "extent"));
        const auto* filter = parameterConstant<std::int64_t>(findParameterBinding(node, "filter"));
        if (translate && pivot && scale && rotation && extent && *extent >= 0 && *extent <= 2 &&
            filter && (*filter == runtime::kResampleFilterBilinear ||
                       *filter == runtime::kResampleFilterNearest)) {
            runtime::Transform2DKernel transform{*translate, *pivot, *scale, *rotation,
                                                 static_cast<runtime::Transform2DExtent>(*extent),
                                                 *filter};
            // Skew and Opacity are later additions: a node saved without them reads as none.
            bool optionalLowered = true;
            for (const auto& [role, target] :
                 {std::pair<const char*, runtime::CompiledScalarParameter*>{"skew",
                                                                            &transform.skew},
                  {"skewAxis", &transform.skewAxis},
                  {"opacity", &transform.opacity}}) {
                if (const auto* binding = findParameterBinding(node, role)) {
                    const auto lowered = compiledScalarParameter(binding);
                    optionalLowered = optionalLowered && lowered.has_value();
                    // A constant at its default is the plan a node without the parameter has, so
                    // a document opened from before the parameter compiles to what it did.
                    if (lowered) {
                        const auto* constant = std::get_if<double>(&lowered->source);
                        if (constant == nullptr || *constant != std::get<double>(target->source))
                            *target = *lowered;
                    }
                }
            }
            if (!optionalLowered) {
                addTopologyFailure(node.id, "Transform 2D parameters could not be lowered.");
                return std::nullopt;
            }
            // #484: with motion blur on, the transform resampled at each sub-frame.
            // Skew, skew axis and opacity ride in the shutter samples with the rest.
            if (auto blurred = transformMotionBlurKernel(node, transform))
                return runtime::CompiledImageEffect{node.id, *input, std::move(*blurred)};
            return runtime::CompiledImageEffect{node.id, *input, std::move(transform)};
        }
        addTopologyFailure(node.id, "Transform 2D parameters could not be lowered.");
        return std::nullopt;
    }
    // #512 Camera Shake: Transform 2D (the same kernel, resampler and motion blur) driven by the
    // node's shake value operation, which the value pass compiled (lowerShakeKernel) so that its
    // offset and roll outputs can drive other parameters.
    if (node.typeId == document::kCameraShakeNodeType) {
        const auto offset = valueOutputs_.find(
            ValueOutputKey{node.id, std::string(document::kCameraShakeOffsetPort)});
        if (offset == valueOutputs_.end()) {
            addTopologyFailure(node.id, "Camera Shake parameters could not be lowered.");
            return std::nullopt;
        }
        const auto output = [&](const std::size_t slot) {
            return runtime::ValueOutputIndex::fromRaw(offset->second.value() + slot);
        };
        runtime::Transform2DKernel transform{
            runtime::CompiledVec2Parameter{{}, output(0)},
            runtime::CompiledVec2Parameter{{}, document::Vec2d{}},
            runtime::CompiledVec2Parameter{{}, output(2)},
            runtime::CompiledScalarParameter{{}, output(1)},
            runtime::Transform2DExtent::Input,
            runtime::kResampleFilterBilinear};
        if (auto blurred = transformMotionBlurKernel(node, transform))
            return runtime::CompiledImageEffect{node.id, *input, std::move(*blurred)};
        return runtime::CompiledImageEffect{node.id, *input, std::move(transform)};
    }
    if (node.typeId == "bloom.resize" || node.typeId == "bloom.fit") {
        const auto* width = parameterConstant<std::int64_t>(findParameterBinding(node, "width"));
        const auto* height = parameterConstant<std::int64_t>(findParameterBinding(node, "height"));
        const auto* numerator =
            parameterConstant<std::int64_t>(findParameterBinding(node, "pixelAspectNumerator"));
        const auto* denominator =
            parameterConstant<std::int64_t>(findParameterBinding(node, "pixelAspectDenominator"));
        const auto* filter = parameterConstant<std::int64_t>(findParameterBinding(node, "filter"));
        const auto* bypass = parameterConstant<bool>(findParameterBinding(node, "bypass"));
        const auto* policy = parameterConstant<std::int64_t>(findParameterBinding(node, "policy"));
        if (width && height && numerator && denominator && filter && bypass &&
            (node.typeId == "bloom.resize" || policy))
            return runtime::CompiledImageEffect{
                node.id, *input,
                runtime::ResizeFitKernel{*width, *height, *numerator, *denominator, *filter,
                                         node.typeId == "bloom.resize" ? 0 : *policy + 1},
                *bypass};
    }
    if (node.typeId == "bloom.ocio-colour-space-transform") {
        const auto* from = parameterConstant<std::string>(findParameterBinding(node, "from"));
        const auto* to = parameterConstant<std::string>(findParameterBinding(node, "to"));
        const auto* bypass = parameterConstant<bool>(findParameterBinding(node, "bypass"));
        if (from && to && bypass)
            return runtime::CompiledImageEffect{node.id, *input, runtime::CstKernel{*from, *to},
                                                *bypass};
    }
    if (node.typeId == "bloom.ocio-file-transform") {
        const auto* lut = parameterConstant<std::int64_t>(findParameterBinding(node, "lut"));
        const auto* interpolation =
            parameterConstant<std::int64_t>(findParameterBinding(node, "interpolation"));
        const auto* direction =
            parameterConstant<std::int64_t>(findParameterBinding(node, "direction"));
        const auto* space =
            parameterConstant<std::string>(findParameterBinding(node, "processSpace"));
        const auto* bypass = parameterConstant<bool>(findParameterBinding(node, "bypass"));
        const auto* look = parameterConstant<bool>(findParameterBinding(node, "look"));
        if (lut && *lut >= 0 && interpolation && direction && space && bypass && look) {
            const auto id = document::AssetId::fromRaw(static_cast<std::uint64_t>(*lut));
            const auto* asset = request_.snapshot.project().findAsset(id);
            return runtime::CompiledImageEffect{
                node.id, *input,
                runtime::FileTransformKernel{id, *interpolation, *direction, *space,
                                             asset && asset->kind == document::AssetKind::Lut
                                                 ? std::optional{*asset}
                                                 : std::nullopt},
                *bypass, *look};
        }
    }
    addTopologyFailure(node.id, "Image effect kernel or parameters are unsupported.");
    return std::nullopt;
}

[[nodiscard]] bool hasReachableOutput(const document::NodeId nodeId,
                                      const std::string_view port) const {
    return reachedMatteOutputs_.contains({nodeId, std::string(port)}) ||
           std::ranges::any_of(reachableEdges_, [&](const auto* edge) {
               return edge->source.nodeId == nodeId && edge->source.port == port;
           });
}

[[nodiscard]] const std::string* channelText(const document::NodeRecord& node,
                                             const std::string_view role) {
    const auto* value = parameterConstant<std::string>(findParameterBinding(node, role));
    if (value == nullptr)
        addTopologyFailure(node.id, "Channel node parameters must be constant text.");
    return value;
}

// Appends the steps that produce the ChannelSet on `output` and returns the index of its step.
// `memo` shares a step between consumers of the same output. `indices` (the operations lowered so
// far) gives an Image to Channel Set or Video source its image operation (#480).
[[nodiscard]] std::optional<std::uint32_t>
lowerChannelStep(const document::OutputPortRef& output,
                 runtime::CompiledChannelExpression& expression,
                 std::map<std::pair<document::NodeId, std::string>, std::uint32_t>& memo,
                 const std::size_t depth,
                 const std::unordered_map<document::NodeId, runtime::OperationIndex>* indices =
                     nullptr) {
    if (depth > 256) {
        addTopologyFailure(output.nodeId, "Channel graph is too deep.");
        return std::nullopt;
    }
    const auto memoKey = std::pair{output.nodeId, output.port};
    if (const auto found = memo.find(memoKey); found != memo.end())
        return found->second;
    const auto* node = findNode(output.nodeId);
    if (node == nullptr) {
        addTopologyFailure(output.nodeId, "Channel source node is missing.");
        return std::nullopt;
    }
    const auto input = [&](const std::string_view port) -> std::optional<std::uint32_t> {
        const auto* edge = fixedInputEdge(node->id, port);
        if (edge == nullptr) {
            addTopologyFailure(node->id, "Channel node input is not connected.");
            return std::nullopt;
        }
        return lowerChannelStep(edge->source, expression, memo, depth + 1, indices);
    };
    runtime::CompiledChannelStep step;
    step.nodeId = node->id;
    // #480: an image packed as named channels, known statically.
    const auto imageSource = [&](std::optional<runtime::OperationIndex> image,
                                 std::vector<render::ChannelDescriptor> channels) {
        runtime::CompiledChannelSource source;
        source.sourceNodeId = node->id;
        source.imageBacked = true;
        source.image = image;
        source.imageChannels = std::move(channels);
        expression.sources.push_back(std::move(source));
        step.kind = runtime::CompiledChannelStepKind::Source;
        step.source = static_cast<std::uint32_t>(expression.sources.size() - 1);
    };
    const auto operationOf = [&](const document::OutputPortRef& port)
        -> std::optional<runtime::OperationIndex> {
        if (indices == nullptr || !indices->contains(port.nodeId))
            return std::nullopt; // an empty image reads zero
        return sourceOperation(port, *indices);
    };
    if (node->typeId == document::kImageToChannelSetNodeType) {
        const auto* layer =
            channelText(*node, document::kImageToChannelSetLayerParameterRole);
        const auto* names =
            channelText(*node, document::kImageToChannelSetChannelsParameterRole);
        const auto* encoding = parameterConstant<std::int64_t>(
            findParameterBinding(*node, document::kImageToChannelSetEncodingParameterRole));
        const auto split =
            names != nullptr ? document::splitImageToChannelSetNames(*names) : std::nullopt;
        if (layer == nullptr || !split || encoding == nullptr ||
            (*encoding != document::kImageToChannelSetDataEncoding &&
             *encoding != document::kImageToChannelSetColourEncoding)) {
            addTopologyFailure(node->id, "Image to Channel Set parameters could not be lowered.");
            return std::nullopt;
        }
        const auto* edge = fixedInputEdge(node->id, document::kImageToChannelSetImagePort);
        imageSource(edge != nullptr && !isMuted(node->id) ? operationOf(edge->source)
                                                          : std::nullopt,
                    runtime::detail::imageChannelSetDescriptors(
                        *layer, *split,
                        *encoding == document::kImageToChannelSetColourEncoding));
    } else if (node->typeId == document::kVideoSourceNodeType &&
               output.port == document::kVideoSourceChannelsOutputPort) {
        // The decoded frame's stored R, G, B and A, colour in the working space.
        const std::vector<std::string> rgba{"R", "G", "B", "A"};
        imageSource(isMuted(node->id) ? std::nullopt
                                      : operationOf({node->id, std::string("image")}),
                    runtime::detail::imageChannelSetDescriptors("", rgba, true));
    } else if (node->typeId == document::kCompositionSourceNodeType &&
               output.port == document::kCompositionSourceChannelsOutputPort) {
        // #480: the nested composite's R, G, B and A (Color, the Composition Source image), then
        // the nested Composition Output's channel layers with the composite copied over their
        // root R, G, B and A -- the channels a layered OpenEXR render of the precomp holds. Every
        // channel is read at the precomp's mapped time. A muted source is an empty composite.
        const auto composition = lowerCompositionSource(*node);
        if (!composition)
            return std::nullopt;
        const bool muted = isMuted(node->id);
        const std::vector<std::string> rgba{"R", "G", "B", "A"};
        imageSource(muted ? std::nullopt : channelComposite(*node, *composition, indices),
                    runtime::detail::imageChannelSetDescriptors("", rgba, true));
        const auto* nested = muted ? nullptr : nestedChannelOutput(*composition);
        if (nested != nullptr) {
            expression.steps.push_back(step);
            const auto composite = static_cast<std::uint32_t>(expression.steps.size() - 1);
            const auto layers =
                inlineNestedChannels(*node, *composition, *nested, expression, indices);
            step = {};
            step.nodeId = node->id;
            step.kind = runtime::CompiledChannelStepKind::Copy;
            step.input = layers;
            step.from = composite;
            step.select = "R,G,B,A";
        }
    } else if (node->typeId == document::kImageSourceNodeType) {
        const auto* asset = parameterConstant<std::string>(findParameterBinding(*node, "asset"));
        const auto* start =
            parameterConstant<std::int64_t>(findParameterBinding(*node, "startFrame"));
        const auto* loop = parameterConstant<std::int64_t>(findParameterBinding(*node, "loopMode"));
        const auto* space =
            parameterConstant<std::int64_t>(findParameterBinding(*node, "colorSpace"));
        const auto* inputSpace =
            parameterConstant<std::string>(findParameterBinding(*node, "inputColorSpaceId"));
        const auto* part = parameterConstant<std::string>(
            findParameterBinding(*node, document::kImagePartParameterRole));
        if (!asset || !start || !loop || !space) {
            addTopologyFailure(node->id, "Image source parameters could not be lowered.");
            return std::nullopt;
        }
        std::uint64_t raw = 0;
        const auto parsed = std::from_chars(asset->data(), asset->data() + asset->size(), raw);
        const auto* record =
            parsed.ec == std::errc{} && parsed.ptr == asset->data() + asset->size()
                ? request_.snapshot.project().findAsset(document::AssetId::fromRaw(raw))
                : nullptr;
        // A muted source contributes no asset; resolution then reports it unavailable.
        expression.sources.push_back(runtime::CompiledChannelSource{
            node->id, record && !isMuted(node->id) ? std::optional{*record} : std::nullopt, *start,
            *loop, *space, inputSpace == nullptr ? std::string{} : *inputSpace,
            part == nullptr ? std::string{} : *part});
        step.kind = runtime::CompiledChannelStepKind::Source;
        step.source = static_cast<std::uint32_t>(expression.sources.size() - 1);
    } else if (isMuted(node->id) && (node->typeId == document::kChannelRemoveNodeType ||
                                     node->typeId == document::kChannelCopyNodeType ||
                                     node->typeId == document::kChannelShuffleNodeType)) {
        // A muted channel node passes its primary set through unchanged.
        const auto through = input(document::kChannelSetPort);
        if (through)
            memo.emplace(memoKey, *through);
        return through;
    } else if (node->typeId == document::kChannelRemoveNodeType) {
        const auto from = input(document::kChannelSetPort);
        const auto* select = channelText(*node, "select");
        const auto* keep = parameterConstant<bool>(findParameterBinding(*node, "keep"));
        if (!from || !select || !keep)
            return std::nullopt;
        step.kind = runtime::CompiledChannelStepKind::Remove;
        step.input = *from;
        step.select = *select;
        step.keep = *keep;
    } else if (node->typeId == document::kChannelCopyNodeType) {
        const auto target = input(document::kChannelSetPort);
        const auto from = input(document::kChannelCopyFromPort);
        const auto* select = channelText(*node, "select");
        if (!target || !from || !select)
            return std::nullopt;
        step.kind = runtime::CompiledChannelStepKind::Copy;
        step.input = *target;
        step.from = *from;
        step.select = *select;
    } else if (node->typeId == document::kChannelShuffleNodeType &&
               output.port == document::kChannelSetPort) {
        const auto from = input(document::kChannelSetPort);
        if (!from)
            return std::nullopt;
        step.kind = runtime::CompiledChannelStepKind::Rename;
        step.input = *from;
        constexpr std::array<std::string_view, 4> kSlots{"red", "green", "blue", "alpha"};
        for (std::size_t slot = 0; slot < 4; ++slot) {
            const auto* value = channelText(*node, kSlots[slot]);
            if (!value)
                return std::nullopt;
            step.mapping[slot] = *value;
        }
        const auto* layer = channelText(*node, "layer");
        if (!layer)
            return std::nullopt;
        step.layer = *layer;
    } else {
        addTopologyFailure(node->id, "Node does not produce a channel set.");
        return std::nullopt;
    }
    expression.steps.push_back(std::move(step));
    const auto index = static_cast<std::uint32_t>(expression.steps.size() - 1);
    memo.emplace(memoKey, index);
    return index;
}

// #480: the Composition Source image a channel expression reads as its composite R, G, B and A:
// the source's own image operation, or one added for a source whose `image` output feeds nothing.
// Only an expression that can read images (`indices`) has one; elsewhere it reads zero.
[[nodiscard]] std::optional<runtime::OperationIndex>
channelComposite(const document::NodeRecord& node,
                 const runtime::CompiledCompositionSource& composition,
                 const std::unordered_map<document::NodeId, runtime::OperationIndex>* indices) {
    if (indices == nullptr || loweringOperations_ == nullptr)
        return std::nullopt;
    if (const auto found = indices->find(node.id); found != indices->end())
        return found->second;
    if (const auto found = channelComposites_.find(node.id); found != channelComposites_.end())
        return found->second;
    const auto index = runtime::OperationIndex::fromRaw(loweringOperations_->size());
    loweringOperations_->emplace_back(composition);
    channelComposites_.emplace(node.id, index);
    return index;
}

// #480: the nested Composition Output of a Composition Source when it writes channel layers.
[[nodiscard]] const runtime::CompiledCompositionOutput*
nestedChannelOutput(const runtime::CompiledCompositionSource& composition) const {
    if (composition.nestedPlanIndex >= nestedPlans_.size())
        return nullptr;
    const auto& nested = *nestedPlans_[composition.nestedPlanIndex];
    if (nested.output().value() >= nested.operations().size())
        return nullptr;
    const auto* output =
        std::get_if<runtime::CompiledCompositionOutput>(&nested.operations()[nested.output().value()]);
    return output != nullptr && output->channelLayers && !output->channelLayers->steps.empty()
               ? output
               : nullptr;
}

// #480: a nested channel layer's image (its CompiledChannelSource::imagePlan) as a Composition
// Source of that plan with the precomp's time mapping, so it evaluates at the mapped nested time
// on every route, exactly like the precomp's own image.
[[nodiscard]] std::optional<runtime::OperationIndex>
channelLayerImage(const document::NodeRecord& node,
                  const runtime::CompiledCompositionSource& composition,
                  const std::shared_ptr<const runtime::CompiledCompositionPlan>& plan) {
    if (loweringOperations_ == nullptr || !plan)
        return std::nullopt;
    const auto key = std::pair{node.id, plan.get()};
    if (const auto found = channelLayerImages_.find(key); found != channelLayerImages_.end())
        return found->second;
    std::size_t planIndex = 0;
    while (planIndex < nestedPlans_.size() && nestedPlans_[planIndex] != plan)
        ++planIndex;
    if (planIndex == nestedPlans_.size()) {
        nestedPlans_.push_back(plan);
        channelLayerPlans_.insert(planIndex);
    }
    auto image = composition;
    image.nestedPlanIndex = planIndex;
    const auto index = runtime::OperationIndex::fromRaw(loweringOperations_->size());
    loweringOperations_->emplace_back(image);
    channelLayerImages_.emplace(key, index);
    return index;
}

// #480: appends the nested Composition Output's channel expression to `expression` and returns the
// index of its root step. File sources gain the precomp's mapping as their outermost nesting
// entry; an image-backed layer reads its plan through channelLayerImage (zero where the
// expression cannot read images). The nested output's Layers list, when set, keeps only those
// layers, as its layered OpenEXR would.
[[nodiscard]] std::uint32_t
inlineNestedChannels(const document::NodeRecord& node,
                     const runtime::CompiledCompositionSource& composition,
                     const runtime::CompiledCompositionOutput& nested,
                     runtime::CompiledChannelExpression& expression,
                     const std::unordered_map<document::NodeId, runtime::OperationIndex>* indices) {
    const auto& layers = *nested.channelLayers;
    const auto sourceBase = static_cast<std::uint32_t>(expression.sources.size());
    const auto stepBase = static_cast<std::uint32_t>(expression.steps.size());
    for (const auto& source : layers.sources) {
        auto copy = source;
        if (source.imageBacked) {
            copy.image = indices != nullptr ? channelLayerImage(node, composition, source.imagePlan)
                                            : std::nullopt;
            copy.imagePlan.reset();
        } else {
            copy.nesting.insert(copy.nesting.begin(), composition);
        }
        expression.sources.push_back(std::move(copy));
    }
    for (auto step : layers.steps) {
        if (step.kind == runtime::CompiledChannelStepKind::Source) {
            step.source += sourceBase;
        } else {
            step.input += stepBase;
            if (step.kind == runtime::CompiledChannelStepKind::Copy)
                step.from += stepBase;
        }
        expression.steps.push_back(std::move(step));
    }
    auto root = static_cast<std::uint32_t>(expression.steps.size() - 1);
    if (nested.layers.find_first_not_of(" \t") != std::string::npos) {
        runtime::CompiledChannelStep keep;
        keep.kind = runtime::CompiledChannelStepKind::Remove;
        keep.nodeId = node.id;
        keep.input = root;
        keep.select = nested.layers;
        keep.keep = true;
        expression.steps.push_back(std::move(keep));
        root = static_cast<std::uint32_t>(expression.steps.size() - 1);
    }
    return root;
}

[[nodiscard]] std::optional<runtime::CompiledOperation>
lowerCryptomatte(const document::NodeRecord& node) {
    runtime::CompiledCryptomatte cryptomatte;
    cryptomatte.sourceNodeId = node.id;
    const auto* edge = fixedInputEdge(node.id, document::kChannelSetPort);
    if (edge == nullptr) {
        addTopologyFailure(node.id, "Cryptomatte has no channel set.");
        return std::nullopt;
    }
    std::map<std::pair<document::NodeId, std::string>, std::uint32_t> memo;
    const auto root = lowerChannelStep(edge->source, cryptomatte.channels, memo, 0);
    if (!root)
        return std::nullopt;
    if (*root + 1 != cryptomatte.channels.steps.size()) {
        auto copy = cryptomatte.channels.steps[*root];
        cryptomatte.channels.steps.push_back(std::move(copy));
    }
    const auto* layer = channelText(node, document::kCryptomatteLayerParameterRole);
    const auto* mattes = channelText(node, document::kCryptomatteMattesParameterRole);
    if (!layer || !mattes)
        return std::nullopt;
    cryptomatte.layer = *layer;
    cryptomatte.mattes = runtime::splitCryptomatteMatteList(*mattes);
    return runtime::CompiledOperation{std::move(cryptomatte)};
}

// A Shuffle's or Pass's image output (#480 for Pass and the image path). Slots that read only
// Image to Channel Set channels and constants become exact Channel Select / Channel Combine effects
// over the image operations; every other mapping is the evaluation-time gather, which also reports
// the refusals (Color on a data channel, image channels mixed with file channels).
[[nodiscard]] std::optional<runtime::OperationIndex>
lowerChannelShuffle(const document::NodeRecord& node,
                    std::vector<runtime::CompiledOperation>& operations,
                    const std::unordered_map<document::NodeId, runtime::OperationIndex>& indices) {
    const bool pass = node.typeId == document::kChannelPassNodeType;
    runtime::CompiledChannelShuffle shuffle;
    shuffle.sourceNodeId = node.id;
    const auto* edge = fixedInputEdge(node.id, document::kChannelSetPort);
    if (edge == nullptr) {
        addTopologyFailure(node.id, pass ? "Pass has no channel set." : "Shuffle has no channel set.");
        return std::nullopt;
    }
    std::map<std::pair<document::NodeId, std::string>, std::uint32_t> memo;
    const auto root = lowerChannelStep(edge->source, shuffle.channels, memo, 0, &indices);
    if (!root)
        return std::nullopt;
    // Steps are emitted in dependency order, so the root is the last step appended.
    if (*root + 1 != shuffle.channels.steps.size()) {
        auto copy = shuffle.channels.steps[*root];
        shuffle.channels.steps.push_back(std::move(copy));
    }
    const auto* mode = parameterConstant<std::int64_t>(findParameterBinding(node, "mode"));
    const auto* nonFiniteBinding =
        findParameterBinding(node, document::kChannelNonFiniteParameterRole);
    const auto* nonFinite =
        nonFiniteBinding != nullptr ? parameterConstant<double>(nonFiniteBinding) : nullptr;
    if (!mode || (nonFiniteBinding != nullptr && !nonFinite)) {
        addTopologyFailure(node.id, "Shuffle parameters could not be lowered.");
        return std::nullopt;
    }
    shuffle.mode = *mode;
    shuffle.nonFinite = nonFinite != nullptr ? static_cast<float>(*nonFinite) : 0.0F;
    const auto set = runtime::detail::staticChannelSet(shuffle.channels);
    std::optional<std::array<std::string, 4>> mapping;
    if (pass) {
        const auto* picked = channelText(node, document::kChannelPassParameterRole);
        if (!picked)
            return std::nullopt;
        shuffle.pass = *picked;
        shuffle.associated = false;
        // A static set (or a pass that names a static image channel or layer) maps here.
        std::vector<runtime::detail::PassCandidate> candidates;
        bool named = false;
        for (const auto& channel : set.known) {
            candidates.push_back({channel.name, channel.descriptor});
            named = named || (!picked->empty() && channel.kind == runtime::detail::StaticChannelKind::Image &&
                              (channel.name == *picked || channel.descriptor.layer == *picked));
        }
        if (!set.files || named)
            mapping = runtime::detail::passMapping(candidates, *picked);
    } else {
        constexpr std::array<std::string_view, 4> kSlots{"red", "green", "blue", "alpha"};
        mapping.emplace();
        for (std::size_t slot = 0; slot < 4; ++slot) {
            const auto* value = channelText(node, kSlots[slot]);
            if (!value)
                return std::nullopt;
            (*mapping)[slot] = *value;
        }
        shuffle.mapping = *mapping;
        const auto* associated = parameterConstant<bool>(findParameterBinding(node, "associated"));
        if (!associated) {
            addTopologyFailure(node.id, "Shuffle parameters could not be lowered.");
            return std::nullopt;
        }
        shuffle.associated = *associated;
    }
    return emitChannelShuffle(std::move(shuffle), mapping, node.id, operations);
}

// #480: emits a Shuffle's image (see lowerChannelShuffle). `mapping` is the static slot list, or
// nullopt when only evaluation can derive it (a Pass over file channels).
[[nodiscard]] runtime::OperationIndex
emitChannelShuffle(runtime::CompiledChannelShuffle shuffle,
                   const std::optional<std::array<std::string, 4>>& mapping,
                   const document::NodeId nodeId,
                   std::vector<runtime::CompiledOperation>& operations) {
    const auto gather = [&] {
        const auto index = runtime::OperationIndex::fromRaw(operations.size());
        operations.emplace_back(std::move(shuffle));
        return index;
    };
    if (!mapping)
        return gather();
    const auto set = runtime::detail::staticChannelSet(shuffle.channels);
    // Classify the slots statically.
    std::array<runtime::detail::StaticChannel, 4> slots;
    bool anyImage = false;
    bool everyStatic = true;
    std::optional<std::string> dataChannel;
    for (std::size_t slot = 0; slot < 4; ++slot) {
        slots[slot] = runtime::detail::staticChannelSlot(set, (*mapping)[slot], slot);
        const auto kind = slots[slot].kind;
        anyImage = anyImage || kind == runtime::detail::StaticChannelKind::Image;
        everyStatic = everyStatic && (kind == runtime::detail::StaticChannelKind::Image ||
                                      kind == runtime::detail::StaticChannelKind::Constant);
        if (slot < 3 && kind == runtime::detail::StaticChannelKind::Image && !dataChannel &&
            runtime::detail::isDataChannelRole(slots[slot].descriptor.role))
            dataChannel = slots[slot].name;
    }
    // The first image a slot reads owns the result. A mapping of constants alone stays the gather
    // (its frame is a file source's), as does Color on a data lane, a slot mixing image and file
    // channels, and one the compiler cannot decide: the gather reports each refusal.
    std::optional<std::uint32_t> baseSource;
    for (const auto& slot : slots)
        if (slot.kind == runtime::detail::StaticChannelKind::Image) {
            baseSource = slot.source;
            break;
        }
    if (!anyImage || !everyStatic || !baseSource ||
        (shuffle.mode == document::kChannelShuffleColourMode && dataChannel))
        return gather();
    // Image channels are already working-space samples: Color and Raw read them unchanged, and
    // `associated` off premultiplies by the mapped alpha.
    const bool premultiply = !shuffle.associated;
    const auto baseImage = shuffle.channels.sources[*baseSource].image;
    if (!baseImage) {
        const auto empty = runtime::OperationIndex::fromRaw(operations.size());
        operations.emplace_back(runtime::CompiledMerge{nodeId, {}});
        return empty;
    }
    const auto laneSource = [](const std::uint8_t lane) {
        constexpr std::array kLanes{runtime::ImageChannelSource::Red, runtime::ImageChannelSource::Green,
                                    runtime::ImageChannelSource::Blue,
                                    runtime::ImageChannelSource::Alpha};
        return kLanes[lane < 4 ? lane : 0];
    };
    runtime::ChannelSelectKernel select;
    std::vector<std::size_t> pending;
    for (std::size_t slot = 0; slot < 4; ++slot) {
        const auto& item = slots[slot];
        const bool alpha = slot == 3;
        const auto constant = [&](const float value) {
            return value == 1.0F ? runtime::ImageChannelSource::One
                                 : runtime::ImageChannelSource::Zero;
        };
        if (item.kind == runtime::detail::StaticChannelKind::Constant) {
            select.sources[slot] = constant(item.constant);
        } else if (item.source == *baseSource) {
            select.sources[slot] = laneSource(item.lane);
        } else if (!shuffle.channels.sources[item.source].image) {
            select.sources[slot] = runtime::ImageChannelSource::Zero; // an absent image reads 0
        } else {
            // A slot a later operation sets keeps a nonzero alpha meanwhile.
            select.sources[slot] =
                alpha ? runtime::ImageChannelSource::One : runtime::ImageChannelSource::Zero;
            pending.push_back(slot);
        }
    }
    select.premultiply = premultiply && pending.empty();
    auto current = runtime::OperationIndex::fromRaw(operations.size());
    operations.emplace_back(runtime::CompiledImageEffect{nodeId, *baseImage, select});
    for (const auto slot : pending) {
        const auto& item = slots[slot];
        runtime::ChannelCombineKernel kernel{static_cast<std::uint8_t>(slot), laneSource(item.lane),
                                             premultiply && slot == pending.back()};
        const auto next = runtime::OperationIndex::fromRaw(operations.size());
        operations.emplace_back(runtime::CompiledImageEffect{
            nodeId, current, kernel, false, false, *shuffle.channels.sources[item.source].image});
        current = next;
    }
    return current;
}

[[nodiscard]] std::optional<runtime::CompiledOperation>
lowerImageSource(const document::NodeRecord& node) {
    const auto* asset = parameterConstant<std::string>(findParameterBinding(node, "asset"));
    const auto* start = parameterConstant<std::int64_t>(findParameterBinding(node, "startFrame"));
    const auto* loop = parameterConstant<std::int64_t>(findParameterBinding(node, "loopMode"));
    const auto* space = parameterConstant<std::int64_t>(findParameterBinding(node, "colorSpace"));
    const auto* inputSpace =
        parameterConstant<std::string>(findParameterBinding(node, "inputColorSpaceId"));
    const auto* premultiply = parameterConstant<bool>(findParameterBinding(node, "premultiply"));
    if (!asset || !start || !loop || !space || !premultiply) {
        addTopologyFailure(node.id, "Image source parameters could not be lowered.");
        return std::nullopt;
    }
    std::uint64_t raw = 0;
    const auto parsed = std::from_chars(asset->data(), asset->data() + asset->size(), raw);
    const auto* record =
        parsed.ec == std::errc{} && parsed.ptr == asset->data() + asset->size()
            ? request_.snapshot.project().findAsset(document::AssetId::fromRaw(raw))
            : nullptr;
    // ADR 0026: part and layer are optional bindings (absent in a migrated v1 node).
    const auto* part = parameterConstant<std::string>(
        findParameterBinding(node, document::kImagePartParameterRole));
    const auto* layer = parameterConstant<std::string>(
        findParameterBinding(node, document::kImageLayerParameterRole));
    return runtime::CompiledImageSource{node.id,
                                        record ? std::optional{*record} : std::nullopt,
                                        *start,
                                        *loop,
                                        *space,
                                        inputSpace == nullptr ? std::string{} : *inputSpace,
                                        *premultiply,
                                        part == nullptr ? std::string{} : *part,
                                        layer == nullptr ? std::string{} : *layer};
}

[[nodiscard]] std::optional<runtime::CompiledOperation>
lowerVideoSource(const document::NodeRecord& node) {
    const auto* asset = parameterConstant<std::string>(findParameterBinding(node, "asset"));
    const auto* start = parameterConstant<std::int64_t>(findParameterBinding(node, "startFrame"));
    const auto* loop = parameterConstant<std::int64_t>(findParameterBinding(node, "loopMode"));
    const auto* space = parameterConstant<std::int64_t>(findParameterBinding(node, "colorSpace"));
    const auto* inputSpace =
        parameterConstant<std::string>(findParameterBinding(node, "inputColorSpaceId"));
    if (!asset || !start || !loop || !space) {
        addTopologyFailure(node.id, "Video source parameters could not be lowered.");
        return std::nullopt;
    }
    std::uint64_t raw = 0;
    const auto parsed = std::from_chars(asset->data(), asset->data() + asset->size(), raw);
    const auto* record =
        parsed.ec == std::errc{} && parsed.ptr == asset->data() + asset->size()
            ? request_.snapshot.project().findAsset(document::AssetId::fromRaw(raw))
            : nullptr;
    return runtime::CompiledVideoSource{
        node.id, record ? std::optional{*record} : std::nullopt,     *start, *loop,
        *space,  inputSpace == nullptr ? std::string{} : *inputSpace};
}

// A driven parameter whose driver this composition cannot read (an output port the driving node
// does not have, or a kind no promotion reaches). Reported against the parameter, in words, instead
// of as a topology failure; true when it reported one. Commands refuse such a binding at commit,
// so only an older document or another client's edit reaches it.
[[nodiscard]] bool unreadableDriver(const document::NodeRecord& node,
                                    const document::ParameterBinding* binding,
                                    const runtime::SocketValueKind kind,
                                    const std::optional<runtime::ValueOutputIndex>& resolved) {
    const auto* parameter = binding == nullptr ? nullptr : findParameter(binding->parameterId);
    const auto* driver = parameter == nullptr
                             ? nullptr
                             : std::get_if<document::DriverBindingSource>(&parameter->source);
    if (driver == nullptr || resolved.has_value())
        return false;
    auto diagnosticSubject = subject(node.id, "parameter." + binding->role);
    diagnosticSubject.parameterId = parameter->id;
    addFailure(runtime::CompileDiagnosticCode::UnknownPort, std::move(diagnosticSubject),
               "Parameter " + std::to_string(parameter->id.value()) + " (" + binding->role +
                   ") cannot read its driver",
               "Node " + std::to_string(driver->sourceNodeId.value()) + " has no " +
                   std::string(document::socketValueKindName(kind)) + " output \"" +
                   driver->outputPort + "\" that reaches it; drive it from an existing output");
    return true;
}

[[nodiscard]] std::optional<runtime::CompiledOperation>
lowerShape(const document::NodeRecord& node) {
    const auto size = compiledVec2Parameter(findParameterBinding(node, "size"));
    const auto fill = compiledColorParameter(findParameterBinding(node, "fillColor"));
    const auto stroke = compiledColorParameter(findParameterBinding(node, "strokeColor"));
    const auto width = compiledScalarParameter(findParameterBinding(node, "strokeWidth"));
    const auto* kind = parameterConstant<std::int64_t>(findParameterBinding(node, "kind"));
    const auto* radius = parameterConstant<double>(findParameterBinding(node, "cornerRadius"));
    const auto* points = parameterConstant<std::int64_t>(findParameterBinding(node, "points"));
    const auto* ratio = parameterConstant<double>(findParameterBinding(node, "innerRatio"));
    const auto start = compiledVec2Parameter(findParameterBinding(node, "lineStart"));
    const auto end = compiledVec2Parameter(findParameterBinding(node, "lineEnd"));
    // #589: the path is an authored constant, or a Path output read per frame.
    const auto* pathBinding = findParameterBinding(node, "path");
    const auto drivenPath = drivenOutput(pathBinding, runtime::SocketValueKind::Path);
    const auto* storedPath = parameterConstant<document::PathValue>(pathBinding);
    if (unreadableDriver(node, pathBinding, runtime::SocketValueKind::Path, drivenPath))
        return std::nullopt;
    const document::PathValue emptyPath;
    const auto* path = storedPath != nullptr ? storedPath : drivenPath ? &emptyPath : nullptr;
    // The switches lower like a Layer Output's blend mode: a constant, or a Boolean driver with
    // the registry default carried beside it.
    const auto* fillBinding = findParameterBinding(node, "fillEnabled");
    const auto* strokeBinding = findParameterBinding(node, "strokeEnabled");
    const auto drivenFill = drivenOutput(fillBinding, runtime::SocketValueKind::Boolean);
    const auto drivenStroke = drivenOutput(strokeBinding, runtime::SocketValueKind::Boolean);
    const auto* storedFill = parameterConstant<bool>(fillBinding);
    const auto* storedStroke = parameterConstant<bool>(strokeBinding);
    const bool fillDefault = true;
    const bool strokeDefault = false;
    if (unreadableDriver(node, fillBinding, runtime::SocketValueKind::Boolean, drivenFill) ||
        unreadableDriver(node, strokeBinding, runtime::SocketValueKind::Boolean, drivenStroke))
        return std::nullopt;
    const auto* fillEnabled = storedFill != nullptr ? storedFill
                              : drivenFill          ? &fillDefault
                                                    : nullptr;
    const auto* strokeEnabled = storedStroke != nullptr ? storedStroke
                                : drivenStroke          ? &strokeDefault
                                                        : nullptr;
    const auto* align = parameterConstant<std::int64_t>(findParameterBinding(node, "strokeAlign"));
    const auto* join = parameterConstant<std::int64_t>(findParameterBinding(node, "strokeJoin"));
    const auto* cap = parameterConstant<std::int64_t>(findParameterBinding(node, "strokeCap"));
    const auto* rule = parameterConstant<std::int64_t>(findParameterBinding(node, "fillRule"));
    if (!size || !fill || !stroke || !width || !kind || !radius || !points || !ratio || !start ||
        !end || !path || !fillEnabled || !strokeEnabled || !align || !join || !cap || !rule) {
        addTopologyFailure(node.id, "Shape parameters could not be lowered.");
        return std::nullopt;
    }
    auto shape = runtime::CompiledShape{node.id,
                                        static_cast<document::ShapeKind>(*kind),
                                        *size,
                                        *radius,
                                        *points,
                                        *ratio,
                                        *start,
                                        *end,
                                        *path,
                                        *fillEnabled,
                                        *fill,
                                        *strokeEnabled,
                                        *stroke,
                                        *width,
                                        static_cast<document::ShapeStrokeAlign>(*align),
                                        static_cast<document::ShapeStrokeJoin>(*join),
                                        static_cast<document::ShapeStrokeCap>(*cap),
                                        static_cast<document::ShapeFillRule>(*rule)};
    shape.fillEnabledParameterId = fillBinding->parameterId;
    shape.drivenFillEnabled = drivenFill;
    shape.strokeEnabledParameterId = strokeBinding->parameterId;
    shape.drivenStrokeEnabled = drivenStroke;
    shape.drivenPath = drivenPath;
    return shape;
}

[[nodiscard]] std::optional<runtime::CompiledOperation>
lowerText(const document::NodeRecord& node) {
    using namespace document;
    const auto* contentBinding = findParameterBinding(node, kTextParameterRole);
    const auto* sizeBinding = findParameterBinding(node, kTextSizeParameterRole);
    const auto* colorBinding = findParameterBinding(node, kTextColorParameterRole);
    const auto* fontBinding = findParameterBinding(node, kTextFontParameterRole);
    const auto* content = parameterConstant<std::string>(contentBinding);
    // Task DRIVE-1: a driven content parameter holds no constant at all -- its source IS the driver
    // -- so the constant is absent exactly when the driver is present, and the lowering needs one
    // of the two rather than both.
    const auto drivenContent = drivenOutput(contentBinding, runtime::SocketValueKind::String);
    const auto size = compiledScalarParameter(sizeBinding);
    const auto color = compiledColorParameter(colorBinding);
    auto face = render::EmbeddedFace::DejaVuSans;
    render::TextFont font = face;
    // The font ID the node stores. A node authored before fonts were named (no binding) or holding
    // anything that is not a font ID draws in the built-in default, with a warning for the latter.
    auto fontId = document::defaultFontId();
    if (fontBinding != nullptr) {
        const auto* stored = parameterConstant<std::string>(fontBinding);
        const auto decoded = stored != nullptr ? document::decodeFontId(*stored) : std::nullopt;
        if (decoded) {
            fontId = *decoded;
        } else {
            auto diagnosticSubject = subject(node.id, "font");
            diagnosticSubject.parameterId = fontBinding->parameterId;
            addWarning(runtime::CompileDiagnosticCode::FontMissing, std::move(diagnosticSubject),
                       "Text font is unavailable",
                       "The Font parameter does not hold a font ID; DejaVu Sans was used.");
        }
    }
    bool fontMissing = false;
    bool fontPending = false;
    {
        auto& resolver = request_.fontResolver ? *request_.fontResolver
                                               : runtime::fonts::processFontResolver();
        const auto resolution = resolver.resolve(
            fontId, request_.fontFetch, [this] { return cancellation_.isCancellationRequested(); });
        if (resolution.bytes) {
            font = render::ExternalFontFile{resolution.bytes, resolution.digest,
                                            resolution.faceIndex};
        } else {
            face = static_cast<render::EmbeddedFace>(resolution.builtinFace);
            font = face;
        }
        fontMissing = resolution.missing();
        fontPending = resolution.pendingDownload();
        if (fontMissing && fontBinding != nullptr) {
            // The font stays in the project; installing it later restores it with no edit.
            auto diagnosticSubject = subject(node.id, "font");
            diagnosticSubject.parameterId = fontBinding->parameterId;
            addWarning(runtime::CompileDiagnosticCode::FontMissing, std::move(diagnosticSubject),
                       "Text font is missing", resolution.diagnostic);
        }
    }
    if (contentBinding == nullptr || sizeBinding == nullptr || colorBinding == nullptr ||
        (content == nullptr && !drivenContent.has_value()) || !size.has_value() ||
        !color.has_value()) {
        addTopologyFailure(node.id, "Validated text parameters could not be lowered.");
        return std::nullopt;
    }
    runtime::CompiledTextLayout layout;
    {
        const auto* alignmentBinding = findParameterBinding(node, kTextAlignmentParameterRole);
        const auto* alignment = parameterConstant<std::int64_t>(alignmentBinding);
        const auto drivenAlignment =
            drivenOutput(alignmentBinding, runtime::SocketValueKind::Integer);
        const auto lineHeight =
            compiledScalarParameter(findParameterBinding(node, kTextLineHeightParameterRole));
        const auto letterSpacing =
            compiledScalarParameter(findParameterBinding(node, kTextLetterSpacingParameterRole));
        if (alignmentBinding == nullptr || (alignment == nullptr && !drivenAlignment.has_value()) ||
            !lineHeight || !letterSpacing) {
            addTopologyFailure(node.id, "Text layout could not be lowered.");
            return std::nullopt;
        }
        layout.alignmentId = alignmentBinding->parameterId;
        layout.alignment = alignment == nullptr ? 0 : *alignment;
        layout.lineHeight = *lineHeight;
        layout.letterSpacing = *letterSpacing;
        layout.drivenAlignment = drivenAlignment;
        const auto* boxBinding = findParameterBinding(node, kTextBoxParameterRole);
        const auto* wrapBinding = findParameterBinding(node, kTextWrapParameterRole);
        const auto* verticalBinding =
            findParameterBinding(node, kTextVerticalAlignmentParameterRole);
        const auto* anchorBinding = findParameterBinding(node, kTextAnchorModeParameterRole);
        const auto* overflowBinding = findParameterBinding(node, kTextOverflowParameterRole);
        if (boxBinding != nullptr) {
            layout.boxId = boxBinding->parameterId;
            if (const auto* box = parameterConstant<Vec2d>(boxBinding))
                layout.box = *box;
        }
        if (wrapBinding != nullptr) {
            layout.wrapId = wrapBinding->parameterId;
            if (const auto* wrap = parameterConstant<bool>(wrapBinding))
                layout.wrap = *wrap;
        }
        const auto readDiscrete = [this](const document::ParameterBinding* binding,
                                         const std::int64_t fallback) {
            const auto* value = parameterConstant<std::int64_t>(binding);
            return value == nullptr ? fallback : *value;
        };
        if (verticalBinding != nullptr) {
            layout.verticalAlignmentId = verticalBinding->parameterId;
            layout.verticalAlignment = readDiscrete(verticalBinding, 0);
        }
        if (anchorBinding != nullptr) {
            layout.anchorModeId = anchorBinding->parameterId;
            layout.anchorMode = readDiscrete(anchorBinding, 0);
        }
        if (overflowBinding != nullptr) {
            layout.overflowId = overflowBinding->parameterId;
            layout.overflow = readDiscrete(overflowBinding, 0);
        }
    }
    runtime::CompiledText result;
    result.sourceNodeId = node.id;
    result.contentParameterId = contentBinding->parameterId;
    result.content = content == nullptr ? std::string{} : *content;
    result.size = *size;
    result.color = *color;
    result.layout = layout;
    result.drivenContent = drivenContent;
    result.face = face;
    result.font = std::move(font);
    result.fontId = document::encodeFontId(fontId);
    result.fontMissing = fontMissing;
    result.fontPending = fontPending;
    return result;
}

#include "snapshot_compiler_entries.ipp"

[[nodiscard]] std::optional<runtime::CompiledOperation> lowerCompositionOutput(
    const document::NodeRecord& node,
    const std::unordered_map<document::NodeId, runtime::OperationIndex>& indices) {
    const auto input = findInputOperation(node.id, document::kCompositionOutputInputPort, indices);
    if (!input) {
        addTopologyFailure(node.id, "Validated Composition Output could not be lowered.");
        return std::nullopt;
    }
    runtime::CompiledCompositionOutput output{node.id, *input};
    if (const auto* edge = fixedInputEdge(node.id, document::kCompositionOutputChannelsInputPort)) {
        runtime::CompiledChannelExpression expression;
        std::map<std::pair<document::NodeId, std::string>, std::uint32_t> memo;
        const auto root = lowerChannelStep(edge->source, expression, memo, 0, &indices);
        if (!root)
            return std::nullopt;
        if (*root + 1 != expression.steps.size()) {
            auto copy = expression.steps[*root];
            expression.steps.push_back(std::move(copy));
        }
        output.channelLayers = std::move(expression);
        if (const auto* layers = parameterConstant<std::string>(
                findParameterBinding(node, document::kCompositionOutputLayersParameterRole)))
            output.layers = *layers;
    }
    return output;
}

// Task DRIVE-1. The value-graph output the parameter behind `binding` is driven by, or nothing.
// The companion of parameterConstant() for the kinds that have no curve table of their own: a
// String, an Integer or a Boolean is either the constant the store holds or the value a driver
// hands it, and this answers the second half of that question for every one of them through the
// same driverOutput() the typed operands already use.
[[nodiscard]] std::optional<runtime::ValueOutputIndex>
drivenOutput(const document::ParameterBinding* binding, const runtime::SocketValueKind kind) {
    const auto* parameter = binding == nullptr ? nullptr : findParameter(binding->parameterId);
    return parameter == nullptr ? std::nullopt : driverOutput(*parameter, kind);
}

template <typename Value>
[[nodiscard]] const Value*
parameterConstant(const document::ParameterBinding* binding) const noexcept {
    if (binding == nullptr) {
        return nullptr;
    }
    const auto* parameter = findParameter(binding->parameterId);
    const auto* constant = parameter == nullptr
                               ? nullptr
                               : std::get_if<document::ConstantValueSource>(&parameter->source);
    return constant == nullptr ? nullptr : std::get_if<Value>(&constant->value);
}

// No longer const: resolving a driver may SYNTHESIZE a promotion operation into the value graph,
// and a widening that needs an operation is better compiled once here than hidden inside whoever
// reads the value.
// ADR 0028: a mask path is a constant PathValue or an inline compiled Path curve.
[[nodiscard]] std::optional<runtime::CompiledPathParameter>
compiledPathParameter(const document::ParameterId id) {
    const auto* parameter = findParameter(id);
    if (parameter == nullptr)
        return std::nullopt;
    if (const auto* constant = std::get_if<document::ConstantValueSource>(&parameter->source)) {
        const auto* value = std::get_if<document::PathValue>(&constant->value);
        return value == nullptr ? std::nullopt
                                : std::optional(runtime::CompiledPathParameter{id, *value});
    }
    if (const auto* source = std::get_if<document::AnimationCurveSource>(&parameter->source)) {
        if (const auto* curve = composition_->animationCurves().findPath(source->curveId))
            return runtime::CompiledPathParameter{id, runtime::compileAnimationCurve(*curve)};
    }
    return std::nullopt;
}

// #541: a gradient is a constant GradientValue or an inline compiled Gradient curve; #543 with
// the value outputs that drive its stops, in stop order (colour before position).
[[nodiscard]] std::optional<runtime::CompiledGradientParameter>
compiledGradientParameter(const document::NodeRecord& node,
                          const document::ParameterBinding& binding) {
    const auto id = binding.parameterId;
    const auto* parameter = findParameter(id);
    if (parameter == nullptr)
        return std::nullopt;
    std::optional<runtime::CompiledGradientParameter> compiled;
    if (const auto* constant = std::get_if<document::ConstantValueSource>(&parameter->source)) {
        if (const auto* value = std::get_if<document::GradientValue>(&constant->value))
            compiled = runtime::CompiledGradientParameter{id, *value};
    } else if (const auto* source =
                   std::get_if<document::AnimationCurveSource>(&parameter->source)) {
        if (const auto* curve = composition_->animationCurves().findGradient(source->curveId))
            compiled =
                runtime::CompiledGradientParameter{id, runtime::compileAnimationCurve(*curve)};
    }
    if (!compiled)
        return std::nullopt;
    std::vector<std::pair<document::GradientStopPort, const document::ParameterBinding*>> stops;
    for (const auto& candidate : node.parameters)
        if (auto stop = document::parseGradientStopPort(candidate.role);
            stop && stop->role == binding.role)
            stops.emplace_back(std::move(*stop), &candidate);
    std::ranges::sort(stops, {}, [](const auto& entry) {
        return std::pair{entry.first.stop, static_cast<int>(entry.first.component)};
    });
    for (const auto& [stop, link] : stops) {
        const auto* record = findParameter(link->parameterId);
        const auto output =
            record == nullptr
                ? std::nullopt
                : driverOutput(*record, stop.component == document::GradientStopComponent::Color
                                            ? runtime::SocketValueKind::Color
                                            : runtime::SocketValueKind::Scalar);
        if (!output)
            return std::nullopt;
        compiled->drivers.push_back({link->parameterId, stop.stop, stop.component, *output});
    }
    return compiled;
}

[[nodiscard]] std::optional<runtime::CompiledScalarParameter>
compiledScalarParameterFor(const document::ParameterId id) {
    const document::ParameterBinding binding{std::string{}, id};
    return compiledScalarParameter(&binding);
}

// ADR 0028: one compiled mask from its four parameters.
[[nodiscard]] std::optional<runtime::CompiledLayerMask>
compiledLayerMask(const document::LayerMaskId id, const document::LayerMaskMode mode,
                  const bool inverted, const document::ParameterId path,
                  const document::ParameterId feather, const document::ParameterId expansion,
                  const document::ParameterId opacity) {
    auto compiledPath = compiledPathParameter(path);
    auto compiledFeather = compiledScalarParameterFor(feather);
    auto compiledExpansion = compiledScalarParameterFor(expansion);
    auto compiledOpacity = compiledScalarParameterFor(opacity);
    if (!compiledPath || !compiledFeather || !compiledExpansion || !compiledOpacity)
        return std::nullopt;
    return runtime::CompiledLayerMask{id,
                                      mode,
                                      inverted,
                                      std::move(*compiledPath),
                                      *compiledFeather,
                                      *compiledExpansion,
                                      *compiledOpacity};
}

[[nodiscard]] std::optional<runtime::LayerMaskKernel>
lowerLayerMasks(const document::LayerOutputBoundary& boundary) {
    runtime::LayerMaskKernel kernel;
    for (const auto& mask : boundary.masks) {
        auto compiled = compiledLayerMask(mask.id, mask.mode, mask.inverted, mask.path,
                                          mask.feather, mask.expansion, mask.opacity);
        if (!compiled) {
            addTopologyFailure(boundary.nodeId, "Validated layer mask could not be lowered.");
            return std::nullopt;
        }
        kernel.masks.push_back(std::move(*compiled));
    }
    return kernel;
}

// The Mask node: one Add mask over an opaque white canvas-sized input.
[[nodiscard]] std::optional<runtime::LayerMaskKernel>
lowerMaskNode(const document::NodeRecord& node) {
    const auto binding = [&](const std::string_view role) {
        const auto* found = findParameterBinding(node, role);
        return found == nullptr ? document::ParameterId{} : found->parameterId;
    };
    const auto* inverted = parameterConstant<bool>(findParameterBinding(node, "inverted"));
    auto compiled = compiledLayerMask({}, document::LayerMaskMode::Add,
                                      inverted != nullptr && *inverted, binding("path"),
                                      binding("feather"), binding("expansion"), binding("opacity"));
    if (!compiled) {
        addTopologyFailure(node.id, "Mask node parameters could not be lowered.");
        return std::nullopt;
    }
    return runtime::LayerMaskKernel{{std::move(*compiled)}};
}

[[nodiscard]] std::optional<runtime::CompiledScalarParameter>
compiledScalarParameter(const document::ParameterBinding* binding) {
    if (binding == nullptr) {
        return std::nullopt;
    }
    const auto* parameter = findParameter(binding->parameterId);
    if (parameter == nullptr) {
        return std::nullopt;
    }
    if (const auto* constant = std::get_if<document::ConstantValueSource>(&parameter->source)) {
        const auto* value = std::get_if<double>(&constant->value);
        return value == nullptr
                   ? std::nullopt
                   : std::optional(runtime::CompiledScalarParameter{parameter->id, *value});
    }
    // Task S7: the third arm. A driver binding resolves to the value-graph output it names, through
    // a promotion operation when the kinds differ -- so an Integer node can drive a Scalar operand
    // without the widening being invisible.
    if (const auto driven = driverOutput(*parameter, runtime::SocketValueKind::Scalar)) {
        return runtime::CompiledScalarParameter{parameter->id, *driven};
    }
    const auto* source = std::get_if<document::AnimationCurveSource>(&parameter->source);
    const auto index =
        source == nullptr ? scalarCurveIndices_.end() : scalarCurveIndices_.find(source->curveId);
    return index == scalarCurveIndices_.end()
               ? std::nullopt
               : std::optional(runtime::CompiledScalarParameter{parameter->id, index->second});
}

[[nodiscard]] std::optional<runtime::CompiledVec2Parameter>
compiledVec2Parameter(const document::ParameterBinding* binding) {
    if (binding == nullptr) {
        return std::nullopt;
    }
    const auto* parameter = findParameter(binding->parameterId);
    if (parameter == nullptr) {
        return std::nullopt;
    }
    if (const auto* constant = std::get_if<document::ConstantValueSource>(&parameter->source)) {
        const auto* value = std::get_if<document::Vec2d>(&constant->value);
        return value == nullptr
                   ? std::nullopt
                   : std::optional(runtime::CompiledVec2Parameter{parameter->id, *value});
    }
    if (const auto driven = driverOutput(*parameter, runtime::SocketValueKind::Vector2)) {
        return runtime::CompiledVec2Parameter{parameter->id, *driven};
    }
    const auto* source = std::get_if<document::AnimationCurveSource>(&parameter->source);
    const auto index =
        source == nullptr ? vec2CurveIndices_.end() : vec2CurveIndices_.find(source->curveId);
    return index == vec2CurveIndices_.end()
               ? std::nullopt
               : std::optional(runtime::CompiledVec2Parameter{parameter->id, index->second});
}

[[nodiscard]] std::optional<runtime::CompiledColorParameter>
compiledColorParameter(const document::ParameterBinding* binding) {
    if (binding == nullptr) {
        return std::nullopt;
    }
    const auto* parameter = findParameter(binding->parameterId);
    if (parameter == nullptr) {
        return std::nullopt;
    }
    if (const auto* constant = std::get_if<document::ConstantValueSource>(&parameter->source)) {
        const auto* value = std::get_if<core::Color4d>(&constant->value);
        return value == nullptr
                   ? std::nullopt
                   : std::optional(runtime::CompiledColorParameter{parameter->id, *value});
    }
    if (const auto driven = driverOutput(*parameter, runtime::SocketValueKind::Color)) {
        return runtime::CompiledColorParameter{parameter->id, *driven};
    }
    const auto* source = std::get_if<document::AnimationCurveSource>(&parameter->source);
    const auto index =
        source == nullptr ? color4CurveIndices_.end() : color4CurveIndices_.find(source->curveId);
    return index == color4CurveIndices_.end()
               ? std::nullopt
               : std::optional(runtime::CompiledColorParameter{parameter->id, index->second});
}

[[nodiscard]] std::optional<runtime::OperationIndex> findInputOperation(
    const document::NodeId nodeId, const std::string_view port,
    const std::unordered_map<document::NodeId, runtime::OperationIndex>& indices) const {
    const auto* edge = fixedInputEdge(nodeId, port);
    if (edge == nullptr) {
        return std::nullopt;
    }
    return sourceOperation(edge->source, indices);
}

// The operation behind an output port: a per-port operation (#364) or the node's own.
[[nodiscard]] std::optional<runtime::OperationIndex> sourceOperation(
    const document::OutputPortRef& source,
    const std::unordered_map<document::NodeId, runtime::OperationIndex>& indices) const {
    if (const auto port = portOperations_.find({source.nodeId, source.port});
        port != portOperations_.end())
        return port->second;
    const auto found = indices.find(source.nodeId);
    return found == indices.end() ? std::nullopt : std::optional(found->second);
}
