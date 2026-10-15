#!/usr/bin/env bash
# Open pull requests against BevActual/helix-desktop, not noctalia-dev/noctalia.
#
# This repo has two remotes: origin (Helix) and upstream (Noctalia). Local `main`
# tracks upstream, so plain `gh pr create` can target the wrong repository.
#
# Usage:
#   tools/gh-pr.sh create --base main --title "..." --body "..."
#   tools/gh-pr.sh pr list
set -euo pipefail
export GH_REPO=BevActual/helix-desktop
exec gh "$@"
