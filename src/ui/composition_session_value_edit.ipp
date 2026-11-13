// Included inside bloom::ui by composition_session.cpp, like the transform interaction seam.
namespace {

double* editedComponent(document::ParameterValue& value,
                        const document::AnimationComponent component) {
    return std::visit(
        [component](auto& held) -> double* {
            using Value = std::decay_t<decltype(held)>;
            if constexpr (std::is_same_v<Value, document::Vec2d> ||
                          std::is_same_v<Value, document::Vec3d>) {
                if (component == document::AnimationComponent::X)
                    return &held.x;
                if (component == document::AnimationComponent::Y)
                    return &held.y;
                if constexpr (std::is_same_v<Value, document::Vec3d>)
                    if (component == document::AnimationComponent::Z)
                        return &held.z;
            } else if constexpr (std::is_same_v<Value, core::Color4d>) {
                switch (component) {
                case document::AnimationComponent::Red:
                    return &held.red;
                case document::AnimationComponent::Green:
                    return &held.green;
                case document::AnimationComponent::Blue:
                    return &held.blue;
                case document::AnimationComponent::Alpha:
                    return &held.alpha;
                default:
                    break;
                }
            }
            return nullptr;
        },
        value);
}

} // namespace

bool CompositionSession::valueEditActive() const noexcept {
    return valueEdit_.has_value() || keyframePreview_.has_value();
}
bool CompositionSession::isValueEditing(const document::ParameterId parameter) const noexcept {
    return valueEdit_ && valueEdit_->parameter == parameter;
}

std::optional<document::ParameterValue>
CompositionSession::liveValue(const document::ParameterId parameterId) const {
    const auto* current = composition();
    const auto* parameter = current ? current->parameters().find(parameterId) : nullptr;
    if (transformInteraction_) {
        const auto& overrides = transformInteraction_->overrides;
        const auto found = std::ranges::find(overrides, parameterId,
                                             &runtime::SnapshotParameterOverride::parameterId);
        if (found != overrides.end())
            return std::visit([](const auto& value) -> document::ParameterValue { return value; },
                              found->value);
    }
    if (valueEdit_ && valueEdit_->anchor && valueEdit_->anchor->parameter == parameterId)
        return valueEdit_->anchor->value;
    if (valueEdit_ && isValueEditing(parameterId))
        return valueEdit_->value;
    if (valueEdit_)
        for (const auto& linked : valueEdit_->linked)
            if (linked.parameter == parameterId)
                return linked.value;
    if (parameter) {
        const auto* constant = std::get_if<document::ConstantValueSource>(&parameter->source);
        if (constant && (std::holds_alternative<document::PathValue>(constant->value) ||
                         std::holds_alternative<document::GradientValue>(constant->value) ||
                         std::holds_alternative<document::NumberListValue>(constant->value)))
            return constant->value;
        // #493: an animated Path (a Curves curve) at the playhead on its own clock.
        if (const auto* animated = std::get_if<document::AnimationCurveSource>(&parameter->source);
            animated && current->animationCurves().findPath(animated->curveId))
            if (auto path = masks::pathParameterAt(*current, parameterId,
                                                   keyTimeAtPlayhead(parameterId)))
                return document::ParameterValue{std::move(*path)};
        // #541: an animated Gradient (a Colorize Gradient Map) at the playhead.
        if (const auto* animated = std::get_if<document::AnimationCurveSource>(&parameter->source))
            if (const auto* curve = current->animationCurves().findGradient(animated->curveId);
                curve && !curve->keyframes.empty())
                if (auto gradient = runtime::sampleGradientCurve(
                        runtime::compileAnimationCurve(*curve), keyTimeAtPlayhead(parameterId)))
                    return document::ParameterValue{std::move(*gradient)};
    }
    if (const auto sample = effectiveParameterValue(parameter))
        return std::visit([](const auto& value) -> document::ParameterValue { return value; },
                          *sample);
    return std::nullopt;
}

bool CompositionSession::beginValueEdit(
    const document::ParameterId parameterId,
    const std::optional<document::AnimationComponent> component) {
    Q_ASSERT(QThread::currentThread() == thread());
    cancelValueEdit();
    cancelTransformInteraction();
    const auto* current = composition();
    const auto* parameter = current ? current->parameters().find(parameterId) : nullptr;
    auto base = liveValue(parameterId);
    if (!parameter || !base || current->parameterLocked(parameterId) ||
        std::holds_alternative<document::DriverBindingSource>(parameter->source) ||
        (component && !editedComponent(*base, *component))) {
        reportUnavailable(tr("This parameter is not available for editing"));
        return false;
    }
    valueEdit_.emplace(
        ValueEdit{snapshot_.revision(), parameterId, currentTime_, component, *base, *base});
    return true;
}

