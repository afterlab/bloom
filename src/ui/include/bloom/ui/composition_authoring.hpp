#pragma once

#include <bloom/core/blend_mode.hpp>
#include <bloom/document/shape.hpp>
#include <bloom/ui/composition_session.hpp>

#include <QString>
#include <bloom/ui/kit/surfaces.hpp>

#include <optional>
#include <string>
#include <vector>

namespace bloom::core {
struct Color4d;
} // namespace bloom::core

namespace bloom::document {
struct ParameterRecord;
} // namespace bloom::document

namespace bloom::ui::kit {
class KDropdown;
} // namespace bloom::ui::kit

namespace bloom::ui {

class CompositionSession;

// --- Shared authoring/presentation truth (task U4, issue #123) ---------------------------------
//
// Three decisions the Properties panel and the Timeline's Add menu already owned, now CALLED by the
// Nodes canvas rather than copied into it. Task U4's decision 4 requires the node editor's Add menu
// to offer "exactly the layer kinds the command layer offers today (mirror the timeline's Add menu
// contents)", and its decision 5 requires in-node fields to read and commit through "the EXACT same
// session/command paths Properties uses". Copying the default-name rule, the built-in proof
// palette, or the source wording into node_editor.cpp would have duplicated four raw color literals
// and three user-visible strings, and would let the two surfaces silently drift. These declarations
// exist so there is exactly one definition of each; every one of them is defined in
// composition_authoring.cpp, beside the anonymous-namespace helpers it already used.

// "Constant" / "Animated" / "Driven by graph" -- the one wording for a parameter's source, shown by
// the Properties rows' tooltips and by the node card's own field tooltips.
[[nodiscard]] QString parameterSourceDescription(const document::ParameterRecord& parameter);

// The exact, never-rounded and never-clipped "R r  G g  B b  A a" rendering of an authoring color
// (negative and HDR channels included -- docs/architecture/evaluation-primitives.md's straight
// Color4d authoring values). The Properties Solid Source row shows it as its value text; the node
// card's read-only color chip carries it in its tooltip, because a quantized 8-bit swatch cannot
// show an out-of-range channel honestly on its own.
[[nodiscard]] QString exactColorText(core::Color4d color);

// One "Add Solid"/"Add Text" gesture, two entry points (the Timeline's Add menu, the Nodes canvas
// context menu): same default numbered name derived from the composition's own existing layers,
// same next built-in reference-linear-sRGB proof color, same single command transaction. Returns
// what CompositionSession::addSolidLayer()/addTextLayer() returned.
// The artist-facing name of one blend mode -- "Normal", "Add", "Multiply", ... -- shown by the
// timeline's Blending dropdown, the Properties Appearance row, and the Layer node card. One
// definition, for the same reason parameterSourceDescription() is one definition: three surfaces
// offering the same vocabulary must offer the same words, and core::kBlendModes already fixes the
// order they appear in.
[[nodiscard]] QString blendModeDisplayName(core::BlendMode mode);

// Fills a blend-mode dropdown: every mode in core::kBlendModes order, named by
// blendModeDisplayName(), with the mode's stored integer as item data and a hairline separator
// between consecutive core::BlendModeGroup runs (Normal | blends | arithmetic | extremes | mattes).
// A separator occupies a row, so a caller maps rows and modes only through the item data --
// blendModeDropdownIndex() and blendModeFromDropdownIndex() -- never by position in kBlendModes.
void addBlendModeItems(kit::KDropdown& dropdown);

// The row of a dropdown filled by addBlendModeItems() that shows `mode`, or -1.
[[nodiscard]] int blendModeDropdownIndex(const kit::KDropdown& dropdown, core::BlendMode mode);

// The mode a row of such a dropdown names; a separator or an out-of-range row names none.
[[nodiscard]] std::optional<core::BlendMode>
blendModeFromDropdownIndex(const kit::KDropdown& dropdown, int index);

[[nodiscard]] bool addDefaultSolidLayer(CompositionSession& session);
[[nodiscard]] bool addDefaultTextLayer(CompositionSession& session);

// One "Add <shape>" gesture, shared by the viewer's Add menu and the Timeline's Add menu (task
// SHAPEFIX-1, deliverable 2): the composition-centred, default-sized shape layer
// CompositionSession::addShapeLayer() already builds for the node editor's own Add > Shapes
// entries, reused rather than re-specified so the three surfaces cannot offer three opinions about
// what "Add > Rectangle" means.
[[nodiscard]] bool addDefaultShapeLayer(CompositionSession& session, document::ShapeKind kind);

// The compositor keyframe diamond (task S5, item 0), shared by the Properties rows and the node
// card's parameter rows for the same reason parameterSourceDescription() is shared: the gesture,
// the three painted states, and the tooltip wording are one decision, and a second copy on the
// canvas would be free to drift from the panel's.
//
// It is a real clickable control, not the indicating QLabel it replaces: clicking it calls
// CompositionSession::toggleKeyframe() for its role and nothing else -- no command construction
// here, no state of its own. The three states come straight from
// CompositionSession::keyframeDiamondState() on every refresh, so the diamond can never claim a key
// the document does not have:
//
//   Constant            empty     dimmed Muted outline  "Click to animate ..."
//   AnimatedWithoutKey  outlined  gold outline          "Click to add a key here"
//   AnimatedWithKey     filled    gold fill             "Click to remove this key"
//   Unsupported         hidden    --                    (no diamond at all)
//
// The weights are the kit's own existing two (IconWeight::Regular / Fill) used exactly as
// docs/ux/visual-language.md's iconography rule states -- "regular is the default visual weight and
// fill for selected or toggled states" -- so this needs no kit change.
class KeyframeDiamond final : public kit::KDiamond {
    Q_OBJECT

