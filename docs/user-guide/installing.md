# Installing, updating and uninstalling Bloom

Status: working

Updated: 2026-11-17

Bloom is in alpha. This page is where you start. It covers getting Bloom, installing it on Linux,
macOS and Windows, what to expect the first time you run it, how it updates, and how to remove it.
The same steps work on a render node. The sections for deployment say so.

## Getting Bloom

Every alpha is a release on the GitHub releases page:
[github.com/afterlab/bloom/releases](https://github.com/afterlab/bloom/releases). Open the newest
release and download the file for your system. `<version>` below is the release's label, for
example `0.1.0-alpha.1`.

| System | File | What it is for |
| --- | --- | --- |
| macOS | `Bloom-<version>-macos-arm64.dmg` | The disk image. Open it and drag Bloom to Applications. |
| macOS | `Bloom-<version>-macos-arm64.pkg` | The installer package, for render nodes and managed Macs. |
| Windows | `Bloom-<version>-windows-x86_64-setup.exe` | The installer. It installs for your user and needs no administrator rights. |
| Windows | `Bloom-<version>-windows-x86_64.zip` | A folder that runs from wherever you unpack it. Nothing is installed. |
| Linux | `install.sh` | The install script. It downloads Bloom, checks it and installs it. |
| Linux | `Bloom-<version>-linux-x86_64.AppImage` | Bloom as one file. Nothing is installed. |
| Linux | `Bloom-<version>-linux-x86_64.tar.xz` | The same files as a folder, to unpack by hand. |

The systems Bloom runs on:

- **Linux:** x86-64, with glibc 2.35 or newer. A desktop installation has the system libraries
  Bloom needs (see Linux below).
- **macOS:** Apple silicon, macOS 13.3 or newer.
- **Windows:** x86-64, Windows 10 version 1903 or newer.

Bloom renders on the GPU through Vulkan (MoltenVK on macOS). It carries everything else it needs:
you do not install Qt, Python or a compiler. To build Bloom yourself instead, see
[Build from source](../../README.md#build-from-source).

## macOS

**Disk image.** Open `Bloom-<version>-macos-arm64.dmg` and drag **Bloom** onto the **Applications**
link in the window. Eject the image.

**Installer package.** For a render node or a managed Mac, install the package from a terminal:

```sh
sudo installer -pkg Bloom-<version>-macos-arm64.pkg -target /
```

It puts `Bloom.app` in `/Applications` without any window and links `/usr/local/bin/bloom-cli` to
the program inside it. If a file that is not a link already has that name, the installer leaves it
alone and still installs Bloom.

**The command-line program.** If you installed from the disk image, choose
**Help > Install Command-Line Tool…** in Bloom. It links `/usr/local/bin/bloom-cli` to the
`bloom-cli` inside the copy you are running. If your account cannot write to `/usr/local/bin`,
macOS asks for an administrator password.

**First run.** This alpha is not notarized, so macOS refuses the first start of a copy you
downloaded. Try to open Bloom once, then open **System Settings > Privacy & Security**, find the
message about Bloom and choose **Open Anyway**. Confirm, and Bloom starts.

**Updating.** Bloom checks for updates (see Updates below). When a newer version exists, the
update window of the platform's updater downloads it, installs it and starts Bloom again.

**Uninstalling.** Quit Bloom and move `Bloom.app` to the Trash. If you linked the command-line
program, remove the link:

```sh
sudo rm /usr/local/bin/bloom-cli
```

Your projects are yours and stay where you saved them. Bloom's preferences, recovery files and
caches are in your user folders, and removing the app leaves them.

## Windows

**Installer.** Run `Bloom-<version>-windows-x86_64-setup.exe`. It installs for the current user
only, into `%LOCALAPPDATA%\Programs\Bloom` unless you choose another folder. It never asks for
administrator rights. It adds:

- a Start menu entry and an entry in **Settings > Apps**;
- the project file type: `.bloom` files are "Bloom Project" files with the Bloom icon, and opening
  one starts Bloom with that file.

The option **Add bloom-cli to PATH** is off by default. Turn it on to run `bloom-cli` from any
terminal. It adds one folder of the installation to your user's `Path`, never the system's.

**The zip.** `Bloom-<version>-windows-x86_64.zip` runs from wherever you unpack it. Nothing is
written outside that folder and nothing has to be installed first. `bloom-cli.exe` is in the
folder.

**Deployment.** The installer takes these switches for a silent install:

```bat
Bloom-<version>-windows-x86_64-setup.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART [/DIR="<folder>"] [/TASKS="addtopath"]
```

A silent install starts nothing. A silent uninstall is:

```bat
"<folder>\unins000.exe" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART
```

**First run.** Windows shows "Windows protected your PC" for a downloaded installer that it does
not know yet. Choose **More info**, then **Run anyway**.

**Updating.** An installed copy updates itself: Bloom checks (see Updates below), the update window
downloads the next installer and runs it silently, and Bloom starts again. Bloom closes for the
installer only when nothing is unsaved and nothing is running. A copy from the zip is not
replaced. Bloom tells you where the next version is and changes nothing.

**Uninstalling.** Remove Bloom in **Settings > Apps**, or run the silent uninstall command above.
Either one removes the installation, the version an update kept, the Start menu and Apps entries,
the file type and the `Path` entry. A copy from the zip is removed by deleting its folder.

## Linux

**The install script.** Download `install.sh` from the release and run it:

```sh
sh install.sh
```

The script needs `tar`, `xz`, `sha256sum`, `openssl` (3.0 or newer) and `curl` or `wget`. It
downloads the release's checksum list and its signature, checks the signature, then downloads Bloom
and compares its checksum with the signed list. Nothing is unpacked before both checks pass, and no
option skips them.

Without options it installs for you only and needs no administrator rights:

- Bloom in `~/.local/opt/bloom`;
- a link `~/.local/bin/bloom-cli` to the command-line program;
- the menu entry, the icons and the project file type under `~/.local/share`.

The options:

- `--system` installs for every user, for example on a render node: Bloom in `/opt/bloom`, the link
  in `/usr/local/bin/bloom-cli` and the rest under `/usr/local/share`. Run it as an administrator,
  for example `sudo sh install.sh --system`.
- `--prefix <folder>` puts Bloom in a folder of your choice. The folder must be for Bloom alone.
  The script refuses a folder that holds other files.
- `--from <folder>` takes the release files from a folder instead of downloading them, for a
  computer with no network connection. It checks them the same way.
- `--uninstall` removes what the script installed (see Uninstalling below).

If the system lacks a library Bloom needs, the script names it, with the command that installs it
on Debian and Ubuntu, on Fedora and on Arch Linux. It completes the install either way.

Run the script again to update or repair an install. It replaces the installed folder with a
newly unpacked one and writes the link and the menu files again.

**The tar.** Unpack `Bloom-<version>-linux-x86_64.tar.xz` anywhere. It runs from that folder.
`bin/Bloom` starts the application, and `bin/bloom-cli` is the command-line program.

**The AppImage.** Make the file executable and run it:

```sh
chmod +x Bloom-<version>-linux-x86_64.AppImage
./Bloom-<version>-linux-x86_64.AppImage
```

An AppImage needs FUSE (`fusermount3` or `fusermount`). On a system without it, run the AppImage
with `--appimage-extract` to unpack it, or use the tar. An AppImage installs nothing, so the first
time it runs Bloom asks "Add Bloom to the Application Menu?". Choose **Add to Application Menu** to
add a menu entry, the icons and the project file type under `~/.local/share`. Choose **Not Now**
and nothing is written. Bloom asks once. The AppImage starts the application only. `bloom-cli` is
in the unpacked AppImage and in the tar, under `bin`.

**The Arch Linux package.** `bloom-bin` is the package for Arch Linux. It installs Bloom in
`/opt/bloom` and links `/usr/bin/bloom-cli`, and pacman installs the libraries it needs. Its source
is in the repository under `packaging/aur`. Publishing it to the AUR is a maintainer's step.

**The system libraries.** A desktop installation already has what Bloom loads from the system. On
a minimal system, `docs/architecture/platform-support.md` lists the packages for Debian, Ubuntu
and Fedora ([Linux Package](../architecture/platform-support.md#linux-package)).

**Updating.** How a copy updates depends on how you installed it:

| Install | How it updates |
| --- | --- |
| Install script, or an unpacked tar in a folder you can write | Bloom downloads the new tar and swaps the folder after it quits. |
| AppImage | Bloom downloads the new AppImage, only the changed blocks when it can, and replaces the file after it quits. |
| System-wide install (`--system`, or a folder you cannot write) | Bloom says the installer must be run again. Whoever installed it runs `sudo sh install.sh --system` again. |
| Package manager (Arch Linux) | Bloom says a newer version exists. Update it with pacman. |

**Uninstalling.**

- Install script: `sh install.sh --uninstall` (with `--system` and `--prefix` as you installed). It
  removes the Bloom folder, the version an update kept beside it, the link when it still points
  into that folder, and the files the script wrote under `~/.local/share`. Projects, preferences
  and caches stay.
- Tar: delete the folder.
- AppImage: delete the file. If you added the menu entry, also delete
  `~/.local/share/applications/id.afterlab.bloom.desktop`.
- Arch Linux package: `sudo pacman -R bloom-bin`.

## Updates

**Help > Check for Updates…** checks when you ask. Bloom also checks by itself, at most once a
day, while **Check for updates automatically** is on in **Preferences > General**. It is on by
default. The check is one plain web request for the list of releases. It sends nothing else: no
account, no identifier and nothing about your computer or your projects. See
[Preferences](preferences.md#updates).

When a newer version exists, Bloom's update window shows the release notes and two choices:
**Download and Install**, or close the window to keep working. The download shows progress, and
you can close the window while it runs: it goes on and shows in the Jobs panel. **Cancel** stops
it. When the download is done, **Restart and Update** closes Bloom, installs the version and opens
it again. On macOS and Windows installs, the platform's updater shows its own window for the
update.

Bloom verifies every update. It checks the signature on the list of releases, and then the size,
the checksum and the signature of each file it downloads. If anything does not match, it installs
nothing and tells you why ("Update not installed", or "The downloaded file was refused, so nothing
was installed."). The version you have keeps running.

Bloom keeps the previous version until the next update succeeds. If the new version cannot start,
the previous one is put back and started. Bloom then shows an **Update** notice: "Bloom `<version>`
could not start, so the version you had was put back." `<version>` is the update that did not start.

A copy that cannot update itself says why in the same window, for example "This copy of Bloom was
installed by pacman. Update it there." or "You have `<version>`. Download the new version from the
releases page." The button **Open the Download Page** opens the releases page. A copy that runs
from a build folder is never updated.

## The command-line program

`bloom-cli` is the program for scripts and render nodes. Where it is after each install:

| Install | `bloom-cli` |
| --- | --- |
| macOS, disk image | Inside the app. **Help > Install Command-Line Tool…** links it as `/usr/local/bin/bloom-cli`. |
| macOS, installer package | `/usr/local/bin/bloom-cli`. |
| Windows installer | In the installation folder. With **Add bloom-cli to PATH**, from any new terminal. |
| Windows zip | `bloom-cli.exe` in the unpacked folder. |
| Linux, install script | `~/.local/bin/bloom-cli`, or `/usr/local/bin/bloom-cli` with `--system`. |
| Linux, tar or AppImage | `bin/bloom-cli` in the unpacked folder. |
| Linux, Arch package | `/usr/bin/bloom-cli`. |

Check it and read its help:

```sh
bloom-cli --version
bloom-cli help render
```

[Headless scripting](scripting.md) covers its commands.

## Opening a project file

Double-click a `.bloom` file in your file manager to open it in Bloom. On Linux and Windows each
file opened this way starts its own copy of Bloom. On macOS a running Bloom receives the file.
