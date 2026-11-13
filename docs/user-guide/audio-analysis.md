# Audio analysis

Eight nodes under **Tracking & Analysis** read sound so a picture can move with it. Wire any audio
output into the node's **Audio** input: an Audio or Video source, an audio effect, a layer's audio
output, or a Layer Stack for the whole mix. The node reads the sound as it is at that point, so an
Equalizer before the node changes what it reads, and a layer's level, in and out points and time
remapping all count. What a node gives is the same in the Viewer, on a seek, in an export and in a
render, and never depends on whether anything has played.

The first time a sound is read, Bloom renders it once in the background and measures it from start
to end. A long sound takes a moment; the status bar shows the progress ("Rendering sound for
analysis", then "Analyzing sound"). After that every frame
reads the stored measurements, until the sound itself changes.

## Audio Bands

The level of a range of frequencies as one number from 0 to 1, ready for any number input such as
Scale, Opacity or Position.

- **Lowest frequency** and **Highest frequency** choose the range (20 Hz to 250 Hz at first, which
  is the bass). A kick drum sits around 40 to 120 Hz, voices around 200 Hz to 3 kHz, cymbals above
  6 kHz.
- **Channel** reads the **Mix**, the **Left** or **Right** channel, the **Mid** (what both channels
  share) or the **Side** (what differs between them).
- **Normalize** decides what counts as 1:
  - **95th percentile** (the default): the level the sound stays under 95 % of the time. Loud
    passages reach 1 without a single peak setting the scale.
  - **Peak**: the loudest moment of the whole sound.
  - **Rolling window**: the loudest moment of the last **Rolling window** seconds (4 s), so a
    quiet section still moves.
  - **Manual range**: **Manual floor** reads 0 and **Manual ceiling** reads 1, in decibels.
- **Range** (48 dB) is how far below the reference reads 0. A smaller range makes the value jump
  more; a larger one keeps quiet detail.
- **Attack** and **Release** (milliseconds) smooth the value as it rises and as it falls. A short
  attack with a longer release gives the familiar bounce. They are worked out from the start of the
  sound, so a frame looks the same however you got to it.
- **Time offset** reads the sound earlier or later. A small positive offset makes motion start just
  before a hit lands.

## Audio Sample

The spectrum or the waveform at one place across it.

- **Read** chooses **Spectrum** (how loud each frequency is, low to high) or **Waveform** (the shape
  of the sound around the frame).
- **Position** (0 to 1) is where across it to read; **Value** gives the number there. Drive
  **Position** from a function's argument inside a Function group and Function Plot draws the
  whole spectrum or waveform as a path.
- **Spectrum** and **Waveform** give the whole thing as a number list: 64 levels from 0 to 1 for the
  spectrum (evenly spaced from 20 Hz to 20 kHz the way pitch is heard), and 256 samples from -1 to 1
  for the waveform.
- **Channel** applies to the spectrum; the waveform is the two channels mixed. **Floor** and
  **Ceiling** (in decibels) set which levels read 0 and 1 in the spectrum. **Waveform window** is
  how much sound the waveform spans. **Time offset** works as in Audio Bands.

## Audio Features

Things about the sound other than how loud it is.

- **Hit** jumps to 1 when a sound starts (a drum hit, a plucked note) and falls back to 0 over
  **Hit pulse length**. **Since hit** is the seconds since the last one and **Hit count** how many
  there have been, which is handy for stepping through a set of values. **Hit sensitivity** sets
  how far a start has to stand out from the sound around it, and **Shortest gap between hits**
  ignores starts that follow too closely.
- **Brightness** is where the sound sits between low and high, 0 to 1 (the spectral centroid): a
  bass note is dark, a cymbal bright.
- **Noisiness** is 0 for a pure tone and approaches 1 for hiss or a snare.
- **Pan** runs from -1 (all left) to 1 (all right); **Width** is 0 for mono and 1 when the channels
  are opposite.
- **Total** adds the level up as the sound plays. It only moves forward, so it suits rotation or
  travel that should never run backwards.

## Audio Beat

Finds a steady beat. **Phase** ramps from 0 to 1 across each beat, **Pulse** is 1 on the beat and
falls to 0 by the next, **Beat** counts them and **Tempo** is the beats per minute. **Slowest
tempo** and **Fastest tempo** (60 and 180) bound the search; narrow them if the node picks half or
double the tempo you hear.

## Audio Pitch

**Classes** is a list of twelve numbers, one for each note name from C to B, that add up to 1: how
much of the pitched sound belongs to each. **Strongest** is the number of the leading note (0 is C,
9 is A; -1 when there is no pitch) and **Strength** is its share.

## Baking to keyframes

Right-click any of these nodes (or Audio Amplitude, or any other node that gives a number) and
choose **Bake to Keyframes**. A node with several number outputs lists them; pick the one to bake.
Bloom reads the value at every frame of the composition in the background and writes the result
as keyframes on a new Number node beside the node, in one undo step. The status bar shows how far
the bake is, and the same menu offers **Cancel Bake to Keyframes** while it runs; a cancelled bake
writes nothing. The keys are ordinary keyframes: move them, delete them, change their easing. They
do not follow the sound afterwards; bake again to refresh them. The live node stays where it is,
so both can be used. A composition longer than 100,000 frames is not baked.

## Audio Amplitude

Gives a number that follows the loudness of the sound at the playhead. **Measure** chooses **Peak** (the
largest sample) or **RMS** (the average power, closer to how loud it sounds, and the default). **Unit**
is **Linear** (0 to about 1) or **Decibels** (silence is -120 dB). **Window** (50 ms) is how much sound
around the frame is measured, and **Time offset** (0 s) looks earlier or later. Wire the **Amplitude**
output into any number socket to make a picture react to the sound, then use Map Range to set how far.

## Audio Waveform

Draws the shape of the sound around the playhead as an image. **Width** and **Height** set the picture
in pixels (1024 by 256), **Window** (2 s) is how much sound it shows, **Channels** mixes them into one
trace or stacks a lane for each channel (**Separate lanes**), **Vertical scale** (1) makes the trace taller, and
**Waveform color** and **Background** set the colors. Both colors can be brighter than white for HDR
work and the background can be transparent.

## Audio Spectrum

Draws how loud each frequency is, as bars. **Transform size** (2048) trades detail in the low
frequencies (bigger) against following quick changes (smaller). **Frequency axis** is **Logarithmic**
(each octave gets the same room, the default) or **Linear**. **Lowest frequency** (20 Hz) and **Highest
frequency** (20000 Hz) set the range, and **Floor** (-90 dB) and **Ceiling** (0 dB) set which levels
fill the picture from empty to full. **Bar color** and **Background** work as in the Waveform.

## Good to know

- A time before the sound starts or after it ends reads silence.
- If an audio file is missing or has changed since it was imported, the node reports it instead of
  giving a wrong answer; relink or re-import the file.
- A sound cannot be driven by its own analysis: wiring a node's output into an effect on the very
  sound it reads is refused, because the result would depend on itself.
- Audio Beat follows a steady pulse. It can settle on half or double the tempo, and it does not
  follow music whose tempo drifts or changes; set the tempo range, or bake and adjust by hand.
- Audio Pitch reports what is sounding, overtones included, so a single low note also shows a
  little of the note a fifth above it, and drums smear across all twelve.
- Heavy reading happens in the background and never blocks the interface.
