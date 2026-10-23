# Helix Models

Helix Models is the app for the model gateway. The gateway (`org.helix.Inference`, `services/helix-inference`) routes every model request on the
desktop; Helix Models is how a person manages it. Mobius stays the place where models are *used* (chat, personas, history, the model picker).
See `inference-fabric.md` for the hard rules (local by default, LAN is not local, remote models never hold Agent Surface) and
`agent-and-inference.md` for the service.

## Split

| | Owns |
|---|---|
| **Gateway** (`helix-inference`) | Backends, routing policy, grants, keys, the queue, engines, downloads, the audit trail. Keeps running with no window open. |
| **Helix Models** (app) | A window onto the gateway: Overview, Backends, Models, Routing, Access, Activity, Endpoint. Never talks to a provider and never reads a key. |
| **Mobius** | Chat, personas, history, the picker at chat time, and its own prompts and tool-call loop. A client of the gateway like any other. |
| **Settings** | One summary row ("Models: 2 local, 1 cloud") that opens Helix Models. |

Ships in the main desktop install. Talks to the gateway over D-Bus. Routing policy lives in a file the gateway owns
(`inference-routing.toml`, `core/helix-config/src/inference_routing.rs`); the app asks the gateway to change it and the gateway validates every
change, so a bug in the page cannot loosen a rule.

## Engines

* **Default local engine: llama.cpp**, run as a managed **`llama-server`** child process (OpenAI-compatible, on a loopback port or a Unix socket,
  one model at a time or several). The gateway starts it, watches it, restarts it, and loads and unloads models on request. This replaces the
  in-process C shim (`native/llama_shim.cpp`), which stays until `llama-server` passes the same tests. Benefits: a crash in the engine cannot take
  the gateway down, no C++ in the service's build, engine updates are one replaced binary, and the child can be sandboxed with `bwrap` (models
  folder and the GPU only).
* **Accelerators.** One engine build carries all of them as loadable backends (`GGML_BACKEND_DL`): CPU variants (the best for the machine is
  picked at start), **CUDA** (NVIDIA, preferred when present), **Vulkan** (AMD, Intel, and NVIDIA without CUDA). The installed engine used to be
  CPU only, so the GPU sat idle.
* **Other local servers** (Ollama, LM Studio, a vLLM on the LAN) are declared as OpenAI-compatible backends. Ollama is supported, not depended on.
* Later: a Rust-native engine (mistral.rs or candle) if the C++ should go entirely. A new dependency; needs approval.

### Building the engine here

`third_party/llama.cpp-0.6.0` is vendored; nothing is fetched to build it. For CUDA, NVIDIA's toolkit components (nvcc, cudart, cuBLAS, headers;
about 0.95 GB from `developer.download.nvidia.com/compute/cuda/redist`, each checked against the SHA-256 in `redistrib_13.4.3.json`) are unpacked
under `~/.local/cuda` (no root; `lib64` is a link to `lib`). nvcc does not accept GCC 16, so `gcc15`/`gcc15-c++` (Fedora's own packages) are the
CUDA host compiler. Configure:

```
PATH=$HOME/.local/cuda/bin:$PATH cmake -S third_party/llama.cpp-0.6.0 -B build \
  -DCMAKE_BUILD_TYPE=Release -DGGML_CUDA=ON -DGGML_VULKAN=ON -DGGML_BACKEND_DL=ON -DGGML_CPU_ALL_VARIANTS=ON \
  -DBUILD_SHARED_LIBS=ON -DGGML_NATIVE=OFF -DCMAKE_CUDA_ARCHITECTURES="86;89" -DCMAKE_CUDA_HOST_COMPILER=/usr/bin/g++-15 \
  -DCUDAToolkit_ROOT=$HOME/.local/cuda -DCMAKE_CUDA_COMPILER=$HOME/.local/cuda/bin/nvcc \
  -DLLAMA_BUILD_SERVER=ON -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_EXAMPLES=OFF -DLLAMA_CURL=OFF
```

`tools/build-llama-engine.sh` does all of this (it finds nvcc, the newest older host compiler, and Vulkan on its own). The result goes to `third_party/llama-vulkan-install` (ignored by git). A package build ships the CUDA runtime libraries it links.

## The app

1. **Overview**: what is running now (loaded model, queue depth, tokens per second, which accelerator), today's cloud spend, and a plain
   "Local only" / "Cloud allowed" state.
2. **Backends**: local engines, LAN machines, cloud providers. Name, address, test button, status light, trust label (Local, LAN, Cloud). Keys are
   typed here and go straight to the keyring; the app never shows them again.
3. **Models**: what is installed (size, quantisation, backend), download and delete, and what fits this machine's VRAM and RAM. Downloads are the
   gateway's: one models folder, checksums, resume, disk-space checks. A short "recommended for this machine" list (a small fast Act model, a
   mid Talk model, a vision model). Nothing is fetched without the person starting it.
