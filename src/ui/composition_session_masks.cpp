#include "layer_mask_support.hpp"

#include <QThread>
#include <algorithm>
#include <bloom/commands/animation_operations.hpp>
#include <bloom/commands/layer_mask_operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/document/layer_masks.hpp>
#include <bloom/document/project.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/runtime/animation_sampling.hpp>
#include <bloom/runtime/curve_compilation.hpp>

namespace bloom::ui {
namespace {

struct MaskOwner final {
    document::LayerId layer;
    const document::LayerMask* mask = nullptr;
};

std::optional<MaskOwner> ownerOfPath(const document::Composition& composition,
                                     const document::ParameterId parameter) {
    for (const auto& layer : composition.graph().layerOutputs())
        for (const auto& mask : layer.masks)
            if (mask.path == parameter)
                return MaskOwner{layer.layerId, &mask};
    return std::nullopt;
}

} // namespace

bool CompositionSession::addLayerMask(const document::LayerId layerId, document::PathValue path,
                                      const document::LayerMaskMode mode) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!composition()) {
        reportUnavailable(tr("No composition is available for the new mask"));
        return false;
    }
    commands::Transaction transaction("Add Mask", snapshot_.revision());
    transaction.emplace<commands::AddLayerMask>(compositionId_, layerId, std::move(path), mode);
    const auto result = runTransaction(std::move(transaction));
    const auto mask = result.outputId<document::LayerMaskId>("mask");
    if (!handleResult(result))
        return false;
    if (mask)
        selectMask(layerId, *mask);
    return true;
}

bool CompositionSession::commitMaskPath(const document::LayerId layerId,
                                        const document::LayerMaskId maskId,
                                        document::PathValue path) {
    Q_ASSERT(QThread::currentThread() == thread());
    const auto* current = composition();
    const auto* layer = current ? current->graph().findLayer(layerId) : nullptr;
    const document::LayerMask* mask = nullptr;
    if (layer != nullptr)
        for (const auto& candidate : layer->masks)
            if (candidate.id == maskId)
                mask = &candidate;
    if (mask == nullptr) {
        reportUnavailable(tr("The mask is no longer available"));
        return false;
    }
    commands::Transaction transaction("Edit Mask Path", snapshot_.revision());
    transaction.emplace<commands::SetLayerMaskPath>(compositionId_, layerId, maskId,
                                                    keyTimeAtPlayhead(mask->path), std::move(path));
    return handleResult(runTransaction(std::move(transaction)));
}

bool CompositionSession::toggleMaskPathKey(const document::ParameterId pathParameter) {
    Q_ASSERT(QThread::currentThread() == thread());
    const auto* current = composition();
    const auto owner = current ? ownerOfPath(*current, pathParameter) : std::nullopt;
    const auto* record = current ? current->parameters().find(pathParameter) : nullptr;
    if (!owner || record == nullptr) {
        reportUnavailable(tr("The mask path is no longer available"));
        return false;
    }
    const auto keyTime = keyTimeAtPlayhead(pathParameter);
    const auto layerId = owner->layer;
    const auto maskId = owner->mask->id;
    if (std::holds_alternative<document::ConstantValueSource>(record->source)) {
        commands::Transaction transaction("Add Mask Path Key", snapshot_.revision());
        transaction.emplace<commands::AnimateLayerMaskPath>(compositionId_, layerId, maskId,
                                                            keyTime);
        return handleResult(runTransaction(std::move(transaction)));
    }
    const auto* source = std::get_if<document::AnimationCurveSource>(&record->source);
    const auto* curve = source ? current->animationCurves().findPath(source->curveId) : nullptr;
    if (curve == nullptr) {
        reportUnavailable(tr("The mask path cannot be keyed"));
        return false;
    }
    const auto existing =
        std::ranges::find(curve->keyframes, keyTime, &document::PathKeyframe::time);
    if (existing == curve->keyframes.end()) {
        auto sampled = masks::maskPathAt(*current, *owner->mask, keyTime);
        if (!sampled) {
            reportUnavailable(tr("The mask path could not be sampled here"));
            return false;
        }
        commands::Transaction transaction("Add Mask Path Key", snapshot_.revision());
        transaction.emplace<commands::SetLayerMaskPath>(compositionId_, layerId, maskId, keyTime,
                                                        std::move(*sampled));
        return handleResult(runTransaction(std::move(transaction)));
    }
    commands::Transaction transaction("Remove Mask Path Key", snapshot_.revision());
    if (curve->keyframes.size() > 1)
        transaction.emplace<commands::DeleteKeyframe>(compositionId_, source->curveId,
                                                      existing->id);
    else
        transaction.emplace<commands::StopAnimatingLayerMaskPath>(compositionId_, layerId, maskId,
                                                                  existing->value);
    return handleResult(runTransaction(std::move(transaction)));
}

