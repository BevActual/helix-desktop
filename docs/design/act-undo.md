# Act results and undo

Status: in progress, 2026-10-15. The nineteen commands the shell already runs record a pair, `GetState` carries it, and `Undo` writes the previous value back. The chat note stores the display pair. A confirm-tier undo is not built: the shell does not run those commands yet, so there is no confirmer path to open.

A finished desktop task stores the shell's sentence (`DesktopTask.summary` and `detail` in `core/helix-chat`) and, for a restore the shell recorded, the display pair. The assistant makes one tool call per request (`services/helix-assistant`). The shell carries that call out in `shell/helix-shell-tauri/src-tauri/src/agent_ipc.rs`. The nineteen live restore commands reply with the sentence. The other commands still reply `ok`. That body is what the service returns as the action result.

## Two facts, separate from the risk tier

`protocols/ipc/msg-risk.toml` `[tiers]` says who may run a command (`observe`, `observe-private`, `act`, `confirm`, `blocked`). The comment on `act` calls that tier "reversible local UI and preference changes". That describes the grant. It is not true of each command: `panel-open` is `act` and has nothing to put back, and `wifi-toggle` is `confirm` and does.

Undo uses one new class, decided per command:

| Class | Before and after | Undo |
|---|---|---|
| `restore` | Yes. One short setting the desktop can read back: a percent, on or off, a name, a mode. | Yes. Write the previous value back. |
| `none` | No. | No. |

Both answers are yes only for `restore`. A command that can show a pair and must not be undone, or the reverse, needs a new class. None of the 158 commands is that case. Clipboard is the one that looks like it: the previous text could be put back, and it must not be. It is `none`. The previous clipboard text never goes into the chat file, the export, or the feed.

The inverse is the previous value, not the opposite verb. `volume-up` while muted also unmutes, and a step clamps at 0 and 100, so "volume down by the same step" is not the inverse. `theme-mode-toggle` is not its own inverse when the mode is `auto`. `workspace-switch next` is not undone by `prev` if the person was already at the end of the list.

`restore` is a claim that a reader and a writer of that one value exist, or should. Until the shell can actually read both sides, it records no pair and offers no undo. It does not invent one.

Choice overrides (`session lock`, and the two confirm choices of `session`) stay `none` with the command. `media` is one command and every choice (`play`, `pause`, `next`, and the rest) is `none`: the player is not a setting this desktop writes. If a later command has one choice that restores and another that does not, it gets a `[change-choices]` table in the same shape as `[choices]`.

## Classification

Every key in `[tiers]`, 158 commands. The shell's live set is `helix_agent::agent_msg::SUPPORTED`. Only those run today. A command still needs a class here, including the ones this shell does not run. The check that enforces it is part of the first code change.

### `restore`

The shell carries these out today, and they are the first slice that records a pair:

```
agent-desk-focus
bar-hide
bar-show
bar-toggle
brightness-down
brightness-set
brightness-up
nightlight-disable
nightlight-enable
nightlight-toggle
notification-dnd-set
notification-dnd-toggle
theme-mode-set
theme-mode-toggle
volume-down
volume-mute
volume-set
volume-up
workspace-switch
```

The value for the volume commands is the percent and the mute flag together, because raising the volume clears mute. Undo writes both back. Workspace undo focuses the workspace id that was current (`workspace_focus` in the shell), which is also the inverse of `agent-desk-focus`. Night light's pair is the setting the shell already writes. Driving the display gamma is a separate gap (roadmap: night light).

Classed `restore`, not carried out by this shell yet. No pair until each one has a real reader:

```
ai-usage-localai-start
ai-usage-localai-stop
ai-usage-ollama-start
ai-usage-ollama-stop
bar-auto-hide-set
bar-layer-set
bar-reserve-toggle
bluetooth-disable
bluetooth-enable
bluetooth-toggle
caffeine-disable
caffeine-enable
caffeine-toggle
color-scheme-set
desktop-widgets-hide
desktop-widgets-show
desktop-widgets-toggle
dock-hide
dock-show
dock-toggle
dpms-off
dpms-on
effects-profile-set
keyboard-backlight-down
keyboard-backlight-set
keyboard-backlight-toggle
keyboard-backlight-up
log-level-set
mic-mute
mic-volume-down
mic-volume-set
mic-volume-up
network-toggle
osd-disable
osd-enable
osd-toggle
power-cycle
power-set
tailscale-down
tailscale-toggle
tailscale-up
wallpaper-next
wallpaper-previous
wallpaper-random
wallpaper-set
wifi-disable
wifi-enable
wifi-toggle
```

