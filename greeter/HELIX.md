# Helix Greeter

Login greeter for the Helix desktop: **greetd → helix-greeter → helix-niri + Helix Shell**.

Product placement: [`docs/design/product-structure.md`](../docs/design/product-structure.md).

## Live path (this tree)

| Path | Role |
|---|---|
| `greeter/helix-greeter-tauri/` | Tauri greeter (workspace member `helix-greeter`); login card is React in `web/` |

```sh
cargo build --profile fast -p helix-greeter
# binary: target/fast/helix-greeter
```

Binary name stays **`helix-greeter`**. Appearance sync and greetd session scripts follow the shell’s contracts (`docs/user/configuration/shell.mdx` greeter section; design: `docs/design/greeter-tauri.md`).

## Chrome match

Sync carries palette, wallpaper, `corner_radius_scale`, font, **glass** (from `transparency_mode`), **refraction**, **high_contrast**, **reduced_motion**, and **ui_scale**. Login chrome is glass; leftover form controls stay leftover (no glass-on-glass). High Contrast frosts chrome and drops refraction. Match the lock screen visually (`docs/design/helix-hig.md`).

## Retired

The C++/noctalia-fork greeter lived in a sibling repo. This Tauri greeter replaces it in the product tree; do not vendor that tree here.