bool CompositionSession::setTrackMatte(const document::LayerId target,
                                       std::optional<document::TrackMatte> matte) {
    Q_ASSERT(QThread::currentThread() == thread());
    commands::Transaction transaction(matte ? "Set Track Matte" : "Clear Track Matte",
                                      snapshot_.revision());
    transaction.emplace<commands::SetTrackMatte>(compositionId_, target, matte);
    return handleResult(runTransaction(std::move(transaction)));
}

// #493: the keyframe gesture for any other Path parameter (a Curves curve), on the parameter's own
// clock: constant -> one key, no key here -> a key at the sampled curve, key here -> delete it (the
// last key becomes the constant it held). One transaction each.
bool CompositionSession::togglePathParameterKey(const document::ParameterRecord& parameter) {
    Q_ASSERT(QThread::currentThread() == thread());
    const auto* current = composition();
    if (current == nullptr)
        return false;
    const auto keyTime = keyTimeAtPlayhead(parameter.id);
    if (std::holds_alternative<document::ConstantValueSource>(parameter.source)) {
        commands::Transaction transaction("Add Curve Key", snapshot_.revision());
        transaction.emplace<commands::CreateAnimationForParameter>(compositionId_, parameter.id,
                                                                   keyTime);
        return handleResult(runTransaction(std::move(transaction)));
    }
    const auto* source = std::get_if<document::AnimationCurveSource>(&parameter.source);
    // #541: a gradient keys exactly as a Curves curve does, one whole-value key at the playhead.
    if (const auto* gradient =
            source ? current->animationCurves().findGradient(source->curveId) : nullptr) {
        const auto existing =
            std::ranges::find(gradient->keyframes, keyTime, &document::GradientKeyframe::time);
        if (existing == gradient->keyframes.end()) {
            auto sampled = runtime::sampleGradientCurve(runtime::compileAnimationCurve(*gradient),
                                                        keyTime);
            if (!sampled) {
                reportUnavailable(tr("The gradient could not be sampled here"));
                return false;
            }
            commands::Transaction transaction("Add Gradient Key", snapshot_.revision());
            transaction.emplace<commands::SetKeyframeAtTime>(compositionId_, source->curveId,
                                                             keyTime, std::move(*sampled));
            return handleResult(runTransaction(std::move(transaction)));
        }
        commands::Transaction transaction("Remove Gradient Key", snapshot_.revision());
        if (gradient->keyframes.size() > 1)
            transaction.emplace<commands::DeleteKeyframe>(compositionId_, source->curveId,
                                                          existing->id);
        else
            transaction.emplace<commands::ConvertAnimationToConstant>(compositionId_, parameter.id,
                                                                      existing->value);
        return handleResult(runTransaction(std::move(transaction)));
    }
    const auto* curve = source ? current->animationCurves().findPath(source->curveId) : nullptr;
    if (curve == nullptr) {
        reportUnavailable(tr("Disconnect the driven parameter before keying its value"));
        return false;
    }
    const auto existing =
        std::ranges::find(curve->keyframes, keyTime, &document::PathKeyframe::time);
    if (existing == curve->keyframes.end()) {
        auto sampled = masks::pathParameterAt(*current, parameter.id, keyTime);
        if (!sampled) {
            reportUnavailable(tr("The curve could not be sampled here"));
            return false;
        }
        commands::Transaction transaction("Add Curve Key", snapshot_.revision());
        transaction.emplace<commands::SetKeyframeAtTime>(compositionId_, source->curveId, keyTime,
                                                         std::move(*sampled));
        return handleResult(runTransaction(std::move(transaction)));
    }
    commands::Transaction transaction("Remove Curve Key", snapshot_.revision());
    if (curve->keyframes.size() > 1)
        transaction.emplace<commands::DeleteKeyframe>(compositionId_, source->curveId,
                                                      existing->id);
    else
        transaction.emplace<commands::ConvertAnimationToConstant>(compositionId_, parameter.id,
                                                                  existing->value);
    return handleResult(runTransaction(std::move(transaction)));
}

} // namespace bloom::ui