Radios, `mic-mute`, and the local-model start and stop commands are `confirm`. Their undo asks in the shell's confirmer. The others in this second list are `act`, and undo applies immediately once the shell can read them.

Wallpaper's machine value is the previous file. The chat shows the file name. The path stays in the shell.

### `none`

Reads, including private ones. They report the current state. They do not change it:

```
ai-usage-localai-endpoint
ai-usage-status
bluetooth-status
brightness-list-backlight-devices
clipboard-text
color-scheme-get
desktop-ai-status
desktop-capture
firewall-status
log-level-status
notification-dnd-status
notification-history
privacy-status
printers-status
screenshot-fullscreen
screenshot-ocr
screenshot-scan-qr
status
tailscale-status
theme-mode-get
wallpaper-get
wifi-status
workspace-alert-status
```

Opening, closing, or focusing a surface. `panel-close` exists, and it is a new action. It is not the undo of `panel-open`. The same for Settings, the window switcher, the taskbar, and the on-screen indicators that do not change a value (`volume-osd`, `brightness-osd`, `mic-volume-osd`, `keyboard-backlight-osd`):

```
ai-usage-localai-open
brightness-osd
desktop-widgets-edit
desktop-widgets-exit
desktop-widgets-toggle-edit
keyboard-backlight-osd
lockscreen-widgets-edit
lockscreen-widgets-exit
lockscreen-widgets-toggle-edit
mic-volume-osd
panel-close
panel-open
panel-toggle
settings-close
settings-open
settings-open-widget
settings-toggle
taskbar-cycle
volume-osd
window-switcher
```

Everything else. A file, a process, a window, a notification, a recording, or a reload is not one setting we can write back. Clipboard write and clear are in this list on purpose.

```
ai-usage-localai-delete
ai-usage-localai-install
ai-usage-localai-model
ai-usage-ollama-cancel
ai-usage-ollama-delete
ai-usage-ollama-model
ai-usage-ollama-pull
ai-usage-refresh
annotate
app-launch
clipboard-clear
clipboard-copy
config-reload
desktop-ai-describe
desktop-ai-dictate
desktop-ai-read
desktop-ai-rewrite
desktop-ai-speak
desktop-ai-summarize
desktop-ai-translate
dock-reload
firewall-panic-toggle
greeter-sync
keyboard-layout-cycle
media
nightlight-force-toggle
notification-clear-active
notification-clear-history
notification-invoke-latest
notification-show
printers-start
screen-record-replay-save
screen-record-replay-start
screen-record-replay-stop
screen-record-replay-toggle
screen-record-start
screen-record-stop
screen-record-toggle
screenshot-annotate
screenshot-pick-color
screenshot-region
session
templates-apply
window-close
workspace-alert-add
workspace-alert-add-window
workspace-alert-clear
workspace-alert-clear-all
```

`session lock`, `session suspend`, and `session lock-and-suspend` are `none` with `session`. Locking and suspending are not put back by the assistant.

## How a result is recorded

The snapshot is taken in the shell, in `agent_ipc::execute`, immediately before the write and again after it succeeds. That is the only place that both holds the live value and performs the change. `helix-chat` never reads volume, brightness, or theme.

On success the shell keeps a ledger entry in the shell process:

| Field | Stays in the shell | Shown and saved |
|---|---|---|
| Task id | Yes | Yes, as today |
| Setting key (so a newer change of the same setting retires the older undo) | Yes | No |
| Previous machine value and the value after the write | Yes | The display form only |
| Command name | Yes | No |

The display form is at most 80 characters, cleaned the way task text already is. Examples: `40%, muted` to `45%`, `dark` to `light`, `off` to `on`, `shown` to `hidden`. A failure, a stop before the write, or a `none` command stores no ledger entry. `before` and `after` are empty.

A newer successful `restore` on the same setting key drops the older entry's undo. The older note keeps the pair it already saved. Volume, mute included, is one key. `workspace-switch` and `agent-desk-focus` share the workspace key. One task keeps one ledger entry, the latest setting it changed, because the feed has one pair.

The ledger dies with the shell process. After a restart the note still shows the pair, and Undo is gone. The task id already restarts with the shell, so a saved chat cannot name a command for the new process to run.

## What is stored on the task

`DesktopTask` gains three fields. Old chat files omit them and load as empty and not undone.

| Field | Meaning |
|---|---|
| `before` | Display form of the previous value. Empty when this task has no pair. |
| `after_text` | Display form of the value after the write. The name is not `after`: that field is already where the note sits in the thread, and old files store a message count there. |
| `undone` | The shell reported that the previous value was written back. |

