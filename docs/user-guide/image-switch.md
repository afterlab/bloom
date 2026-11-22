# Image Switch

An Image Switch shows one of up to eight images, chosen by a number you can key or drive: versions of
a shot to compare, a choice of plates, a different look on a cut. Only the image it shows is worked
out on each frame, so the others cost nothing while they are not shown. A **Channel Set Switch** does
the same for channel sets, such as render passes. The full specification is
[Image Switch](../architecture/image-switch.md).

## Adding one

In the Node editor, choose **Image Switch** from the Add menu (Compositing), or **Channel Set
Switch** (Channels). Connect the images to **Input 1**, **Input 2** and on, and its **Image** output
to a layer or another node.

## Choosing the input

| Row | What it does |
| --- | --- |
| **Show input** | The input shown, counted from 1. Only its whole part counts: 2.7 shows Input 2 |
| **Wrap around** | Past the last input, start again at the first (and below 1, from the last) |

The switch counts its inputs up to the highest one connected. A number past them shows the last
input, or with Wrap around goes round again. An input left unconnected below the last one shows
nothing. A muted switch shows Input 1.

Key **Show input** to change the picture over time: it steps from one input to the next at each
whole number. Connect an Integer or Scalar value node to its socket to drive it.

## Reading the choice

The **Selected** output is the input shown, as a whole number (0 when nothing is shown). Connect it
to another switch's Show input to make the two follow each other: switch a beauty render with an
Image Switch and its passes with a Channel Set Switch whose Show input reads the Image Switch's
Selected, and the passes always belong to the beauty on screen.

## Channel Set Switch

A Channel Set Switch chooses between channel sets, such as the passes of two renders. With a
constant Show input, anything can read it. With a keyed or driven Show input it can feed a Shuffle
or a Pass, which then switch with it; another node reading it (a composition's written channel
layers, Cryptomatte) shows an error asking you to switch their images instead, or to give the switch
a constant Show input.

## Speed

The inputs not shown are not read, decoded or drawn, on the CPU or the GPU, in the Viewer, RAM
Preview and exports. Going back to a frame already drawn draws nothing again, and changing an input
that is not shown does not redraw the frames that do not show it.
