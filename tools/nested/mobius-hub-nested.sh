#!/bin/bash
# Mobius with Hugging Face answered from files (no network): one search, three repositories (a known maker with three ways to get it, a
# gated one, and one whose files list no checksum).
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
F=$XDG_STATE_HOME/hub-fixtures; mkdir -p "$F"
python3 - "$F" <<'PY'
import json, sys
d = sys.argv[1]
sha = "d" * 64
def f(path, size, oid=sha):
    e = {"type": "file", "path": path, "size": size}
    if oid: e["lfs"] = {"oid": oid}
    return e
w = lambda name, v: json.dump(v, open(f"{d}/{name}", "w"))
w("search.json", [
    {"id": "Qwen/Qwen3-4B-GGUF", "downloads": 554740, "likes": 194, "tags": ["gguf", "license:apache-2.0"], "pipeline_tag": "text-generation"},
    {"id": "randomperson/odd-model-GGUF", "downloads": 12, "likes": 0, "tags": ["gguf"], "pipeline_tag": "text-generation"},
    {"id": "acme/locked-GGUF", "downloads": 90, "likes": 3, "tags": ["gguf", "license:other"]},
])
w("info-Qwen-Qwen3-4B-GGUF.json", {"id": "Qwen/Qwen3-4B-GGUF", "gated": False, "downloads": 554740, "likes": 194, "tags": ["gguf", "license:apache-2.0"]})
w("tree-Qwen-Qwen3-4B-GGUF.json", [
    f("Qwen3-4B-Q4_K_M.gguf", 2497280256), f("Qwen3-4B-Q8_0.gguf", 4280404704), f("Qwen3-4B-BF16-00001-of-00002.gguf", 4000000000),
    f("Qwen3-4B-BF16-00002-of-00002.gguf", 4051285536), f("Qwen3-4B-F32.gguf", 90000000000), f("mmproj-F16.gguf", 800000000), f("README.md", 500, None)])
w("info-randomperson-odd-model-GGUF.json", {"id": "randomperson/odd-model-GGUF", "gated": False, "tags": ["gguf"]})
w("tree-randomperson-odd-model-GGUF.json", [f("odd-Q4_K_M.gguf", 2000000000, None)])
w("info-acme-locked-GGUF.json", {"id": "acme/locked-GGUF", "gated": "manual", "tags": ["gguf"]})
w("tree-acme-locked-GGUF.json", [])
PY
export HELIX_HUB_FIXTURES=$F
exec "$ROOT/tools/nested/mobius-nested.sh"
