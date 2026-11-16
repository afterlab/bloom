# Captures of the installed application

These scripts take pictures of the installed Bloom on a desktop: its icon in the places an artist
sees it. The release workflow runs one script per platform on a hosted runner, after it has
installed the files it built, and uploads the pictures as one artifact. Each script writes PNG files
named `670-<platform>-<what>.png` and a `captures.txt` that lists each file with one line of what it
shows. A script exits non-zero if a picture is missing, empty or one flat color. A step that fails
prints one line, `<script>: step failed: <picture>: <the reason>`, with the real output of the
command that failed, and the other steps still run.

Nobody runs these on a personal desktop: the Linux script starts its own private display, and the
macOS and Windows scripts are meant for a runner's own session.

## Linux: `linux.sh <out> <installed prefix> <home>`

- `<installed prefix>` is the folder `install.sh` installed Bloom in (it holds `bin/Bloom`).
- `<home>` is the home folder the install was made for. The menu entry
  (`.local/share/applications/id.afterlab.bloom.desktop`) and the icons
  (`.local/share/icons/hicolor`) are read from there.
- Needs these Ubuntu packages, plus the libraries Bloom itself declares
  (`tools/install/linux-library-packages.txt`):
  `xvfb openbox tint2 rofi imagemagick x11-utils xdotool`
- Starts its own Xvfb display (1280x800, a free display number from 90 up), a window manager
  (openbox), and sets `HOME` and the XDG folders for everything it starts. It never touches another
  display, and it stops everything it started on exit, by process id.

Pictures:

- `670-linux-menu.png`: the application menu (`rofi -show drun`) filtered to Bloom, with the entry
  and its icon.
- `670-linux-launcher.png`: a tint2 panel whose launcher holds the Bloom menu entry.
- `670-linux-window.png`: Bloom running, started from the menu entry's `Exec` line, with its window,
  the window icon in the title bar and the entry in the task bar. The script waits for the window
  and then closes Bloom.

The window and task bar icons are the icon Bloom sets on its own window from its package's icon
files (`installWindowIcon`, `src/ui/application_identity.cpp`).

## macOS: `macos.sh <out> <disk image file> [<project file>]`

Bloom is in `/Applications/Bloom.app` and `/usr/local/bin/bloom-cli` is linked. The session must be a
logged-in desktop where the shell may record the screen and drive System Events (the hosted
runner image allows both). A refused step prints the message of `osascript` or `screencapture`.

- `670-macos-applications.png`: Finder, `/Applications`, icon view, Bloom selected, and the Dock.
- `670-macos-project-file.png`: Finder, a folder with one `.bloom` file in icon view with large
  icons. The project is made with `bloom-cli new`; a third argument names a project to copy instead.
- `670-macos-disk-image.png`: the disk image mounted and opened in Finder: its background picture,
  the app and the Applications link. The script closes the window and detaches the image.
- `670-macos-dock.png`: Bloom running, with its icon in the Dock.
- `670-macos-help-menu.png`: Bloom's Help menu open, showing Install Command-Line Tool. The script
  closes the menu and quits Bloom.

## Windows: `windows.ps1 -Out <folder> -Installed <install folder> [-Project <file>]`

PowerShell 7. `-Installed` holds `Bloom.exe`; Bloom was installed for the current user by the
installer (Start menu shortcut and the `.bloom` file type). Without `-Project` the project is made
with `bloom-cli.exe` from the install folder.

- `670-windows-start-menu.png`: an Explorer window of the user's Start menu Programs folder with the
  Bloom shortcut and its icon. Fails if the installer made no shortcut.
- `670-windows-start-search.png`: the Start menu with "Bloom" typed in. This is the one optional
  picture: a runner session may not open the Start menu for `SendKeys`. If it does not, the reason
  goes into `captures.txt` and the script still exits 0 for this picture.
- `670-windows-project-file.png`: an Explorer window of a folder with one `.bloom` file and its icon.
- `670-windows-taskbar.png`: Bloom running, with its icon on the taskbar. The script closes Bloom.

## How the workflow calls them

    bash tools/ci/captures/linux.sh   captures "$HOME/.local/opt/bloom" "$HOME"
    bash tools/ci/captures/macos.sh   captures Bloom-<version>-macos-arm64.dmg
    pwsh tools/ci/captures/windows.ps1 -Out captures -Installed "$env:LOCALAPPDATA\Programs\Bloom"

Upload the `captures` folder as one artifact. The workflow decides where the project file for the
macOS and Windows scripts comes from; without one, they make it with `bloom-cli`.
