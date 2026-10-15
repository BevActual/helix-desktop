# Source layering

Dependency direction between workspace crates is checked by `tools/check-layers.py` (`tools/test-integration.sh layering`). It reads
`cargo metadata`, applies the rules in `tools/layers.toml`, and compares against `tools/layers-baseline.txt`: a new
violation fails, and a baseline entry that has been fixed must be removed.

```sh
python3 tools/check-layers.py .                    # check
python3 tools/check-layers.py . --list             # show current violations
python3 tools/check-layers.py . --update-baseline  # only after removing a violation
```

## Rules

A crate may depend (by path) only on the directories listed for its own top-level directory.

| Directory | May depend on | Why |
|---|---|---|
| `core/` | `core/`, `third_party/` | Pure models and clients; no process or UI knowledge |
| `services/` | `core/`, `third_party/` | One process each; services talk over contracts in `protocols/`, not by linking |
| `shell/` | `core/`, `third_party/` | Presentation; backends belong to the service |
| `apps/` | `core/`, `third_party/` | Peers on the bus, never linked to another process's crate |
| `greeter/` | `third_party/` | Login is a trust surface; it links nothing from the session |
| `dev/` | anything | Test tooling |

Inside `core/` the order of crates is checked too, from the `[crates]` table in `tools/layers.toml`:

| Crate | May depend on |
|---|---|
| `helix-base`, `helix-theme`, `helix-desktop-api` | none of the others |
| `helix-agent`, `helix-config`, `helix-models`, `helix-compositor`, `helix-software-model`, `helix-monitor-model` | `helix-base` |
| `helix-config-export` | `helix-config` |
| `helix-shell-model` | `helix-base`, `helix-compositor` |
| `helix-chat` | `helix-base`, `helix-config` |

A new crate in `core/` fails the check until it has a row there, so every new edge between core crates is a deliberate edit to that
file. See `shell-core-split.md` for what each crate holds.

`compositor/` is its own workspace and is not checked here.

## Known exceptions

`tools/layers-baseline.txt` lists them. Today: `helix-settings -> helix-wallpaper` (Settings links the wallpaper helper's
library). Fix it by moving the shared code into `core/` or by calling the helper over its interface, then drop the line.
