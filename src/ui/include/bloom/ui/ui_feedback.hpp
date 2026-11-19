#pragma once

// Typed UI feedback: the one place that decides whether an application event is heard or shown.
//
// Callers emit semantic events (FeedbackEvent), never file names. The service maps an event to a
// sound and a category (Event or Interaction), checks the Preferences switches, rate-limits, and
// enqueues the sound on a SoundPlayer. It never decodes, reads a file, or touches an audio device
// itself; those live behind the SoundPlayer seam (ui_sound_engine.hpp), which tests replace with a
// fake. It also posts an operating-system notification for a render result when Bloom is not the
// active application, through the Notifier seam (system_notifier.hpp).
//
// Agent activity (issue #746) is told apart from the artist's own: an agent's render, export or
// capture never plays a sound or posts a notification of its own. It is counted into one summary
// ("Agent: 37 renders, 1 failed") that is posted once the agent has been quiet for
// kAgentQuietInterval, and only while Bloom is not the active application; a composition an agent
// leaves unable to compile is warned once per composition per interval. An agent session that
// asked to be quiet holds all of it until the session ends. The Preferences switch "Sounds and
// notifications for agent activity" (FeedbackSettings::agentActivity) turns the summaries and
// warnings off entirely.
//
// The application owns one UiFeedback and passes it explicitly to the few call sites that need it
// (docs/architecture/ui-feedback.md). There is no global instance, locator or event bus.

#include <QString>

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>

class QTimer;

namespace bloom::ui {

struct ApplicationPreferences;

// A meaning, not a sound. Event kinds report a result the artist may be waiting for; Interaction
// kinds acknowledge a gesture and are off by default.
enum class FeedbackEvent : std::uint8_t {
    // Events.
    RenderSucceeded,
    RenderFailed,
    ImportFinished,
    ImportFailed,
    Warning,
    Refused,
    // Interactions.
    ControlPress,
    Select,
    ToggleOn,
    ToggleOff,
    ModalOpen,
    ModalClose,
    ToolSwitch,
    DragStart,
    Drop,
    Disabled,
};

enum class FeedbackCategory : std::uint8_t { Event, Interaction };

// The bundled, processed SND01 "sine" sounds (src/ui/sound/assets). Several events share one.
enum class FeedbackSound : std::uint8_t {
    RenderSucceeded,
    RenderFailed,
    ImportFinished,
    Refused,
    Button,
    Select,
    ToggleOn,
    ToggleOff,
    ModalOpen,
    ModalClose,
    ToolSwitch,
    DragStart,
    Drop,
};
inline constexpr std::size_t kFeedbackSoundCount = 13;

[[nodiscard]] FeedbackCategory feedbackCategory(FeedbackEvent event) noexcept;
[[nodiscard]] FeedbackSound feedbackSound(FeedbackEvent event) noexcept;
// A per-event trim on top of the sound's own level, for an event that reuses a louder sound.
[[nodiscard]] float feedbackGain(FeedbackEvent event) noexcept;
// The Qt resource file stem of a sound ("render_succeeded" for src/ui/sound/assets/...).
[[nodiscard]] const char* feedbackSoundName(FeedbackSound sound) noexcept;
[[nodiscard]] const char* feedbackEventName(FeedbackEvent event) noexcept;

// The Preferences state that governs feedback (application_preferences.hpp owns the stored form).
struct FeedbackSettings final {
    bool soundsEnabled = true;
    // 0 to 100; 100 plays the bundled level, which is already quiet.
    int volumePercent = 40;
    bool eventSounds = true;
    bool interactionSounds = false;
    bool renderNotifications = true;
    // #746: summaries and once-per-composition warnings for agent activity. Off is silent.
    bool agentActivity = true;

    friend bool operator==(const FeedbackSettings&, const FeedbackSettings&) = default;
};

// What an agent did since the last summary (#746).
struct AgentActivityCounts final {
    std::uint32_t renders = 0;
    std::uint32_t failed = 0;
    // Distinct compositions an agent's edit left unable to compile.
    std::uint32_t brokenCompositions = 0;

    [[nodiscard]] bool empty() const noexcept { return renders == 0 && brokenCompositions == 0; }
    friend bool operator==(const AgentActivityCounts&, const AgentActivityCounts&) = default;
};

// "Agent: 37 renders, 1 failed", "Agent session “Shot 12”: 1 render, 2 compositions can't
// render". `heading` replaces "Agent" when it is not empty.
[[nodiscard]] QString agentActivitySummaryText(const AgentActivityCounts& counts,
                                               const QString& heading = {});

[[nodiscard]] FeedbackSettings feedbackSettingsFrom(const ApplicationPreferences& preferences);

// What a finished batch import is heard as: ImportFinished when everything landed, Warning when
// some items were skipped, ImportFailed when nothing was added, and nothing for a cancelled or
// discarded import. `failure` is true when the import reported a reason it added nothing.
[[nodiscard]] std::optional<FeedbackEvent> importFeedbackEvent(bool cancelled, bool discarded,
                                                               std::size_t imported,
                                                               std::size_t skipped, bool failure);

// Plays a decoded sound. play() only enqueues: it never blocks, decodes, or opens a device.
class SoundPlayer {
  public:
    virtual ~SoundPlayer() = default;
    virtual void play(FeedbackSound sound, float gain) noexcept = 0;
    // Releases the audio output. Idempotent; play() after it is a no-op.
    virtual void shutdown() noexcept = 0;
};

struct SystemNotification final {
    QString title;
    QString body;
    bool failure = false;
};

// Posts an operating-system notification. Implementations never block the caller.
class Notifier {
  public:
    virtual ~Notifier() = default;
    virtual void post(const SystemNotification& notification) = 0;
    // Called (on the UI thread) when the artist clicks a notification this notifier posted.
    virtual void setActivationHandler(std::function<void()> handler) = 0;
};

class UiFeedback final {
  public:
    using Clock = std::function<std::chrono::steady_clock::time_point()>;
    // True while Bloom is the active application.
    using ActiveProbe = std::function<bool()>;

