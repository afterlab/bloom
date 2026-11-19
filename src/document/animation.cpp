#include <bloom/document/animation.hpp>
#include <bloom/document/grade_curves.hpp>
#include <bloom/document/keyframe_handles.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace {

using bloom::document::AnimationComponent;
using bloom::document::AnimationCurveId;
using bloom::document::AnimationCurveRecord;
using bloom::document::Color4AnimationCurve;
using bloom::document::ComponentAnimationCurve;
using bloom::document::KeyframeId;
using bloom::document::KeyframeInterpolation;
using bloom::document::PathAnimationCurve;
using bloom::document::PathKeyframe;
using bloom::document::GradientAnimationCurve;
using bloom::document::GradientKeyframe;
using bloom::document::ScalarAnimationCurve;
using bloom::document::ScalarKeyframe;
using bloom::document::ValidationCode;
using bloom::document::ValidationResult;
using bloom::document::Vec2AnimationCurve;
using bloom::document::Vec3AnimationCurve;

[[nodiscard]] bool validInterpolation(const KeyframeInterpolation interpolation) noexcept {
    return bloom::document::isValidKeyframeInterpolation(interpolation);
}

// "Value is representable". Every stored key is a ScalarKeyframe -- a scalar curve's own or one
// component of a vector or colour parameter -- so there is one finiteness rule, not four.
[[nodiscard]] bool finiteValue(const ScalarKeyframe& keyframe) noexcept {
    return std::isfinite(keyframe.value);
}

// A Path key's value is representable when the path itself is.
[[nodiscard]] bool finiteValue(const PathKeyframe& keyframe) noexcept {
    return keyframe.value.isValid();
}

// #541: likewise a Gradient key's.
[[nodiscard]] bool finiteValue(const GradientKeyframe& keyframe) noexcept {
    return keyframe.value.isValid();
}

template <typename Keyframe> [[nodiscard]] bool validHandles(const Keyframe& keyframe) noexcept {
    return bloom::document::isValidKeyframeHandle(keyframe.outgoingHandle) &&
           bloom::document::isValidKeyframeHandle(keyframe.incomingHandle);
}

template <typename Keyframe> [[nodiscard]] bool validEasing(const Keyframe& keyframe) noexcept {
    return bloom::document::isValidKeyframeEasing(keyframe.easing) &&
           bloom::document::isValidKeyframeHandleMode(keyframe.handleMode);
}

template <typename Curve> void normalizeFinalInterpolation(Curve& curve) noexcept {
    if (!curve.keyframes.empty()) {
        curve.keyframes.back().outgoingInterpolation = KeyframeInterpolation::Linear;
    }
}

// Calls `visit` with the id of every keyframe of `record`, every component's keys included.
template <typename Visit>
void forEachKeyframeId(const AnimationCurveRecord& record, Visit&& visit) {
    std::visit(
        [&](const auto& curve) {
            using Curve = std::decay_t<decltype(curve)>;
            if constexpr (bloom::document::kWholeValueAnimationCurve<Curve>) {
                for (const auto& keyframe : curve.keyframes) {
                    visit(keyframe.id);
                }
            } else {
                for (const auto& component : curve.components) {
                    for (const auto& keyframe : component.keyframes) {
                        visit(keyframe.id);
                    }
                }
            }
        },
        record);
}

