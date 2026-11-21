#pragma once

// #752: the edits of a Subtitles node's cue list (docs/architecture/subtitles.md, "Operations").
// Each is one operation and so one undo step; each replaces the node's cue-list constant with a new
// list that shares every cue it did not touch (cues are immutable shared records), keeps every
// cue id, and changes only parameters, so its render footprint is proven from the list diff (the
// changed cues' spans; render_footprint.cpp). A cue list another node supplies (an edge into the
// node's Cues input, or a driver) is refused, naming that node; a list linked to a file is refused
// by every edit but load, reload and unlink. Reading files happens before an operation is built
// (off the UI thread, by the caller); an operation only stores the list it was given.

#include <bloom/commands/operation.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/cue_list.hpp>
#include <bloom/document/ids.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace bloom::commands {

// What every cue-list edit reports: one "cue" output per cue it added or changed (for a load or
// reload, per cue of the list it stored, so their count is the list's), one "removedCue" per cue it
// removed. InsertCue's one "cue" output is the new cue's id.
inline constexpr std::string_view kCueOutput = "cue";
inline constexpr std::string_view kRemovedCueOutput = "removedCue";
inline constexpr std::string_view kInsertCueOutput = kCueOutput;

// A cue named by its id, or by its position in the sorted list (0 first).
struct CueById final {
    std::uint64_t id = 0;
};
struct CueByIndex final {
    std::size_t index = 0;
};
using CueSelector = std::variant<CueById, CueByIndex>;

// The members of a cue an edit sets; each absent member keeps the cue's own. An empty `speaker` or
// `style` clears it; `placement` replaces the whole placement (an empty one clears it).
struct CueEdit final {
    std::optional<core::RationalTime> start;
    std::optional<core::RationalTime> end;
    std::optional<std::string> text;
    std::optional<std::string> speaker;
    std::optional<std::string> style;
    std::optional<document::CuePlacement> placement;
};

// The base of every cue-list edit: one Subtitles node of one composition.
class CueListEdit : public Operation {
  public:
    CueListEdit(const document::CompositionId composition, const document::NodeId node)
        : composition_(composition), node_(node) {}

    [[nodiscard]] OperationResult apply(document::Draft& draft) const final;
    [[nodiscard]] std::optional<document::CompositionId>
    parameterEditScope() const noexcept override {
        return composition_;
    }

  protected:
    // Whether a list linked to a file may be edited (load, reload and unlink replace or release
    // the link; every other edit is refused).
    [[nodiscard]] virtual bool editsLinkedList() const noexcept { return false; }
    // Whether the "cue" outputs name every cue of the stored list, not only the changed ones.
    [[nodiscard]] virtual bool reportsWholeList() const noexcept { return false; }
    // Warnings the applied edit publishes (CommandResult::warnings).
    [[nodiscard]] virtual std::vector<std::string> warnings() const { return {}; }
    // The new list from the current one; a rejected result refuses the edit. `outputs` receives
    // what the operation reports besides its cue outputs.
    [[nodiscard]] virtual std::variant<document::CueListValue, OperationResult>
    edit(const document::CueListValue& current, std::vector<OperationOutput>& outputs) const = 0;

    document::CompositionId composition_;
    document::NodeId node_;
};

// bloom.subtitles.set-cues: the whole list replaced. A cue that names an id keeps it (ids must be
// unique); the others take new ids. Styles, language and provenance are replaced when given.
class SetCues final : public CueListEdit {
  public:
    SetCues(document::CompositionId composition, document::NodeId node,
            std::vector<document::Cue> cues, std::optional<std::vector<document::CueStyle>> styles,
            std::optional<std::string> language, std::optional<document::CueProvenance> provenance)
        : CueListEdit(composition, node), cues_(std::move(cues)), styles_(std::move(styles)),
          language_(std::move(language)), provenance_(std::move(provenance)) {}

    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] std::size_t workUnits() const noexcept override {
        return cues_.empty() ? 1 : cues_.size();
    }

  private:
    [[nodiscard]] std::variant<document::CueListValue, OperationResult>
    edit(const document::CueListValue& current,
         std::vector<OperationOutput>& outputs) const override;

    std::vector<document::Cue> cues_;
    std::optional<std::vector<document::CueStyle>> styles_;
    std::optional<std::string> language_;
    std::optional<document::CueProvenance> provenance_;
};

// bloom.subtitles.insert-cue: one cue added with the list's next id (its `id` is ignored).
class InsertCue final : public CueListEdit {
  public:
    InsertCue(document::CompositionId composition, document::NodeId node, document::Cue cue)
        : CueListEdit(composition, node), cue_(std::move(cue)) {}

    [[nodiscard]] std::string_view typeId() const noexcept override;

  private:
    [[nodiscard]] std::variant<document::CueListValue, OperationResult>
    edit(const document::CueListValue& current,
         std::vector<OperationOutput>& outputs) const override;

    document::Cue cue_;
};

// bloom.subtitles.edit-cue: one cue's members changed.
class EditCue final : public CueListEdit {
  public:
    EditCue(document::CompositionId composition, document::NodeId node, CueSelector cue,
            CueEdit edit)
        : CueListEdit(composition, node), cue_(cue), edit_(std::move(edit)) {}

