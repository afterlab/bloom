# Bloom application icon

`Bloom.icon` is the one source of the artwork. It is a layered icon in the macOS 26 format: a
background gradient fill (`icon.json`) and the flower as SVG (`Assets/petals.svg`). Edit the
artwork there and nowhere else.

Everything else in this folder is made from it and checked in:

| File | What it is |
| --- | --- |
| `Bloom.icon/` | The layered icon. The one source. |
| `bloom-glass-2048.png` | The glass image used outside macOS 26, exported from `Bloom.icon` at 2048 pixels. |
| `bloom-tile-1024.png` | The source of the Windows and Linux icons: the glass tile at 922 pixels centered on a transparent 1024-pixel canvas (a margin of 51 pixels). |
| `bloom-macos-legacy-1024.png` | The source of the macOS 13.3 to 15 icon: an 824-pixel body and its shadow on a 1024-pixel canvas. |

## Export the glass image again

This needs a Mac with Xcode 26. Run the `ictool` program inside Xcode's Icon Composer from this
folder:

    ictool Bloom.icon --export-image --output-file bloom-glass-2048.png \
        --platform macOS --rendition Default --width 2048 --height 2048 --scale 1

## Make the two 1024-pixel sources from the glass image

These commands use ImageMagick 7 and run in this folder. The tile is exact: the commands give
the checked-in pixels.

    magick bloom-glass-2048.png -filter Box -resize 922x922 -background none \
        -gravity center -extent 1024x1024 bloom-tile-1024.png

The older-macOS source is the glass image scaled to an 824-pixel body, a soft black shadow, and
the two placed on a transparent 1024-pixel canvas. The body is 100 pixels from the left and top.
The shadow is the alpha of the body on an 864-pixel square (20 pixels of margin on every side),
blurred with `-blur 0x10`, at 30 percent opacity. It sits at 80 pixels from the left and 90
from the top, so it lies 10 pixels lower than the body.

    magick bloom-glass-2048.png -filter Box -resize 824x824 body.png
    magick body.png -alpha extract -bordercolor black -border 20 -blur 0x10 \
        -evaluate multiply 0.30 shadow-alpha.png
    magick shadow-alpha.png -alpha off -fill black -colorize 100 \
        -compose copy_opacity shadow-alpha.png -composite shadow.png
    magick -size 1024x1024 xc:none shadow.png -geometry +80+90 -composite \
        body.png -geometry +100+100 -composite bloom-macos-legacy-1024.png

The body in these commands is the checked-in body, pixel for pixel. The shadow depends on how
the blur rounds, so a new run can differ from the checked-in file by one level in some shadow
pixels. The checked-in `bloom-macos-legacy-1024.png` is the source; do not replace it with a
new run unless the artwork changed. The generator does not make these two files, because the
shadow cannot be made with the same pixels by the Python standard library.

## Make the platform files

    python3 tools/quality/generate_app_icons

| Source | Files |
| --- | --- |
| `bloom-tile-1024.png` | `apps/bloom/windows/Bloom.ico` (16 to 256 pixels) and `apps/bloom/linux/icons/<size>x<size>/id.afterlab.bloom.png` (16 to 512 pixels) |
| `bloom-macos-legacy-1024.png` | `apps/bloom/macos/Bloom.icns` (every size from 16 to 1024 pixels) |

The `bloom.quality.app_icons` test runs `python3 tools/quality/generate_app_icons --check`. It
fails, and names the stale file and its source, when a platform file is not what its source
gives.
