# Audio Amplitude, Waveform and Spectrum

Three nodes under **Tracking & Analysis** read the sound of an Audio or Video source. Wire the source's
audio into the node's **Audio** input. They read the source's own sound, so audio effects between them
are not applied, and what they show is the same in the Viewer, in an export and in a render.

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

- A time before the sound starts or after it ends shows silence.
- If the audio file is missing or has changed since it was imported, the node reports it instead of
  drawing something wrong; relink or re-import the file.
- Heavy reading happens in the background and never blocks the interface; changing a value or the
  playhead reads only the window that is needed.