  public:
    // `role` is the parameter role this diamond keys (document::kPositionParameterRole, ...). The
    // session must outlive this widget, as it does for every other composition editor child.
    KeyframeDiamond(CompositionSession& session, std::string role, QWidget* parent = nullptr);

    // Binds this diamond to ONE exact parameter instead of "whatever the selection exposes for my
    // role". The node canvas sets it on every card refresh, because a card shows its OWN node's
    // parameters whether or not that node is selected; the Properties panel leaves it unset,
    // because its rows ARE the selection's rows. An invalid id clears the binding.
    void setParameterId(document::ParameterId parameterId);
    void setRole(std::string role);
    void setComponent(std::optional<document::AnimationComponent> component);
    // #502: binds a plain Merge entry's row (no layer, so no parameter yet): the diamond shows its
    // default layer's constant `role` and a click keys it, creating the entry's layer in the same
    // step. Ignored while a parameter is bound.
    void setEntry(std::optional<document::MergeEntryRef> entry);
    [[nodiscard]] std::optional<document::AnimationComponent> component() const noexcept {
        return component_;
    }
    // Which parameter this diamond currently keys, or nothing when it follows the selection's own
    // row instead. Diagnostic: a card names every one of its diamonds "nodeKeyframeDiamond", so a
    // reader asking whether the right diamond is on the right row has to be able to ask it.
    [[nodiscard]] const std::optional<document::ParameterId>& parameterId() const noexcept {
        return parameterId_;
    }

    // Re-reads the session and repaints. Called from the owning surface's own refresh pass, so a
    // diamond is never a frame behind the row it sits in.
    void refresh();
    [[nodiscard]] KeyframeDiamondState state() const noexcept { return state_; }

  protected:
    void mousePressEvent(QMouseEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

  private:
    CompositionSession& session_;
    std::string role_;
    std::optional<document::ParameterId> parameterId_;
    std::optional<document::AnimationComponent> component_;
    std::optional<document::MergeEntryRef> entry_;
    KeyframeDiamondState state_ = KeyframeDiamondState::Unsupported;
    bool hovered_ = false;
};

// #501: one diamond for a widget that edits several values together (a Grade wheel's master and
// colour, a Levels channel's five points), drawn on the widget itself. It paints the group's state
// in the same four forms -- a muted outline when every value is constant, gold once one is
// animated, a half fill when some have a key at the playhead and a fill when all do -- and a click
// keys or unkeys the whole group as one command (CompositionSession::toggleKeyframesForParameters).
class KeyframeGroupDiamond final : public kit::KDiamond {
    Q_OBJECT

  public:
    KeyframeGroupDiamond(CompositionSession& session, QString name, QWidget* parent = nullptr);

    void setParameters(std::vector<document::ParameterId> parameters);
    [[nodiscard]] const std::vector<document::ParameterId>& parameters() const noexcept {
        return parameters_;
    }
    void refresh();
    // How many of the group's keyable values have a key at the playhead, of how many.
    [[nodiscard]] std::size_t keyed() const noexcept { return keyed_; }
    [[nodiscard]] std::size_t keyable() const noexcept { return keyable_; }
    // The click: keys or unkeys the group as one command.
    bool toggle();

  protected:
    void mousePressEvent(QMouseEvent* event) override;

  private:
    CompositionSession& session_;
    QString name_;
    std::vector<document::ParameterId> parameters_;
    std::size_t keyed_ = 0;
    std::size_t keyable_ = 0;
};

} // namespace bloom::ui
