#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace bloom::ui {

// Playback preroll (#404): on Play, the transport clock holds until the first frames are ready,
// then starts in sync. A hold never lasts longer than the maximum; past it the clock starts anyway
// and the ordinary dropped-frame behavior applies from there. Frames that are already cached (a
// RAM preview) start at once, with no hold.
//
// This is the state machine only. It owns no clock and no cache: the caller passes the injected
// clock's reading and whether the first frames are ready, so tests drive every transition with a
// manual clock and no wall-clock sleeps (docs/architecture/animation-and-time.md, "Playback
// preroll").
class PlaybackPreroll final {
  public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::chrono::milliseconds kMaximumHold{500};

    enum class Outcome : std::uint8_t {
        None,       // no preroll has finished yet
        Immediate,  // the first frames were ready at Play: no hold
        Ready,      // the hold ended because the first frames became ready
        CapReached, // the frames were still not ready at the maximum hold: started anyway
        Cancelled,  // Stop or Pause during the hold
    };

    struct Result final {
        Outcome outcome = Outcome::None;
        Clock::duration held{};
        // Times the hold restarted at a new position (a seek, a scrub, a work-area change).
        std::uint32_t restarts = 0;
    };

    explicit PlaybackPreroll(Clock::duration maximumHold = kMaximumHold) noexcept
        : maximumHold_(maximumHold) {}

    // Play. Returns true when the clock must hold; false (outcome Immediate) when the first frames
    // are already ready.
    bool begin(Clock::time_point now, bool ready) noexcept {
        holding_ = false;
        restarts_ = 0;
        if (ready) {
            last_ = {.outcome = Outcome::Immediate};
            return false;
        }
        holding_ = true;
        began_ = now;
        restartedAt_ = now;
        return true;
    }

    // A seek during the hold: the hold starts over at the new position, with a fresh maximum.
    void restart(Clock::time_point now) noexcept {
        if (!holding_)
            return;
        restartedAt_ = now;
        ++restarts_;
    }

    // While holding: nullopt keeps holding; a result means the clock starts now.
    [[nodiscard]] std::optional<Result> poll(Clock::time_point now, bool ready) noexcept {
        if (!holding_)
            return std::nullopt;
        if (ready)
            return finish(now, Outcome::Ready);
        if (now >= restartedAt_ && now - restartedAt_ >= maximumHold_)
            return finish(now, Outcome::CapReached);
        return std::nullopt;
    }

    // Stop or Pause during the hold. No-op when not holding.
    void cancel(Clock::time_point now) noexcept {
        if (holding_)
            (void)finish(now, Outcome::Cancelled);
    }

    [[nodiscard]] bool holding() const noexcept { return holding_; }
    // Time left before the maximum releases the clock; zero when not holding.
    [[nodiscard]] Clock::duration remaining(Clock::time_point now) const noexcept {
        if (!holding_ || now < restartedAt_)
            return holding_ ? maximumHold_ : Clock::duration::zero();
        const auto spent = now - restartedAt_;
        return spent >= maximumHold_ ? Clock::duration::zero() : maximumHold_ - spent;
    }
    [[nodiscard]] Clock::duration maximumHold() const noexcept { return maximumHold_; }
    // The last finished preroll: how it ended and how long the clock held.
    [[nodiscard]] const Result& last() const noexcept { return last_; }

  private:
    Result finish(Clock::time_point now, Outcome outcome) noexcept {
        holding_ = false;
        last_ = {.outcome = outcome,
                 .held = now >= began_ ? now - began_ : Clock::duration::zero(),
                 .restarts = restarts_};
        return last_;
    }

    Clock::duration maximumHold_;
    Clock::time_point began_{};
    Clock::time_point restartedAt_{};
    std::uint32_t restarts_ = 0;
    bool holding_ = false;
    Result last_{};
};

} // namespace bloom::ui
