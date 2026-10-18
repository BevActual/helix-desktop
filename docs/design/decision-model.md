# A local decision model

Status: experiment, 2026-10-17. Nothing here is wired into Mobius or the assistant yet.

Helix makes small, typed decisions all the time: which model should answer this message, is this a request to change the desktop, did the command the
assistant picked do what was asked. A decision is not prose. It is a choice among a few options, with how sure the model is. A model that does only
that can be small, quick and always loaded (Jev from TypeSafe AI is one: typed questions in, typed answers with probabilities out, 70 to 500 ms; it is a
closed cloud API, so not for us, but the shape is right).

## Constraint

The inference service holds **one model at a time** (one llama handle). A separate decision model in front of the answering model would swap it out
on every message, so a decision lane has to be its own resident model on its own handle: about 1 GB beside the 5.5 GB vision model fits this
machine's GPU, and an embedding model of 150 MB can sit on the CPU.

## What was measured

34 hand-labelled messages (quick, deep, code, desktop action), real models, one run, this machine. `tools/bench/decision/` reproduces it.

| Decider | Right | Latency | Size | Notes |
|---|---|---|---|---|
| Qwen2.5 1.5B, grammar-constrained tool call | 22 / 34 (65%) | 450 ms median | 1.1 GB (already installed) | Calls nearly every desktop action "code" |
| Qwen2.5 7B, same | about 28 / 34 (82%) | 1.8 s | 4.7 GB | Several answers were cut off by my 16-token cap, so this is understated; slow because it writes `{"tool":"quick","args":[]}` |
| [Arch-Router 1.5B](https://huggingface.co/katanemo/Arch-Router-1.5B) Q4_K_M, our four routes in its prompt | 29 / 34 (85%), 30 / 34 with clearer route descriptions; 33 / 34 counting its `other` for a plain fact as quick | 1.0 s | 986 MB | A Qwen2.5-1.5B fine-tune made for this; it answers `{'route': 'name'}`. Latency is its 320-token prompt. **Katanemo research license** |
| [nomic-embed-text v1.5](https://huggingface.co/nomic-ai/nomic-embed-text-v1.5-GGUF) Q8_0 embeddings, a centroid per route from 8 to 10 labelled examples each (none of them test messages) | **33 / 34 (97%)**; 3-nearest-neighbour 30 / 34 | **about 19 ms per message on the CPU**, 0.4 s for one message including starting the tool | 146 MB | No generation at all; the similarity gap between the best and second-best route is a built-in confidence (the one miss, "What does HTTP stand for?", had a gap of 0.021). Apache-2.0 |
| Yes/no verifier: the loaded 7B asked "does this command do what was asked?" about the assistant's pick | kept 73 of 75 right calls, caught 3 of 7 wrong ones | 2.3 s | none extra | Caught "Turn off Wi-Fi → wifi-status" and "Connect to Tailscale → tailscale-status"; missed "Stop playback → pause" and "Close this window → window-switcher" |

Read with care: 34 messages I wrote and labelled myself, with easy, well-separated classes; real traffic is messier. The ranking is clear and the
sizes of the gaps are, but the 97% is not a promise.

## What it says

* **An embedding classifier is the right first decision model.** It beat every generative option on accuracy, by a wide margin on speed (19 ms against
  0.45 to 2 s), costs 146 MB, runs on the CPU without touching the chat model, has a confidence for free, and is Apache-2.0. It also needs no
  prompt engineering: a route is a handful of example sentences in a file the person can read and edit.
* **Arch-Router is a good second opinion**, particularly for routes that are about preference rather than topic ("sounds like writing a poem, use the
  creative model"), but it is a second model to keep resident, 50 times slower, and under a restrictive license. Worth keeping as an optional lane.
* **A yes/no verifier by the loaded model is a weak guard** as built (it missed four of seven), and slow only because it generates JSON. It does catch
  the worst stand-ins ("turn off Wi-Fi" answered with a status read). A probability read instead of generation would be about 100 to 200 ms; the
  embedding approach does not apply to it.
* **The weak spot in the assistant** (declining: it acts on 3 to 6 of 10 impossible requests) needs something else: an out-of-scope signal. An
  embedding of the request against the tool descriptions, and abstaining when no tool is close enough, is the cheap thing to try.

## Without llama (the choice)

Embeddings need no llama.cpp and no inference service: `core/helix-embed` runs a small BERT-family sentence model in Rust with
[candle](https://github.com/huggingface/candle) on the CPU, inside the process that asks (Mobius). No C++ shim, no second resident model, no
D-Bus round trip, and it follows the direction of `cpp-to-rust.md`. The same 34 messages, the same training examples
(`cargo run --release -p helix-embed --example decide -- <model dir> cls`):

| Model (safetensors, fp32) | Right | One message | Load | Size | License |
|---|---|---|---|---|---|
| **bge-small-en-v1.5** (first-token pooling) | **32 / 34 (94%)** | **8 ms** | 60 to 100 ms | 133 MB | MIT |
| all-MiniLM-L6-v2 (mean pooling) | 31 / 34 | 4.4 ms | 75 ms | 91 MB | Apache-2.0 |
| nomic-embed v1.5 Q8 through llama.cpp (above) | 33 / 34 | 19 ms | | 146 MB | Apache-2.0 |

bge-small is kept: within one message of nomic, faster, no engine. The misses had margins of 0.008 and 0.019 (an act message scores 0.15 to
0.24), so a threshold of 0.03 turns them into "not sure". One thing to know: candle splits every matrix product across all cores by default, which
costs 36 ms a message on 32 cores against 8 ms on four, so `helix_embed::limit_threads()` runs first in `main`.

The model is not bundled: it is a folder (`config.json`, `tokenizer.json`, `model.safetensors`) in `$HELIX_EMBED_DIR` or
`~/.local/share/helix/embed/bge-small-en-v1.5`, and without it Mobius simply offers nothing. Adding it to the model library (a download with a checksum,
shown with what it is for) is the next step.

First use: the composer's **"Looks like a desktop action · Use Act"** offer (`decide_message`; routes and examples in
`apps/helix-mobius/src-tauri/routes.json`). `tools/nested/mobius-hint-test.sh`.

## Plan (revised: no `Embed` call in `helix-inference`)

1. **Done:** `core/helix-embed` (the embedder and a pure decider: a centroid per route, the gap between the best and second as confidence).
2. **Routes from a readable file** the person can edit (shipped defaults plus their own), instead of the built-in `routes.json`.
3. **Use it, in order of value:**
   * the Act offer in the composer (done);
   * the **Auto** row of the model menu (desktop action → offer Act; code → the code model if one is installed; picture → the vision model;
     otherwise stay on the loaded model, since a swap costs seconds);
   * an **out-of-scope gate** for the assistant (no tool close enough → decline), measured with `helix-assistant-eval` before and after;
   * **keep sensitive text local** when a cloud route is selected.
4. **Make models easy to add** (the other half of the request): the catalog gets a "Decision" and an "Embedding" kind so the library lists them with
   a purpose, and the model menu shows only chat-capable models.

## Not decided

Whether Arch-Router is worth shipping given its license; where the examples file lives (shipped defaults plus the person's own); how a person
corrects a wrong decision (a "route this differently" action that adds the message as an example is the natural loop).