These are display. A hostile chat file can put text in them, and that text is cleaned and shown. It cannot make the shell run a command. There is no stored command line.

`context()` keeps omitting tasks, so the pair is never sent to a model. Fork and the 100-note cap treat the new fields like the rest of the task. The Markdown export adds the pair to the existing desktop note (`40%, muted to 45%`) when the sentence does not already contain both sides, and does not add a command.

`detail` stays the sentence for a failure or a `none` result. When a pair exists, the sentence is that pair in words, and `before` and `after_text` are the two sides.

## The feed

`org.helix.Shell.Act1` `GetState` gains three keys, beside the ones it has now (`task`, `phase`, `request`, `text`, `description`, `reason`, `failed`):

| Key | Type | Meaning |
|---|---|---|
| `before` | string | Display form, or empty |
| `after` | string | Display form, or empty |
| `undoable` | bool | This task id is still in the ledger and the live value still equals `after` |

Missing keys mean empty and not undoable, so an older shell and a newer Mobius still work. The inverse is not on the bus.

`Undo` takes the task id and nothing else. It does not take a command, a value, or an approval.

- Unknown id, empty ledger, or the live value no longer equals `after`: return an error, change nothing. The note keeps the pair. `undoable` becomes false.
- `act` tier: write the previous value back through the same code as the sliders and key bindings. No second confirmation. The person is restoring their own previous value, and the model did not choose the inverse.
- `confirm` tier: open the shell confirmer with a sentence the shell wrote ("Turn Wi-Fi back off"). Apply only if the person allows it there. Mobius can bring that surface forward (Review) or Stop. It cannot allow it. That is the same rule as any other confirmation (`trusted-surface.md`).

When the write-back succeeds, the shell clears that ledger entry and the feed reports the restored value. Mobius sets `undone` and shows the restored value. Undo of a task that never changed anything does nothing.

`Stop` stays cancel-only. `Undo` is only for a task whose write already succeeded.

## Order of the code, when it is written

1. Done. `[changes]` in `protocols/ipc/msg-risk.toml`, one string per command, `restore` or `none`, matching this note. `risk.rs` tests fail if a command is missing, unknown, or extra. `RiskPolicy::change_for` reads the table. The `act` comment no longer calls that tier reversible.
2. Done for the nineteen commands the shell already runs. `agent_ipc::execute` reads the value, writes, reads again, and the reply body is the sentence (`Volume 40%, muted to 45%.`). The service already forwards that body. The machine value stays in a ledger in the shell process (`ChangeLedger`), and only while the assistant is thinking or acting. A workspace step cannot be re-read until niri's event arrives, so its "after" is the next or previous workspace in the strip. `panel-open` and the other `none` commands still reply `ok`.
3. Done. `GetState` adds `before`, `after`, and `undoable`. `undoable` is computed from the ledger and the live value, and a missing key is empty and not undoable. `Undo` takes the task id only. It writes the previous value back through the same code as the sliders and key bindings, and only while the live value still equals what that task wrote. The ledger entry is removed after that write succeeds. One task keeps one pair, the latest setting it changed. A confirm-tier undo is not in this step: none of the nineteen live commands is confirm tier, so `Undo` does not open the confirmer.
4. Done for the display pair. `DesktopTask` stores `before`, `after_text`, and `undone`. The note shows Undo only while the shell's latest `undoable` flag names that task. The button calls `Undo` and does not send the pair anywhere. After a restart the flag is false, so the button is gone and the saved pair remains. The shell's own Act door shows the same Undo on its result: a change asked there is not written into a Mobius chat, so the door is the only place that result can be put back. If this Mobius window handed the task over, undoing from the door still marks that note.

## Tests that land with the code

- Policy: every command in the schema has a class, and the class is one of the two words.
- Shell: a volume step records percent and mute; undo restores both; undo is refused if the percent changed again; `panel-open` records no pair.
- Chat: old files load with empty `before` and `after_text`; the pair is in the saved file and the export; `context()` still does not contain it; a file that puts a command string in `before` keeps it as text.
- Feed: `GetState` carries the new keys; `Undo` with a task the shell does not hold is an error and writes nothing. A confirm-tier undo is not tested here, because the shell does not run a confirm-tier restore command yet.

## Out of scope

A multi-step plan (this assistant makes one call). Synthetic input. Offering undo for clipboard contents. An 8 second undo timer (that rule in `app-visual-language.md` is for an in-app edit). Guessing a pair for a `restore` command this shell does not run yet.