    // The same sound is not enqueued twice within this window (an event followed at once by the
    // warning dialog that reports it is heard once).
    static constexpr std::chrono::milliseconds kMinimumRepeat{80};

    // Any argument may be empty: no player is silent, no notifier posts nothing, and the default
    // probe and clock are the application's own.
    explicit UiFeedback(std::unique_ptr<SoundPlayer> player,
                        std::unique_ptr<Notifier> notifier = nullptr, ActiveProbe isActive = {},
                        Clock clock = {});
    ~UiFeedback();
    UiFeedback(const UiFeedback&) = delete;
    UiFeedback& operator=(const UiFeedback&) = delete;

    void setSettings(const FeedbackSettings& settings) noexcept;
    [[nodiscard]] const FeedbackSettings& settings() const noexcept { return settings_; }

    // Plays `event` if the master switch, its category and the rate limit allow it. Returns whether
    // a sound was enqueued.
    bool play(FeedbackEvent event);
    // The settings-only part of play(), for callers that avoid building a cue that would be muted.
    [[nodiscard]] bool wouldPlay(FeedbackEvent event) const noexcept;

    // Interaction sounds are muted while the artist plays back or scrubs/drags a canvas. The drag
    // and drop cues themselves are never muted by a gesture: they are the gesture's feedback.
    enum class Suppression : std::uint8_t { Playback = 1, Gesture = 2 };
    void setInteractionSuppressed(Suppression reason, bool on) noexcept;
    [[nodiscard]] bool interactionSuppressed() const noexcept { return suppression_ != 0; }

    // A render or export finished. Plays RenderSucceeded or RenderFailed and, when the Preferences
    // switch is on and Bloom is not the active application, posts an operating-system notification.
    void announceRender(bool succeeded, const QString& detail);

    // ---- Agent activity (#746).

    // How long an agent must stay quiet before its summary is posted; two summaries are never
    // closer than this.
    static constexpr std::chrono::seconds kAgentQuietInterval{10};

    // An agent's (transient) render, export or capture finished. No sound and no notification:
    // it is counted into the next summary.
    void noteAgentRender(bool succeeded);
    // An agent's edit left `composition` unable to compile. Plays Warning at most once per
    // composition per kAgentQuietInterval, never while held and never with the switch off.
    // Returns whether a sound was enqueued.
    bool noteAgentCompileFailure(std::uint64_t composition);
    // An agent session that asked to be quiet: while held, nothing is summarized or warned. On
    // release the held activity is told at once as one summary under `heading` ("Agent session
    // “Shot 12”"): a notification and a sound while Bloom is not the active application, one
    // warning if a composition broke while it is.
    void holdAgentActivity(bool hold, const QString& heading = {});
    [[nodiscard]] bool agentActivityHeld() const noexcept { return held_; }
    // Posts the summary when it is due (the agent quiet for kAgentQuietInterval) or `force`d; a
    // summary that comes due while Bloom is the active application is dropped, since the artist
    // is watching. The internal timer calls it; tests call it after moving the clock.
    void flushAgentActivity(bool force = false);
    [[nodiscard]] const AgentActivityCounts& pendingAgentActivity() const noexcept {
        return agent_;
    }

    // Plays a bundled sample at `draft`'s volume regardless of the category switches (still
    // honouring its master switch), for the Preferences "Play test sound" button.
    bool playTest(const FeedbackSettings& draft);

    void setNotificationActivationHandler(std::function<void()> handler);
    // Releases the audio output. Safe to call more than once; the destructor calls it.
    void shutdown() noexcept;

  private:
    // `gain` is the final linear level (event trim times the volume).
    bool enqueue(FeedbackSound sound, float gain);
    void armAgentTimer();
    void tellAgentActivity(const QString& heading);

    std::unique_ptr<SoundPlayer> player_;
    std::unique_ptr<Notifier> notifier_;
    ActiveProbe isActive_;
    Clock clock_;
    FeedbackSettings settings_;
    std::uint8_t suppression_ = 0;
    bool shutDown_ = false;
    std::chrono::steady_clock::time_point lastPlayed_[kFeedbackSoundCount]{};
    bool hasPlayed_[kFeedbackSoundCount]{};
    // Agent activity since the last summary, and when the agent last did something.
    AgentActivityCounts agent_;
    std::set<std::uint64_t> agentBroken_;
    std::optional<std::chrono::steady_clock::time_point> agentLastNote_;
    // The last compile-failure warning per composition, pruned after the interval (bounded by the
    // compositions an agent broke within one interval).
    std::map<std::uint64_t, std::chrono::steady_clock::time_point> agentWarned_;
    bool held_ = false;
    std::unique_ptr<QTimer> agentTimer_;
};

} // namespace bloom::ui
