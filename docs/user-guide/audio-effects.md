# Audio Effects

Five nodes change how an audio layer sounds: **Equalizer**, **Compressor**, **Auto Level**, **Mono** and
**Hiss**. Find them under **Sources** in the Add menu. Wire an audio source into an effect's **Audio**
input, then wire its **Audio** output into the layer's audio input. Effects can follow each other; the
sound passes through them in the order they are wired. An effect that is muted is skipped, so the sound
comes through unchanged, and an effect with nothing connected is silent. What you hear while playing is
what exports: the same processing runs in both.

Every parameter can be animated. The notes below give the starting value of each.

## Equalizer

- **High pass** and **Low pass** cut everything below or above a frequency, with a steep slope of 24 dB per
  octave. At 20 Hz and 20000 Hz they do nothing.
- **Low**, **Mid** and **High** are three bands. Each has a **Gain** in decibels, where 0 dB leaves the sound
  alone, and a **Frequency**. The low band is a shelf starting at 120 Hz, the high band a shelf starting at
  6000 Hz, and the mid band a bell at 1000 Hz with a **Q** that narrows it as it grows.

## Compressor

The Compressor turns loud parts down. Above the **Threshold** (-18 dB), every **Ratio** (3) decibels of
extra level come out as one. **Attack** (10 ms) is how fast it reacts, **Release** (150 ms) how fast it lets
go, **Knee** (6 dB) rounds the corner at the threshold, and **Makeup** (0 dB) raises the result.

## Auto Level

Auto Level evens out a recording that gets louder and quieter. It measures the level over about a third
of a second and moves it toward the **Target** (-20 dB), by at most **Max gain** (18 dB), at the speed of
**Response** (1 s). It turns down faster than it turns up. Below the **Gate** (-55 dB) it holds its gain, so
silence is not boosted into noise.

## Mono

**Amount** (100%) blends the channels together. At 100% both sides carry the same mix; at 0% the sound is
unchanged.

## Hiss

Hiss adds steady noise. **Level** (-50 dB) is its loudness, **Tone** (1000 Hz) removes the noise below that
frequency (at 20 Hz it is plain white noise), and **Seed** picks which noise it is, so a render is
repeatable.

## Playback

While you play, a value that is changing over time is refreshed with the transport, so a fast animation
can sound stepped on screen; the export follows it smoothly. See
[Exporting video](exporting-video.md) for how audio is delivered, including **Normalize loudness**.
