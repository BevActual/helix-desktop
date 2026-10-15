# Source layering

Dependency direction between workspace crates is checked by `tools/check-layers.py` (meson test `layering`). It reads
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

`compositor/` is its own workspace and is not checked here.

## Known exceptions

`tools/layers-baseline.txt` lists them. Today: `helix-settings -> helix-wallpaper` (Settings links the wallpaper helper's
library). Fix it by moving the shared code into `core/` or by calling the helper over its interface, then drop the line.
