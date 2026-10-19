#!/usr/bin/env python3
"""A pretend ACP agent for the tests. FAKE names what it does; FAKE_LOG is a file every line it receives is copied to."""
import json, os, sys, time

SCENARIO = os.environ.get("FAKE", "turn")
LOG = os.environ.get("FAKE_LOG")

def out(obj):
    sys.stdout.write(json.dumps(obj) + "\n"); sys.stdout.flush()

def update(session, body):
    out({"jsonrpc": "2.0", "method": "session/update", "params": {"sessionId": session, "update": body}})

def chunk(session, kind, text):
    update(session, {"sessionUpdate": kind, "content": {"type": "text", "text": text}})

def read():
    line = sys.stdin.readline()
    if not line: sys.exit(0)
    if LOG:
        with open(LOG, "a") as f: f.write(line)
    return json.loads(line)

def ask(next_id, method, params):
    out({"jsonrpc": "2.0", "id": next_id, "method": method, "params": params})
    while True:
        m = read()
        if m.get("id") == next_id and "method" not in m: return m

def answer(req, result):
    out({"jsonrpc": "2.0", "id": req["id"], "result": result})

while True:
    req = read()
    method = req.get("method")
    if method == "initialize":
        version = 2 if SCENARIO == "wrongversion" else 1
        answer(req, {"protocolVersion": version, "agentCapabilities": {"loadSession": False, "promptCapabilities": {}},
                     "authMethods": [{"id": "login", "name": "Sign in", "description": "Sign in in the browser"}],
                     "agentInfo": {"name": "fake-agent", "title": "Fake agent", "version": "0.1"}})
        if SCENARIO == "bad_line": sys.stdout.write("this is not json\n"); sys.stdout.flush()
        if SCENARIO == "huge_line": sys.stdout.write("x" * (5 * 1024 * 1024) + "\n"); sys.stdout.flush()
        if SCENARIO == "nonrpc": out({"hello": "world"})
    elif method == "session/new":
        answer(req, {"sessionId": "s1"})
    elif method == "session/prompt":
        s = req["params"]["sessionId"]
        if SCENARIO == "turn":
            chunk(s, "agent_thought_chunk", "Thinking about the files.")
            chunk(s, "agent_message_chunk", "Hello")
            update(s, {"sessionUpdate": "tool_call", "toolCallId": "t1", "title": "Run the tests", "kind": "execute", "status": "pending",
                       "locations": [{"path": "/work/a.rs", "line": 3}], "rawInput": {"command": "cargo test"}})
            perm = ask(100, "session/request_permission", {"sessionId": s, "toolCall": {"toolCallId": "t1", "title": "Run the tests", "kind": "execute", "status": "pending"},
                       "options": [{"optionId": "yes", "name": "Allow once", "kind": "allow_once"}, {"optionId": "no", "name": "Reject", "kind": "reject_once"}]})
            update(s, {"sessionUpdate": "tool_call_update", "toolCallId": "t1", "status": "in_progress"})
            update(s, {"sessionUpdate": "tool_call_update", "toolCallId": "t1", "status": "completed", "content": [{"type": "content", "content": {"type": "text", "text": "ok"}}]})
            read_back = ask(101, "fs/read_text_file", {"sessionId": s, "path": "/work/a.rs"})
            chunk(s, "agent_message_chunk", "read:" + str(read_back.get("result", read_back.get("error", {})).get("content", read_back.get("error", {}).get("message", ""))))
            chunk(s, "agent_message_chunk", "choice:" + json.dumps(perm["result"]["outcome"]))
            update(s, {"sessionUpdate": "plan", "entries": [{"content": "Run tests", "priority": "high", "status": "completed"}]})
            update(s, {"sessionUpdate": "usage_update", "used": 1200, "size": 200000})
            update(s, {"sessionUpdate": "some_future_update", "anything": 1})
            answer(req, {"stopReason": "end_turn"})
        elif SCENARIO == "cancel":
            chunk(s, "agent_message_chunk", "working")
            while True:
                m = read()
                if m.get("method") == "session/cancel": break
            answer(req, {"stopReason": "cancelled"})
        elif SCENARIO == "crash":
            chunk(s, "agent_message_chunk", "about to fail")
            os._exit(3)
        elif SCENARIO == "unknown":
            r = ask(200, "foo/bar", {})
            chunk(s, "agent_message_chunk", "unknown:" + str(r.get("error", {}).get("code")))
            answer(req, {"stopReason": "end_turn"})
        elif SCENARIO == "flood":
            for i in range(3000): chunk(s, "agent_message_chunk", str(i))
            answer(req, {"stopReason": "end_turn"})
        elif SCENARIO == "burst":
            for i in range(40):
                out({"jsonrpc": "2.0", "id": 300 + i, "method": "fs/read_text_file", "params": {"sessionId": s, "path": "/work/a.rs"}})
            ok = refused = 0
            for _ in range(40):
                m = read()
                if "error" in m and "Too many" in m["error"].get("message", ""): refused += 1
                else: ok += 1
            chunk(s, "agent_message_chunk", "ok:%d refused:%d" % (ok, refused))
            answer(req, {"stopReason": "end_turn"})
        elif SCENARIO == "terminal":
            made = ask(400, "terminal/create", {"sessionId": s, "command": "ls", "args": ["-l"], "cwd": "/work"})
            chunk(s, "agent_message_chunk", "created:" + json.dumps(made.get("result", made.get("error"))))
            answer(req, {"stopReason": "end_turn"})
        else:
            answer(req, {"stopReason": "end_turn"})
