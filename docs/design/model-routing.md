# Model routing: research and direction

Written 2026-10-23 after reading NVIDIA NeMo Switchyard (source and docs, via the GitHub API) and surveying the routing literature. The question: what
should decide *which model answers a request* in the Helix gateway (`docs/design/helix-models.md`), beyond a person choosing a backend per slot?

## What exists

| Project | What it is | What it routes on | Fit for a desktop |
|---|---|---|---|
| **Switchyard** (NVIDIA, Apache-2.0, Rust, v0.3 / 0.2 "reborn in Rust", Aug 2026) | `switchyard-libsy`: algorithms that decide, and *hand every model call back to the host* (`Algorithm::run_stream` yields `Step::CallModel` / `CallDecision` / `Done`). Also a proxy (demo-only), OpenAI-chat / Responses / Anthropic-messages translation (`switchyard-translation`), a price table, a macOS menu-bar status app | Coding-agent signals: tool errors, no-progress loops, writes, passing tests (`stage_router`); a judge model's `p_solve` (`llm_classifier`); judge-latched escalation after sustained trouble | The decision/driver split is exactly our shape. The algorithms are tuned for coding agents and need calibration per workload |
| **RouteLLM** (LMSYS, 2024) | Learned strong/weak routers trained on preference data (matrix factorisation etc.) | The prompt | Published: ~85% cost cut on MT-Bench at 95% of GPT-4 quality, 45% on MMLU, 35% on GSM8K. Chat-benchmark results; needs a trained router |
| **Arch-Router** (Katanemo, 1.5B) | A small model that picks among *natural-language routing policies* ("travel questions", "image editing") with no retraining | The prompt, against policies you write | Small enough to run on the local engine; reported 93% policy-matching accuracy. Licence and quality on our slots need checking |
| **vLLM Semantic Router** | BERT-class intent/domain classifier plus keyword, embedding, PII/jailbreak, feedback signals; semantic cache | Intent and safety signals | Docker/Python stack; the *signals* are good ideas, the stack is not for a desktop |
| **Hybrid on-device/cloud research** (HybridInfer and surveys) | Tiered routing on difficulty, data sensitivity, device load, latency budget; cascades with confidence deferral | Many signals at once | Confirms the signal set below; reports on-device models are several times slower and fail on long inputs, so routing wins on latency and coverage, not only cost |
| **Products** | Apple Intelligence (on-device, then Private Cloud Compute), Gemini Nano then cloud | Task and size; privacy is the first rule | The product pattern to emulate: local by default, escalate visibly |

## What Switchyard teaches (from its docs)

* Roles are *per route*, not per model: "capable" and "efficient" are jobs. The models you pick matter more than any threshold.
* "A cheaper model call does not guarantee a cheaper successfully completed task": judge by completed tasks against a single-model baseline.
* No honest default threshold: the author of a hands-on review found the right `base_threshold` varied widely by workload. NVIDIA's own tuning guidance
  needs roughly 40 to 75 capable-model runs and 20 efficient probes per workload.
* A judge model adds latency: 17.5 s → 3.1 s once the classifier saw only the last 4 turns. Fine for background work, wrong for a spoken or typed
  reply. Stage routing avoids the judge on most turns by reading tool signals.
* Known gaps in 0.2: work continues after the client disconnects (cost after cancel), tier attribution missing in some stats. Pre-1.0, "pin versions".
* Useful extras: a bundled price table with savings estimates, routing logs (`routing.jsonl`), response headers naming the model and why.

## What a desktop knows that a coding-agent router does not

Signals available to Helix at routing time, most of them free (no judge call):

1. **Privacy of the content**: a picture, a screenshot, file contents, clipboard, anything from the desktop → local only unless the person says otherwise.
2. **Slot**: Act is local, always. Talk, Summarise, Code, Pictures are routable.
3. **Fit**: prompt length against the local model's context window; whether the slot needs tool calling the local model does badly; vision.
4. **Machine state**: free video memory and the model already loaded (a swap costs seconds), power profile / battery, thermal state.
5. **Network**: reachable, metered, a LAN peer present.
6. **Budget and preference**: monthly cloud budget, "always best", "never cloud" per slot.
7. **Outcome signals** (agent tasks): tool errors, repeated actions, no progress, verifier failures. The Switchyard `stage_router` idea.
8. **Feedback**: the person pressing "Try a stronger model", thumbs, regenerate. The cheapest and most honest quality signal there is.

## Direction

