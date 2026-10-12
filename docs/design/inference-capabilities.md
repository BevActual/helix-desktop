# Inference capabilities: models for different tasks

Goal: Mobius (and the assistant) can use the right model for the job — read an image, generate one, transcribe speech, speak, search by
meaning — the way ChatGPT routes a request to a vision model, an image model or voice, **without** giving up what Helix guarantees: the
engine runs sandboxed on this machine, a client can only ask, nothing writes outside its own scratch, and no model is downloaded
without the person's consent. This replaces the "add endpoints and see" shortcut: the supported path is first-class engines behind the
existing inference service.

## What exists

`helix-inference` (C++) is a D-Bus service (`org.helix.Inference1`) with a queue, limits and one engine: llama.cpp behind the `Backend`
seam (`src/inference/backend.h`). The engine is built with `libmtmd` (llama.cpp's multimodal library) already, the service runs in its
own user namespace, and routing (`inference-routing.toml`) maps task profiles (`act`, `chat`, `code`, `summarize`, `vision`) to a backend
and model. Only text works end to end.

## Capabilities

| Capability | Input → output | Engine | Same weights as chat? |
|---|---|---|---|
| `chat` | text → text | llama.cpp | — |
| `vision` | image(s) + text → text | llama.cpp + `libmtmd` (mmproj) | Yes, if the chat model is a VLM |
| `embed` | text → vector | llama.cpp (embeddings) | No (small dedicated model) |
| `transcribe` | audio → text | whisper.cpp | No |
| `speak` | text → audio | llama.cpp TTS (OuteTTS) or Piper | No |
| `image` | text (+ image) → image | stable-diffusion.cpp | No |

Task profiles gain `image`, `transcribe`, `speak`, `embed`; each resolves to a capability, a backend and a model exactly like today.

## Architecture

```
Mobius / assistant ──D-Bus──▶ helix-inference (supervisor: queue, limits, routing, resource manager, signals)
                                 │ in-process: llama.cpp (chat, vision, embed, tts)
                                 ├──▶ worker: whisper.cpp      (own process, own sandbox)
                                 └──▶ worker: stable-diffusion.cpp
```

- **In-process** for what shares llama.cpp's ggml (chat, vision, embeddings, TTS).
- **Out-of-process workers** for engines that bundle their own ggml (whisper.cpp, stable-diffusion.cpp): they cannot share symbols with
  llama.cpp, and a crash or out-of-memory in a diffusion run must not take the chat service down. A worker is started on demand by the
  supervisor, speaks a small length-prefixed protocol over a socketpair, runs in its own namespace with no network, and is killed on
  cancel or idle. The supervisor owns the queue, so the client API does not change.
- **Resource manager.** One heavy model resident on the GPU at a time (12 GB card). Requests carry a priority; the manager swaps
  models only between requests, keeps the chat model resident when it can, reports `loading` to the UI, and runs background work
  (embeddings) only when idle. Free/total VRAM comes from the engine's own device report (already in `GetSnapshot`).

## Protocol

`Action(kind, args)` gains kinds `image`, `transcribe`, `speak`, `embed`. `chat` messages accept content parts:

```json
{"role": "user", "content": [{"type": "text", "text": "What is wrong here?"}, {"type": "image", "fd": 0}]}
```

- **Inputs by file descriptor**, not path: the client passes a sealed memfd (D-Bus `h`); the service never opens a client-chosen path.
- **Decode with Wuffs** (already in `third_party/wuffs`, memory-safe): sniff the format (PNG, JPEG, WebP), bound dimensions and bytes,
  re-encode to raw pixels for the engine. Anything else is refused.
- **Outputs as files** in a private per-client directory under the runtime dir (`0700`, removed after an hour or on request); the
  `Completed` signal carries the file name and metadata. Streaming text stays on `Delta`.
- **Progress** for long jobs (`image` steps, model load) on a `Progress(op, fraction, stage)` signal.
- Act is unchanged: its JSON tool grammar, local-only rule and confirmation path are not touched.

## Models

- A catalog (`models.toml` shipped with Helix, user-extendable) lists capability, files, sha256, size, license and source.
- **Never automatic.** A model is fetched only after the person approves its name, source and size in the Models page or
  `helix-models get <id>`; files are verified by sha256 and written atomically into the model directory.
- The installed set already includes Qwen2.5 1.5B and 7B Instruct. First additions (measured, then adopted or dropped):

| Capability | Candidate | Size |
|---|---|---|
| vision | Qwen2.5-VL 7B Instruct Q4_K_M + mmproj Q8_0 (Apache-2.0) | 4.68 + 0.85 GB |
| transcribe | whisper small / medium (MIT) | 0.5 – 1.5 GB |
| image | an SDXL-class or Flux-schnell quantised model | 4 – 8 GB |
| embed | a small multilingual embedding model | < 0.5 GB |

## Mobius

- **Image in:** the paperclip accepts images, shows a thumbnail chip; the turn routes to `vision`. Transcripts keep images beside the
  chat file (`0600`), never inlined in the JSON.
- **Image out:** `/image <prompt>` and a "Create image" entry; the result is an image card in the thread with Copy, Save, Regenerate,
  and "Use as prompt". A badge says which model made it and how long it took.
- **Voice:** a mic button (dictation into the composer) and "Read aloud" on a reply.
- **Models & routing:** task cards for Image, Speech and Embed beside Chat, Act and Code; each shows installed/missing, size and
  whether it fits next to the chat model.
- **Act suggestion:** an imperative message ("turn the volume down") offers Act instead of relying on a model to refuse well.

## Phases (each ends with a measured, real-model test)

1. **Vision in.** `libmtmd` in `LlamaBackend`, content parts + fd inputs, Wuffs decode, `ModelInfo.vision`, Mobius image attach and
   thumbnails, routing `vision`. Fake backend echoes image metadata so the protocol and UI are testable without weights.
2. **Resource manager + Progress signal**, model-swap UX ("Loading Qwen2.5-VL…").
3. **Transcribe** worker (whisper.cpp) and the composer mic.
4. **Image generation** worker (stable-diffusion.cpp), `/image`, image cards, cancel and progress.
5. **Embeddings** and cross-chat search/memory; **speak**.
6. Per phase: bench rows in `docs/design/mobius-bench.md`, nested UI tests, a security review (sizes, formats, fds, sandbox).

## Risks

- VRAM: a 7B chat model (4.7 GB) and a diffusion model (5–8 GB) do not fit together with a display; swaps cost 10–15 s. The resource
  manager and clear UI matter more than raw speed.
- Prompt injection through images ("text in a screenshot"): vision output is data; Act still confirms every change, and image content
  never becomes tool input without the person's confirmation.
- Build: stable-diffusion.cpp and whisper.cpp are new vendored dependencies; each is built into `third_party/` like llama.cpp today.
