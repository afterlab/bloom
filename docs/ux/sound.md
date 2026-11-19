# Sound Feedback And Notifications

Status: current. Architecture: [`architecture/ui-feedback.md`](../architecture/ui-feedback.md).
Settings: [`user-guide/preferences.md`](../user-guide/preferences.md).

## Principles

- Sound reports a result the artist may be waiting for. It does not decorate every gesture.
- **Event sounds are on by default** (render complete and failed, import results, warnings and
  refused commands). **Interaction sounds are off by default** (switches, choices, dialogs, tools,
  drag and drop) and are an opt-in in Preferences.
- Sounds are quiet. The bundled set is level-matched well below full scale and the default volume is
  40%.
- Never during scrubbing, and no interaction sound during playback. Playback itself is silent apart
  from the timeline audio; a render or import result is still heard.
- No hover sounds, no typing sounds, no sound for selection or for every keystroke.
- A missing audio device is silent and not an error. Nothing here ever shows a dialog.
- The same sound is not played twice within 80 ms: an error dialog that follows a failed render is
  heard once.

## Render notifications

When a render or export finishes or fails and Bloom is **not the active application**, the operating
system shows a notification ("Render complete" or "Render failed" with the message). Clicking it
brings Bloom forward where the platform supports that. The sound plays in either case if Event
sounds are on; the notification follows its own Preferences switch.

| Platform | Mechanism | Status |
| --- | --- | --- |
| Linux | `org.freedesktop.Notifications` over QtDBus (a click raises Bloom); the tray message without QtDBus or a session bus | Verified: builds with QtDBus; the notifier logic is covered by tests with a fake notifier |
| macOS | `UNUserNotificationCenter` from a bundle; the tray message from an unbundled binary | Written to the platform API, not built or run on macOS |
| Windows | `QSystemTrayIcon::showMessage` (the supported fallback); a click raises Bloom | Not built or run on Windows |

## Agent activity

An MCP agent can work far faster than an artist, and Bloom must stay quiet while it does. Only
what the artist did is reported as it happens. An agent's work is reported once, as a summary.

- **Renders, exports and captures an agent or a script started.** These are transient export queue
  jobs (`transient` on `ExportJobSucceeded` and `ExportJobFailed`). They play no sound and post no
  notification of their own. Bloom counts them. After a quiet interval of 10 s with no new agent
  activity, it posts at most one summary, for example "Agent: 37 renders, 1 failed", and plays
  render_succeeded, or render_failed if anything failed. The summary is posted only while Bloom is
  **not** the active application; when the artist is watching, the counts are dropped.
- **An agent's refused command.** It is answered to the agent over MCP and listed in the MCP Server
  panel's activity. It plays no refused cue and flashes no status message. A retired undo step
  caused by an agent's edit is not reported either.
- **An agent's edit that broke a composition's compile.** One warning per composition per quiet
  interval. A repeat updates that composition's line in the MCP Server panel ("×3") and is
  counted into the summary ("2 compositions failed to compile").
- **An agent session** (`session.begin`, with `quiet`, which is on by default). It holds all of the
  above until the session ends, then tells it once: "Agent session finished: Agent session “Shot
  12”: 37 renders, 1 failed". If Bloom is active at that moment, there is no summary, but one
  warning plays if the session broke a composition.
- **Preferences > Sounds > "Sounds and notifications for agent activity"** controls all of the
  above. It is **on by default**: agent activity stays quiet and is summarized. Off makes agent
  activity silent: no summary, no warning, no notification. Event sounds, the master switch and
  the render notification switch still apply to the summary. The MCP preferences key is
  `agentActivityFeedback`.

