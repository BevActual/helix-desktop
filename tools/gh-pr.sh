#!/usr/bin/env bash
# Pull requests on BevActual/helix-desktop (standalone repo; not a GitHub fork).
#
# Legacy clones may track noctalia-dev/noctalia as `upstream`; run tools/git-helix-origin.sh.
#
# Usage:
#   tools/gh-pr.sh create --base main --title "..." --body "..."
#   tools/gh-pr.sh pr list
set -euo pipefail
export GH_REPO=BevActual/helix-desktop
exec gh "$@"
