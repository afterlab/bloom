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

## Render passes

A multi-layer OpenEXR render carries passes such as depth, mist, normals, motion vectors and IDs
next to its colour. These are data, not colour: they must reach your nodes with their exact values.

**Shuffle** builds an image from four channels of a channel set. Its **Mode** is:

| Mode | What it does |
| --- | --- |
| Auto (default) | Raw when a red, green or blue slot reads a data channel (depth, normal, motion, position, UV, ID or other data), otherwise Color. |
| Color | Converts the colour channels from the file's input color space to the working space. Refused on a data channel. |
| Raw | Uses the values exactly as stored. |

Color on a data channel is refused, and the Shuffle shows "Shuffle Color mode would color-convert
the data channel …" and produces no image. Choose Auto or Raw. A project saved with Color mode on a
data channel opens with this message rather than a converted image.

**Pass** (in the **Compositing** section) is the quick way to pull one pass out. Pick it from the
**Pass** dropdown, which lists the input's layers and channels. A channel gives a grey image, and a
layer gives its R, G, B (or X, Y, Z, or U, V, W) channels as red, green and blue. The alpha is
always 1, so data values are never lost under transparent pixels. The default, **First data layer**,
reads the first data pass in the file.

**Non-finite value.** Renders can hold NaN or infinite samples (a depth pass where nothing was hit,
for example). Shuffle and Pass replace them with the **Non-finite value**, 0 by default; set it to a
far distance for a depth pass. The node reports how many pixels it repaired.

**Zero alpha.** Bloom's images are premultiplied, so where a Shuffle's alpha is 0 the result is
transparent black, whatever the colour slots hold. Map the alpha slot to `1` (or use Pass) to keep
data under transparent pixels.

**Image to Channel Set** (in the **Compositing** section) turns an image back into a channel set:
its red, green, blue and alpha become the channels of one named layer. Use it to send a processed
pass to **Channel Copy** and on to a layered OpenEXR through the Composition Output's channels.
**Layer** names the layer (default `pass`), **Channels** names its channels in red, green, blue,
alpha order (default `R,G,B,A`; `Z` for a depth pass, `X,Y,Z` for normals), and **Encoding** says
whether they are **Data** (default, read raw) or **Color**. A Shuffle or Pass that reads only these
channels uses the image's values exactly. A single Shuffle cannot mix them with a file's channels;
shuffle each and join them with Combine Channels.

A Video source has a **channels** output too: its frame's R, G, B and A, as colour in the working
space.

**Passes through a precomp.** A **Composition** source (a precomp) has a **channels** output as
well, so you can precomp a multilayer render and still pull its passes downstream. It holds what a
layered OpenEXR render of that composition would: its composited R, G, B and A, plus every layer
connected to its Composition Output's **channels** input (only the layers the output's **Layers**
list names, when you set one). Connect it to **Pass**, **Shuffle**, **Channel Copy**, Cryptomatte
or your own Composition Output's channels exactly like an Image source's channels.

The passes follow the precomp's timing: its **Time Offset**, **Time Scale** (animated or not) and
**Loop Mode** pick the frame of each pass, at the precomp's own frame rate, and precomps inside
precomps add up. A Shuffle with its default slots shows the precomp's composite. Like Image to
Channel Set layers, the composite cannot share one Shuffle with file passes: shuffle each and join
them with Combine Channels. A precomp cannot contain itself, directly or through another precomp;
Bloom refuses the cycle.

**Resize, Fit and Transform 2D** have a **Filter** choice. Use **Nearest** for data and ID images:
it takes each pixel from one source pixel, so depth or ID values are never blended. **Bilinear**
(the default) is for pictures.

## Colour value nodes

The nodes that work on a single colour value rather than an image are named with "(Value)" and are
in the **Color Values** section: Separate Color (Value), Combine Color (Value), Separate HSV
(Value), Combine HSV (Value), Hue Shift (Value) and Luminance (Value). Projects that use them open
unchanged.
