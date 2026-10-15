#!/usr/bin/env python3
"""Retired: the C++ `inference_eval` binary was removed with the C++ inference test harness.

Tool-calling evaluation against live models should use the Rust `helix-inference` service
(see docs/design/agent-and-inference.md and docs/design/mobius-bench.md). Reintroduce a
Cargo-based eval harness when needed; do not revive Meson C++ eval targets.
"""
from __future__ import annotations

import sys

sys.stderr.write(
    "error: tools/run-inference-eval.py is retired (C++ inference_eval removed).\n"
    "Use services/helix-inference / Mobius bench docs for live evaluation.\n"
)
raise SystemExit(1)
