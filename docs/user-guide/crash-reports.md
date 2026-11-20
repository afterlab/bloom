# Crash reports

Status: working

Updated: 2026-11-20

When Bloom, `bloom-cli` or `bloom-mcp` quits unexpectedly, it leaves a crash report on your
computer. Bloom never sends a report anywhere. There is no crash server, and nothing goes to a
third-party service. You decide whether to share a report, and what to share.

## What a report is

Each crash gets its own folder with two files:

- `report.txt` is a short text report: the program that crashed, Bloom's version and build, your
  operating system and its version, the GPU and its driver, when the program started and crashed,
  and the last 100 lines Bloom logged before the crash. Read it before you share it. It can hold
  the names of files and folders Bloom logged, for example a project or media path.
- `minidump.dmp` is a minidump: the stack of every thread at the moment of the crash, the loaded
  libraries, and a small amount of the memory around each stack. It does not hold your project,
  your media or the picture on screen. It can hold small pieces of whatever Bloom had in memory
  near a stack, such as a file name or a piece of text.

A separate small program, the crash handler (`bloom-crash-handler`), writes the minidump. It runs
beside each Bloom program and writes the report even when the crash happened on another thread,
in a GPU driver, from a stack overflow or from an abort.

## Where reports are kept

Reports are in the `crashes` folder of Bloom's per-user data folder:

| System | Crash folder |
| --- | --- |
| Linux | `~/.local/share/Afterlab/Bloom/crashes` (or `$XDG_DATA_HOME/Afterlab/Bloom/crashes` when `XDG_DATA_HOME` is set) |
| macOS | `~/Library/Application Support/Afterlab/Bloom/crashes` |
| Windows | `%LOCALAPPDATA%\Afterlab\Bloom\crashes` |

Each crash is a folder named after when it was found, the program and a short id, for example
`2026-11-20_14-03-27_bloom_6f1c2a9e`. The `database` and `sessions` folders beside them are the
crash handler's own working files; you do not need to open them.

Bloom keeps the 20 newest crashes and none older than 30 days. Any Bloom program removes older
ones when it starts. You can delete the folder of a crash, or the whole `crashes` folder, at any
time.

## After a crash

The next time you start Bloom, it says that Bloom quit unexpectedly and offers:

- **Report on GitHub** opens a new issue in Bloom's GitHub repository in your browser, with the
  text report filled in, and shows the crash's minidump in its folder. Nothing is sent until you
  submit the issue yourself, and you can edit the text first. The issue gets only the text report.
  To include the minidump, drag `minidump.dmp` from the folder onto the issue.
- **Copy Report** copies the text report, to paste wherever you like.
- **Open Crash Folder** shows the crash's folder in your file manager.
- **Dismiss** closes the notice.

The notice appears once for each crash. Whichever button you choose, the next start does not ask
about that crash again; the report stays in the crash folder until it is pruned. When several
crashes happened since the last notice, the notice is about the newest one and says how many more
are in the folder.

A crash of `bloom-cli` or `bloom-mcp` is offered the same way the next time you start the
application. The notice never blocks the window. It does not appear in a session that runs
without a window or that an agent drives over the MCP server; the crash then waits in the folder
for the next start you are at the keyboard for.

## Sharing a report

1. Open the [issues page](https://github.com/afterlab/bloom/issues) and check whether the crash is
   already reported. If it is, add your text report to that issue.
2. Otherwise choose **Report on GitHub** in the notice, or open a new issue and paste the text
   report (**Copy Report**, or open `report.txt` from the crash folder).
3. Say what you were doing when Bloom quit, and whether it happens again when you repeat it.
4. If you are willing to share it, attach `minidump.dmp` from the same folder. GitHub accepts it
   when its name ends in `.dmp`; if it refuses the file, put it in a zip file first.

The developers read the minidump with the debug symbols kept for each release, so the stacks name
Bloom's functions and source lines. Nothing else from your computer is needed.
