{
  lib,
  config,
  stdenv,
  meson,
  ninja,
  pkg-config,
  wayland-scanner,
  wayland,
  wayland-protocols,
  libGL,
  libglvnd,
  freetype,
  fontconfig,
  cairo,
  pango,
  harfbuzz,
  libxkbcommon,
  sdbus-cpp_2,
  systemd,
  pipewire,
  pam,
  curl,
  libwebp,
  libjxl,
  libsndfile,
  glib,
  polkit,
  librsvg,
  libqalculate,
  libxml2,
  libsecret,
  libsodium,
  stb,
  fetchFromGitHub,
  nlohmann_json,
  tomlplusplus,
  libical,
  wireplumber,
  jemalloc,
  makeWrapper,
  git,
  gsettings-desktop-schemas,
  autoAddDriverRunpath,
  # DEPRECATED: no longer affects the build; kept for `.override` compat.
  cudaSupport ? config.cudaSupport,
}:
let
  inherit (builtins) head match readFile;
  version = head (match ".*version: '([0-9][^']+)'.*" (readFile ../meson.build));
  stb' = stb.overrideAttrs (_: {
    version = "unstable-2025-10-26";
    src = fetchFromGitHub {
      owner = "nothings";
      repo = "stb";
      rev = "f1c79c02822848a9bed4315b12c8c8f3761e1296";
      hash = "sha256-BlyXJtAI7WqXCTT3ylww8zoG0hBxaojJnQDvdQOXJPE=";
    };
  });
in
lib.warnIf cudaSupport
  "helix: `cudaSupport` no longer has any effect (autoAddDriverRunpath is now always applied); this argument will be removed in the future."
  stdenv.mkDerivation {
  pname = "helix";
  inherit version;

  src = lib.cleanSource ./..;

  postFixup = ''
    wrapProgram $out/bin/helix \
      --prefix PATH : ${lib.makeBinPath [ git ]} \
      --prefix XDG_DATA_DIRS : "${glib.getSchemaDataDirPath gsettings-desktop-schemas}"

    $out/bin/helix completions bash | install -D /dev/stdin $out/share/bash-completion/completions/helix
    $out/bin/helix completions zsh  | install -D /dev/stdin $out/share/zsh/site-functions/_helix
    $out/bin/helix completions fish | install -D /dev/stdin $out/share/fish/vendor_completions.d/helix.fish
  '';

  nativeBuildInputs = [
    meson
    ninja
    pkg-config
    wayland-scanner
    jemalloc
    makeWrapper
    autoAddDriverRunpath
  ];

  buildInputs = [
    wayland
    wayland-protocols
    libGL
    libglvnd
    freetype
    fontconfig
    cairo
    pango
    harfbuzz
    libxkbcommon
    sdbus-cpp_2
    systemd
    pipewire
    wireplumber
    pam
    curl
    libwebp
    libjxl
    libsndfile
    glib
    polkit
    librsvg
    libqalculate
    libxml2
    libsecret
    libsodium
    stb'
    nlohmann_json
    tomlplusplus
    libical
  ];

  mesonBuildType = "release";

  mesonFlags = [ "-Dtests=disabled" ];

  ninjaFlags = [ "-v" ];

  meta = with lib; {
    description = "Helix Shell — a niri-first Wayland desktop shell (command: helix).";
    homepage = "https://github.com/BevActual/helix-desktop";
    license = licenses.mit;
    platforms = platforms.linux;
    mainProgram = "helix";
  };
}
