# Trusted surface: the compositor protects the approval dialog

Status: design, 2026-10-08. For review before any change to the compositor fork (`helix-niri`). Nothing here is built.

## The problem, with evidence

Agent1 makes an agent ask a person before a risky action. The person's answer is a click on **Allow** in a dialog the shell draws
(`rust/helix-shell-tauri/src-tauri/src/confirmer.rs`: an Overlay layer surface, namespace `helix-tauri-confirmer`, exclusive keyboard).
The design says "a client can never confirm its own request" because the confirmer is a different D-Bus connection. That holds on D-Bus. It
does not hold on the screen:

1. **Synthetic input.** The compositor offers `zwlr_virtual_pointer_v1`, `zwp_virtual_keyboard_v1` and `zwp_input_method_v2` to every client that is
   not in a sandbox (`client_is_unrestricted` in `helix-niri/src/niri.rs`). An agent that runs as the person can ask for an approval and then click
   Allow itself. **This is already demonstrated by our own test**: `tools/nested/confirmer-test.sh` approves a dialog with a virtual-pointer click
   (`click_id confirm-allow`). The 700 ms arming delay and "Enter never approves" do not help against a pointer click that waits 700 ms.
2. **Capture.** `wlr-screencopy`, `ext-image-copy-capture` and the PipeWire screencast can read the dialog (the arguments an action will run with,
   the verified client). An agent can watch the approval it is waiting for, or read what the person is being asked.
3. **Covering and clickjacking.** Another Overlay surface can be placed above the dialog (the later-created one wins), cover the real arguments with
   harmless text, or sit transparent over **Allow** and take the click.
4. **Focus theft.** Another client can ask for exclusive keyboard on the same layer and receive the keys meant for the dialog.
5. **Imitation.** A client can draw something that looks like the dialog. Content cannot prove it is the real one.

Threats 3 to 5 need only an ordinary client; threat 1 and 2 need nothing but the protocols the compositor offers by default.

## Non-goals

A program that can edit the compositor's config, attach a debugger to the shell, or replace the shell binary already owns the session; sandboxing
(security contexts, per-app Flatpak) is a deployment matter. This design makes the *compositor* stop the five things above for programs that are
merely running as the person.

## Proposal: a trusted-surface role, enforced by the compositor

### 1. Who may hold the role

A layer surface is *trusted* only if both hold: its namespace is the configured one, and the client's executable (read from the connection's pid,
`/proc/<pid>/exe`, canonical path) is on the configured list. Both are in the compositor's config, which a running program cannot change for the
running compositor, and the exe cannot be claimed by a program. A surface that has only the namespace is an ordinary surface.

```kdl
trusted-surface {
    namespace "helix-tauri-confirmer"
    exe "/usr/local/bin/helix-shell-tauri"
    exe "/usr/bin/helix-shell-tauri"
    rim-color "#ff9f0a"        // drawn by the compositor around trusted surfaces
}
```

With no `trusted-surface` block nothing changes: stock behaviour.

### 2. What the compositor guarantees while a trusted surface is mapped

| Guarantee | Defends | How, in the fork |
|---|---|---|
| Always the topmost thing on its output, above every layer surface, popup and lock-less overlay | covering, clickjacking | render its elements last in the output element list; no layer ordering can place anything above |
| It holds keyboard and pointer focus until it closes; no other client's exclusive-keyboard request can take them | focus theft | focus is decided by the role before layer-shell exclusivity |
| Input that did not come from a physical device is dropped when it would reach a trusted surface: virtual pointer, virtual keyboard, input method, and synthetic events from any other protocol | threat 1 | mark events by origin at the protocol handlers (`on_virtual_pointer_*`, the virtual keyboard path, input method commit) and refuse delivery while the target is trusted. Physical devices and the compositor's own bindings are unaffected |
| Its pixels are never given to screencopy, image capture, screencast or screenshots (the area is filled, as `block-out-from` does today for rules) | capture | reuse `block_out_from`'s machinery (`src/layer/mod.rs`, `should_block_out`) but force it for the role instead of leaving it to a rule a user may forget |
| The compositor draws a rim around trusted surfaces in `rim-color`; no client can draw over or under it | imitation | one extra render element per trusted surface; clients cannot influence it. A person learns "approve only if the rim is there" |

While a trusted surface is mapped, a short list of compositor actions that could hide it stays as they are (the person's own key bindings keep working:
that is the person, not a program).

### 3. Telling the shell

`niri msg helix-capabilities` (already used to detect the glass effect) gains `trusted_surfaces: true`, and `niri msg trusted-surfaces` lists which
surfaces currently hold the role (namespace, output, whether the exe check passed), so the shell and tests can see what the compositor believes.
The shell's behaviour:

* **Role granted**: nothing extra; the dialog works as today and is now protected.
* **Role not granted or compositor lacks the feature** (stock niri, wrong exe path after an upgrade): the dialog shows a plain line "This compositor
  cannot protect approvals" and, by default, **confirm-tier actions are refused** rather than approved without protection. A setting
  (`Privacy & agents`) allows approving anyway, off by default, and every such approval is marked in the audit.

### 4. What stays the same

Agent1, the broker, tiers, the arming delay, "Enter never approves", the queue, the audit. The compositor adds the guarantee that the surface those
rules protect cannot be clicked, read or covered by the agent.

