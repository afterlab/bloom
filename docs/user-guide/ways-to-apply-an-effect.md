# Ways to apply an effect

Bloom's layer model is three rules:

1. Images flow along the wires, and every node applies exactly where it sits.
2. A Merge stacks its entries bottom to top. **Every entry is a layer**, whatever image feeds it.
3. An **Adjustment** entry processes everything below it in its own Merge.

Because of these rules there is usually more than one graph for the same picture. The graphs on this
page give identical pixels; pick the one that reads best for your comp. The Timeline shows each
Merge entry as a row with the full layer controls, so whichever graph you build, every row works.

## An effect over everything below

Three graphs, one picture: Curves applied to a stack of layers.

```text
Adjustment layer (top entry)          Effect after the Merge           Effect into a second Merge

Adjustment ─→ Curves ─→ Merge [3]     Source ─→ Merge [1, 2]            Source ─→ Merge A [1, 2]
Source ───────────────→ Merge [1, 2]           └─→ Curves ─→ Output             └─→ Curves ─→ Merge B [bottom]
                         └─→ Output                                                 Merge B ─→ Output
```

- **Adjustment layer.** Add one from the Timeline's **Add** menu (**Adjustment Layer**) or from the
  node editor's **Add → Compositing → Adjustment**. Its Image output is the stack below the entry its
  chain lands in. Put any effects after it and feed the result into the Merge. That entry replaces
  the stack below it rather than compositing over it.
- **Effect after the Merge.** Wire the Merge's output through Curves.
- **Second Merge.** Feed the curved stack into the bottom entry of another Merge and stack further
  layers on top.

The adjustment layer is the only one of the three that stays a row in the same Timeline, so you can
trim it, fade it, mask it and move it up and down the stack like any other layer.

## Fading an adjustment

An adjustment layer's **Opacity** mixes the adjusted stack with the unadjusted one. An adjustment
at 40 % gives the same pixels as the curved stack mixed over the unadjusted stack at 40 %. Masks,
a track matte, the blend mode and the layer's in and out points apply as they do on any layer: a
mask limits where the effect shows, and outside the layer's range the stack below is unchanged.

## Limiting an effect, or a layer, to a matte

Every grade, colour and filter effect has an optional **Mask** input. Wire a Mask into it (a
Cryptomatte selection, a Mask node, a Luma Key) and the effect applies only where the Mask covers;
elsewhere its input passes through unchanged. An Image wired in instead is read through the
effect's **Mask channel** (Alpha, Red, Green, Blue or Luma, each also inverted).

```text
Source ─→ Grade ─→ Merge             Cryptomatte ─→ Mask Combine ─→ Layer mask, track matte
Cryptomatte ─┘ (mask)                Mask ────────┘ (b)               or an effect's Mask input
```

A layer can be shaped by a node's Mask too: in Properties, **Add Mask from Node** adds a mask
whose **Shape from** is that node (switch it back to **Path** to draw), and the **Track Matte**
source lists the composition's node Masks beside its layers. **Mask Combine** adds, subtracts,
intersects or takes the difference of two Masks; **Image to Mask** turns an image's alpha, luma or
one channel into a Mask.

## Layer controls on the entry or on a Layer node

```text
Entry controls                         Layer node

Source ─→ Merge [entry: position,      Source ─→ Layer (position, scale,
                 scale, opacity, …]               opacity, …) ─→ Merge
```

Every Merge entry stores its own layer controls, so a plain image wired into a Merge is already a
layer. A **Layer** node is a handle for those same controls in the node graph: there is one set of
values, and edits on the Layer node show on the Timeline row and in Properties, and the other way
round. Inserting a Layer node in front of an entry takes over the entry's values, and removing it
gives them back, in one undo step; the picture does not change. With no filter after the Layer
node, the two graphs above give identical pixels.

Where a Layer node sits matters once a filter follows it: `Source → Layer → Blur → Merge` moves the
image and then blurs it. A Layer node also works on its own, feeding no Merge at all. When a chain
has several Layer nodes, the one nearest the Merge is the entry's layer; Layer or transform steps
with no filter between them are combined into one resample, so stacking them costs no sharpness.

A Layer node that feeds two Merge entries shares its values between them, like a clone. Both
Timeline rows show the shared marker.

## Nesting a Merge

```text
Nested                                  Flattened

Source 1 ─→ Merge A ─→ Merge B ─→ Out   Source 1 ─┐
Source 2 ─┘          ↑                  Source 2 ─┼→ Merge ─→ Out
Source 3 ────────────┘                  Source 3 ─┘
```

A Merge that feeds another Merge is a layer of the Merge it feeds. Its Timeline row is a group row
with live controls; expand it to see and edit its own entries in place. With the group row's
controls at their defaults, the nested graph gives the same pixels as flattening the layers into one
Merge. Bloom never flattens it for you: blend modes other than Normal see a different backdrop inside
a group, which is often exactly why you group.

## Older projects

Projects saved before every entry became a layer open unchanged. A plain entry shows its controls
at their defaults; the first edit stores them on the entry.
