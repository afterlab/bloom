# The status bar

The bar along the bottom of the window shows what Bloom is doing, in labelled groups. Numbers are in
a fixed-width font so they stay put as they change, and hovering a group explains it in a sentence
and gives the detailed counts.

| Group | Shows |
| --- | --- |
| Viewer | **Render** (GPU or CPU produced the frame you see), **Exposure**, **Channel**, **Dropped** (frames playback skipped; a dash when playback is not being measured), and **Decode** when the composition reads video |
| Activity | What is happening now: a save, an import, an export, the RAM preview filling, playback preparing or waiting for frames, or a problem with the preview (with **Details** when the composition cannot render). **Idle** when nothing is |
| Pixel | The color under the pointer in the Viewer, while it is over the frame |
| RAM | What the caches hold against what they may use, and **Cache**, the frames held for preview. Hover for hits and misses, and for how much of it decoded source media holds |
| Disk cache | What the disk cache holds and what it may hold now ("used / available"): the local copies Bloom keeps of source media on slow storage, and the free space beyond the [Disk headroom](memory.md#the-disk-cache-disk-headroom) it may still grow into. Hover for RAM hits, disk hits, network reads, evictions, refused writes and dropped writes, the headroom and the free space on the cache drive |
| MCP | **MCP :port** while the MCP server runs. Click it to open the MCP Server panel; it is not shown while the server is stopped |
| Version | The running version of Bloom |

A group with nothing to show keeps its place with a dash. In a narrow window whole groups fold away
instead of squeezing, in this order: Version, MCP, Disk cache, Pixel, then RAM. Every group that
stays keeps its label.
