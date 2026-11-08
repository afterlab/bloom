# Frame Rate Hold

**Frame Rate Hold** shows its input at a lower frame rate than the composition, holding each picture
until the next one is due. Find it under **Time** in the Add menu. The time mapping is the one behind
Echo's Posterize time; [Motion blur](../architecture/motion-blur.md#frame-rate-hold) gives the
arithmetic and the cache and parity rules.

Connect your picture to **Input**. Everything upstream is evaluated at the held time, so animation,
footage and anything else that moves steps at the target rate. A composition at 24 frames a second
with a **Frame rate** of 12 shows every picture for two composition frames; one of 15 shows them for
one or two frames unevenly, because 15 does not divide 24.

## Controls

- **Frame rate** is the rate pictures change at, in frames a second, from 0.1 to 240. It defaults
  to 12. A whole multiple of the composition's rate (24, 48) changes nothing.
- **Irregular cadence** is off by default, which holds every picture for the same time. Turn it on
  for the uneven rhythm of a phone camera.
- **Drop and double** (shown with the irregular cadence on) is how often a picture is held
  a beat longer than its turn: from 0%, which is regular, to 100%, about one picture in four. When a
  picture is doubled, the picture that would have followed it is dropped, so the rhythm slips and
  recovers instead of drifting. A picture is never doubled twice in a row.
- **Seed** (shown with the irregular cadence on) picks which pictures are doubled. It is stored on the
  node: a new node gets a seed of its own, so two of them never stutter together, and the dice button
  beside it draws another. The same seed always gives the same cadence, in the Viewer, in a RAM
  preview and in an export.

Frame Rate Hold takes the usual effect mask. The rate, the amount and the seed are constants: they
cannot be keyed or driven.