The one agent-caused notice that remains is the status message for an edit that would remove a
composition's Output. That is the artist's composition losing its picture, so it is shown whoever
made the edit (#716).

## Sounds

UI sounds: SND01 "sine" by Yasuhiro Tsuchiya, from SND (snd.dev) by Dentsu Inc. CX Creative Center.
The credit is in `THIRD_PARTY_NOTICES.md` and the Open Source Licenses window; the selection,
processing and digests are in `src/ui/sound/assets/provenance.md`. Bloom bundles 13 processed
sounds, not the pack.

| Sound | From SND01 | Used by |
| --- | --- | --- |
| `render_succeeded` | celebration ("Check me all") | RenderSucceeded; the Preferences test sound |
| `render_failed` | caution | RenderFailed, ImportFailed, Warning |
| `import_finished` | notification | ImportFinished |
| `refused` | disabled | Refused; Disabled at half level |
| `button` | button | ControlPress |
| `select` | select | Select |
| `toggle_on`, `toggle_off` | toggle_on, toggle_off | ToggleOn, ToggleOff |
| `modal_open`, `modal_close` | transition_up, transition_down | ModalOpen, ModalClose |
| `tool_switch` | tap_01 | ToolSwitch |
| `drag_start` | swipe_01 | DragStart |
| `drop` | tap_03 | Drop |

## Audit of user-facing events

Category **E** is an event sound (default on); **I** is an interaction sound (default off).
"Silent" means the event was considered and deliberately has no sound.

| Event | Where it fires | Cat. | Sound | Default |
| --- | --- | --- | --- | --- |
| Export queue job finished | `ExportJobSucceeded` event read by `ExportQueueAdapter` (`export_queue_adapter.cpp`); every export, frame, range or composition, is a queue job | E | render_succeeded + notification when inactive | On |
| Agent's or script's render, export or capture finished or failed | the same events with `transient` set (MCP `render`, `export`, `capture`) | E | None per job; counted into the agent summary | On (agent switch) |
| Agent summary | `UiFeedback::flushAgentActivity`, 10 s after the last agent activity, or at the end of a quiet agent session | E | render_succeeded, or render_failed if anything failed or broke; notification only when inactive | On (agent switch) |
| Export queue job finished with warnings | same event with `withWarnings` | E | render_succeeded + notification | On |
| Export queue job failed or refused (including verification failures) | `ExportJobFailed` event | E | render_failed + notification; the error dialog adds no second sound within 80 ms | On |
| Export cancelled, queue drained | `ExportJobCancelled`, `ExportQueueDrained` | none | Silent: the artist asked for the cancel, and every job in a drained queue was already announced | n/a |
| RAM preview cache complete | `RamPreviewController::cachingFinished` | none | Silent: it refills after most edits and would sound constantly; autoplay is the cue | n/a |
| Import finished | `AssetController::importFinished` (`asset_controller.cpp`) | E | import_finished | On |
| Import partial (some items skipped) | same signal | E | render_failed (caution) | On |
| Import failed (nothing added) | same signal | E | render_failed (caution) | On |
| Import cancelled or discarded | same signal | none | Silent | n/a |
| Project save failed | `ProjectHost::saveFinished` inform dialog (`main_window.cpp`) | E | render_failed via the Error prompt | On |
| Project open or copy failed | `openFinished`, `copyFinished` inform dialogs (`main_window.cpp`) | E | render_failed via the Error prompt | On |
| Recover offered | `projectRecoverUnsaved` confirmation, a Question prompt (`project_host.cpp`) | I | modal_open, like any confirmation | Off |
| Command refused | `CompositionSession::commandRejected` (`composition_session.cpp`), wired in `apps/bloom/main.cpp` | E | refused | On |
| Undo steps retired to make room for the artist's edit | `CompositionSession::noticePosted` from `noteUndoRetirement` (`composition_session_panel_rebuild.cpp`) | none | Silent: a status bar notice naming the step limit or the memory limit, once per burst (no notice within 10 s of the previous retirement); it is information, not a refusal (#744) | n/a |
| Agent's command refused, or undo steps retired for an agent's edit | `CompositionSession::handleResult` and `handleCommandEvent` with `CommandOrigin::Agent` | none | Silent, with no status message: answered over MCP and listed in the MCP Server activity | n/a |
| Agent's edit broke a composition's compile | `McpServerAdapter::noteCompileFailure` calls `UiFeedback::noteAgentCompileFailure` | E | render_failed (Warning), once per composition per 10 s; held during a quiet agent session | On (agent switch) |
| Error dialog (any kit prompt with Error tone) | `kit::inform` / `kit::buildConfirm` (`kit/dialog_prompts.cpp`), seen by `InteractionSounds` | E | render_failed | On |
| Confirmation, Warning-tone and Info dialogs | same | I | modal_open (see Modal open) | Off |
| Modal dialog opened | `QDialog` show (`interaction_sounds.cpp`) | I | modal_open | Off |
| Modal dialog closed | `QDialog` hide | I | modal_close | Off |
| Pressed a disabled button or control | mouse press on a disabled kit control (`interaction_sounds.cpp`) | I | refused at half level | Off |
| Button press | `KButton::clicked`, `KIconButton::clicked` (not inside a dialog; its own open and close cue instead) | I | button | Off |
| Switch, checkbox or icon toggle changed by the user | `KSwitch`, `KCheckBox`, `KIconToggle::toggled` after a press | I | toggle_on or toggle_off | Off |
| Programmatic toggle (restore, reset, sync) | same signal without a press | none | Silent | n/a |
| Dropdown item chosen | `KDropdown::activated` | I | select | Off |
| Radio group or segmented control changed by the user | `KRadioGroup::currentIndexChanged` during a press or key | I | select | Off |
| Tool switched | a `KIconToggle` in the toolbar tool group (`contextBarTools`) checked | I | tool_switch | Off |
| Drag started (Assets to Timeline, files) | `QEvent::DragEnter` the first time (`interaction_sounds.cpp`) | I | drag_start | Off |
| Drop | `QEvent::Drop` (`asset_drop.cpp`, `layer_drop.cpp` targets) | I | drop | Off |
| Scrub, playhead drag, canvas drag, marquee | timeline, viewer | none | Silent, and mutes interaction cues until release | n/a |
| Playback start, stop, loop | `PlaybackController::stateChanged` | none | Silent; mutes interaction cues while playing | n/a |
| Menu open, menu item chosen, hover, tooltip | menus | none | Silent | n/a |
| Typing, value-field edit, text entry | fields | none | Silent | n/a |
| Selection changes | session | none | Silent | n/a |
| Loading or progress loop | task progress | none | Not bundled: no task is long enough to benefit, and a loop contradicts the quiet default | n/a |
| Script errors | script panel | none | Silent for now: they are shown inline in the panel | n/a |

## Not covered

- Per-sound customization, other packs and custom files.
- Sound for panel focus, layout changes or notifications from other applications.
