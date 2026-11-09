#pragma once

#include <bloom/runtime/memory_headroom.hpp>

// The application's one typed owner of its global (user-wide, non-project) preferences.
//
// Settings used to be read and written ad hoc at each call site: a panel read a QSettings key in
// its constructor and wrote it back from a menu toggle, with the key string and the default spelled
// again in both places. That is fine while exactly one surface edits a key, and it stops being fine
// the moment a Preferences window is a second writer. This header is the single definition of every
// global key, its default, and its stored spelling, so the window, the panels, and the tests cannot
// disagree about any of them.
//
// Scope: global preferences only. Per-area viewer state (`viewer/analysis/<area>`,
// `viewer/display-view/<revision>/<area>`, `viewer/look/<area>`), per-composition safe-area
// presets, per-project export settings, the legacy `appearance/chrome` key, window geometry,
// the workspace layout, and the user's composition presets (the `compositionPresets` array, owned
// by composition_presets.hpp) are deliberately NOT here.
// Project truth lives in the document; these are session preferences that must never influence
// evaluation semantics (docs/architecture/overview.md, "State Categories").
//
// This is a public bloom_ui header (src/ui/include/bloom/ui), not a private one: the
// PreferencesAware interface in preferences_aware.hpp is a public editor base class and names
// ApplicationPreferences, so any target that includes a public editor header already sees this
// type. Placement does not widen its ownership: it remains the single definition of every global
// key, default, and stored spelling, and no target outside bloom_ui should write global
// preferences directly.

#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <cstdint>
#include <optional>
#include <string>

class QSettings;

namespace bloom::ui {

enum class TimelineTimeFormat : std::uint8_t {
    Frames,
    Timecode,
};

enum class NodeLinkStyle : std::uint8_t {
    Spline,
    Straight,
    Angled,
};

enum class ViewerResolutionPreference : std::uint8_t {
    Auto,
    Full,
    Half,
    Quarter,
};

// The Viewer's surround outside the composition frame (ViewerBackground).
enum class ViewerBackgroundPreference : std::uint8_t {
    Solid,
    Grey,
    Black,
    White,
};

// One value per global key. Byte budgets of 0 mean "use the machine-derived default", exactly as
// the existing readers treat a missing, zero, or unparseable override. An empty
// mediaDiskCacheDirectory means "use the platform cache directory".
struct ApplicationPreferences final {
    // General / playback.
    bool audioEnabled = true;
    bool loopPlayback = true;
    // Crash recovery (#572): off by default. Off writes no recovery data and offers none at
    // startup; recovery files already on disk are left untouched. Applied live.
    bool crashRecoveryEnabled = false;

    // Memory and caches. Headroom is what Bloom leaves free for the rest of the computer (0 to 256
    // GiB each); Bloom uses everything else. Applied live on Apply and OK. The defaults are
    // computed when the preferences are built: 10% of RAM (or of a tighter process limit) and of
    // VRAM.
    std::uint64_t ramHeadroomBytes = runtime::defaultRamHeadroomBytes();
    std::uint64_t vramHeadroomBytes = runtime::defaultVramHeadroomBytes();
    bool mediaDiskCacheEnabled = true;
    std::string mediaDiskCacheDirectory;
    std::uint64_t mediaDiskCacheBudgetBytes = 0;
    // Export workers (#581). Opt-in limits on the media worker processes an export launches: the
    // most memory one may hold (0 = no limit) and its encoder threads (0 = automatic). Applied to
    // every export that starts after the change; no restart.
    std::uint64_t exportWorkerMemoryCapBytes = 0;
    std::uint32_t exportWorkerThreads = 0;

    // Timeline.
    TimelineTimeFormat timelineTimeFormat = TimelineTimeFormat::Frames;
    bool timelineSnapping = true;
    bool timelineKeyframesVisible = true;
    bool timelineGraphEditor = false;
    // 0 means "use TimelineEditor's own default width".
    int timelineLayerColumnWidth = 0;

    // Node graph.
    NodeLinkStyle nodeLinkStyle = NodeLinkStyle::Spline;
    bool nodeSnap = false;
    double nodeGridSize = kit::px(kit::Size::NodeGrid);

