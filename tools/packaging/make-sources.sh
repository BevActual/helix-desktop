#!/usr/bin/env bash
# Makes the three source archives packaging/helix.spec names, so an RPM build needs no network:
#   helix-desktop-<version>.tar.gz              the committed tree (git archive; nothing from your working copy, no build output)
#   helix-desktop-<version>-vendor.tar.zst      every Rust dependency (cargo vendor) and the cargo config that points at it
#   helix-desktop-<version>-node-modules.tar.zst  node_modules (npm ci), for the web views
#
#   tools/packaging/make-sources.sh [OUTDIR]          OUTDIR defaults to ./rpm-sources
#   OFFLINE=1 tools/packaging/make-sources.sh         use only what the machine already has cached
#
# The two dependency archives are specific to the lock files (Cargo.lock, package-lock.json) of the commit, and the node_modules one to
# the architecture (x86_64): make them again when either lock file changes.
set -euo pipefail
cd "$(dirname "$0")/../.."
ROOT=$PWD
OUT=${1:-$ROOT/rpm-sources}
VERSION=$(sed -n 's/^Version: *//p' packaging/helix.spec)
NAME=helix-desktop-$VERSION
command -v zstd >/dev/null || { echo "make-sources: zstd is needed" >&2; exit 1; }
[ -z "$(git status --porcelain --untracked-files=no)" ] || echo "note: uncommitted changes are not in the archive (it is made from HEAD)" >&2
mkdir -p "$OUT"
WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT

git archive --format=tar --prefix="$NAME/" HEAD | tar -x -C "$WORK"
tar -C "$WORK" -czf "$OUT/$NAME.tar.gz" "$NAME"
echo "wrote $OUT/$NAME.tar.gz"

# Rust: vendor from the tree being packaged, so Cargo.lock is the one in the archive. cargo prints the source-replacement config (including the
# git source of the compositor's smithay) on stdout; the spec appends it to .cargo/config.toml.
cd "$WORK/$NAME"
mkdir -p "$WORK/v"
# cargo empties the directory it vendors into, so the config goes beside it, not in it.
cargo vendor ${OFFLINE:+--offline} --locked "$WORK/v/vendor" > "$WORK/v/cargo-vendor-config.toml"
# The config refers to the directory as "vendor"; the archive unpacks both at the top of the source tree.
tar -C "$WORK/v" --zstd -cf "$OUT/$NAME-vendor.tar.zst" vendor cargo-vendor-config.toml
echo "wrote $OUT/$NAME-vendor.tar.zst ($(du -h "$OUT/$NAME-vendor.tar.zst" | cut -f1))"

# JavaScript: one npm workspace at the root.
npm ci ${OFFLINE:+--offline} --ignore-scripts --no-audit --no-fund >/dev/null
tar --zstd -cf "$OUT/$NAME-node-modules.tar.zst" node_modules
echo "wrote $OUT/$NAME-node-modules.tar.zst ($(du -h "$OUT/$NAME-node-modules.tar.zst" | cut -f1))"
echo "build with: rpmbuild -bb --define \"_sourcedir $OUT\" packaging/helix.spec"
