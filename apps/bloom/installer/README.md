# Installer pictures

The pictures the macOS disk image and the Windows installer show. They are finished files: the
installers read them as they are. The test `bloom.quality.installer_pictures`
(`tools/quality/check_installer_pictures`) checks the file names, the sizes and the formats, and
that nothing else is in the two folders except the masters and this file.

## macOS disk image

The disk image window is 660 x 400 points. It shows the logotype, an arrow from the app to the
Applications folder, and one line: "Drag Bloom to the Applications folder to install it".

| File | Size | Use |
| --- | --- | --- |
| `macos/dmg-background.png` | 660 x 400 | Standard displays |
| `macos/dmg-background@2x.png` | 1320 x 800 | Retina displays |
| `macos/dmg-background.svg` | 1320 x 800 | Master drawing: the gradient, the arrow and the logotype. The line of text is added when the PNG is made. |

Both PNG files are 8-bit RGBA. Only the dark background is used.

## Windows installer

Inno Setup's wizard shows the large picture on the welcome and finish pages and the small picture
in the header of the other pages. It picks the file that fits the display scale. Each kind has
seven sizes, all 24-bit uncompressed BMP.

| Display scale | Large (`windows/wizard-large-<W>x<H>.bmp`) | Small (`windows/wizard-small-<W>x<H>.bmp`) |
| --- | --- | --- |
| 100 % | 164 x 314 | 55 x 55 |
| 125 % | 192 x 386 | 64 x 68 |
| 150 % | 246 x 459 | 83 x 80 |
| 175 % | 273 x 556 | 92 x 97 |
| 200 % | 328 x 604 | 110 x 106 |
| 225 % | 355 x 700 | 119 x 123 |
| 250 % | 410 x 797 | 138 x 140 |

The large picture is the glass tile above the logotype on a dark gradient. The small picture is
the glass tile on white. `windows/wizard-large-master.svg` is the master drawing of the large
picture without the glass tile.

## How they were made

The masters are SVG files drawn from the logotype and the icon artwork. These tools make the
files from them:

- `rsvg-convert -w <width> -h <height> master.svg -o master.png` draws a master at double size.
- `magick` adds the line of text to the background, with the Inter Regular font, 26 points, gray
  (`-gravity north -annotate +0+652`), and the glass tile to the large picture (460 pixels wide,
  placed at +180+430).
- `magick master.png -filter Box -resize 660x400 dmg-background.png` makes the standard
  background from the double-size one.
- For each wizard size, `magick big.png -filter Box -resize <W>x<H>^ -gravity center
  -extent <W>x<H> -strip picture.png`, then `magick picture.png -type TrueColor
  BMP3:picture.bmp`. The small pictures are the glass tile scaled to 92 % of the shorter side,
  centered, flattened on white, and written the same way.

The files are checked in as results, so building Bloom needs none of these tools.
