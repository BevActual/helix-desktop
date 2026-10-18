# Mobius Settings

Status: first pass built (2026-10-18). Replaces the "Models & routing" page. Entry points: the gear in the sidebar footer, `Ctrl+,` (and `Ctrl+M`), `/settings`, `/models`, the palette, and "Settings…" in the model menu.

## Shape

A rail of sections beside one column of content, the way Cursor's Settings and system settings pages are laid out; under 720 px the rail becomes a row of tabs.

| Section | What it holds |
|---|---|
| **Models** | One row per **job**: Chat, Code, Pictures, Summaries, Act, Dictation, Image creation. Each shows the model that does it (a menu of what is installed here, on the network, or in the cloud), where it runs and how it fits this computer. A job with nothing suitable installed offers the model to get, with the reason ("The most capable model that fits your graphics card") |
| **Library** | Every model Mobius can run: search, filter (installed, chat, code, pictures, speech, images, fits this computer), sort (best for this computer, smallest, name), Get with progress, Remove |
| **Cloud models** | OpenRouter's catalog (471 models when measured, 2026-10-18): search by name, maker or what it does; filter (in my menu, free, sees pictures, thinks first, uses tools, long documents); sort (newest, cheapest, longest documents). Each row shows price per million tokens, context size and abilities. **Add to my menu** puts a model in the chat's model menu; **Use for…** assigns it to Chat, Code, Pictures or Summaries; any `vendor/model` id can be added by hand. Chats on these leave the computer, and Act never uses one |
| **Providers** | Another computer on the network, and OpenRouter with its key in the Secret Service |
| **Generation** | Temperature, longest reply, provider timeout, battery-saver rule |
| **Experiments** | The switches for things being tried, and the routing file's path |

## Decisions

* **Jobs, not backends.** People think "what should write my code", not "which backend profile". The old page asked them to read a task map of sources. Now the question is per job, and the answer lists models.
* **Changes apply as they are made.** No Save and Discard bar: choosing a model writes the routing file at once and a quiet "Saved" appears. Sliders write once, after the last move. A provider's address and key are saved by its **Save & test** button, because half a URL must not be saved.
* **Suggestions come from this computer.** `recommend.ts` (unit tested) ranks by how a model fits the graphics card: the most capable that fits for Chat and Pictures, a code model for Code, the smallest for Summaries. It never suggests one that is too big, and never offers a download unprompted: **nothing downloads until Get is pressed**.
* **Quiet when there is nothing to choose.** The "a better fit" line appears only under Auto with two or more models installed.

## From Cursor's Settings (read from the installed app, 3.24.9)

Its sections are General, Appearance, Agents, Tab, Models, Voice, Rules/Skills/Subagents, Plugins, Code Intelligence, Tools & MCPs, Hooks, Beta, Network, Cloud Agents, Worktrees and Developer. Worth taking:

* **A model switcher you curate.** Each model has an "Add model to switcher" toggle and there is an "Add or search model" box, so the picker holds what the person uses. Mobius's picker lists every installed model plus six cloud ones. Next: a "Show in the model menu" choice per model, and "Add a model by id" for cloud and network providers.
* **Keys reveal their fields.** "Use OpenAI API Key" is a switch that opens the key and an optional base URL, with "Secret saved" shown once stored. Mobius's key already behaves like this; the status line should be the same words.
* **Per-model parameters with "Restore default parameters"** and per-role overrides (a subagent can inherit, use a default, or be disabled). Mobius's analogue is Summaries and Act following Chat unless set.
* **A Beta page** for experiments, which Experiments already is.

## Cloud models

`cloud.rs` reads OpenRouter's public `/models` list (price, context, input and output kinds, supported parameters such as tools and reasoning) and keeps it in `~/.local/share/helix/mobius/openrouter-models.json`, so the page opens at once and works offline. It is fetched only when the person presses **Refresh the list**, or once the first time after OpenRouter has a key; over https only, and **without the key** (the list is public). A price OpenRouter writes as -1 ("depends on the request") is shown as "Price varies", never as free. The pins live in `cloud-pins.json`; the chat's model menu shows the pins (or, until there are any, a short default list) and a "More cloud models…" link. A model menu taller than the room above its button now scrolls, opened at its bottom.

What the other products do, from their own pages: Cursor lets you add and search models in Settings and picks one itself under **Auto**; ChatGPT shows three choices (Instant, Thinking, Pro) with automatic switching to deeper reasoning behind a Configure link; OpenRouter filters by input kind, context length and series. Mobius takes the curated switcher (Cursor) and the filters and price lines (OpenRouter), and leaves automatic switching to the decision lane in `decision-model.md`.

## Not built

* Finding models that are not in the curated list: search, add by link, import a `.gguf` from disk. Anything fetched needs a pinned checksum or a clear "unverified" state, a fixed https host, a size limit, and the person's yes per download.
* A Knowledge section (indexed folders, reference packs) once `knowledge.md` is wired in.
* Rules and Memory as sections (they are dialogs today).
