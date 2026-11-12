# Qt 6.11.3 Provenance

Reviewed: 2026-11-12

## Source

- Upstream project: <https://www.qt.io/> (the Qt Project, <https://code.qt.io/>)
- Release: 6.11.3, the newest open-source patch release of the 6.11 series with official
  binaries for Linux x86-64, macOS and Windows MSVC
- Source archive: `https://download.qt.io/official_releases/qt/6.11/6.11.3/single/qt-everywhere-src-6.11.3.tar.xz`
- SHA-256 of the acquired archive:
  `ad0112f2aef1b94b42a7ab653cebda2f7fc9ba88ef509ec456b17be80b0bfdab`
- Exact archive size: `1020081224` bytes
- The archive records the commit of each module in a `.tag` file: `qtbase`
  `138f955f0b5e50e4a6c2ee1e78a2279db52ae9e3`, which is what the `v6.11.3` tag of the public
  `qtbase` repository points to, and `qtsvg` `f5d99721911e4a1989965a087813cf1277519bc6`.

## Binaries

Qt is not built by Bloom. Every profile takes the Qt Project's official open-source binaries,
package version `6.11.3-0-202610220312`, from the online installer repository
(`https://download.qt.io/online/qtsdkrepository/`). The lock's `prebuilt` lists are the only
place their URLs are written; these are the archives, with SHA-256, size and entry count:

| Profile | Archive | SHA-256 | Bytes | Entries |
| --- | --- | --- | --- | --- |
| Linux | `qtbase-Linux-RHEL_9_6-GCC-Linux-RHEL_9_6-X86_64.7z` | `ce282da4d8a83dc216ed96b715202872d655177189c16205dc5fab09b41a9f09` | 22883961 | 5145 |
| Linux | `qtsvg-Linux-RHEL_9_6-GCC-Linux-RHEL_9_6-X86_64.7z` | `7b71eea32674d6ac8e9b5b1d3819f510c6081619ca440b6cac86fd970942cf7a` | 298621 | 135 |
| Linux | `icu-linux-Rhel8.6-x86_64.7z` | `16f1e8f409bd9f0b248e36d10deab498d0c1b33d25e0a515a076b1f6b5a1ab28` | 9660662 | 12 |
| macOS | `qtbase-MacOS-MacOS_15-Clang-MacOS-MacOS_15-X86_64-ARM64.7z` | `fb9e4a68f2d217dcea330208a79b087b3f729cc7118ff2ca40be193cf80c7606` | 31040190 | 4776 |
| macOS | `qtsvg-MacOS-MacOS_15-Clang-MacOS-MacOS_15-X86_64-ARM64.7z` | `ba1e3941d01b00413fa424650191324c42e989719a6ed4f41ce43de7e8fb87a7` | 415296 | 154 |
| Windows | `qtbase-Windows-Windows_11_24H2-MSVC2022-Windows-Windows_11_24H2-X86_64.7z` | `68be61b613b963daf206834fdcc57dd3b84baff3082e50679e5846ca4a7a8b86` | 39790318 | 4732 |
| Windows | `qtsvg-Windows-Windows_11_24H2-MSVC2022-Windows-Windows_11_24H2-X86_64.7z` | `e8c789e1a059601dd82114dd9560952891e7c8e94e1cb291166eb61d74800353` | 683023 | 143 |

The Linux package also publishes a `qtwayland` archive. It is not taken: in this release the
Wayland client, its platform plugin, the XDG shell integration and a window decoration are part
of `qtbase`, and the `qtwayland` archive holds only shell integrations and a decoration Bloom
does not use.

The Linux `icu` archive holds the ICU 73.2 shared libraries the Linux `qtbase` binaries link
(`libicui18n.so.73`, `libicuuc.so.73`, `libicudata.so.73`). The Qt Project publishes it inside
the same installer package, so it is recorded with Qt rather than as a component of its own.
Qt's bill of materials names ICU as a system library and states no version; the version is the
libraries' own file names and embedded version string.

## Acquisition Record

- Acquired 2026-11-12 over HTTPS (curl). The binary archives came from `download.qt.io`. The
  source archive came from `master.qt.io`, the Qt Project's own server behind `download.qt.io`
  (the mirror that `download.qt.io` redirected to was too slow to finish); the locked URL is the
  `download.qt.io` one, and the SHA-256 is what binds the bytes.
- The Qt Project publishes no detached signature for these archives. It publishes a SHA-1 beside
  each binary archive and an `md5sums.txt` beside the source archive. All seven binary archives'
  SHA-1 values and the source archive's MD5 (`f5bc717bd4ec3e5058391a96f6418c98`, the same in the
  file served by both servers) equal the values computed from the acquired bytes. Those are
  checksums from the same servers over the same channel, not independent signatures, so the
  recorded SHA-256 values are the binding identity (`provenancePolicy: not-published`).
- Source archive inspection: 434781 entries (391194 files, 43587 directories), **0 symbolic or
  hard links, 0 absolute paths, 0 `..` path components**. The build never unpacks it.

## Correspondence Of The Binaries And The Source Archive

The source archive is recorded as the corresponding source of the binaries, so the two were
compared rather than assumed to match.

- Every binary archive carries Qt's own software bill of materials (`sbom/`), and its
  `*.source.spdx` document lists every source file the binaries were built from with its SHA-1:
  22535 files of `qtbase` and 638 of `qtsvg`, the same lists on all three platforms.
- Each listed file was compared with the same path in the source archive. Linux and macOS:
  22498 of `qtbase` and 628 of `qtsvg` are byte-identical. Windows: 6966 and 78 are
  byte-identical and 15532 and 550 are identical once line endings are made the same (the
  Windows binaries were built from a checkout with CRLF line endings).
- The rest, the same on every platform: 36 `qtbase` and 9 `qtsvg` files that the source archive
  does not contain, every one a `.gitattributes` or `.gitignore`, and each module's `.tag`
  file, which the archive fills with the commit.
- **No source file differs.** The bills of materials name other commit identifiers for `qtbase`
  (`b21f6a785516e898388cda2fe5d3e7dacd6d4b7d` for Linux and Windows,
  `574695a7bb2caa78400cb39589b5b3fe6df3be82` for macOS) than the archive's `.tag`; neither is in
  the public repository, and the per-file comparison shows they have the release's content.
- Limit: this shows the binaries' recorded inputs equal the source archive. It does not rebuild
  the binaries, so it is not a reproducibility proof.

## Status

- NOT QUALIFIED. Acquisition provenance only. No production prefix manifest exists, so this
  component is not release-qualified.
