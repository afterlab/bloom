# Separate Channels and Combine Channels

Separate Channels splits an image into its red, green, blue and alpha channels, and Combine
Channels builds an image from channels. Use them to rebuild an image from other images' channels,
pack render passes into one image, or pull one value out of a pass, such as a mist, depth, normal,
motion vector or ID pass. Both nodes are in the **Color** section of the Add menu and the Tab
search; searching "Separate RGB" or "Combine RGB" finds them first.

Both work on any image: footage, a nested composition, a generator or another node's result. The
values are used exactly as stored. Nothing is clamped or colour converted, so values above 1
(HDR) and below 0 pass through, and a Raw data pass keeps its exact values. Only alpha stays
between 0 and 1.

## Separate Channels

Separate Channels takes an image on **Input** and has four outputs: **Red**, **Green**, **Blue**
and **Alpha**. Each output is a grey image: that channel's value in red, green and blue, with an
alpha of 1. Images in Bloom are premultiplied, so the colour outputs of a partly transparent pixel
are its colour multiplied by its alpha.

## Combine Channels

Combine Channels has four inputs, **Red**, **Green**, **Blue** and **Alpha**, and one output,
**Image**. Every input is optional: an unconnected red, green or blue is 0, and an unconnected
alpha is 1.

Each input has a source dropdown (**Red Source**, **Green Source**, **Blue Source** and **Alpha
Source**) that picks which channel of the connected image to use:
**Red**, **Green**, **Blue**, **Alpha** or **Luma** (the image's Rec. 709 brightness). The defaults
are Red, Green, Blue for the colour inputs and Red for the alpha, which is where a grey image keeps
its value. Wiring each output of Separate Channels into the matching input of Combine Channels
therefore gives back the original image exactly.

**Alpha Mode** says how the combined colour relates to the alpha:

| Alpha Mode | Use when |
| --- | --- |
| Premultiplied (default) | The colour channels are already multiplied by the alpha, as with Separate Channels' outputs and Bloom's images. |
| Straight | The colour channels are not multiplied by the alpha. Combine Channels multiplies them for you. |

Where the alpha is 0 the result is transparent black.

The first connected input sets the size and position of the result. Other inputs are read at the
same pixels, and are 0 where they have no pixels. If the first connected input has no image at the
current time, or nothing is connected, the node produces no image. A muted Combine Channels passes
its first input through unchanged.

Every dropdown can be undone and is saved with the project.

## Colour value nodes

The nodes that work on a single colour value rather than an image are named with "(Value)" and are
in the **Color Values** section: Separate Color (Value), Combine Color (Value), Separate HSV
(Value), Combine HSV (Value), Hue Shift (Value) and Luminance (Value). Projects that use them open
unchanged.
