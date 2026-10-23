#!/usr/bin/env python3
"""A tiny MCP server (stdio) for the nested tests: one tool, `add`, that adds two numbers."""
import json, sys
for line in sys.stdin:
    m = json.loads(line)
    if "id" not in m:
        continue
    if m["method"] == "initialize":
        r = {"protocolVersion": "2024-11-05", "capabilities": {"tools": {}}, "serverInfo": {"name": "fake", "version": "1"}}
    elif m["method"] == "tools/list":
        r = {"tools": [{"name": "add", "description": "Add two numbers", "inputSchema": {"required": ["a", "b"], "properties": {"a": {}, "b": {}}}}]}
    elif m["method"] == "tools/call":
        a = m["params"]["arguments"]
        r = {"content": [{"type": "text", "text": "sum=" + str(a["a"] + a["b"])}]}
    else:
        r = {}
    print(json.dumps({"jsonrpc": "2.0", "id": m["id"], "result": r}), flush=True)
