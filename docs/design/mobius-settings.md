# Mobius Settings

Status: first pass built (2026-10-18). Replaces the "Models & routing" page. Entry points: the gear in the sidebar footer, `Ctrl+,` (and `Ctrl+M`), `/settings`, `/models`, the palette, and "Settings…" in the model menu.

## Shape

A rail of sections beside one column of content, the way Cursor's Settings and system settings pages are laid out; under 720 px the rail becomes a row of tabs.

| Section | What it holds |
|---|---|
| **Models** | One row per **job**: Chat, Code, Pictures, Summaries, Act, Dictation, Image creation. Each shows the model that does it (a menu of what is installed here, on the network, or in the cloud), where it runs and how it fits this computer. A job with nothing suitable installed offers the model to get, with the reason ("The most capable model that fits your graphics card") |
| **Library** | Every model Mobius can run: search, filter (installed, chat, code, pictures, speech, images, fits this computer), sort (best for this computer, smallest, name), Get with progress, Remove |
| **Cloud models** | OpenRouter's catalog (471 models when measured, 2026-10-18): search by name, maker or what it does; filter (in my menu, free, sees pictures, thinks first, uses tools, long documents); sort (newest, cheapest, longest documents). Each row shows price per million tokens, context size and abilities. **Add to my menu** puts a model in the chat's model menu; **Use for…** assigns it to Chat, Code, Pictures or Summaries; any `vendor/model` id can be added by hand. Chats on these leave the computer, and Act never uses one |
| **Knowledge** | Folders Mobius reads into a local search index (see `knowledge.md`): add, read again, remove, and try a search that shows what would be found, by words and by meaning. Nothing leaves the computer |
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

## Model types

A model is built for something, and a job accepts only models built for it. `recommend.ts` (`eligible`, `specialist`) holds the rule:

| Type | Does | Jobs that take it |
|---|---|---|
| Chat, Code, Pictures | Language models; Code ones are tuned for code, Pictures ones read images | Chat, Code, Pictures, Summaries, Act (Pictures needs one that sees) |
| **Thinks first** (new) | A chat model that reasons before it answers: Qwen3 4B. Its reasoning shows as the folded "Thought it through" row | Chat, Code, Summaries (it is a chat model with a tag) |
| Speech | Speech to text, on the processor | Dictation |
| Image generation | Pictures from a description | Image creation |
| **Meaning search** (new) | Reads what text means, in about 10 ms on the processor: bge-small-en-v1.5 | Meaning search |

Checked on 2026-10-18: the bundled llama 0.6.0 supports the Qwen3, Qwen3.5, Gemma 3 and DeepSeek architectures, and Qwen3 4B Q4_K_M (downloaded from Qwen's own repository, size and SHA-256 matching Hugging Face's published values) loads in the real inference service, answers 17 × 23 = 391 and has its `<think>` text folded (`mobius-think-real`, 32 s for the turn including the load).

A specialist never appears under a chat job, and a chat model never under a specialist's. Cloud models carry their own tags from OpenRouter's data: sees pictures, thinks first, uses tools, makes pictures, free, long documents.

