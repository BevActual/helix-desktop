# Mobius software analysis

Status: first ELF analysis slice implemented, 2026-10-12. Broader artifact support and model-directed tool loops remain planned.

## Implemented slice

- Mobius menu → Analyze software: select an ELF up to 64 MiB, optionally name a function, run analysis, cancel, inspect evidence, and explain with an installed local model.
- `apps/helix-mobius/src-tauri/src/analysis.rs` owns job state, immutable input copies, SHA-256 identity, Bubblewrap namespaces, output limits, a ten-minute deadline, cleanup, and saved evidence.
- `apps/helix-mobius/src-tauri/analysis-worker.mjs` uses REA's MCP client dependencies, discovers tool schemas, validates every call, and admits only six reviewed lifecycle/static tools. It gathers overview, functions, function pseudocode/references, and strings.
- Analysis conversations persist their job and local model. Saving, reopening, editing, regenerating, and forking retain the local inference boundary regardless of persona or global routing.
- No external network or host home access inside the analysis namespace. Node and Java heaps are bounded; this is not a complete memory or disk quota.
- Evidence manifests live under `~/.local/share/helix/mobius/analysis/<job>/evidence.json`. Input copies and engine scratch are removed after completion, cancellation, or failure. Abrupt app termination kills the worker namespace, but can leave scratch directories to be cleaned manually.

Optional setup: `python3 tools/scripts/install_analysis.py`. It installs REA 6.4.0, Ghidra 12.1.4, and a private JDK 21 when needed under the Helix data directory. Ghidra/JDK archives are checksum-verified; no IDE/agent registration is changed. Node and Bubblewrap must already be installed.

Engine verification: `cargo test -p helix-mobius --profile fast analysis::tests -- --include-ignored`. This uses a real compiled ELF/Ghidra fixture and verifies cancellation leaves no owned analysis process. Chat tests verify analysis metadata survives saving and forking.

Nested UI coverage: `tools/nested/mobius-analysis-test.sh`, invoked through `tools/nested/run.sh` with `HELIX_TEST_ANALYSIS_CONFIG` pointing at the installed configuration. It uses real Ghidra and scripted local inference to check evidence, explanation routing, reopening, and cancellation. It does not assess model answer quality.

Mobius should let a person select software, ask how it works, and receive an answer tied to inspectable evidence. REA supplies agent-facing analysis tools; Ghidra supplies native disassembly and decompilation through REA's existing provider. This is an optional capability of Mobius.

Sources:
- [REA overview](https://github.com/morluto/rea)
- [REA installation and provider compatibility](https://github.com/morluto/rea/blob/main/docs/installation.md)
- [REA MCP contracts](https://github.com/morluto/rea/blob/main/docs/mcp-contracts.md)
- [Ghidra](https://github.com/NationalSecurityAgency/ghidra)

## Integration boundary

Helix's MCP server exports desktop actions to external clients. It is not an MCP client for Mobius. Mobius's remote chat path deliberately does not request tools and ignores returned `tool_calls`. Adding REA to another agent's configuration therefore does not add analysis to Mobius.

The implemented runner adds its own MCP client, validated tool dispatch, job lifecycle, and evidence store. It does not turn arbitrary chat responses into desktop commands. Existing Agent Surface grants and AssistantSubmit remain the boundary for desktop actions.

## Target product flow

1. Choose **Analyze software** in Mobius and select a file or application directory.
2. Show the target, analysis mode, available engine, and where analysis results will go.
3. Start static analysis, with progress, elapsed time, and Cancel visible in the thread.
4. Ask questions such as "What libraries does this use?", "Explain this function", or "Where does this application handle clipboard data?"
5. Show findings with evidence links, addresses or file locations, and unresolved questions. Label decompiled code as pseudocode.
6. Export a report and optionally open Ghidra for manual investigation.

Initially support Linux ELF and JavaScript/Electron artifacts. Add runtime observation later as a separate explicitly selected mode.

## Process boundary

Implemented path:

```text
Mobius UI -> analysis runner -> REA MCP -> Ghidra provider / static JS tools
                   |
                   +-> bounded evidence -> selected model -> explanation
```

Keep analysis subprocesses outside the inference service and session shell. Java analysis can consume substantial CPU and memory; it should not block the desktop or compete in the inference request queue.

The runner owns private job directories, target copies, process groups, deadlines, resource limits, and cleanup. Give the analysis process access to the selected artifact and its scratch directory, rather than the user's entire home. Verify that REA and Ghidra actually work under those restrictions; do not assume their existing process isolation matches Helix's policy.

Only reviewed static tools enter the first allowlist. Discover schemas from the connected REA version and validate arguments against them. Reject unreviewed tools even when advertised. Tool output is untrusted evidence; it cannot grant permissions or change analysis scope.

Keep analysis local by default. Sending evidence to LAN/cloud follows an explicit data-sharing decision naming the destination. A future model-directed investigation loop requires a distinct analysis policy; do not enable remote desktop tool calls to achieve it.

## Evidence and lifecycle

Store the target digest, engine and REA versions, analysis profile, tool arguments, results, and errors under a job ID. Answers link to these records. Preserve limitations and truncation markers; retrieve paged evidence when necessary instead of silently losing it.

Job states: preparing, analyzing, ready, cancelling, cancelled, failed. Closing a thread must not orphan workers. Cancellation stops the owned process group, closes provider sessions, and reports cleanup failures. Cold import and follow-up queries need different deadlines.

Use a pinned REA release and a verified compatible Ghidra/JDK pair. Ghidra development-head requirements can differ from the release supported by REA. Dependency readiness should describe missing components before a job starts; installation belongs in an explicit optional setup flow.

## Delivery slices

1. A deterministic static-analysis job from the UI: target selection, runner, REA connection, Ghidra provider, bounded output, cancel, cleanup, evidence card. No model tool loop required.
2. Questions over stored findings, including citations and insufficient-evidence responses.
3. A bounded model investigation loop using the reviewed analysis tools, followed by report export and manual Ghidra handoff.
4. Separately scoped runtime observation and additional artifact formats.

First acceptance fixture: compile a small ELF with a known function and string. Mobius must identify the artifact, retrieve the function's pseudocode and string reference, explain them with evidence, and finish without remaining owned subprocesses. Also verify missing dependencies, invalid targets, cold-start timeout, cancellation, oversized results, and attempts to escape the selected target scope.
