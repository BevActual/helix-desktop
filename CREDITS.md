# Credits

Helix builds on the open-source projects below. `Cargo.lock` and `package-lock.json` are the authoritative list of every crate and package;
each dependency is governed by its own license.

## Design & Branding

- **[Tabler Icons](https://tabler.io/icons)** — Icon set used throughout the shell and apps (MIT; license in `assets/fonts/tabler-icons-license.txt`)
- **[Riyan Resdian on Noun Project](https://thenounproject.com/creator/yaicon/)** — Plug icon

## Compositor and session

- **[niri](https://github.com/YaLTeR/niri)** — The scrolling Wayland compositor that `compositor/` forks (GPL-3.0-or-later)
- **[greetd](https://sr.ht/~kennylevinsen/greetd/)** — Login manager the greeter speaks to
- **[Wayland](https://wayland.freedesktop.org/)**, **[wlroots protocols](https://gitlab.freedesktop.org/wlroots/wlr-protocols)** — Display protocol and the layer-shell, gamma-control, screencopy and output-management extensions the shell uses
- **[PipeWire](https://pipewire.org/)** and **[WirePlumber](https://pipewire.pages.freedesktop.org/wireplumber/)** — Audio
- **[NetworkManager](https://networkmanager.dev/)**, **[BlueZ](https://www.bluez.org/)**, **[polkit](https://gitlab.freedesktop.org/polkit/polkit)**, **[systemd](https://systemd.io/)** — System services Helix talks to over D-Bus

## Application framework

- **[Tauri](https://tauri.app/)** — Desktop application host for the shell, greeter and apps (MIT / Apache-2.0)
- **[WebKitGTK](https://webkitgtk.org/)** and **[GTK](https://www.gtk.org/)** — Web view and toolkit under Tauri; **[gtk-layer-shell](https://github.com/wmww/gtk-layer-shell)** — layer surfaces for the bar and panels
- **[React](https://react.dev/)**, **[Vite](https://vite.dev/)**, **[TypeScript](https://www.typescriptlang.org/)** — App frontends (MIT)
- **[ts-rs](https://github.com/Aleph-Alpha/ts-rs)** — TypeScript types generated from Rust view structs

## Rust libraries

The main ones; see `Cargo.lock` for the rest.

- **[zbus](https://github.com/dbus2/zbus)** — D-Bus client and server
- **[tokio](https://tokio.rs/)**, **[serde](https://serde.rs/)**, **[toml](https://github.com/toml-rs/toml)**, **[reqwest](https://github.com/seanmonstar/reqwest)**, **[tracing](https://github.com/tokio-rs/tracing)**
- **[image](https://github.com/image-rs/image)** — Image decoding and encoding
- **[pipewire-rs](https://gitlab.freedesktop.org/pipewire/pipewire-rs)** — Microphone level metering
- **[keyring](https://github.com/hwchen/keyring-rs)** — Secret Service access for stored credentials
- **[scraper](https://github.com/causal-agent/scraper)**, **[roxmltree](https://github.com/RazrFalcon/roxmltree)** — Reading web pages and appstream data

## Colour and theming

- **[Material Color Utilities](https://github.com/material-foundation/material-color-utilities)** — Material 3 palette generation (Apache-2.0). `third_party/material_color_utilities` is the C++ reference the HCT golden tests were generated from; `third_party/material-colors` is the Rust port the theme engine uses.

## Local models

Optional engines Helix runs on this computer when you install them:

- **[llama.cpp](https://github.com/ggml-org/llama.cpp)** — Text and vision models (MIT)
- **[whisper.cpp](https://github.com/ggml-org/whisper.cpp)** — Speech recognition (MIT)
- **[stable-diffusion.cpp](https://github.com/leejet/stable-diffusion.cpp)** — Image generation (MIT)
- **[Ghidra](https://ghidra-sre.org/)** — Software analysis for Mobius, when you set it up (Apache-2.0)

## Vendored in `third_party/`

- **[Wuffs](https://github.com/google/wuffs)** — Memory-safe image decoders (Apache-2.0); not built yet, reserved for the image-decode plan in `docs/design/inference-capabilities.md`

## Special Thanks

- The **Wayland** community for building the future of Linux desktop graphics
- The **niri** team for the compositor Helix is built on
- All the contributors and users who have helped make Helix better

## License

Helix is licensed under the MIT License. See [LICENSE](LICENSE) for details. The compositor in `compositor/` is a fork of niri and keeps its GPL-3.0-or-later license.
