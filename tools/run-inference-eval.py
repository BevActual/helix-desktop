#!/usr/bin/env python3
"""Runs the tool-calling evaluation across models and prompting strategies and prints a comparison.

  tools/run-inference-eval.py <repo-root> [--models a.gguf b.gguf ...] [--out DIR] [--quick]

Needs the engine built into third_party/ and the models in ~/.local/share/helix/models. Takes a while (minutes per run).
Reports are written as JSON, one per run, so a later change can be compared with an earlier one.
"""
import argparse
import json
import pathlib
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument("root")
parser.add_argument("--models", nargs="*", default=["qwen2.5-1.5b-instruct-q4_k_m.gguf", "qwen2.5-7b-instruct-q4_k_m-00001-of-00002.gguf"])
parser.add_argument("--out", default=None)
parser.add_argument("--quick", action="store_true", help="first 20 cases only")
args = parser.parse_args()

root = pathlib.Path(args.root).resolve()
out = pathlib.Path(args.out) if args.out else root / "build-debug" / "eval"
out.mkdir(parents=True, exist_ok=True)
models_dir = pathlib.Path.home() / ".local/share/helix/models"
binary = root / "build-debug/inference_eval"

CONFIGS = [  # (label, strategy, decline, fewshot)
    ("flat", "flat", 0, 0),
    ("flat + decline", "flat", 1, 0),
    ("staged + decline", "staged", 1, 0),
    ("flat + decline + 5 examples", "flat", 1, 5),
    ("staged + decline + 5 examples", "staged", 1, 5),
]
rows = []
for model in args.models:
    if not (models_dir / model).exists():
        print(f"skipping {model}: not installed", file=sys.stderr)
        continue
    for label, strategy, decline, fewshot in CONFIGS:
        report_file = out / f"{model.split('-')[1]}_{strategy}_d{decline}_f{fewshot}.json"
        command = [str(binary), "--root", str(root), "--model", str(models_dir / model), "--strategy", strategy,
                   "--decline", str(decline), "--fewshot", str(fewshot), "--out", str(report_file)]
        if args.quick:
            command += ["--limit", "20"]
        print(f"running {model} / {label} ...", flush=True)
        result = subprocess.run(command, capture_output=True, text=True)
        if not report_file.exists():
            print(result.stdout[-600:], result.stderr[-600:])
            raise SystemExit(f"no report for {model} / {label}")
        rows.append((model, label, json.loads(report_file.read_text())))

print()
print(f"{'model':8s} {'configuration':32s} {'exact':>7s} {'right tool':>11s} {'declined ok':>12s} {'wrongly declined':>17s} {'acted instead':>14s} {'median':>8s}")
for model, label, report in rows:
    s, c = report["summary"], report["summary"]["counts"]
    print(f"{model.split('-')[1]:8s} {label:32s} {100*s['exact_rate']:6.0f}% {100*s['right_tool_rate']:10.0f}% "
          f"{c.get('declined-correctly', 0):>6d}/{s['must_decline']:<5d} {c.get('declined-wrongly', 0):>17d} "
          f"{c.get('acted-instead-of-declining', 0):>14d} {s['median_ms']:>6d}ms")
print()
best = max(rows, key=lambda r: r[2]["summary"]["exact_rate"])
print(f"best exact rate: {best[0].split('-')[1]} / {best[1]} at {100*best[2]['summary']['exact_rate']:.0f}%")
print("by category for the best configuration:")
for category, counts in sorted(best[2]["by_category"].items()):
    n = counts["_n"]
    print(f"  {category:14s} exact {counts.get('exact', 0):2d}/{n:<2d}  " + ", ".join(f"{k}={v}" for k, v in sorted(counts.items()) if k not in ('_n', 'exact')))