    // Viewer.
    ViewerResolutionPreference viewerResolution = ViewerResolutionPreference::Auto;
    ViewerBackgroundPreference viewerBackground = ViewerBackgroundPreference::Solid;
    // The checkerboard inside the composition frame. Off by default (as in a layer-based
    // compositor), so the composition's own background colour shows.
    bool viewerTransparencyGrid = false;
    bool viewerSafeAreas = false;
    bool viewerCentreCross = false;
    bool viewerThirds = false;
    bool viewerRulers = false;
    bool viewerPixelGrid = false;

    // Import (#381). What a newly imported image sequence assumes as its frame rate (absent: one
    // image per composition frame) and how long a newly imported still lasts (absent: the
    // composition's length). The import command writes them into each new asset's
    // interpretation; assets already imported keep theirs.
    std::optional<document::FrameRate> importSequenceFrameRate =
        document::FrameRate::framesPerSecond24();
    std::optional<core::RationalTime> importStillDuration;

    // Sounds and notifications (docs/ux/sound.md). Event sounds (render and import results,
    // warnings, refusals) are on by default; interaction sounds are opt-in.
    bool soundsEnabled = true;
    // 0 to 100.
    int soundVolumePercent = 40;
    bool eventSoundsEnabled = true;
    bool interactionSoundsEnabled = false;
    // An operating-system notification for a render result while Bloom is not the active window.
    bool renderNotificationsEnabled = true;

    // Agent routes that act on the machine rather than the project. Both are off by default and
    // only this model (the Preferences window) turns them on: the MCP preferences.set refuses to.
    // Answer open dialogs: the dialog.answer tool may press a button of the dialog that blocks the
    // window (#579). Run Python scripts: the script.run tool may run Python in the live session
    // (#603).
    bool agentsAnswerDialogs = false;
    bool agentsRunScripts = false;

    friend bool operator==(const ApplicationPreferences&, const ApplicationPreferences&) = default;
};

// The defaults above, named so a Reset affordance and a test share one definition.
[[nodiscard]] ApplicationPreferences defaultApplicationPreferences() noexcept;

// Reads every global key, falling back to the default for a missing, unparseable, or out-of-range
// value. A relative media-disk-cache directory reads as empty (the existing
// mediaDiskCacheDirectoryFromSettings() rule), and an unrecognized enum spelling reads as its
// default.
[[nodiscard]] ApplicationPreferences loadApplicationPreferences(const QSettings& settings);

// Writes exactly the keys this model owns, in the spellings the existing readers parse. A 0 byte
// budget is written as 0 (the readers treat it as "default"); a relative media disk cache directory
// is refused and cleared rather than silently persisted.
void saveApplicationPreferences(QSettings& settings, const ApplicationPreferences& preferences);

// Stored spellings, exposed so a test and a UI control can share the exact vocabulary the readers
// already use instead of re-spelling it.
[[nodiscard]] const char* timelineTimeFormatValue(TimelineTimeFormat format) noexcept;
[[nodiscard]] const char* nodeLinkStyleValue(NodeLinkStyle style) noexcept;
[[nodiscard]] const char* viewerResolutionValue(ViewerResolutionPreference policy) noexcept;
[[nodiscard]] const char* viewerBackgroundValue(ViewerBackgroundPreference background) noexcept;

// The stored Viewer surround and Transparency Grid, read with the one migration rule both the
// Preferences model and the Viewer use: the pre-split "Checkerboard" background (which drew the
// checker in the frame) reads as Solid with the grid on, unless a grid value was stored since, so
// an old choice keeps showing a checker and a later grid choice is never overridden. Missing or
// unrecognized values read as Solid with the grid off.
struct ViewerBackdropSetting final {
    ViewerBackgroundPreference background = ViewerBackgroundPreference::Solid;
    bool transparencyGrid = false;
};
[[nodiscard]] ViewerBackdropSetting viewerBackdropFromSettings(const QSettings& settings);
inline constexpr auto kViewerBackgroundSettingKey = "viewer/background";
inline constexpr auto kViewerTransparencyGridSettingKey = "viewer/transparency-grid";

} // namespace bloom::ui