bool CompositionSession::updateValueEdit(document::ParameterValue value) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!valueEdit_)
        return false;
    auto& edit = *valueEdit_;
    if (edit.revision != snapshot_.revision() || edit.time != currentTime_) {
        cancelValueEdit();
        return false;
    }
    if (edit.component) {
        auto next = edit.base;
        auto* target = editedComponent(next, *edit.component);
        const auto* supplied = std::get_if<double>(&value);
        if (!supplied)
            supplied = editedComponent(value, *edit.component);
        if (!target || !supplied)
            return false;
        *target = *supplied;
        value = std::move(next);
    }
    const auto* parameter = composition()->parameters().find(edit.parameter);
    document::ParameterStore validation;
    if (value.index() != edit.base.index() || !parameter ||
        !validation.insert(
            {edit.parameter, parameter->schemaKey, document::ConstantValueSource{value}}) ||
        !validation.validate().ok()) {
        reportUnavailable(tr("The value is outside this parameter's domain"));
        return false;
    }
    if (edit.value != value) {
        edit.value = std::move(value);
        emit liveValueChanged();
    }
    return true;
}

bool CompositionSession::beginValueEdits(const std::vector<document::ParameterId>& parameters,
                                         const QString& commandLabel) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (parameters.empty() || !beginValueEdit(parameters.front()))
        return false;
    const auto* current = composition();
    for (std::size_t index = 1; index < parameters.size(); ++index) {
        const auto id = parameters[index];
        const auto* parameter = current->parameters().find(id);
        auto base = liveValue(id);
        const bool repeated =
            id == parameters.front() ||
            std::ranges::find(valueEdit_->linked, id, &ValueEdit::LinkedEdit::parameter) !=
                valueEdit_->linked.end();
        if (!parameter || !base || repeated || current->parameterLocked(id) ||
            std::holds_alternative<document::DriverBindingSource>(parameter->source)) {
            valueEdit_.reset();
            reportUnavailable(tr("This parameter is not available for editing"));
            return false;
        }
        valueEdit_->linked.push_back({id, *base, *base});
    }
    valueEdit_->label = commandLabel;
    return true;
}

bool CompositionSession::updateValueEdits(const std::vector<document::ParameterValue>& values) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!valueEdit_ || valueEdit_->component || values.size() != valueEdit_->linked.size() + 1)
        return false;
    // Every value is checked before any is taken, so a refused step changes nothing.
    for (std::size_t index = 1; index < values.size(); ++index) {
        const auto& linked = valueEdit_->linked[index - 1];
        const auto* parameter = composition()->parameters().find(linked.parameter);
        document::ParameterStore validation;
        if (values[index].index() != linked.base.index() || !parameter ||
            !validation.insert({linked.parameter, parameter->schemaKey,
                                document::ConstantValueSource{values[index]}}) ||
            !validation.validate().ok()) {
            reportUnavailable(tr("The value is outside this parameter's domain"));
            return false;
        }
    }
    if (!updateValueEdit(values.front()) || !valueEdit_)
        return false;
    bool changed = false;
    for (std::size_t index = 1; index < values.size(); ++index) {
        auto& linked = valueEdit_->linked[index - 1];
        if (linked.value != values[index]) {
            linked.value = values[index];
            changed = true;
        }
    }
    if (changed)
        emit liveValueChanged();
    return true;
}

void CompositionSession::holdKeysDuringValueEdits(const document::ParameterId linked,
                                                  std::vector<core::RationalTime> times) {
    if (!valueEdit_)
        return;
    for (auto& edit : valueEdit_->linked)
        if (edit.parameter == linked)
            edit.holdKeys = std::move(times);
}

bool CompositionSession::setParameterValues(
    std::vector<std::pair<document::ParameterId, document::ParameterValue>> values,
    const QString& commandLabel) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (values.empty())
        return false;
    commands::Transaction transaction(commandLabel.toStdString(), snapshot_.revision());
    for (auto& [parameter, value] : values)
        if (!appendParameterEdit(transaction, parameter, currentTime_, std::move(value)))
            return false;
    return executeTransaction(std::move(transaction)).succeeded();
}