**Own the router; borrow the structure and, only if it earns it, the library.** Do not adopt the Switchyard proxy. Build a small policy-first router in the
gateway whose interface mirrors libsy's (a decision step that can ask the host to make a call), so libsy's algorithms can be plugged in later without
rewriting callers.

1. **Policy router (rules, no ML).** One function takes the request and the signals above and returns a *decision with a reason*: backend, model,
   fallbacks, and a human sentence ("Answered on this computer: it contains a picture"). Hard rules first (Act local, private content local, battery,
   unreachable, over-context), then the person's per-slot preference. Shown in Mobius as a quiet chip; logged in the audit line. No calibration needed.
2. **Local-first escalation the person can see.** Talk answers locally by default (when a capable local model is installed). A visible *Ask a stronger
   model* action re-runs the turn on the escalation backend the person chose for that slot. Automatic escalation only for plain failures (engine
   error, context overflow, empty or truncated answer) and only if their policy allows the destination. Never silent to the internet.
3. **Telemetry and savings.** Extend the audit line into a routing log (decision, reason, tokens, cost from a price table, latency) and show it in
   Helix Models → Activity, like Switchyard's menu-bar summary: today's requests, what ran where, estimated spend and savings against "always the
   best model".
4. **Agent-loop routing.** For Mobius agent tasks only: stage-style signals (consecutive tool errors, repeated identical actions, no progress over N
   steps, failed verification) escalate the planner model for the next steps. Built on our nested and bench harnesses, calibrated per policy against a
   single-model baseline before it is on by default.
5. **Optional judge or learned router, last.** A small local judge (Arch-Router class, or a `p_solve`-style classifier) for *background* tasks only,
   off by default, behind an evaluation that shows it beats the rules on our own task set. Interactive turns never wait for a judge.
6. **Protocols.** Native Anthropic Messages and OpenAI Responses clients matter (Claude directly; the ChatGPT-plan flow). `switchyard-translation` converts
   between OpenAI chat, Responses and Anthropic messages and is Apache-2.0: read it before writing our own.

### Adopting libsy: what it would cost

`switchyard-libsy` is a git dependency (tag-pinned, `publish` lists crates.io), Beta, "API will change". Its dependencies include crates we do not have in
`Cargo.lock`: `opentelemetry` 0.32 (+ `tracing-opentelemetry`), `jsonschema` (a large tree), `tokio-stream`, `futures`; the rest we already carry
(`serde`, `tokio`, `regex`, `rand`, `uuid`, `parking_lot`, `async-trait`, `http`, `thiserror`, `jsonptr`, `shlex`). Adding any of them needs the owner's OK.
Decide after step 4, when we know whether `stage_router` or the escalation judge beats our own signals on agent tasks.

### Evaluate before enabling anything clever

A routing eval set from Helix itself: Talk turns, summaries, code questions, agent tasks, each run on every candidate model; measure completed-task
quality, cost and latency against the single best model (`tools/bench/mobius-bench.py` and the nested agent tests are the starting harness). A router
ships on by default only if it wins there.

## Status

* 2026-10-29: steps 1 and 2 of the order below are built. `services/helix-inference/src/router.rs` is the policy router: hard rules first (Act only on this
  computer; private content, meaning any picture or anything the caller passes as `private`, never leaves this computer, and a slot routed elsewhere is
  quietly kept local with the sentence saying so; a named backend or model is honoured; "ask a stronger model" uses the slot's stronger-model choice;
  a request too long for the local engine goes to that choice if the person set one and the content may leave), then the person's route for the slot.
  Every decision carries a sentence, returned in the `Completed` result as `route {backend, model, reason}`, shown in Mobius as a tooltip on the reply
  details, and written in the audit line. Each slot can have a stronger-model backend and model (`escalate_backend`, `escalate_model` in
  `inference-routing.toml`), set in Helix Models → Routing ("When it is not enough"); Mobius's redo menu has "A stronger model". Nothing is asked of a
  model to route and there is nothing to calibrate. 8 router tests.
* Next: the routing log and price table (Activity page), agent-loop signals, protocol clients.

## Order of work

1. Policy router with reasons and a routing log; reason chip in Mobius. (small, no new dependencies)
2. "Ask a stronger model" action and per-slot escalation backend in Helix Models → Routing.
3. Price table and Activity page (spend, savings, where things ran).
4. Anthropic Messages and Responses clients (read `switchyard-translation` first).
5. Agent-loop signals and the eval harness.
6. Optional judge, then reconsider libsy.
