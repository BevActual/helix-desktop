# Mobius: Hermes Agent, as Apple would build it

Hermes Agent (Nous Research, 2026) is the reference for a personal agent that gets better with use: memory across sessions, skills it
writes itself after a complex task, one gateway so you can reach it from Telegram, Signal, Slack and the rest, scheduled jobs in plain
language, and any model provider. Mobius takes that list and builds each part the way Apple would: private by design, visible, editable,
reversible, and part of the operating system rather than a bot you talk to. It sits on the north star (`north-star.md`): the catalogue of
actions is its hands, the system presence is how you reach it, and trust you can see is what lets it do more.

## Part by part

| Hermes | Mobius, Apple's way |
|---|---|
| **Memory**: what it learned about you, searched across sessions | **What Mobius knows**: a page of plain sentences ("You prefer metric units", "Your projects live in ~/Code"), each with where it came from and when, editable and deletable one by one. Written only on this computer, never sent to a cloud model unless that request goes there anyway, and never from private screen content. A switch per source. |
| **Skills** it distils itself after a task | **Learned skills you approve**: after a task that worked, Mobius offers "Save this as a skill?" with the steps shown as a card (catalogue actions plus model steps, like a Shortcut). Saved skills are listed, editable, can be run by name, and say when they last ran. Nothing changes its own behaviour silently. |
| **Messaging gateway** (Telegram, Discord, Slack, WhatsApp, Signal, CLI) | **Mobius everywhere you are**: your phone and other computers reach it through a paired channel (Signal or Matrix first, end-to-end), with Continuity's feel: start on the phone, the result is waiting on the desktop. Approvals reach the phone as a plain question; anything consequential still needs the person at the computer or a passkey. Only paired accounts can talk to it. |
| **Scheduled automations** in plain language | **Automations you can see**: "every weekday at 8, summarise my inbox" becomes an automation card (trigger, steps, which model, what it may touch) in Scheduled, switchable off, with its run history. |
| **Any model provider** | **The model gateway** (Helix Models): local first, your network next, the internet when you say so, with the reason shown for each answer and the cost. |
| **Terminal backends**: local, Docker, SSH, cloud sandboxes | **Places to work**: a boxed copy of a project, the agent display for apps, another computer you paired; the place is shown on the task and nothing leaves it unless you apply the result. |
| **Subagents** in parallel | **Bots** on the same catalogue, with budgets, a Stop-all, and their steps attributed in the one history. |
| **Learning loop** | **A visible learning loop**: "Mobius learned 2 things this week" in What Mobius knows; every learned fact and skill can be undone like any other change. |

## What makes it Apple's rather than another agent

1. **It is the system, not a chat window.** You ask from anywhere (the presence); Mobius, the app, is where long work and its history live.
2. **Hands are actions.** It acts through the catalogue (`actions.md`), so what it did is a list of sentences with Undo, not a log of clicks. The
   agent display is the fallback for apps without actions.
3. **Privacy you can read.** Every task shows a label: which model, where it ran, what it read, whether anything left the computer.
4. **Asking in plain words.** Approvals say exactly what will happen ("Send the summary to Sam on Signal?"), at the computer or on the phone.
5. **Calm by default.** One glow while it acts, one place for running work (the island), notifications only when it needs you or is done.
6. **It gets better where you can see it.** Memory and skills are pages you can read and correct, not hidden prompt files.

## Where Mobius stands

Built: tasks with an Agents page (needs-you first, plan first, boxed project copies with diff and Apply), skills with `skill_read`, a
Memory section, Knowledge, Scheduled prompts, model routing through the gateway, the agent display with watch-and-help, bring-your-own
agents (ACP, MCP), the modern look. Started today: the actions catalogue (Settings first).

## Order of work

1. **Hands**: Mobius's tool loop reads the actions catalogue (settings now; Models, Software, the shell and Mobius's own environments next).
2. **Presence**: one invocation, the compositor glow, the island for running tasks; the separate entry points become ways to open it.
3. **What Mobius knows**: the memory page (sources, edit, delete, per-source switches), written from conversations and tasks, local only.
4. **Learned skills**: "Save as a skill?" after a successful task, skill cards built from actions and model steps, run by name.
5. **Automations**: Scheduled becomes automation cards with triggers, steps, permissions and run history.
6. **Mobius everywhere**: the paired messaging channel (Signal or Matrix first), approvals on the phone, Continuity-style handoff.
7. **Privacy labels** on every answer and task.