template <typename Curve> [[nodiscard]] bool curveCanEnterStore(const Curve& curve) {
    if (!curve.id.isValid() || curve.keyframes.empty()) {
        return false;
    }

    std::unordered_set<KeyframeId> ids;
    for (std::size_t index = 0; index < curve.keyframes.size(); ++index) {
        const auto& keyframe = curve.keyframes[index];
        if (!keyframe.id.isValid() || !ids.insert(keyframe.id).second || !finiteValue(keyframe) ||
            !validHandles(keyframe) || !validEasing(keyframe) ||
            !validInterpolation(keyframe.outgoingInterpolation)) {
            return false;
        }
        if (index > 0 && !(curve.keyframes[index - 1].time < keyframe.time)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool componentCurveCanEnterStore(const ComponentAnimationCurve& curve) {
    for (std::size_t index = 0; index < curve.keyframes.size(); ++index) {
        const auto& keyframe = curve.keyframes[index];
        if (!keyframe.id.isValid() || !finiteValue(keyframe) || !validHandles(keyframe) ||
            !validEasing(keyframe) || !validInterpolation(keyframe.outgoingInterpolation) ||
            (index > 0 && !(curve.keyframes[index - 1].time < keyframe.time))) {
            return false;
        }
    }
    return true;
}

template <std::size_t Count>
[[nodiscard]] bool
componentCurveRecordCanEnterStore(const AnimationCurveId id,
                                  const std::array<ComponentAnimationCurve, Count>& components) {
    if (!id.isValid()) {
        return false;
    }
    bool hasKey = false;
    for (const auto& component : components) {
        if (!component.keyframes.empty()) {
            hasKey = true;
        }
        if (!componentCurveCanEnterStore(component)) {
            return false;
        }
    }
    return hasKey;
}

// The record's keyframe ids, sorted (duplicates kept, so a caller can refuse them).
[[nodiscard]] std::vector<KeyframeId> sortedKeyframeIds(const AnimationCurveRecord& record) {
    std::vector<KeyframeId> ids;
    forEachKeyframeId(record, [&](const KeyframeId id) { ids.push_back(id); });
    std::ranges::sort(ids);
    return ids;
}

[[nodiscard]] bool canEnterStore(const AnimationCurveRecord& record) {
    return std::visit(
        [](const auto& curve) {
            using Curve = std::decay_t<decltype(curve)>;
            if constexpr (bloom::document::kWholeValueAnimationCurve<Curve>) {
                return curveCanEnterStore(curve);
            } else {
                return componentCurveRecordCanEnterStore(curve.id, curve.components);
            }
        },
        record);
}

void normalizeRecordFinalInterpolation(AnimationCurveRecord& record) {
    std::visit(
        [](auto& curve) {
            using Curve = std::decay_t<decltype(curve)>;
            if constexpr (bloom::document::kWholeValueAnimationCurve<Curve>) {
                normalizeFinalInterpolation(curve);
            } else {
                for (auto& component : curve.components) {
                    normalizeFinalInterpolation(component);
                }
            }
        },
        record);
}

template <typename Curve> void validateCurve(const Curve& curve, ValidationResult& result) {
    const auto curvePath = "[" + std::to_string(curve.id.value()) + "]";
    if (!curve.id.isValid()) {
        result.add(ValidationCode::InvalidId, curvePath + ".id",
                   "Animation curve ID must not be zero");
    }
    if (curve.keyframes.empty()) {
        result.add(ValidationCode::InvalidValue, curvePath + ".keyframes",
                   "Animation curve must contain at least one keyframe");
        return;
    }

    for (std::size_t index = 0; index < curve.keyframes.size(); ++index) {
        const auto& keyframe = curve.keyframes[index];
        const auto keyframePath =
            curvePath + ".keyframes[" + std::to_string(keyframe.id.value()) + "]";
        if (!keyframe.id.isValid()) {
            result.add(ValidationCode::InvalidId, keyframePath + ".id",
                       "Keyframe ID must not be zero");
        }
        if (!finiteValue(keyframe)) {
            result.add(ValidationCode::InvalidValue, keyframePath + ".value",
                       "Keyframe value must be finite");
        }
        if (!validHandles(keyframe)) {
            result.add(ValidationCode::InvalidValue, keyframePath + ".handle",
                       "Keyframe handle time must be within [0, 1] and its value offset finite");
        }
        if (!validEasing(keyframe)) {
            result.add(ValidationCode::InvalidValue, keyframePath + ".easing",
                       "Keyframe easing parameters must be finite with a non-negative amplitude");
        }
        if (!validInterpolation(keyframe.outgoingInterpolation)) {
            result.add(ValidationCode::InvalidInterpolation,
                       keyframePath + ".outgoingInterpolation",
                       "Keyframe interpolation mode is unsupported");
        }
        if (index > 0) {
            const auto& previous = curve.keyframes[index - 1];
            if (previous.time == keyframe.time) {
                result.add(ValidationCode::DuplicateTime, keyframePath + ".time",
                           "Animation curve contains duplicate exact keyframe times");
            } else if (keyframe.time < previous.time) {
                result.add(ValidationCode::InvalidOrder, keyframePath + ".time",
                           "Animation keyframes must be ordered by increasing exact time");
            }
        }
    }
    if (curve.keyframes.back().outgoingInterpolation != KeyframeInterpolation::Linear) {
        const auto& finalKey = curve.keyframes.back();
        result.add(ValidationCode::InvalidInterpolation,
                   curvePath + ".keyframes[" + std::to_string(finalKey.id.value()) +
                       "].outgoingInterpolation",
                   "The final keyframe interpolation must be canonical Linear");
    }
}

void validateComponentCurve(const ComponentAnimationCurve& curve, const std::string& path,
                            ValidationResult& result) {
    for (std::size_t index = 0; index < curve.keyframes.size(); ++index) {
        const auto& keyframe = curve.keyframes[index];
        const auto keyframePath = path + ".keyframes[" + std::to_string(keyframe.id.value()) + "]";
        if (!keyframe.id.isValid()) {
            result.add(ValidationCode::InvalidId, keyframePath + ".id",
                       "Keyframe ID must not be zero");
        }
        if (!finiteValue(keyframe)) {
            result.add(ValidationCode::InvalidValue, keyframePath + ".value",
                       "Keyframe value must be finite");
        }
        if (!validHandles(keyframe)) {
            result.add(ValidationCode::InvalidValue, keyframePath + ".handle",
                       "Keyframe handle time must be within [0, 1] and its value offset finite");
        }
        if (!validEasing(keyframe)) {
            result.add(ValidationCode::InvalidValue, keyframePath + ".easing",
                       "Keyframe easing parameters must be finite with a non-negative amplitude");
        }
        if (!validInterpolation(keyframe.outgoingInterpolation)) {
            result.add(ValidationCode::InvalidInterpolation,
                       keyframePath + ".outgoingInterpolation",
                       "Keyframe interpolation mode is unsupported");
        }
        if (index > 0) {
            const auto& previous = curve.keyframes[index - 1];
            if (previous.time == keyframe.time) {
                result.add(ValidationCode::DuplicateTime, keyframePath + ".time",
                           "Animation curve contains duplicate exact keyframe times");
            } else if (keyframe.time < previous.time) {
                result.add(ValidationCode::InvalidOrder, keyframePath + ".time",
                           "Animation keyframes must be ordered by increasing exact time");
            }
        }
    }
    if (!curve.keyframes.empty() &&
        curve.keyframes.back().outgoingInterpolation != KeyframeInterpolation::Linear) {
        const auto& finalKey = curve.keyframes.back();
        result.add(ValidationCode::InvalidInterpolation,
                   path + ".keyframes[" + std::to_string(finalKey.id.value()) +
                       "].outgoingInterpolation",
                   "The final keyframe interpolation must be canonical Linear");
    }
}

template <typename Curve, typename Keyframe>
[[nodiscard]] bool insertKeyframe(Curve& curve, Keyframe keyframe, const bool idAlreadyExists) {
    if (!keyframe.id.isValid() || idAlreadyExists || !finiteValue(keyframe) ||
        !validHandles(keyframe) || !validInterpolation(keyframe.outgoingInterpolation)) {
        return false;
    }
    const auto position = std::lower_bound(
        curve.keyframes.begin(), curve.keyframes.end(), keyframe.time,
        [](const auto& candidate, const auto time) { return candidate.time < time; });
    if (position != curve.keyframes.end() && position->time == keyframe.time) {
        return false;
    }
    // A sequence with Aligned or Auto keys keeps its handles consistent.
    const bool settles = keyframe.handleMode != bloom::document::KeyframeHandleMode::Free ||
                         bloom::document::hasNonFreeHandleMode(curve.keyframes);
    auto before = settles ? curve.keyframes : decltype(curve.keyframes){};
    curve.keyframes.insert(position, std::move(keyframe));
    normalizeFinalInterpolation(curve);
    if (settles)
        bloom::document::settleKeyframeHandles(curve.keyframes, before);
    return true;
}

template <typename Curve, typename Keyframe>
[[nodiscard]] bool updateKeyframe(Curve& curve, Keyframe keyframe) {
    if (!keyframe.id.isValid() || !finiteValue(keyframe) || !validHandles(keyframe) ||
        !validInterpolation(keyframe.outgoingInterpolation)) {
        return false;
    }
    const auto current = std::ranges::find(curve.keyframes, keyframe.id, &Keyframe::id);
    if (current == curve.keyframes.end()) {
        return false;
    }
    const auto occupied = std::ranges::find(curve.keyframes, keyframe.time, &Keyframe::time);
    if (occupied != curve.keyframes.end() && occupied->id != keyframe.id) {
        return false;
    }

    const bool settles = keyframe.handleMode != bloom::document::KeyframeHandleMode::Free ||
                         bloom::document::hasNonFreeHandleMode(curve.keyframes);
    auto before = settles ? curve.keyframes : decltype(curve.keyframes){};
    *current = std::move(keyframe);
    std::ranges::sort(curve.keyframes, {}, &Keyframe::time);
    normalizeFinalInterpolation(curve);
    if (settles)
        bloom::document::settleKeyframeHandles(curve.keyframes, before);
    return true;
}

template <typename Curve>
[[nodiscard]] bool eraseCurveKeyframe(Curve& curve, const KeyframeId keyframeId) {
    if (curve.keyframes.size() <= 1) {
        return false;
    }
    const auto keyframe =
        std::ranges::find_if(curve.keyframes, [keyframeId](const auto& candidate) {
            return candidate.id == keyframeId;
        });
    if (keyframe == curve.keyframes.end()) {
        return false;
    }
    const bool settles = bloom::document::hasNonFreeHandleMode(curve.keyframes);
    auto before = settles ? curve.keyframes : decltype(curve.keyframes){};
    curve.keyframes.erase(keyframe);
    normalizeFinalInterpolation(curve);
    if (settles)
        bloom::document::settleKeyframeHandles(curve.keyframes, before);
    return true;
}

[[nodiscard]] bool isScalar(const AnimationCurveRecord& record) noexcept {
    return std::holds_alternative<ScalarAnimationCurve>(record);
}

[[nodiscard]] bool isVec2(const AnimationCurveRecord& record) noexcept {
    return std::holds_alternative<Vec2AnimationCurve>(record);
}

[[nodiscard]] bool isVec3(const AnimationCurveRecord& record) noexcept {
    return std::holds_alternative<Vec3AnimationCurve>(record);
}

[[nodiscard]] bool isColor4(const AnimationCurveRecord& record) noexcept {
    return std::holds_alternative<Color4AnimationCurve>(record);
}

[[nodiscard]] bool isPath(const AnimationCurveRecord& record) noexcept {
    return std::holds_alternative<PathAnimationCurve>(record);
}

[[nodiscard]] bool isGradient(const AnimationCurveRecord& record) noexcept {
    return std::holds_alternative<GradientAnimationCurve>(record);
}

template <typename Curve>
inline constexpr bool kWholeValueCurve = bloom::document::kWholeValueAnimationCurve<Curve>;

} // namespace

namespace bloom::document {

bool isValidKeyframeHandle(const KeyframeHandle& handle) noexcept {
    return std::isfinite(handle.value) && handle.time >= 0.0 && handle.time <= 1.0;
}

namespace {
constexpr std::array<std::string_view, 13> kInterpolationTokens{
    "hold",  "linear", "ease-in-out", "sine", "quad",   "cubic",  "quart",
    "quint", "expo",   "circ",        "back", "bounce", "elastic"};
constexpr std::array<std::string_view, 4> kEasingTokens{"auto", "in", "out", "in-out"};
} // namespace

std::string_view keyframeInterpolationToken(const KeyframeInterpolation mode) noexcept {
    const auto index = static_cast<std::size_t>(mode);
    return index < kInterpolationTokens.size() ? kInterpolationTokens[index] : std::string_view{};
}

std::optional<KeyframeInterpolation>
keyframeInterpolationFromToken(const std::string_view token) noexcept {
    for (std::size_t index = 0; index < kInterpolationTokens.size(); ++index)
        if (kInterpolationTokens[index] == token)
            return static_cast<KeyframeInterpolation>(index);
    return std::nullopt;
}

std::string_view keyframeEasingToken(const KeyframeEasing easing) noexcept {
    const auto index = static_cast<std::size_t>(easing);
    return index < kEasingTokens.size() ? kEasingTokens[index] : std::string_view{};
}

std::optional<KeyframeEasing> keyframeEasingFromToken(const std::string_view token) noexcept {
    for (std::size_t index = 0; index < kEasingTokens.size(); ++index)
        if (kEasingTokens[index] == token)
            return static_cast<KeyframeEasing>(index);
    return std::nullopt;
}

std::string_view keyframeHandleModeToken(const KeyframeHandleMode mode) noexcept {
    switch (mode) {
    case KeyframeHandleMode::Free:
        return "free";
    case KeyframeHandleMode::Aligned:
        return "aligned";
    case KeyframeHandleMode::Auto:
        return "auto";
    }
    return {};
}

std::optional<KeyframeHandleMode>
keyframeHandleModeFromToken(const std::string_view token) noexcept {
    for (const auto mode :
         {KeyframeHandleMode::Free, KeyframeHandleMode::Aligned, KeyframeHandleMode::Auto})
        if (keyframeHandleModeToken(mode) == token)
            return mode;
    return std::nullopt;
}

bool isValidKeyframeEasing(const KeyframeEasingParameters& parameters) noexcept {
    return isValidKeyframeEasingMode(parameters.easing) && std::isfinite(parameters.back) &&
           std::isfinite(parameters.amplitude) && parameters.amplitude >= 0.0 &&
           std::isfinite(parameters.period);
}

namespace {

template <std::size_t Count>
[[nodiscard]] const ComponentAnimationCurve*
componentAt(const std::array<ComponentAnimationCurve, Count>& components,
            const AnimationComponent component, const std::array<AnimationComponent, Count> names) {
    for (std::size_t index = 0; index < Count; ++index) {
        if (names[index] == component) {
            return &components[index];
        }
    }
    return nullptr;
}

template <std::size_t Count>
[[nodiscard]] ComponentAnimationCurve*
componentAt(std::array<ComponentAnimationCurve, Count>& components,
            const AnimationComponent component, const std::array<AnimationComponent, Count> names) {
    for (std::size_t index = 0; index < Count; ++index) {
        if (names[index] == component) {
            return &components[index];
        }
    }
    return nullptr;
}

} // namespace

const ComponentAnimationCurve*
Vec2AnimationCurve::component(const AnimationComponent component) const noexcept {
    return componentAt(components, component,
                       std::array{AnimationComponent::X, AnimationComponent::Y});
}

ComponentAnimationCurve*
Vec2AnimationCurve::component(const AnimationComponent component) noexcept {
    return componentAt(components, component,
                       std::array{AnimationComponent::X, AnimationComponent::Y});
}

const ComponentAnimationCurve*
Vec3AnimationCurve::component(const AnimationComponent component) const noexcept {
    return componentAt(
        components, component,
        std::array{AnimationComponent::X, AnimationComponent::Y, AnimationComponent::Z});
}

ComponentAnimationCurve*
Vec3AnimationCurve::component(const AnimationComponent component) noexcept {
    return componentAt(
        components, component,
        std::array{AnimationComponent::X, AnimationComponent::Y, AnimationComponent::Z});
}

const ComponentAnimationCurve*
Color4AnimationCurve::component(const AnimationComponent component) const noexcept {
    return componentAt(components, component,
                       std::array{AnimationComponent::Red, AnimationComponent::Green,
                                  AnimationComponent::Blue, AnimationComponent::Alpha});
}

ComponentAnimationCurve*
Color4AnimationCurve::component(const AnimationComponent component) noexcept {
    return componentAt(components, component,
                       std::array{AnimationComponent::Red, AnimationComponent::Green,
                                  AnimationComponent::Blue, AnimationComponent::Alpha});
}

AnimationCurveId animationCurveId(const AnimationCurveRecord& record) noexcept {
    if (const auto* scalar = std::get_if<ScalarAnimationCurve>(&record)) {
        return scalar->id;
    }
    if (const auto* vector = std::get_if<Vec2AnimationCurve>(&record)) {
        return vector->id;
    }
    if (const auto* vector = std::get_if<Vec3AnimationCurve>(&record)) {
        return vector->id;
    }
    if (const auto* color = std::get_if<Color4AnimationCurve>(&record)) {
        return color->id;
    }
    if (const auto* path = std::get_if<PathAnimationCurve>(&record)) {
        return path->id;
    }
    if (const auto* gradient = std::get_if<GradientAnimationCurve>(&record)) {
        return gradient->id;
    }
    return {};
}

std::size_t KeyframeIdIndex::chunkFor(const KeyframeId id) const noexcept {
    const auto at = std::ranges::lower_bound(chunks_, id, std::less<>{},
                                             [](const auto& chunk) { return chunk->back(); });
    return static_cast<std::size_t>(at - chunks_.begin());
}

KeyframeIdIndex::Chunk& KeyframeIdIndex::own(const std::size_t index) {
    // A chunk only this index holds is changed in place; one another index (a published snapshot,
    // a history state) still reads is copied first.
    if (chunks_[index].use_count() != 1)
        chunks_[index] = std::make_shared<const Chunk>(*chunks_[index]);
    return const_cast<Chunk&>(*chunks_[index]);
}

bool KeyframeIdIndex::contains(const KeyframeId id) const noexcept {
    // Above the highest id held: every id an operation has just allocated.
    if (chunks_.empty() || chunks_.back()->back() < id)
        return false;
    const auto& chunk = *chunks_[chunkFor(id)];
    return std::ranges::binary_search(chunk, id);
}

void KeyframeIdIndex::insert(const KeyframeId id) {
    if (chunks_.empty() || chunks_.back()->back() < id) {
        if (chunks_.empty() || chunks_.back()->size() >= kKeyframeIdChunk)
            chunks_.push_back(std::make_shared<const Chunk>(Chunk{id}));
        else
            own(chunks_.size() - 1).push_back(id);
        ++size_;
        return;
    }
    const auto index = chunkFor(id);
    const auto& held = *chunks_[index];
    const auto at = std::ranges::lower_bound(held, id);
    if (at != held.end() && *at == id)
        return;
    const auto offset = at - held.begin();
    auto& chunk = own(index);
    chunk.insert(chunk.begin() + offset, id);
    ++size_;
    if (chunk.size() > 2 * kKeyframeIdChunk) {
        // Split a full chunk in two, so an edit never copies more than 2 * kKeyframeIdChunk ids.
        const auto middle = chunk.begin() + static_cast<std::ptrdiff_t>(chunk.size() / 2);
        auto upper = std::make_shared<const Chunk>(middle, chunk.end());
        chunk.erase(middle, chunk.end());
        chunks_.insert(chunks_.begin() + static_cast<std::ptrdiff_t>(index) + 1, std::move(upper));
    }
}

void KeyframeIdIndex::insertSorted(const std::span<const KeyframeId> ids) {
    for (const auto id : ids)
        insert(id);
}

void KeyframeIdIndex::erase(const KeyframeId id) {
    if (chunks_.empty() || chunks_.back()->back() < id)
        return;
    const auto index = chunkFor(id);
    const auto& held = *chunks_[index];
    const auto at = std::ranges::lower_bound(held, id);
    if (at == held.end() || *at != id)
        return;
    const auto offset = at - held.begin();
    if (held.size() == 1) {
        chunks_.erase(chunks_.begin() + static_cast<std::ptrdiff_t>(index));
    } else {
        auto& chunk = own(index);
        chunk.erase(chunk.begin() + offset);
    }
    --size_;
}

std::size_t KeyframeIdIndex::sharedChunks(const KeyframeIdIndex& other) const noexcept {
    std::unordered_set<const void*> mine;
    for (const auto& chunk : chunks_)
        mine.insert(chunk.get());
    return static_cast<std::size_t>(std::ranges::count_if(
        other.chunks_, [&](const auto& chunk) { return mine.contains(chunk.get()); }));
}

std::optional<std::size_t> AnimationCurveStore::indexOf(const AnimationCurveId id) const noexcept {
    const auto records = records_.view();
    const auto record = std::lower_bound(records.begin(), records.end(), id,
                                         [](const auto& candidate, const auto curveId) {
                                             return animationCurveId(candidate) < curveId;
                                         });
    if (record == records.end() || animationCurveId(*record) != id)
        return std::nullopt;
    return static_cast<std::size_t>(record - records.begin());
}

const AnimationCurveRecord* AnimationCurveStore::find(const AnimationCurveId id) const noexcept {
    const auto index = indexOf(id);
    return index ? &records_[*index] : nullptr;
}

AnimationCurveRecord* AnimationCurveStore::findMutable(const AnimationCurveId id) {
    const auto index = indexOf(id);
    return index ? &records_.edit(*index) : nullptr;
}

const ScalarAnimationCurve*
AnimationCurveStore::findScalar(const AnimationCurveId id) const noexcept {
    const auto* record = find(id);
    return record == nullptr ? nullptr : std::get_if<ScalarAnimationCurve>(record);
}

const Vec2AnimationCurve* AnimationCurveStore::findVec2(const AnimationCurveId id) const noexcept {
    const auto* record = find(id);
    return record == nullptr ? nullptr : std::get_if<Vec2AnimationCurve>(record);
}

const Vec3AnimationCurve* AnimationCurveStore::findVec3(const AnimationCurveId id) const noexcept {
    const auto* record = find(id);
    return record == nullptr ? nullptr : std::get_if<Vec3AnimationCurve>(record);
}

const Color4AnimationCurve*
AnimationCurveStore::findColor4(const AnimationCurveId id) const noexcept {
    const auto* record = find(id);
    return record == nullptr ? nullptr : std::get_if<Color4AnimationCurve>(record);
}

const PathAnimationCurve* AnimationCurveStore::findPath(const AnimationCurveId id) const noexcept {
    const auto* record = find(id);
    return record == nullptr ? nullptr : std::get_if<PathAnimationCurve>(record);
}

const GradientAnimationCurve*
AnimationCurveStore::findGradient(const AnimationCurveId id) const noexcept {
    const auto* record = find(id);
    return record == nullptr ? nullptr : std::get_if<GradientAnimationCurve>(record);
}

const ComponentAnimationCurve*
AnimationCurveStore::findComponent(const AnimationCurveId id,
                                   const AnimationComponent component) const noexcept {
    const auto* record = find(id);
    if (record == nullptr) {
        return nullptr;
    }
    if (const auto* vector = std::get_if<Vec2AnimationCurve>(record)) {
        return vector->component(component);
    }
    if (const auto* vector = std::get_if<Vec3AnimationCurve>(record)) {
        return vector->component(component);
    }
    if (const auto* color = std::get_if<Color4AnimationCurve>(record)) {
        return color->component(component);
    }
    return nullptr;
}

bool AnimationCurveStore::insert(AnimationCurveRecord record) {
    const auto id = animationCurveId(record);
    if (!canEnterStore(record) || find(id) != nullptr) {
        return false;
    }
    // The record's keyframe ids must be unique within it and not already held by the store: one
    // index lookup per id (issues #737 and #744), never a pass over the store's keys.
    const auto recordIds = sortedKeyframeIds(record);
    if (std::ranges::adjacent_find(recordIds) != recordIds.end() ||
        std::ranges::any_of(recordIds, [this](const KeyframeId keyframeId) {
            return keyframeIds_.contains(keyframeId);
        })) {
        return false;
    }
    normalizeRecordFinalInterpolation(record);
    const auto records = records_.view();
    const auto position = std::lower_bound(records.begin(), records.end(), id,
                                           [](const auto& candidate, const auto curveId) {
                                               return animationCurveId(candidate) < curveId;
                                           });
    records_.insert(static_cast<std::size_t>(position - records.begin()), std::move(record));
    keyframeIds_.insertSorted(recordIds);
    return true;
}

bool AnimationCurveStore::erase(const AnimationCurveId id) {
    const auto index = indexOf(id);
    if (!index) {
        return false;
    }
    forEachKeyframeId(records_[*index],
                      [this](const KeyframeId keyframeId) { keyframeIds_.erase(keyframeId); });
    records_.erase(*index);
    return true;
}

bool AnimationCurveStore::replace(AnimationCurveRecord record) {
    const auto id = animationCurveId(record);
    const auto index = indexOf(id);
    const auto* current = index ? &records_[*index] : nullptr;
    if (current == nullptr || current->index() != record.index() || !canEnterStore(record)) {
        return false;
    }
    const auto recordIds = sortedKeyframeIds(record);
    const auto currentIds = sortedKeyframeIds(*current);
    if (std::ranges::adjacent_find(recordIds) != recordIds.end()) {
        return false;
    }
    // An id the replaced curve already holds stays; any other must be free in the whole store.
    std::vector<KeyframeId> added;
    std::ranges::set_difference(recordIds, currentIds, std::back_inserter(added));
    if (std::ranges::any_of(added, [this](const KeyframeId keyframeId) {
            return keyframeIds_.contains(keyframeId);
        })) {
        return false;
    }
    std::vector<KeyframeId> removed;
    std::ranges::set_difference(currentIds, recordIds, std::back_inserter(removed));
    normalizeRecordFinalInterpolation(record);
    records_.replace(*index, std::move(record));
    for (const auto keyframeId : removed) {
        keyframeIds_.erase(keyframeId);
    }
    keyframeIds_.insertSorted(added);
    return true;
}

bool AnimationCurveStore::insertKeyframe(const AnimationCurveId curveId, ScalarKeyframe keyframe) {
    const auto keyframeId = keyframe.id;
    auto* record = findMutable(curveId);
    auto* curve = record == nullptr ? nullptr : std::get_if<ScalarAnimationCurve>(record);
    if (curve == nullptr || !::insertKeyframe(*curve, keyframe, containsKeyframe(keyframeId))) {
        return false;
    }
    keyframeIds_.insert(keyframeId);
    return true;
}

bool AnimationCurveStore::insertKeyframe(const AnimationCurveId curveId,
                                         const AnimationComponent component,
                                         ScalarKeyframe keyframe) {
    const auto keyframeId = keyframe.id;
    auto* record = findMutable(curveId);
    if (record == nullptr || containsKeyframe(keyframeId)) {
        return false;
    }
    ComponentAnimationCurve* curve = nullptr;
    std::visit(
        [&](auto& value) {
            using Curve = std::decay_t<decltype(value)>;
            if constexpr (!kWholeValueCurve<Curve>) {
                curve = value.component(component);
            }
        },
        *record);
    if (curve == nullptr || !::insertKeyframe(*curve, keyframe, false)) {
        return false;
    }
    keyframeIds_.insert(keyframeId);
    return true;
}

bool AnimationCurveStore::updateKeyframe(const AnimationCurveId curveId, ScalarKeyframe keyframe) {
    auto* record = findMutable(curveId);
    auto* curve = record == nullptr ? nullptr : std::get_if<ScalarAnimationCurve>(record);
    return curve != nullptr && ::updateKeyframe(*curve, keyframe);
}

bool AnimationCurveStore::insertKeyframe(const AnimationCurveId curveId, PathKeyframe keyframe) {
    const auto keyframeId = keyframe.id;
    auto* record = findMutable(curveId);
    auto* curve = record == nullptr ? nullptr : std::get_if<PathAnimationCurve>(record);
    if (curve == nullptr ||
        !::insertKeyframe(*curve, std::move(keyframe), containsKeyframe(keyframeId))) {
        return false;
    }
    keyframeIds_.insert(keyframeId);
    return true;
}

bool AnimationCurveStore::updateKeyframe(const AnimationCurveId curveId, PathKeyframe keyframe) {
    auto* record = findMutable(curveId);
    auto* curve = record == nullptr ? nullptr : std::get_if<PathAnimationCurve>(record);
    return curve != nullptr && ::updateKeyframe(*curve, std::move(keyframe));
}

bool AnimationCurveStore::insertKeyframe(const AnimationCurveId curveId,
                                         GradientKeyframe keyframe) {
    const auto keyframeId = keyframe.id;
    auto* record = findMutable(curveId);
    auto* curve = record == nullptr ? nullptr : std::get_if<GradientAnimationCurve>(record);
    if (curve == nullptr ||
        !::insertKeyframe(*curve, std::move(keyframe), containsKeyframe(keyframeId))) {
        return false;
    }
    keyframeIds_.insert(keyframeId);
    return true;
}

bool AnimationCurveStore::updateKeyframe(const AnimationCurveId curveId,
                                         GradientKeyframe keyframe) {
    auto* record = findMutable(curveId);
    auto* curve = record == nullptr ? nullptr : std::get_if<GradientAnimationCurve>(record);
    return curve != nullptr && ::updateKeyframe(*curve, std::move(keyframe));
}

bool AnimationCurveStore::updateKeyframe(const AnimationCurveId curveId,
                                         const AnimationComponent component,
                                         ScalarKeyframe keyframe) {
    auto* record = findMutable(curveId);
    if (record == nullptr) {
        return false;
    }
    ComponentAnimationCurve* curve = nullptr;
    std::visit(
        [&](auto& value) {
            using Curve = std::decay_t<decltype(value)>;
            if constexpr (!kWholeValueCurve<Curve>) {
                curve = value.component(component);
            }
        },
        *record);
    return curve != nullptr && ::updateKeyframe(*curve, keyframe);
}

bool AnimationCurveStore::eraseKeyframe(const AnimationCurveId curveId,
                                        const KeyframeId keyframeId) {
    auto* record = findMutable(curveId);
    if (record == nullptr) {
        return false;
    }
    bool erased = false;
    if (auto* scalar = std::get_if<ScalarAnimationCurve>(record)) {
        erased = eraseCurveKeyframe(*scalar, keyframeId);
    } else if (auto* path = std::get_if<PathAnimationCurve>(record)) {
        erased = eraseCurveKeyframe(*path, keyframeId);
    } else if (auto* gradient = std::get_if<GradientAnimationCurve>(record)) {
        erased = eraseCurveKeyframe(*gradient, keyframeId);
    }
    if (erased) {
        keyframeIds_.erase(keyframeId);
        return true;
    }
    std::visit(
        [&](auto& curve) {
            using Curve = std::decay_t<decltype(curve)>;
            if constexpr (!kWholeValueCurve<Curve>) {
                for (auto& component : curve.components) {
                    if (std::ranges::find(component.keyframes, keyframeId, &ScalarKeyframe::id) !=
                        component.keyframes.end()) {
                        erased = eraseCurveKeyframe(component, keyframeId);
                        return;
                    }
                }
            }
        },
        *record);
    if (erased) {
        keyframeIds_.erase(keyframeId);
    }
    return erased;
}

bool AnimationCurveStore::eraseKeyframe(const AnimationCurveId curveId,
                                        const AnimationComponent component,
                                        const KeyframeId keyframeId) {
    auto* record = findMutable(curveId);
    if (record == nullptr) {
        return false;
    }
    ComponentAnimationCurve* curve = nullptr;
    std::visit(
        [&](auto& value) {
            using Curve = std::decay_t<decltype(value)>;
            if constexpr (!kWholeValueCurve<Curve>) {
                curve = value.component(component);
            }
        },
        *record);
    if (curve == nullptr || curve->keyframes.empty()) {
        return false;
    }
    const auto found = std::ranges::find(curve->keyframes, keyframeId, &ScalarKeyframe::id);
    if (found == curve->keyframes.end()) {
        return false;
    }
    const bool settles = bloom::document::hasNonFreeHandleMode(curve->keyframes);
    auto before = settles ? curve->keyframes : decltype(curve->keyframes){};
    curve->keyframes.erase(found);
    keyframeIds_.erase(keyframeId);
    normalizeFinalInterpolation(*curve);
    if (settles)
        bloom::document::settleKeyframeHandles(curve->keyframes, before);
    // A component's final key may be removed when another component still owns the grouped curve.
    // The command layer erases the grouped record or converts the parameter to a constant when no
    // component remains; the store reports the successful local mutation here.
    return true;
}

void AnimationCurveStore::validateRecord(const AnimationCurveRecord& record,
                                         ValidationResult& result) {
    std::visit(
        [&](const auto& curve) {
            using Curve = std::decay_t<decltype(curve)>;
            if constexpr (kWholeValueCurve<Curve>) {
                validateCurve(curve, result);
            } else {
                for (std::size_t componentIndex = 0; componentIndex < curve.components.size();
                     ++componentIndex) {
                    const auto path = "[" + std::to_string(curve.id.value()) + "].components[" +
                                      std::to_string(componentIndex) + "]";
                    validateComponentCurve(curve.components[componentIndex], path, result);
                }
                if (!std::ranges::any_of(curve.components, [](const auto& item) {
                        return !item.keyframes.empty();
                    })) {
                    result.add(ValidationCode::InvalidValue,
                               "[" + std::to_string(curve.id.value()) + "].components",
                               "Component animation curve must contain at least one keyframe");
                }
            }
        },
        record);
}

ValidationResult AnimationCurveStore::validate() const {
    ValidationResult result;
    std::unordered_set<AnimationCurveId> curveIds;
    std::unordered_set<KeyframeId> keyframeIds;
    AnimationCurveId previousId;
    for (const auto& record : records_.view()) {
        const auto id = animationCurveId(record);
        if (!curveIds.insert(id).second) {
            result.add(ValidationCode::DuplicateId, "[" + std::to_string(id.value()) + "].id",
                       "Animation curve ID is duplicated");
        }
        if (previousId.isValid() && id < previousId) {
            result.add(ValidationCode::InvalidOrder, "[" + std::to_string(id.value()) + "].id",
                       "Animation curves must be ordered by ID");
        }
        previousId = id;
        validateRecord(record, result);
        std::visit(
            [&](const auto& curve) {
                using Curve = std::decay_t<decltype(curve)>;
                if constexpr (kWholeValueCurve<Curve>) {
                    for (const auto& keyframe : curve.keyframes) {
                        if (keyframe.id.isValid() && !keyframeIds.insert(keyframe.id).second) {
                            result.add(ValidationCode::DuplicateId,
                                       "[" + std::to_string(curve.id.value()) + "].keyframes[" +
                                           std::to_string(keyframe.id.value()) + "].id",
                                       "Keyframe ID is duplicated within the composition");
                        }
                    }
                } else {
                    for (std::size_t componentIndex = 0; componentIndex < curve.components.size();
                         ++componentIndex) {
                        const auto path = "[" + std::to_string(curve.id.value()) + "].components[" +
                                          std::to_string(componentIndex) + "]";
                        for (const auto& keyframe : curve.components[componentIndex].keyframes) {
                            if (keyframe.id.isValid() && !keyframeIds.insert(keyframe.id).second) {
                                result.add(ValidationCode::DuplicateId,
                                           path + ".keyframes[" +
                                               std::to_string(keyframe.id.value()) + "].id",
                                           "Keyframe ID is duplicated within the composition");
                            }
                        }
                    }
                }
            },
            record);
    }
    return result;
}

void validateAnimationCurveReference(const ParameterRecord& parameter,
                                     const AnimationCurveRecord& curve, ValidationResult& result) {
    const auto parameterPath = "parameters[" + std::to_string(parameter.id.value()) + "]";
    // Animatability and curve kind both come from the shared schema predicates in
    // bloom/document/parameter.hpp, so this validation and the animation commands cannot
    // disagree about which parameters may be animated.
    if (isVec2AnimatableSchemaKey(parameter.schemaKey)) {
        if (!isVec2(curve)) {
            result.add(ValidationCode::TypeMismatch, parameterPath + ".source.curveId",
                       "This transform parameter requires a Vec2 animation curve");
        }
    } else if (isVec3AnimatableSchemaKey(parameter.schemaKey)) {
        if (!isVec3(curve)) {
            result.add(ValidationCode::TypeMismatch, parameterPath + ".source.curveId",
                       "This value parameter requires a Vec3 animation curve");
        }
    } else if (isScalarAnimatableSchemaKey(parameter.schemaKey)) {
        if (!isScalar(curve)) {
            result.add(ValidationCode::TypeMismatch, parameterPath + ".source.curveId",
                       "This scalar parameter requires a scalar animation curve");
        }
    } else if (isColor4AnimatableSchemaKey(parameter.schemaKey)) {
        if (!isColor4(curve)) {
            result.add(ValidationCode::TypeMismatch, parameterPath + ".source.curveId",
                       "This color parameter requires a Color4 animation curve");
        }
    } else if (isPathAnimatableSchemaKey(parameter.schemaKey)) {
        if (!isPath(curve)) {
            result.add(ValidationCode::TypeMismatch, parameterPath + ".source.curveId",
                       "This path parameter requires a Path animation curve");
        }
    } else if (isGradientAnimatableSchemaKey(parameter.schemaKey)) {
        if (!isGradient(curve)) {
            result.add(ValidationCode::TypeMismatch, parameterPath + ".source.curveId",
                       "This gradient parameter requires a Gradient animation curve");
        }
    } else {
        result.add(ValidationCode::InvalidValue, parameterPath + ".source",
                   "This parameter schema does not support animation");
    }

    // A scalar DOMAIN belongs to the schema, not to scalar curves in general: opacity is
    // confined to [0, 1] and text size to (0, kMaximumTextSizePixels], while a rotation curve
    // measures degrees and must be free to wind past a full turn in either direction. The one
    // gate is isScalarWithinSchemaDomain() so a key and a constant are admitted identically.
    if (const auto* scalar = std::get_if<ScalarAnimationCurve>(&curve)) {
        for (const auto& keyframe : scalar->keyframes) {
            if (!isScalarWithinSchemaDomain(parameter.schemaKey, keyframe.value)) {
                result.add(ValidationCode::InvalidValue,
                           "animationCurves[" + std::to_string(scalar->id.value()) +
                               "].keyframes[" + std::to_string(keyframe.id.value()) + "].value",
                           "Keyframe value is outside the domain its schema declares");
            }
        }
    } else if (const auto* path = std::get_if<PathAnimationCurve>(&curve);
               path != nullptr && isGradeCurveSchemaKey(parameter.schemaKey)) {
        // #493: a Curves key is a point list within its curve's domain, like a constant.
        for (const auto& keyframe : path->keyframes)
            if (!isGradeCurveValueValid(parameter.schemaKey, keyframe.value))
                result.add(ValidationCode::InvalidValue,
                           "animationCurves[" + std::to_string(path->id.value()) + "].keyframes[" +
                               std::to_string(keyframe.id.value()) + "].value",
                           "Curve key is outside the domain its schema declares");
    } else if (const auto* maskPath = std::get_if<PathAnimationCurve>(&curve);
               maskPath != nullptr && parameter.schemaKey == kLayerMaskPathSchemaKey) {
        // #589: a mask holds one contour, keyed or not.
        for (const auto& keyframe : maskPath->keyframes)
            if (!keyframe.value.isSimple())
                result.add(ValidationCode::InvalidValue,
                           "animationCurves[" + std::to_string(maskPath->id.value()) +
                               "].keyframes[" + std::to_string(keyframe.id.value()) + "].value",
                           "A mask path key holds one contour");
    } else if (const auto* gradient = std::get_if<GradientAnimationCurve>(&curve)) {
        // #541: a Gradient key satisfies its schema like a constant.
        for (const auto& keyframe : gradient->keyframes) {
            ParameterStore probe;
            if (!probe.insert(
                    {parameter.id, parameter.schemaKey, ConstantValueSource{keyframe.value}}))
                result.add(ValidationCode::InvalidValue,
                           "animationCurves[" + std::to_string(gradient->id.value()) +
                               "].keyframes[" + std::to_string(keyframe.id.value()) + "].value",
                           "Gradient key is outside the domain its schema declares");
        }
    } else if (const auto* color = std::get_if<Color4AnimationCurve>(&curve)) {
        if (const auto* alpha = color->component(AnimationComponent::Alpha)) {
            for (const auto& keyframe : alpha->keyframes) {
                if (keyframe.value < 0.0 || keyframe.value > 1.0) {
                    result.add(ValidationCode::InvalidValue,
                               "animationCurves[" + std::to_string(color->id.value()) +
                                   "].components[3].keyframes[" +
                                   std::to_string(keyframe.id.value()) + "].value",
                               "Alpha keyframe value is outside the color domain");
                }
            }
        }
    }
}

ValidationResult validateAnimationCurveReferences(const ParameterStore& parameters,
                                                  const AnimationCurveStore& animationCurves) {
    ValidationResult result;
    std::unordered_map<AnimationCurveId, ParameterId> owners;

    for (const auto& parameter : parameters.records()) {
        const auto* source = std::get_if<AnimationCurveSource>(&parameter.source);
        if (source == nullptr) {
            continue;
        }
        const auto parameterPath = "parameters[" + std::to_string(parameter.id.value()) + "]";
        const auto* curve = animationCurves.find(source->curveId);
        if (curve == nullptr) {
            result.add(ValidationCode::MissingReference, parameterPath + ".source.curveId",
                       "Animation source does not resolve to a curve in this composition");
            continue;
        }

        const auto [owner, inserted] = owners.emplace(source->curveId, parameter.id);
        if (!inserted && owner->second != parameter.id) {
            result.add(ValidationCode::SharedReference, parameterPath + ".source.curveId",
                       "Animation curve may be owned by only one parameter");
        }

        validateAnimationCurveReference(parameter, *curve, result);
    }

    for (const auto& record : animationCurves.records()) {
        const auto id = animationCurveId(record);
        if (!owners.contains(id)) {
            result.add(ValidationCode::OrphanObject,
                       "animationCurves[" + std::to_string(id.value()) + "]",
                       "Animation curve must be owned by exactly one parameter");
        }
    }
    return result;
}

} // namespace bloom::document