The meaning-search model is now in the library as a checked download (three files, sizes and SHA-256 written in the catalog, verified against Hugging Face's published hashes), installed to `models/embed/bge-small-en-v1.5/`. `decide.rs` looks there first, then at `$HELIX_EMBED_DIR`, then at the folder earlier versions used, and looks again until it finds one, so a model downloaded while Mobius runs is used from then on. This is the same model the knowledge store (`knowledge.md`) will use for search by meaning.

## Cloud models

`cloud.rs` reads OpenRouter's public `/models` list (price, context, input and output kinds, supported parameters such as tools and reasoning) and keeps it in `~/.local/share/helix/mobius/openrouter-models.json`, so the page opens at once and works offline. It is fetched only when the person presses **Refresh the list**, or once the first time after OpenRouter has a key; over https only, and **without the key** (the list is public). A price OpenRouter writes as -1 ("depends on the request") is shown as "Price varies", never as free. The pins live in `cloud-pins.json`; the chat's model menu shows the pins (or, until there are any, a short default list) and a "More cloud models…" link. A model menu taller than the room above its button now scrolls, opened at its bottom.

What the other products do, from their own pages: Cursor lets you add and search models in Settings and picks one itself under **Auto**; ChatGPT shows three choices (Instant, Thinking, Pro) with automatic switching to deeper reasoning behind a Configure link; OpenRouter filters by input kind, context length and series. Mobius takes the curated switcher (Cursor) and the filters and price lines (OpenRouter), and leaves automatic switching to the decision lane in `decision-model.md`.

## Adding a model you already have

Library → **Add a model file…** takes a `.gguf` from this computer. It must start with the GGUF marker (a renamed document is refused) and be at least 1 KiB; its name is cleaned to letters, digits and `. - _`; a catalog file name or one already in the folder is refused. It is **hard-linked** when it is on the same disk (instant, no second copy, the original untouched), else copied under a `.part` name and renamed when whole. It is listed as **Added by you**, with the plain statement that Mobius has not checked it against a published list. It can be used for Chat, Code or Summaries and removed again (only a plain file directly in the models folder, never one the catalog owns). **Open the models folder** is there for people who prefer to drop files in. `mobius-import` tests it with a fake picker.

## Finding models on Hugging Face

Library → **Find more models on Hugging Face**: type words to search, or paste a `huggingface.co` link or `owner/name`. Safeguards (approved 2026-10-18):

* **Only `https://huggingface.co` is asked**, from `hub.rs`, and a search sends the typed words and nothing else (said on the panel). A pasted link must be that host over https, or a bare `owner/name`.
* **A file is offered only if Hugging Face's listing gives its SHA-256**, which the download is checked against; the checksum is read from the listing again when the download starts and never taken from the page. A `.gguf` without one, a gated repository (needs a login), a name that is not a plain `.gguf` name, and picture projectors (`mmproj`) are not offered. Numbered parts (`-00001-of-00003`) are one choice and only when every part is listed.
* **Limits**: 40 GB per download, and enough free disk (the size plus 5% plus 0.5 GB) or it refuses with the numbers. API replies over 8 MB are not read.
* **Redirects** are followed only to Hugging Face's own hosts (`huggingface.co`, `hf.co` and their subdomains) over https, for these downloads and the catalog's.
* **Each download is confirmed** with the repository, maker, licence and the checksum statement; a maker outside a short list of widely used ones is called out. It is only ever started from a button.
* After a download the model is noted in `sources.json` beside it, so its card says "From huggingface.co/owner/repo. Checked against the checksum Hugging Face publishes for it."

Tested by `hub::` unit tests (names, links, checksums, parts, redirects, a download built from fixtures), the nested `mobius-hub` (answered from files, no network) and, by hand, `cargo test -p helix-mobius real_download -- --ignored`, which searched Hugging Face and downloaded a 1.2 MB model through the real path.

Not built: picture support for a Hugging Face model (its `mmproj` file must be named to match the model), and a quantisation recommendation per computer.

## Routing a job through OpenRouter (2026-10-18)

Cursor chooses models by task (its subagents, each with its own model and a default picked "by availability and cost", plus fallback lists and best-of-N). OpenRouter can do the same work at the provider level, so a job that runs on OpenRouter gets **Cloud routing** under its row in Settings → Models:

* **Prefer**: best match (OpenRouter balances), lowest price (`provider.sort: price`), or fastest (`throughput`).
* **If it fails, try**: up to three fallback models, sent as OpenRouter's `models` list (the model that actually answers is reported back and is the one billed). Triggers are OpenRouter's: an error, a rate limit, or a conversation too long for the first model.
* **Only providers that promise not to keep or train on my prompts** (`provider.data_collection: deny`), **on by default**. Some models, mostly free ones, are offered only by providers that keep prompts and will not work while it is on; the refusal is explained and says where to change it.
* The request asks for `usage`, so a **reply shows what it cost** ("about $0.0012", "no charge", or nothing when not reported) and the model that really answered.

The options are in `~/.local/share/helix/mobius/cloud-routes.json`, not the routing file, because the inference service reads that file with a deliberately small parser. Act never uses the cloud, so it has none. Tested by `lan::` (request body, reply parsing, refusals), `cloud::` (option cleaning) and the nested `mobius-routing`, which sends real requests to a pretend OpenRouter and checks the body, the reply's details and that changed options are used by the next message.

Not built yet: choosing the route **per message** (the decision lane could pick a job's route from what was asked), best-of-N, spend limits, and escalating from a local model to a cloud one.

## Connecting a Claude or ChatGPT subscription

Checked 2026-10-18, from the companies' own statements as reported and their public discussion; **not legal advice**.

* **Claude (Free, Pro, Max): not allowed.** Anthropic says OAuth tokens from consumer plans may be used only in Claude Code and Claude.ai; using them in any other product, tool or service, the Agent SDK included, breaks its Consumer Terms, and enforcement began in January 2026. A third-party app is meant to use an **API key** (pay as you go). So Mobius does **not** read Claude Code's credentials or sign in with a Claude account. What works: a Claude API key (directly, or **Claude models through OpenRouter**, already supported), and, as a plain hand-off, opening the person's own `claude` in a terminal on the project (the person is using Claude Code itself).
* **ChatGPT (Plus, Pro): partly, with conditions not yet confirmed.** OpenAI has an official OAuth, **Sign in with ChatGPT**, for apps; its terms say requests must come from the user's own local runtime. OpenAI's staff have said it is not approved for ad-hoc third-party **Codex** clients, only pure open-source clients and partners, and that others must ask. Helix is public and MIT-licensed, so it may qualify, but OpenAI's terms page could not be read here (403) and the maintainers' reply in the public discussion did not settle it. Needs the owner to confirm with OpenAI, and a client registration only the owner can make.
* **Never** read `~/.claude`, `~/.codex/auth.json` or any other app's stored token to reuse it: that is the borrowed-token pattern both companies prohibit, and it is also taking a credential the person did not give Mobius.
* **ACP** (the Agent Client Protocol Zed made) would let Mobius drive installed agents such as Codex or Gemini CLI. The Claude adapter is built on the Agent SDK, which falls under the same rule, so it is not a way around it.

## Not built

* Knowledge: reference packs and fetched pages (each a download the person approves), and using the index in chat.
* Rules and Memory as sections (they are dialogs today).
