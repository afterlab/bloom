#!/bin/bash
# Captures the installed Bloom's icon on macOS (#670): Finder, the disk image window, the Dock and
# the Help menu. Bloom is installed in /Applications and /usr/local/bin/bloom-cli is linked. The
# shell needs screen recording and accessibility rights (the hosted runner image grants them); a
# refused step prints the tool's own message.
#
#   macos.sh <out folder> <disk image file> [<project file>]
#
# Without <project file>, the project is made with `bloom-cli new`.
set -u

[ $# -ge 2 ] && [ $# -le 3 ] || { echo "usage: macos.sh <out folder> <disk image file> [<project file>]" >&2; exit 2; }
out=$1
image=$2
fixture=${3:-}
app=/Applications/Bloom.app
cli=/usr/local/bin/bloom-cli

mkdir -p "$out"
out=$(cd "$out" && pwd)
work=$(mktemp -d)
failed=0
mount_point=
finder_windows=()
bloom_pid=

: > "$out/captures.txt"

fail() {
    echo "macos.sh: step failed: $*" >&2
    failed=1
}

# script <AppleScript> [arguments...]: runs it, prints what it returns, keeps the error text.
script() {
    osascript - "${@:2}" <<< "$1" 2>&1
}

# shoot <file name> <what it shows>: takes the whole screen; fails on a missing or tiny picture.
shoot() {
    local file=$out/$1 log size
    if ! log=$(screencapture -x "$file" 2>&1); then
        fail "$1: screencapture: $log"
        return 1
    fi
    # A flat or empty screen is a PNG of a few kilobytes; a desktop with a window is far larger.
    size=$(stat -f %z "$file" 2> /dev/null || echo 0)
    if [ "$size" -lt 30000 ]; then
        fail "$1: the picture is empty or flat ($size bytes); screen recording may not be granted"
        return 1
    fi
    echo "$1: $2" >> "$out/captures.txt"
}

# Opens a folder in a Finder window in icon view with the given icon size, in front, and prints
# the window's id.
finder_icons() {
    script '
on run argv
    set target to POSIX file (item 1 of argv) as alias
    tell application "Finder"
        activate
        open target
        set w to front Finder window
        set current view of w to icon view
        set icon size of icon view options of w to (item 2 of argv as integer)
        set bounds of w to {80, 60, 1180, 760}
        set index of w to 1
        return id of w
    end tell
end run' "$1" "$2"
}

# Polls (a bounded number of times) until a command succeeds.
poll() {
    local tries=$1
    shift
    for _ in $(seq "$tries"); do
        if "$@" > /dev/null 2>&1; then return 0; fi
        sleep 0.5
    done
    return 1
}

close_finder_windows() {
    for id in "${finder_windows[@]+"${finder_windows[@]}"}"; do
        script 'on run argv
    tell application "Finder" to close (every Finder window whose id is (item 1 of argv as integer))
end run' "$id" > /dev/null 2>&1 || true
    done
    finder_windows=()
}

bloom_has_window() {
    [ "$(osascript -e 'tell application "System Events" to tell process "Bloom" to get count of windows' 2> /dev/null)" -gt 0 ] 2> /dev/null
}

quit_bloom() {
    [ -n "$bloom_pid" ] || return 0
    osascript -e 'tell application "Bloom" to quit' > /dev/null 2>&1 || true
    for _ in $(seq 20); do
        kill -0 "$bloom_pid" 2> /dev/null || break
        sleep 0.5
    done
    kill "$bloom_pid" > /dev/null 2>&1 || true
    bloom_pid=
}

cleanup() {
    close_finder_windows
    quit_bloom
    if [ -n "$mount_point" ]; then
        hdiutil detach "$mount_point" > /dev/null 2>&1 || hdiutil detach -force "$mount_point" > /dev/null 2>&1 || true
    fi
    rm -rf "$work"
}
trap cleanup EXIT

[ -d "$app" ] || { echo "macos.sh: $app is not installed" >&2; exit 1; }
[ -f "$image" ] || { echo "macos.sh: the disk image $image does not exist" >&2; exit 1; }

# --- 1. Finder, /Applications, Bloom selected ----------------------------------------------------

step=670-macos-applications.png
if log=$(open -R "$app" 2>&1); then
    if id=$(finder_icons /Applications 128); then
        finder_windows+=("$id")
        log=$(script 'tell application "Finder" to reveal (POSIX file "/Applications/Bloom.app" as alias)' ) || fail "$step: Finder: $log"
        sleep 2
        shoot "$step" "A Finder window of /Applications in icon view with Bloom selected, and the Dock."
    else
        fail "$step: Finder scripting: $id"
    fi
else
    fail "$step: open -R: $log"
fi
close_finder_windows

# --- 2. Finder, a folder with one project file ---------------------------------------------------

step=670-macos-project-file.png
folder=$work/Projects
mkdir -p "$folder"
if [ -n "$fixture" ]; then
    log=$(cp "$fixture" "$folder/Example.bloom" 2>&1) || log="cp: $log"
else
    log=$("$cli" new "$folder/Example.bloom" 2>&1) || log="$cli new failed: $log"
fi
if [ -s "$folder/Example.bloom" ]; then
    if id=$(finder_icons "$folder" 256); then
        finder_windows+=("$id")
        script 'tell application "Finder" to select (POSIX file "'"$folder"'/Example.bloom" as alias)' > /dev/null 2>&1 || true
        sleep 2
        shoot "$step" "A Finder window of a folder with one Bloom project file, in icon view with large icons."
    else
        fail "$step: Finder scripting: $id"
    fi
else
    fail "$step: no project file was made: $log"
fi
close_finder_windows

# --- 3. The disk image window --------------------------------------------------------------------

step=670-macos-disk-image.png
if log=$(hdiutil attach "$image" 2>&1); then
    mount_point=$(printf '%s\n' "$log" | awk -F '\t' '/\/Volumes\// { print $NF }' | tail -n 1)
    if [ -d "$mount_point" ]; then
        volume=$(basename "$mount_point")
        open "$mount_point"
        if poll 30 osascript -e 'tell application "Finder" to get window "'"$volume"'"'; then
            id=$(script 'tell application "Finder"
    activate
    set w to window "'"$volume"'"
    set index of w to 1
    return id of w
end tell') && finder_windows+=("$id")
            sleep 2
            shoot "$step" "The disk image window with its background picture, the app and the Applications link."
        else
            fail "$step: Finder showed no window for the volume $volume"
        fi
    else
        fail "$step: hdiutil attached the image but printed no mount point: $log"
    fi
else
    fail "$step: hdiutil attach: $log"
fi
close_finder_windows
if [ -n "$mount_point" ]; then
    if log=$(hdiutil detach "$mount_point" 2>&1 || hdiutil detach -force "$mount_point" 2>&1); then
        mount_point=
    else
        fail "670-macos-disk-image.png: hdiutil detach: $log"
    fi
fi

# --- 4. The Dock with Bloom running --------------------------------------------------------------

step=670-macos-dock.png
if log=$(open -a "$app" 2>&1); then
    if poll 60 pgrep -x Bloom; then
        bloom_pid=$(pgrep -x Bloom | head -n 1)
        if poll 60 bloom_has_window; then
            script 'tell application "System Events" to set frontmost of process "Bloom" to true' > /dev/null 2>&1 || true
            sleep 2
            shoot "$step" "The desktop with Bloom running: its window and its icon in the Dock."
        else
            fail "$step: Bloom started (process $bloom_pid) but showed no window; System Events: $(script 'tell application "System Events" to tell process "Bloom" to get count of windows')"
        fi
    else
        fail "$step: Bloom did not start: no process named Bloom"
    fi
else
    fail "$step: open -a: $log"
fi

# --- 5. The Help menu ----------------------------------------------------------------------------

step=670-macos-help-menu.png
if [ -n "$bloom_pid" ]; then
    item="Install Command-Line Tool…"
    log=$(script '
on run argv
    tell application "System Events"
        tell process "Bloom"
            set frontmost to true
            click menu bar item "Help" of menu bar 1
            delay 1
            if not (exists menu item (item 1 of argv) of menu 1 of menu bar item "Help" of menu bar 1) then error "the Help menu has no item " & (item 1 of argv)
        end tell
    end tell
end run' "$item") && opened=1 || opened=0
    if [ "$opened" -eq 1 ]; then
        sleep 1
        shoot "$step" "Bloom's Help menu open, showing the item Install Command-Line Tool."
    else
        fail "$step: System Events refused or could not find the item: $log"
    fi
    script 'tell application "System Events" to key code 53' > /dev/null 2>&1 || true
else
    fail "$step: Bloom is not running"
fi
quit_bloom

# --- Result --------------------------------------------------------------------------------------

for name in 670-macos-applications.png 670-macos-project-file.png 670-macos-disk-image.png \
    670-macos-dock.png 670-macos-help-menu.png; do
    if [ ! -s "$out/$name" ]; then
        echo "macos.sh: $name was not produced" >&2
        failed=1
    fi
done
exit "$failed"
