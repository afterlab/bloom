#!/bin/bash
# Captures the installed Bloom's icon on Linux (#670): the application menu, a panel launcher and the
# running window, on a private X display. Needs these Ubuntu packages (the release workflow installs
# them) and the libraries Bloom itself declares:
#
#   xvfb openbox tint2 rofi imagemagick x11-utils xdotool
#
#   linux.sh <out folder> <installed prefix> <home>
#
# <installed prefix> holds bin/Bloom (the folder install.sh put Bloom in). <home> is the HOME it was
# installed for: the menu entry and the icons are under <home>/.local/share.
set -u

[ $# -eq 3 ] || { echo "usage: linux.sh <out folder> <installed prefix> <home>" >&2; exit 2; }
out=$1
prefix=$2
home=$3
application=id.afterlab.bloom
desktop_file=$home/.local/share/applications/$application.desktop

mkdir -p "$out"
out=$(cd "$out" && pwd)
work=$(mktemp -d)
pids=()
failed=0

cleanup() {
    for pid in "${pids[@]}"; do
        kill "$pid" > /dev/null 2>&1 || true
    done
    rm -rf "$work"
}
trap cleanup EXIT

fail() {
    echo "linux.sh: step failed: $*" >&2
    failed=1
}

[ -x "$prefix/bin/Bloom" ] || { echo "linux.sh: $prefix/bin/Bloom is not installed" >&2; exit 1; }
[ -f "$desktop_file" ] || { echo "linux.sh: no menu entry at $desktop_file" >&2; exit 1; }
for tool in Xvfb openbox tint2 rofi import identify xdotool xdpyinfo xprop; do
    command -v "$tool" > /dev/null 2>&1 || { echo "linux.sh: $tool is not installed" >&2; exit 1; }
done

# The environment of everything started here: nothing of the real session leaks in.
mkdir -p "$work/config" "$work/cache" "$work/run"
chmod 700 "$work/run"
unset WAYLAND_DISPLAY
export HOME=$home
export XDG_DATA_HOME=$home/.local/share
export XDG_CONFIG_HOME=$work/config
export XDG_CACHE_HOME=$work/cache
export XDG_RUNTIME_DIR=$work/run
export XDG_DATA_DIRS=$XDG_DATA_HOME:/usr/local/share:/usr/share
export QT_QPA_PLATFORM=xcb

# --- A private display ---------------------------------------------------------------------------

number=
for candidate in $(seq 90 140); do
    if [ ! -e "/tmp/.X$candidate-lock" ] && [ ! -e "/tmp/.X11-unix/X$candidate" ]; then
        number=$candidate
        break
    fi
done
[ -n "$number" ] || { echo "linux.sh: no free X display number between 90 and 140" >&2; exit 1; }
unset DISPLAY
Xvfb ":$number" -screen 0 1280x800x24 -nolisten tcp > "$work/xvfb.log" 2>&1 &
pids+=("$!")
export DISPLAY=":$number"
ready=0
for _ in $(seq 50); do
    if xdpyinfo > /dev/null 2>&1; then ready=1; break; fi
    kill -0 "${pids[0]}" 2> /dev/null || break
    sleep 0.2
done
[ "$ready" -eq 1 ] || { echo "linux.sh: Xvfb did not start:" >&2; cat "$work/xvfb.log" >&2; exit 1; }

openbox > "$work/openbox.log" 2>&1 &
pids+=("$!")
ready=0
for _ in $(seq 50); do
    if xprop -root _NET_SUPPORTING_WM_CHECK 2> /dev/null | grep -q 'window id'; then ready=1; break; fi
    sleep 0.2
done
[ "$ready" -eq 1 ] || { echo "linux.sh: openbox did not start:" >&2; cat "$work/openbox.log" >&2; exit 1; }

# --- Helpers -------------------------------------------------------------------------------------

: > "$out/captures.txt"

# shoot <file name> <what it shows>: takes the whole screen and checks it is not blank.
shoot() {
    local file=$out/$1 log colors
    if ! log=$(import -window root "$file" 2>&1); then
        fail "$1: import could not take the screen: $log"
        return
    fi
    colors=$(identify -format '%k' "$file" 2>&1) || { fail "$1: identify: $colors"; return; }
    if [ ! -s "$file" ] || [ "$colors" -lt 8 ] 2> /dev/null; then
        fail "$1: the picture is empty or flat ($colors colors)"
        return
    fi
    echo "$1: $2" >> "$out/captures.txt"
}

# wait_for_window <tries> <xdotool search arguments...>: polls, returns 0 when a window is found.
wait_for_window() {
    local tries=$1
    shift
    for _ in $(seq "$tries"); do
        if xdotool search "$@" > /dev/null 2>&1; then return 0; fi
        sleep 0.2
    done
    return 1
}

# --- 1. The application menu ---------------------------------------------------------------------

rofi -show drun -show-icons -icon-theme hicolor -filter Bloom \
    -theme-str 'listview { lines: 3; } element-icon { size: 32px; }' \
    > "$work/rofi.log" 2>&1 &
rofi_pid=$!
pids+=("$rofi_pid")
if wait_for_window 50 --class rofi; then
    sleep 1
    shoot 670-linux-menu.png "The application menu filtered to Bloom, with the Bloom entry and its icon."
else
    fail "670-linux-menu.png: rofi showed no window: $(cat "$work/rofi.log")"
fi
kill "$rofi_pid" > /dev/null 2>&1 || true
wait "$rofi_pid" 2> /dev/null || true

# --- 2. A panel launcher -------------------------------------------------------------------------

cat > "$work/tint2rc" << END
rounded = 0
border_width = 0
background_color = #202020 100
border_color = #202020 100

rounded = 0
border_width = 0
background_color = #404040 100
border_color = #404040 100

rounded = 0
border_width = 0
background_color = #606060 100
border_color = #606060 100

panel_items = LT
panel_size = 100% 56
panel_position = bottom center horizontal
panel_margin = 0 0
panel_padding = 8 6 8
panel_background_id = 1
launcher_padding = 6 6 8
launcher_icon_size = 44
launcher_icon_theme = hicolor
launcher_item_app = $desktop_file
task_icon = 1
task_text = 1
task_icon_asb = 100 0 0
task_active_icon_asb = 100 0 0
task_maximum_size = 180 44
task_padding = 6 4
task_background_id = 2
task_active_background_id = 3
END
tint2 -c "$work/tint2rc" > "$work/tint2.log" 2>&1 &
tint2_pid=$!
pids+=("$tint2_pid")
if wait_for_window 50 --class tint2; then
    sleep 1
    shoot 670-linux-launcher.png "A panel whose launcher holds the Bloom menu entry with its icon."
else
    fail "670-linux-launcher.png: tint2 showed no panel: $(cat "$work/tint2.log")"
fi

# --- 3. The running window -----------------------------------------------------------------------

# The menu entry's Exec line, the way a menu runs it: field codes dropped.
exec_line=$(sed -n 's/^Exec=//p' "$desktop_file" | head -n 1 | sed 's/ *%[fFuUdDnNickvm]//g')
[ -n "$exec_line" ] || fail "670-linux-window.png: the menu entry has no Exec line"
if [ -n "$exec_line" ]; then
    sh -c "exec $exec_line" > "$work/bloom.log" 2>&1 &
    bloom_pid=$!
    pids+=("$bloom_pid")
    if timeout 120 xdotool search --sync --onlyvisible --class '^Bloom$' > /dev/null 2> "$work/xdotool.log"; then
        # A smaller window, so the panel under it and the title bar above it are on the picture.
        for window in $(xdotool search --onlyvisible --class '^Bloom$' 2> /dev/null); do
            xdotool windowsize "$window" 1000 640 windowmove "$window" 140 40 > /dev/null 2>&1 || true
        done
        sleep 3
        shoot 670-linux-window.png "The running Bloom window with its icon in the title bar and the task bar."
    else
        fail "670-linux-window.png: no Bloom window appeared (the program's own output follows): $(cat "$work/xdotool.log" "$work/bloom.log")"
    fi
    # Ask the window to close, then stop the process this script started.
    for window in $(xdotool search --onlyvisible --class '^Bloom$' 2> /dev/null); do
        xdotool windowquit "$window" > /dev/null 2>&1 || true
    done
    for _ in $(seq 25); do
        kill -0 "$bloom_pid" 2> /dev/null || break
        sleep 0.2
    done
    kill "$bloom_pid" > /dev/null 2>&1 || true
    wait "$bloom_pid" 2> /dev/null || true
fi

# --- Result --------------------------------------------------------------------------------------

for name in 670-linux-menu.png 670-linux-launcher.png 670-linux-window.png; do
    if [ ! -s "$out/$name" ]; then
        echo "linux.sh: $name was not produced" >&2
        failed=1
    fi
done
exit "$failed"
