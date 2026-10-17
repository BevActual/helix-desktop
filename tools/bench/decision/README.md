# Decision-model experiments

Can a small local model make the quick, typed decisions Helix needs (which model to route a message to; whether a chosen command does what was asked)
fast and well enough to be worth a lane of its own? Each script prints accuracy and latency on the 34 messages in `routing_set.py` (quick, deep,
code, act) or on an assistant evaluation result. They talk to the real inference service (`org.helix.Inference`) on a private bus, or run
`llama-embedding`; nothing is changed on the desktop. Results and the decision: `docs/design/decision-model.md`.

| Script | Needs | What it measures |
|---|---|---|
| `generic_router.py <model>` | the service, an installed chat model | A general model (1.5B, 7B) classifying with a grammar-constrained tool call |
| `arch_router.py <model>` | the service, `Arch-Router-1.5B-Q4_K_M.gguf` in the models folder | Katanemo's routing model, with our four routes as its route list |
| `embedding_router.py` | `llama-embedding` (built with the engine), `nomic-embed-text-v1.5.Q8_0.gguf` | A centroid and a 3-nearest-neighbour classifier over embeddings of labelled examples (none of them the test messages) |
| `verifier.py <model>` | the service, an evaluation result (`EVAL_JSON`, from `helix-assistant-eval --json`) | A yes/no check, by the model already loaded, of whether the command the assistant picked does what was asked |

The two extra models are not in Helix's catalog; download them yourself into a folder of your own and point the service at it with
`HELIX_MODEL_DIR` (a folder of symlinks to the installed models plus the new files keeps your own models folder untouched):

```sh
# Arch-Router (Katanemo research license): https://huggingface.co/katanemo/Arch-Router-1.5B.gguf
# nomic-embed-text v1.5 (Apache-2.0):      https://huggingface.co/nomic-ai/nomic-embed-text-v1.5-GGUF
```

Start the service on a private bus the way `tools/run-assistant-eval.sh` does, then run a script with a model file name as its argument.