4. **Routing**: one row per task slot (Act, Talk, Vision, Code, Embeddings, Images) with a primary model, a fallback order, and a cloud rule
   (never, ask each time, allowed). Slots that may emit desktop tools stay locked to a local model, shown as such.
5. **Access**: which apps and agents may call the gateway, which slots and backends each may use, a budget, and revoke. Same idea as Agent
   Surface grants.
6. **Activity**: time, caller, slot, backend, tokens, cost, latency, whether a cloud call happened. Prompt text is not stored unless switched on.
7. **Endpoint**: an optional loopback OpenAI-compatible URL so other tools use the same routing and grants. Off until turned on; per-client
   identities, so a stray local process cannot spend a cloud budget.

## Order of work

1. Engine: Vulkan + CUDA `llama-server` build and a benchmark against the CPU-only engine (done: built from the vendored source).
2. The gateway supervises `llama-server` (start, health, load/unload, restart, sandbox), behind the existing backend trait, and the inference
   tests run against it.
3. Providers move out of Mobius into the gateway one at a time (OpenAI-compatible first: `lan.rs`, then `cloud.rs`/`chatgpt.rs`), with the
   local/LAN/cloud policy checks, grants and the audit line in the gateway. Mobius keeps native tool calling through the gateway.
4. The Helix Models app: Backends, Models (downloads), Routing; then Overview and Activity; then Access and the endpoint.
5. Retire the in-process C shim.

## Status

* 2026-10-23 (live): the running user service now uses it (`~/.config/systemd/user/helix-inference.service.d/95-llama-server.conf`; remove that file
  to go back to the C++ build). Under the real unit sandbox (no network, read-only home, private temp) the engine starts, uses the GPU (4.7 GB of
  VRAM for the 7B) and answers. `tools/bench/mobius-bench.py`, same 7B model, first request including the load: **30.7 s before, 4.8 s now**; a
  507-character answer in 2.8 s. The engine child currently inherits the service's sandbox; a `bwrap` sandbox of its own is needed before the
  service itself is given network access for LAN and cloud backends.

* 2026-10-23 (later): `helix-inference` has a `llama-server` backend (`src/server.rs`): it starts the engine on the model asked for over a private
  Unix socket (`--host <path>.sock` in a 0700 folder; no TCP port), waits for health, streams OpenAI chat chunks, passes pictures as PNG data URLs
  with the model's `mmproj`, constrains tool calls with the same GBNF the C++ engine used (`src/grammar.rs`, ported), reports prompt-cache hits,
  cancels by closing the connection, restarts an engine that died between requests, and kills it with the service (`PR_SET_PDEATHSIG`). Model
  discovery (shards, projectors, only-listed-models-load) is `src/models.rs`. Real-engine tests (`--ignored`) pass: streaming, prompt cache,
  grammar-constrained call, cancel, kill-and-restart, refusal of an unlisted path, and reading a picture. Selected when `HELIX_LLAMA_SERVER` (or
  `~/.local/libexec/helix/llama-server`) exists; otherwise the C shim is still used.

* 2026-10-23: llama.cpp 0.6.0 builds from the vendored source with CPU variants, CUDA 13.4 and Vulkan as loadable backends and lists the RTX 4080
  Laptop GPU under both. Same 7B Q4 model, short chat reply on this laptop (the desktop is also using the GPU): **CPU 4.1 tokens/s, Vulkan 27.7,
  CUDA 28.6**, so the GPU is about 7x the CPU engine the installed service uses now. CUDA and Vulkan are within a few percent here; Vulkan stays
  the fallback for AMD, Intel and NVIDIA without the toolkit. Not yet done: supervision by the gateway, packaging the CUDA runtime libraries.