    [[nodiscard]] std::string_view typeId() const noexcept override;

  private:
    [[nodiscard]] std::variant<document::CueListValue, OperationResult>
    edit(const document::CueListValue& current,
         std::vector<OperationOutput>& outputs) const override;

    CueSelector cue_;
    CueEdit edit_;
};

// bloom.subtitles.remove-cues.
class RemoveCues final : public CueListEdit {
  public:
    RemoveCues(document::CompositionId composition, document::NodeId node,
               std::vector<CueSelector> cues)
        : CueListEdit(composition, node), cues_(std::move(cues)) {}

    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] std::size_t workUnits() const noexcept override {
        return cues_.empty() ? 1 : cues_.size();
    }

  private:
    [[nodiscard]] std::variant<document::CueListValue, OperationResult>
    edit(const document::CueListValue& current,
         std::vector<OperationOutput>& outputs) const override;

    std::vector<CueSelector> cues_;
};

// bloom.subtitles.shift-cues: the chosen cues (every cue when none are named) moved by `by`.
class ShiftCues final : public CueListEdit {
  public:
    ShiftCues(document::CompositionId composition, document::NodeId node,
              std::optional<std::vector<CueSelector>> cues, core::RationalTime by)
        : CueListEdit(composition, node), cues_(std::move(cues)), by_(by) {}

    [[nodiscard]] std::string_view typeId() const noexcept override;

  private:
    [[nodiscard]] std::variant<document::CueListValue, OperationResult>
    edit(const document::CueListValue& current,
         std::vector<OperationOutput>& outputs) const override;

    std::optional<std::vector<CueSelector>> cues_;
    core::RationalTime by_;
};

// bloom.subtitles.scale-cues: the chosen cues' times t become origin + (t - origin) * factor, a
// positive exact ratio (a frame-rate conversion, 1001/1000 or 25/24).
class ScaleCues final : public CueListEdit {
  public:
    ScaleCues(document::CompositionId composition, document::NodeId node,
              std::optional<std::vector<CueSelector>> cues, core::RationalTime factor,
              core::RationalTime origin)
        : CueListEdit(composition, node), cues_(std::move(cues)), factor_(factor),
          origin_(origin) {}

    [[nodiscard]] std::string_view typeId() const noexcept override;

  private:
    [[nodiscard]] std::variant<document::CueListValue, OperationResult>
    edit(const document::CueListValue& current,
         std::vector<OperationOutput>& outputs) const override;

    std::optional<std::vector<CueSelector>> cues_;
    core::RationalTime factor_;
    core::RationalTime origin_;
};

// bloom.subtitles.load and bloom.subtitles.reload: the list replaced by one read from a file or
// text (its provenance a linked file, or authored). The new cues take the ids of the cues they
// match in the current list (document::reconcileCueIds), so references to unchanged cues hold.
// Reload is what the linked-file monitor publishes (with host origin), and refuses a list that is
// not linked.
class ReplaceCues final : public CueListEdit {
  public:
    enum class Kind : std::uint8_t { Load, Reload };
    // `warnings` are the read's report rows, published as the operation's warnings.
    ReplaceCues(document::CompositionId composition, document::NodeId node,
                document::CueListValue cues, Kind kind, std::vector<std::string> warnings = {})
        : CueListEdit(composition, node), cues_(std::move(cues)), kind_(kind),
          warnings_(std::move(warnings)) {}

    [[nodiscard]] std::string_view typeId() const noexcept override;
    [[nodiscard]] std::size_t workUnits() const noexcept override {
        return cues_.cues.empty() ? 1 : cues_.cues.size();
    }

  private:
    [[nodiscard]] bool editsLinkedList() const noexcept override { return true; }
    [[nodiscard]] std::variant<document::CueListValue, OperationResult>
    edit(const document::CueListValue& current,
         std::vector<OperationOutput>& outputs) const override;

    [[nodiscard]] bool reportsWholeList() const noexcept override { return true; }
    [[nodiscard]] std::vector<std::string> warnings() const override { return warnings_; }

    document::CueListValue cues_;
    Kind kind_;
    std::vector<std::string> warnings_;
};

// bloom.subtitles.unlink: the list stops following its file and becomes the project's own.
class UnlinkCues final : public CueListEdit {
  public:
    UnlinkCues(document::CompositionId composition, document::NodeId node)
        : CueListEdit(composition, node) {}

    [[nodiscard]] std::string_view typeId() const noexcept override;

  private:
    [[nodiscard]] bool editsLinkedList() const noexcept override { return true; }
    [[nodiscard]] std::variant<document::CueListValue, OperationResult>
    edit(const document::CueListValue& current,
         std::vector<OperationOutput>& outputs) const override;
};

// Where a node's cue list comes from, for a caller that reads it before building an edit.
struct CueListLocation final {
    document::ParameterId parameter;
    const document::CueListValue* list = nullptr;
};
// The Subtitles node's own cue list, or the refusal any edit of it gives (`allowLinked` as
// CueListEdit::editsLinkedList).
[[nodiscard]] std::variant<CueListLocation, OperationResult>
locateCueList(const document::Composition& composition, document::NodeId node, bool allowLinked);

} // namespace bloom::commands
