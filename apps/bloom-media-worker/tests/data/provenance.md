# VP9 and AV1 decode reference fixtures

Three tiny, original synthetic clips that exercise Bloom's bundled VP9 (FFmpeg's native decoder)
and AV1 (dav1d) decode paths with streams from independent reference encoders. Bloom's own build
has no VP9 or AV1 encoder at this intake, so these bytes cannot be generated at test time; they are
small enough to review byte for byte and contain no third-party content.

Content: a flat Rec.709 limited-range color (lavfi `color=c=0x3366cc`), 64x48, 12 frames at 24
fps, tagged BT.709 primaries, transfer and matrix with limited range. Every decoded sample has one
exact code: 8-bit Y'CbCr 98/177/101, 10-bit 403/714/407.

| File | Codec | Encoder | SHA-256 |
| --- | --- | --- | --- |
| `vp9-flat.webm` | VP9 Profile 0, 4:2:0 8-bit, WebM | libvpx 1.17.0 | `c3236e990f80052df550dbac647103f84772e1dc564d562ca7b8a72177b27d8e` |
| `av1-flat.mp4` | AV1 Main, 4:2:0 8-bit, MP4 | libaom 3.15.1 | `aa0b24bc7eacf8398b5718024f749f3fed4ec14ea349131158a06285d16b40f8` |
| `av1-flat10.webm` | AV1 Main, 4:2:0 10-bit, WebM | libaom 3.15.1 | `a90e41e10eef04e3daf601d147913a1493f905f405a9d983518835ef9177ba0b` |

Generated 2026-10-31 with FFmpeg 9.0.2 (bit-exact flags), for example:

```sh
ffmpeg -f lavfi -i "color=c=0x3366cc:s=64x48:r=24:d=0.5,format=yuv420p" \
  -color_primaries bt709 -color_trc bt709 -colorspace bt709 -color_range tv \
  -c:v libvpx-vp9 -b:v 0 -crf 10 -g 12 -row-mt 0 -threads 1 -bitexact -fflags +bitexact \
  vp9-flat.webm
```

The AV1 files use `-c:v libaom-av1 -crf 10 -g 12 -cpu-used 8 -threads 1` with the same input
(`format=yuv420p10le` for the 10-bit file).

# Apple-written AAC references (issue #370)

Two tiny, original synthetic clips written by the macOS system encoder (AVAssetWriter), whose AAC
starts with priming packets ahead of the track's edit: 2112 sample frames, the last 64 of them
inside the first packet of the edit. No other system writes this layout, so the bytes cannot be
generated at test time on Linux or Windows. They contain no third-party content.

| File | Content | Written by | SHA-256 |
| --- | --- | --- | --- |
| `apple-aac.mp4` | One second of 48 kHz stereo AAC (a 440 Hz and a 1 kHz tone with low noise), no pictures | `writeAac` in `videotoolbox_parity_tests.mm` | `9cb776a60afa644abb99a7ef7ba6e85bdac57a6ebc64f462fcc988339d6fb3bd` |
| `apple-h264-aac.mp4` | Two seconds of 64x48 H.264 High at 24 fps with 48 kHz stereo AAC | `export-h264.mp4` of `bloom.media.encode` on macOS (the VideoToolbox provider) | `5543692dc5f24b5db2966951a7b0056fed5976eb84dcc11c22efe65040820942` |

Generated 2026-11-12 on macOS (Apple silicon). `bloom.media.ffmpeg` decodes both from sample 0 on
every system, and `bloom.media.videotoolbox-parity` compares the two readers on them.
