# Helix System Monitor (Tauri)

> **Frontend:** React + TypeScript + Vite (`apps/helix-system-monitor/web`, built into `dist/react/helix-monitor.js`); `dist/monitor.js` is the no-node fallback. Rust samples and emits `monitor`; the page reads when told. The React page adds meters, a minute of CPU and memory history, sortable processes (CPU time, memory, name, PID) with a memory bar, and screen-time bars.

Frontend direction: React + TypeScript + Vite like the other apps (`frontend-stack.md`); the page is hand-written until it next grows a page.

Standalone resource viewer. Reads `/proc` and screen-time report files directly (no shell, no desktop service). Pure parsing and list models in `helix-monitor-model::monitor`; thin Tauri host schedules polls.

## Pages

| Page | Content | Deep-link |
|------|---------|-----------|
| Overview | CPU, memory, network, disk, GPU graphs | default / `--page overview` |
| Processes | Searchable / sortable process table | `--page processes` |
| Screen Time | Report list from Helix screen-time files | `--page screen-time` |

## Poll rules (stated reasons)

Match `docs/user/services/system-monitor.mdx` defaults:

| Group | Interval | Reason |
|-------|----------|--------|
| CPU / load / temp | 2 s | Same sampling pass as the service |
| Memory | 2 s | RAM changes with CPU pass cadence |
| Network | 3 s | `/proc/net/dev` throughput |
| Disk / swap | 10 s | Slow-changing capacity |
| GPU | 5 s, **only while GPU UI is shown** | Avoid waking a discrete GPU when Overview is not showing GPU |

Timers are documented; no anonymous `setInterval` in the page for sampling — the host owns polls and pushes views.

## v1 omissions

- Process kill / renice stay unimplemented.
- Bar / Control Center `sysmon` widgets keep using `system_monitor_service` until a later cutover.

## Budgets

Own process. Memory is PSS of the app and its WebKit processes:

| Measurement | Value | Notes |
|-------------|-------|-------|
| Nested Overview with fake `/proc` | not measured | The nested compositor's start-up (about 14 s) dominates; it says nothing about the app |
| Memory after first nested run (PSS, app and its WebKit processes) | about 296 MB | `tools/nested/measure-app.sh` with `MEASURE_READY=page-overview MEASURE_BIN=helix-system-monitor`; unchanged 15 s later (295.5 MB). Measured 2026-10-16 on a `fast` build |

## Nested tests

Fake `/proc` tree under the scratch dir; assert layout controls and that poll cadence matches the intervals above (host clock under test injection).