bool CompositionSession::beginTextEdit(const document::ParameterId parameter) {
    const auto* current = composition();
    const auto* record = current ? current->parameters().find(parameter) : nullptr;
    if (!record || record->schemaKey != document::kTextParameterSchemaKey)
        return false;
    if (valueEdit_ && isValueEditing(parameter)) {
        valueEdit_->text = true;
        return true;
    }
    if (!beginValueEdit(parameter) || !valueEdit_)
        return false;
    valueEdit_->text = true;
    return true;
}

bool CompositionSession::beginPathEdit(const document::ParameterId parameter,
                                       const bool compensateAnchor) {
    if (!beginValueEdit(parameter) || !valueEdit_)
        return false;
    // A layer that pivots on the frame has no content-centre anchor to keep still.
    if (!compensateAnchor) {
        if (!std::holds_alternative<document::PathValue>(valueEdit_->base)) {
            cancelValueEdit();
            return false;
        }
        return true;
    }
    const auto* anchor = parameterForSelection(document::kAnchorParameterRole);
    const auto base = anchor ? liveValue(anchor->id) : std::nullopt;
    if (!anchor || !base || !std::holds_alternative<document::Vec2d>(*base) ||
        composition()->parameterLocked(anchor->id) ||
        std::holds_alternative<document::DriverBindingSource>(anchor->source) ||
        !std::holds_alternative<document::PathValue>(valueEdit_->base)) {
        cancelValueEdit();
        return false;
    }
    const auto point = std::get<document::Vec2d>(*base);
    valueEdit_->anchor = ValueEdit::PathAnchorEdit{anchor->id, point, point};
    return true;
}

bool CompositionSession::beginMaskPathEdit(const document::ParameterId parameterId,
                                           document::PathValue base) {
    Q_ASSERT(QThread::currentThread() == thread());
    cancelValueEdit();
    cancelTransformInteraction();
    const auto* current = composition();
    const auto* parameter = current ? current->parameters().find(parameterId) : nullptr;
    if (!parameter || current->parameterLocked(parameterId) || !base.isValid() ||
        std::holds_alternative<document::DriverBindingSource>(parameter->source)) {
        reportUnavailable(tr("This mask path is not available for editing"));
        return false;
    }
    document::ParameterValue value = std::move(base);
    valueEdit_.emplace(
        ValueEdit{snapshot_.revision(), parameterId, currentTime_, std::nullopt, value, value});
    return true;
}

bool CompositionSession::updatePathEdit(document::PathValue path,
                                        const document::Vec2d centreDelta) {
    if (valueEdit_ && !valueEdit_->anchor)
        return path.isValid() && updateValueEdit(std::move(path));
    if (!valueEdit_ || !valueEdit_->anchor || !std::isfinite(centreDelta.x) ||
        !std::isfinite(centreDelta.y))
        return false;
    auto& anchor = *valueEdit_->anchor;
    const document::Vec2d value{anchor.base.x - centreDelta.x, anchor.base.y - centreDelta.y};
    if (!std::isfinite(value.x) || !std::isfinite(value.y) || !path.isValid())
        return false;
    anchor.value = value;
    return updateValueEdit(std::move(path));
}

std::vector<runtime::SnapshotParameterOverride> CompositionSession::valueEditOverrides() const {
    if (keyframePreview_ && !valueEdit_)
        return keyframePreview_->overrides;
    if (!valueEdit_)
        return {};
    auto result = std::visit(
        [this](const auto& value) -> std::vector<runtime::SnapshotParameterOverride> {
            // Only the kinds the override variant carries can be previewed live.
            using Override = decltype(runtime::SnapshotParameterOverride::value);
            if constexpr (std::is_constructible_v<Override, const std::decay_t<decltype(value)>&>)
                return {{valueEdit_->revision, valueEdit_->parameter, value}};
            else
                return {};
        },
        valueEdit_->value);
    if (valueEdit_->anchor)
        result.push_back(
            {valueEdit_->revision, valueEdit_->anchor->parameter, valueEdit_->anchor->value});
    for (const auto& linked : valueEdit_->linked)
        std::visit(
            [&](const auto& value) {
                using Override = decltype(runtime::SnapshotParameterOverride::value);
                if constexpr (std::is_constructible_v<Override,
                                                      const std::decay_t<decltype(value)>&>)
                    result.push_back({valueEdit_->revision, linked.parameter, value});
            },
            linked.value);
    return result;
}

