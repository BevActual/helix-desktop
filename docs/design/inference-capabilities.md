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
and model. Text chat, vision input, dictation, and local text-to-image generation work end to end. Speech output and embeddings remain planned.

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
- **Resource manager.** The first handoff is implemented: the shared queue unloads the chat backend before starting diffusion, and the image worker exits after each request. A richer residency manager is still planned. One heavy model resident on the GPU at a time (12 GB card). Requests carry a priority; the manager swaps
  models only between requests, keeps the chat model resident when it can, reports `loading` to the UI, and runs background work
  (embeddings) only when idle. Free/total VRAM comes from the engine's own device report (already in `GetSnapshot`).

## Protocol

`Action(kind, args)` gains kinds `image`, `transcribe`, `speak`, `embed`. `chat` messages accept content parts:

```json
{"role": "user", "content": [{"type": "text", "text": "What is wrong here?"}, {"type": "image", "index": 0}]}
```

- **Inputs as bytes, never paths**: the client sends each picture as a byte array in `images` (`aay`, at most 4 and 8 MiB each, 16 MiB together) and refers to it by `index` in the message parts. The service never opens a client-chosen path. (File descriptors were the first idea; bytes under a hard size cap are simpler, as safe, and keep the D-Bus message the only channel.)
- **Decode with Wuffs** (already in `third_party/wuffs`, memory-safe): sniff the format (PNG, JPEG, WebP), bound dimensions and bytes,
  re-encode to raw pixels for the engine. Anything else is refused.
- **Generated PNG output:** the first image worker returns a bounded base64 PNG (at most 4 MiB decoded) through the caller-only `Completed` JSON. The service decodes and validates dimensions with Wuffs before sending it. Mobius stores the PNG and provenance beside its transcripts (`0600` files in a private directory). No worker-selected path crosses D-Bus. Streaming text stays on `Delta`.
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
- **Voice:** a mic button provides dictation into the composer. "Read aloud" remains planned.
- **Models & routing:** task cards for Image, Speech and Embed beside Chat, Act and Code; each shows installed/missing, size and
  whether it fits next to the chat model.
- **Act suggestion:** an imperative message ("turn the volume down") offers Act instead of relying on a model to refuse well.

## Phases (each ends with a measured, real-model test)

1. **Vision in.** `libmtmd` in `LlamaBackend`, content parts + fd inputs, Wuffs decode, `ModelInfo.vision`, Mobius image attach and
   thumbnails, routing `vision`. Fake backend echoes image metadata so the protocol and UI are testable without weights.
2. **Resource manager + Progress signal** (the `Loading(operation, model, fraction)` signal and Mobius's chip are done; the manager that decides what may stay resident is not), model-swap UX ("Loading Qwen2.5-VL…").
3. **Transcribe** — done: `helix-transcribe` (whisper.cpp v1.9.5, CPU only, built into `third_party/whisper-install`) is a worker
   process the service runs per request; `Action("transcribe", {audio: ay, language?})` takes raw 16 kHz 16-bit mono samples (at most
   ten minutes, no container parsed anywhere), and `Completed` carries `{text, language}`. The worker refuses a bad language, drops
   network/exec/ptrace/mount syscalls with a seccomp filter, and answers one JSON line. Models are `<models>/speech/ggml-*.bin`
   (Whisper small, multilingual, q5_1, MIT, in the Mobius library). Mobius: the composer microphone records with `pw-record`
   only between two clicks, transcribes locally and puts the words in the composer (never sends them). Measured: 11 s of speech in
   2.6 s on CPU.
4. **Image generation** — initial slice done: stable-diffusion.cpp pinned at `d637cfb935feb2254898a8f1524046c958f3df80`, statically linked into a separate Vulkan worker (`helix-image`). `Action("image", {prompt, model?, width?, height?, steps?, seed?})` accepts text up to 4000 bytes, dimensions 256–768 in multiples of 64, steps 1–8, and a seed up to 2147483647. Models are plain GGUF file names under `<models>/image`; source paths, reference images, negative prompts, and remote providers are not accepted in this slice. `Progress(operation, fraction, stage)` and `Completed` go only to the requesting client. Cancellation kills and reaps the worker; malformed output and a ten-minute deadline fail the operation.

   Mobius menu → **Create image**, or `/image <prompt>`: choose size and optional seed, generate with SD-Turbo, see the PNG in the thread, Save image, Copy image, Generate again, or reuse its prompt. Images and prompt/model/size/seed/timing metadata survive chat reopening. The model library offers the pinned, checksum-verified GPUStack SD-Turbo Q4 download (2.19 GB), with its source and non-commercial license visible. Reference-image editing is next-stage work.

   Build: `bash tools/build-image-engine.sh`, then `ninja -C build-native-clean helix-inference helix-image`. Development activation without root: `python3 tools/use-image-runtime.py` creates a user service override pointing at this build while retaining the original sandbox. Remove only `~/.config/systemd/user/helix-inference.service.d/90-mobius-images.conf`, reload systemd, and restart inference to return to the installed runtime. Root installs continue to use Meson's `inference` install tag.

   The runtime now has a private `0700` shader cache (`CacheDirectory=helix-inference`, `XDG_CACHE_HOME=%C/helix-inference`), writable alongside temporary space; the rest of home remains read-only and network remains disabled. This avoids recompiling GPU pipelines on every worker launch. First sandboxed 512×512 generation: 28.82 s; next: 2.36 s on this RTX 4080 Laptop. Measurements and test scope: `mobius-bench.md`.
5. **Embeddings** and cross-chat search/memory; **speak**.
6. Per phase: bench rows in `docs/design/mobius-bench.md`, nested UI tests, a security review (sizes, formats, fds, sandbox).

## Risks

- VRAM: a 7B chat model (4.7 GB) and a diffusion model (5–8 GB) do not fit together with a display; swaps cost 10–15 s. The resource
  manager and clear UI matter more than raw speed.
- Prompt injection through images ("text in a screenshot"): vision output is data; Act still confirms every change, and image content
  never becomes tool input without the person's confirmation.
- Build: stable-diffusion.cpp and whisper.cpp are new vendored dependencies; each is built into `third_party/` like llama.cpp today.
