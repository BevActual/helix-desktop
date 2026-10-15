#!/usr/bin/env bash
# Point this clone at BevActual/helix-desktop as the only GitHub product remote.
#
# Older clones added `upstream` → noctalia-dev/noctalia and tracked `main` there.
# Helix is a standalone repository now (not a GitHub fork). Run once after pull:
#
#   tools/git-helix-origin.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

if ! git remote get-url origin &>/dev/null; then
  echo "error: no origin remote; clone from https://github.com/BevActual/helix-desktop.git" >&2
  exit 1
fi

case "$(git remote get-url origin)" in
  *BevActual/helix-desktop* | *helix-desktop*) ;;
  *)
    echo "warning: origin is not BevActual/helix-desktop: $(git remote get-url origin)" >&2
    ;;
esac

git fetch origin
git config branch.main.remote origin
git config branch.main.merge refs/heads/main
git config --unset branch.main.vscode-merge-base 2>/dev/null || true

if git remote get-url upstream &>/dev/null; then
  echo "Removing legacy upstream remote (noctalia-dev/noctalia). Re-add only if you need historical diff:"
  echo "  git remote add noctalia https://github.com/noctalia-dev/noctalia.git"
  git remote remove upstream
fi

if command -v gh >/dev/null; then
  gh repo set-default BevActual/helix-desktop
fi

echo "main → origin/main; gh default → BevActual/helix-desktop"