void CompositionSession::cancelValueEdit() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (valueEdit_) {
        valueEdit_.reset();
        // #502: a cancelled gesture on a plain entry leaves no layer behind it.
        withdrawPendingEntryLayer();
        emit liveValueChanged();
    }
}

bool CompositionSession::commitValueEdit() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!valueEdit_)
        return false;
    auto edit = std::move(*valueEdit_);
    valueEdit_.reset();
    bool result = edit.revision == snapshot_.revision() && edit.time == currentTime_;
    // #494: a gesture over several parameters commits when any of them changed.
    const bool linkedChanged = std::ranges::any_of(
        edit.linked, [](const auto& linked) { return linked.value != linked.base; });
    if (result && (edit.value != edit.base || linkedChanged)) {
        const auto* parameter = composition()->parameters().find(edit.parameter);
        const auto label = !edit.label.isEmpty() ? edit.label
                           : edit.text           ? QStringLiteral("Edit Text")
                           : parameter
                               ? componentCommandLabel(*parameter, QStringLiteral("Set Parameter"))
                               : QStringLiteral("Set Parameter");
        commands::Transaction transaction(label.toStdString(), edit.revision);
        if (edit.value == edit.base) {
            // Only the linked parameters moved.
        } else if (edit.component && parameter &&
                   std::holds_alternative<document::AnimationCurveSource>(parameter->source)) {
            transaction.emplace<commands::SetKeyframeAtTimeForParameterComponent>(
                compositionId_, edit.parameter, *edit.component,
                keyTimeOf(edit.parameter, edit.time),
                *editedComponent(edit.value, *edit.component));
        } else {
            result = appendParameterEdit(transaction, edit.parameter, edit.time, edit.value);
        }
        if (result && edit.anchor && edit.anchor->base != edit.anchor->value)
            result = appendParameterEdit(transaction, edit.anchor->parameter, edit.time,
                                         edit.anchor->value);
        for (const auto& linked : edit.linked) {
            if (!result || linked.value == linked.base)
                continue;
            const auto* held = composition()->parameters().find(linked.parameter);
            const auto* base = std::get_if<document::Vec2d>(&linked.base);
            const auto* value = std::get_if<document::Vec2d>(&linked.value);
            if (linked.holdKeys.empty() || held == nullptr || base == nullptr || value == nullptr) {
                result = appendParameterEdit(transaction, linked.parameter, edit.time, linked.value);
                continue;
            }
            // Pin the held key times to what the parameter is now, then key the playhead.
            const auto at = keyTimeOf(linked.parameter, edit.time);
            const auto* source = std::get_if<document::AnimationCurveSource>(&held->source);
            const auto* curve = source ? composition()->animationCurves().findVec2(source->curveId)
                                       : nullptr;
            for (const auto time : linked.holdKeys) {
                if (time == at)
                    continue;
                if (curve != nullptr &&
                    std::ranges::any_of(curve->components, [&](const auto& component) {
                        return std::ranges::any_of(component.keyframes, [&](const auto& key) {
                            return key.time == time;
                        });
                    }))
                    continue;
                document::Vec2d pinned = *base;
                if (source != nullptr)
                    if (const auto sampled = sampleParameterValue(*held, time))
                        if (const auto* vec = std::get_if<document::Vec2d>(&*sampled))
                            pinned = *vec;
                transaction.emplace<commands::SetKeyframeAtTimeForParameter>(
                    compositionId_, linked.parameter, time, pinned);
            }
            transaction.emplace<commands::SetKeyframeAtTimeForParameter>(
                compositionId_, linked.parameter, at, *value);
        }
        // #543: the gradient's stop links follow their stops in the same step.
        if (result && edit.stopLinks && edit.value != edit.base)
            transaction.emplace<commands::RemapGradientStopLinks>(
                compositionId_, edit.stopLinks->node, edit.stopLinks->role,
                edit.stopLinks->newIndexOfOld);
        if (result)
            result = executeTransaction(std::move(transaction)).succeeded();
    }
    // #502: a gesture on a plain entry that committed nothing leaves no layer behind it.
    withdrawPendingEntryLayer();
    emit liveValueChanged();
    return result;
}
