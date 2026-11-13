#pragma once
#include <QWidget>
#include <array>
#include <bloom/document/asset.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/project.hpp>
#include <bloom/document/shape.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/value_field.hpp>
#include <vector>
class QLineEdit;
class QPlainTextEdit;
namespace bloom::ui {
class CompositionSession;
namespace kit {
class KAngleDial;
class KButton;
} // namespace kit
QString imageDimensionsText(const document::AssetRecord* asset);
QString imageRangeText(const document::AssetRecord* asset);
// Video source metadata as shown in Properties and the Assets tooltip.
QString videoFieldOrderText(std::uint32_t fieldOrder);
QString videoTimecodeText(const document::AssetVideoStream& stream);
// #381: the asset's start timecode and field order as Interpret Footage reads them; an override
// is marked.
QString footageTimecodeText(const document::AssetRecord& asset);
QString footageFieldOrderText(const document::AssetRecord& asset);
QString imageAssetDisplayName(const document::AssetRecord& asset);
QString mediaLayerDisplayName(const CompositionSession& session, document::LayerId id);
void refreshVideoAssetSelector(kit::KDropdown& selector, const CompositionSession& session,
                               const QString& stored);
void refreshImageAssetSelector(kit::KDropdown& selector, const CompositionSession& session,
                               const QString& stored);
void refreshAudioAssetSelector(kit::KDropdown& selector, const CompositionSession& session,
                               const QString& stored);
// OCIO color-space dropdowns (Color Transform from/to, OCIO File process space, image and video
// input color space) and the LUT asset dropdown: one fill/refresh shared by Properties and the
// node cards, so the two cannot offer different lists. Both write the stored id string (empty =
// Auto / Working space); the LUT writes the asset id integer.
bool isEffectColorSpaceSchema(std::string_view schemaKey);
bool isInputColorSpaceSchema(std::string_view schemaKey);
void populateColorSpaceSelector(kit::KDropdown& selector, const CompositionSession& session,
                                std::string_view schemaKey, const QString& current);
void refreshColorSpaceSelector(kit::KDropdown& selector, const CompositionSession& session,
                               const document::NodeRecord* node, std::string_view schemaKey,
                               const QString& stored);
// #610: what to tell the artist when Apply LUT's Process Space, or a Color Space Transform's From
// or To, holds a space the project's color configuration does not have (the node then leaves the
// image unchanged). Empty when every stored space is in the configuration, or none is known.
[[nodiscard]] QString effectColorSpaceProblem(const CompositionSession& session,
                                              const document::NodeRecord& node);
void refreshLutSelector(kit::KDropdown& selector, const CompositionSession& session,
                        std::int64_t current);
// ADR 0026 / #364: the Image source's Part and Layer and the Cryptomatte's Layer, offered only as
// the values the probe actually found (requested on a worker when not yet known; the selector
// stays disabled and says so until they arrive). Shared by node cards and Properties. Each
// answers whether the current value is one the probe lists.
bool refreshImagePartSelector(kit::KDropdown& selector, const CompositionSession& session,
                              const document::NodeRecord& node);
bool refreshImageLayerSelector(kit::KDropdown& selector, const CompositionSession& session,
                               const document::NodeRecord& node);
bool refreshCryptomatteLayerSelector(kit::KDropdown& selector, const CompositionSession& session,
                                     const document::NodeRecord& node);
// #487: the Shuffle R/G/B/A slots offer the channels of the node's input channel set (the Viewer
// channel menu's listing, read on a worker through ChannelSetController) plus the constants 0 and
// 1; an empty (default) slot shows the default colour layer's channel it reads. #480: the Pass node's
// pass picker shares the listing and offers its layers and channels.
[[nodiscard]] bool isShuffleSlotSchema(std::string_view schemaKey) noexcept;
// #501, D15: a parameter row's name, the one function a Properties row and a node card row (and a
// card widget's own row) take it from, so the two surfaces cannot name a value differently. A unit
// is the field's, never the name's.
[[nodiscard]] QString parameterRowLabel(std::string_view schemaKey, std::string_view role);
// #509: a Node Group instance's value slot named after the one inner value its group wires it to
// ("Glow Size", "Path Stroke Width"); nothing when the node is not an instance, the slot feeds
// nothing or more than one value, or the group is missing. The card and Properties both ask.
[[nodiscard]] std::optional<QString> nodeGroupSlotLabel(const document::Project& project,
                                                        const document::Composition& composition,
                                                        document::NodeId node,
                                                        std::string_view slot);
bool refreshShuffleSlotSelector(kit::KDropdown& selector, const CompositionSession& session,
                                const document::NodeRecord& node, std::string_view schemaKey);
// #487: a data reader's block, offered as the project's data blocks of the reader's kind; 0 (the
// default) shows the first such block, which is the one it reads.
bool refreshDataBlockSelector(kit::KDropdown& selector, const CompositionSession& session,
                              const document::NodeRecord& node);
// #642: what a Vector Artwork node's `artwork` reads out as, on its card and in Properties: the SVG's
// file name and the size the project holds for it. A reference, never a number to edit.
[[nodiscard]] QString vectorArtworkSourceText(const document::Project& project, std::int64_t stored);
// #509: the node a Mask Path or Text Outline reads, offered as the composition's nodes it can read;
// 0 (the default) shows the first such node, which is the one it reads.
bool refreshPathSourceSelector(kit::KDropdown& selector, const CompositionSession& session,
                               const document::NodeRecord& node);
// #487: the Video source's audio stream, shared by Properties and the node card, and its commit.
void refreshAudioStreamSelector(kit::KDropdown& selector, const CompositionSession& session,
                                const document::NodeRecord& node);
bool commitAudioStream(CompositionSession& session, document::NodeId node, std::uint32_t ordinal);
// #487: the one input colour space control of an Image or Video source. Besides Auto and the OCIO
// spaces it offers "Raw (no conversion)" (the legacy Raw choice), and shows a legacy sRGB or Linear
// choice. Committing writes the OCIO id and clears the legacy choice (or sets
// Raw) in one undoable edit. Answers false when the schema is not a source input colour space.
[[nodiscard]] bool isSourceInputColorSpaceSchema(std::string_view schemaKey) noexcept;
bool commitSourceInputColorSpace(CompositionSession& session, document::NodeId node,
                                 const QString& choice);
// The plain note an Image source shows when its selected part and layer have no colour channels
// ("No color layer — use Channels"); empty when it has colour or the probe is not known yet.
QString imageSourceColourNote(const CompositionSession& session, const document::NodeRecord& node);
void resetPropertiesParameter(CompositionSession& session, document::ParameterId parameter);
void jumpToPropertiesNode(CompositionSession& session, document::NodeId node, QWidget* panel);
[[nodiscard]] document::ShapeKind nodeShapeKind(const document::Composition& composition,
                                                const document::NodeRecord& node);
class KeyframeDiamond;
class ChannelChecklistField;
class FontField;
namespace kit {
class KColorChip;
class KLineEdit;
class KTextArea;
class KDropdown;
class KSwitch;
class KRadioGroup;
class KSlider;
} // namespace kit
// SegmentedEnum is the icon-only alignment bar; Radio (#528) is a labelled segmented choice for a
// short closed set that is better seen than picked from a list (Inner Glow's Edge or Center, #527
// Dither's pattern and domain), on the node card as in Properties.
enum class PropertiesRowControl : std::uint8_t { Automatic, SegmentedEnum, Radio, Stepper };
enum class PropertiesRowVisibility : std::uint8_t { Visible, Hidden };
[[nodiscard]] PropertiesRowVisibility
propertiesRowVisibility(std::string_view role, std::string_view schemaKey,
                        std::optional<document::ShapeKind> shape = std::nullopt) noexcept;
// #524 and #514: a value that only matters while another value of the same node says so (Gaussian
// Blur's vertical radius needs Separate axes, its per-channel radii need Per-channel radius,
// Denoise's frame count needs Temporal mode and its radius needs any other mode) has no row while it
// would do nothing, as a Shape source's rows follow its kind. Both toggles and the mode are
// constants, so the answer is read straight from the document.
[[nodiscard]] bool propertiesRowInactive(const document::Composition& composition,
                                         const document::NodeRecord& node,
                                         std::string_view schemaKey);
[[nodiscard]] PropertiesRowControl propertiesRowControl(std::string_view schemaKey);
QList<std::pair<QString, std::int64_t>> propertiesSelectorItems(std::string_view schemaKey);
class PropertiesRegistryRow final : public QWidget {
  public:
    PropertiesRegistryRow(CompositionSession& session, document::NodeId node,
                          document::ParameterId parameter, document::ParameterDefinition definition,
                          QWidget* parent);
    void refresh();
    void reset();
    // CRASH-2: a font row's catalogue poll re-arms itself via QTimer::singleShot for as long as
    // the (process-wide) font scan stays in flight. configureRegistryRows()/configureUpstream()
    // tear a stale row down with setParent(nullptr) + deleteLater(), and deleteLater() only
    // destroys the row once the event loop gets back to its DeferredDelete event -- there is no
    // guarantee that wins the race against the row's own pending poll. Callers MUST call this
    // before orphaning a row for deferred deletion, so a poll that still fires in that window
    // can no longer touch session_ (which may, by then, refer to a destroyed CompositionSession).
    void detachFromSession();

  private:
    void commit(std::optional<std::size_t> component = {});
    void populateInputColorSpaceSelector();
    [[nodiscard]] double displayScale() const;
    CompositionSession& session_;
    document::NodeId node_;
    document::ParameterId parameter_;
    document::ParameterDefinition definition_;
    std::array<kit::KValueField*, 4> fields_{};
    kit::KRadioGroup* segments_ = nullptr;
    kit::KDropdown* selector_ = nullptr;
    FontField* fontField_ = nullptr;
    kit::KSlider* slider_ = nullptr;
    kit::KSwitch* toggle_ = nullptr;
    kit::KColorChip* color_ = nullptr;
    kit::KLineEdit* text_ = nullptr;
    kit::KValueField* integer_ = nullptr;
    kit::KTextArea* multiline_ = nullptr;
    ChannelChecklistField* checklist_ = nullptr;
    KeyframeDiamond* diamond_ = nullptr;
    // #512: the dial before a Camera Shake angle's field, the button after its Seed field.
    kit::KAngleDial* dial_ = nullptr;
    kit::KButton* randomise_ = nullptr;
    bool refreshing_ = false;
    bool detached_ = false;
};
} // namespace bloom::ui