## Tests (written before the code, all nested, all must fail without the change)

* Synthetic click: an attacker program connects to the nested compositor with `wl-pointer` and clicks **Allow** of a pending approval. With the role
  active the request stays pending; with the role off (control run) the same click approves. The same for `wtype` keys and an input-method commit.
* Capture: `grim`-style screencopy and an image-copy capture of the output while the dialog is up: the dialog's rectangle is uniform fill, the rest is
  the desktop.
* Covering: an attacker maps an Overlay surface above, then one covering Allow; a hit test at Allow still reaches the dialog; a screenshot shows the
  dialog (and its rim) on top.
* Focus theft: an attacker asks for exclusive keyboard after the dialog; the dialog still receives Tab/Space; the attacker receives nothing.
* Imitation: an attacker draws a perfect copy of the dialog; it has no rim.
* Wrong executable: the same namespace from a copy of the shell at another path gets none of the protection and the shell reports the role as
  not granted (so confirm-tier refuses).
* Physical input still works: a click injected as a *device* event (the nested winit backend) approves.
* Rebase safety: with the config block absent, a render and input trace of an ordinary session is unchanged.

The physical-input test needs a way to produce device events in the nested compositor; the winit backend's pointer does that (the real window
receives real clicks), so the existing pointer tool that uses virtual pointers will have a `--device` variant for it.

## Cost and risk

* **Rebase burden** (`HELIX.md` already lists touch points): new, small, additive points in `src/niri.rs` (render order, focus), `src/layer/mod.rs`
  (role), `src/handlers/mod.rs` (origin marks), `niri-config` (the block), `niri-ipc` and `src/ipc/*` (capabilities + the list). No change to layout
  code. Listed in `HELIX.md` as the fork grows.
* **Lock-out**: a trusted surface that never closes would hold the focus. It is only ever the shell's approval dialog, which closes on a decision, on
  Escape, on timeout (60 s in Agent1) and when the confirmer disconnects. The person's own key bindings (power, switch to a terminal) stay.
* **Tests that use virtual pointers** (all our nested UI tests) are unaffected: the role exists only in the confirmer test, which gets a device-input
  path; every other test never opens a trusted surface.
* **Not a sandbox**: see Non-goals.

## Status (2026-10-08)

Decision (the person): when the role is missing, **confirm-tier actions are refused** (default; `HELIX_ALLOW_UNPROTECTED_APPROVALS=1` or
`allow_unprotected` in `~/.config/helix/approvals.json` allows them, and the dialog says so).

**Built.** In the fork (`helix-niri`, branch `trusted-surface`, see its `HELIX.md`): the `trusted-surface` config block, role detection by
namespace and `/proc/<pid>/exe`, always-on-top rendering and hit testing, keyboard priority, forced capture block-out, dropping of virtual-pointer and
touch input while mapped, `niri msg layers` ("Trusted: yes") and `helix-capabilities` (`trusted-surfaces`). In the shell: the dialog asks the
compositor whether it holds the role (`niri_query.rs`, `Confirmer::set_protection`), shows nothing until it knows, arms Allow 700 ms after, refuses
confirm-tier actions and the `confirm` tier of grants when unprotected, and says so in the dialog.

**Measured** (nested, `tools/nested/trusted-run.sh <mode>`, attacks in `trusted-attack-test.sh`): on stock niri, a synthetic pointer click approves the
dialog, screencopy reads it, an Overlay surface covers it and an exclusive-keyboard surface takes the keys (all four succeed); with the role, all four
fail, and with the right namespace but another executable (`wrong-exe`) they succeed again, so the check is not vacuous. `protection-test.sh` shows an
unprotected compositor refusing a confirm-tier action without showing it, and a protected one arming Allow.

**One finding changed the design.** `zwp_virtual_keyboard_v1` events are delivered by smithay straight to the focused client; they do not pass through the
compositor, so they cannot be filtered there. The dialog therefore refuses to be allowed by *any* key (a click that a key produced has `detail 0`
and is ignored), which protects approvals even on a stock compositor. A person with no pointing device cannot approve; an accessibility path is future
work (`allow-virtual-input` in the compositor config opens the pointer path for such tools).

**Not built.** The compositor-drawn rim (imitation defence), a Settings toggle for `allow_unprotected`, an audit mark for approvals made without protection,
and a physical-device test (the nested compositor only receives virtual input; the device path is checked on the real session).

**Deploying.** The compositor needs the fork build installed and this block in the person's niri config, then a new session; until then the shell
(which is the confirmer) refuses confirm-tier actions. Nothing here touches the running session.

## Order of work

1. Add the failing tests (attacker program for each threat) and the `--device` pointer. Prove each attack works today.
2. Fork: config block and exe check; role detection; `helix-capabilities` and `trusted-surfaces`.
3. Fork: topmost rendering and focus; the rim.
4. Fork: input origin marks and refusal; capture block-out.
5. Shell: use the role (the namespace already matches), show the "cannot protect approvals" state, refuse confirm-tier without it, audit marks.
6. Document in `HELIX.md` and `tauri-rust-shell.md`; update the threat model in `agent-and-inference.md` (the confirm tier is only as strong as this).

Open question for the person: refuse confirm-tier when the role is missing (my recommendation, safe by default), or warn and allow?
