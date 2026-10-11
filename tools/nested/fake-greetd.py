#!/usr/bin/env python3
"""A scripted greetd: asks for a password, accepts only "correct horse", logs every request it receives as JSON lines."""
import json, os, socket, struct, sys

path, log = sys.argv[1], sys.argv[2]
if os.path.exists(path):
    os.unlink(path)
server = socket.socket(socket.AF_UNIX)
server.bind(path)
server.listen(4)

def read(conn):
    head = b""
    while len(head) < 4:
        chunk = conn.recv(4 - len(head))
        if not chunk:
            return None
        head += chunk
    (length,) = struct.unpack("=I", head)
    body = b""
    while len(body) < length:
        body += conn.recv(length - len(body))
    return json.loads(body)

def send(conn, value):
    raw = json.dumps(value).encode()
    conn.sendall(struct.pack("=I", len(raw)) + raw)

while True:
    conn, _ = server.accept()
    while True:
        request = read(conn)
        if request is None:
            break
        with open(log, "a") as out:
            out.write(json.dumps(request) + "\n")
        kind = request["type"]
        if kind == "create_session":
            send(conn, {"type": "auth_message", "auth_message_type": "secret", "auth_message": "Password: "})
        elif kind == "post_auth_message_response":
            if request.get("response") == "correct horse":
                send(conn, {"type": "success"})
            else:
                send(conn, {"type": "error", "error_type": "auth_error", "description": "Authentication failed"})
        elif kind == "start_session":
            send(conn, {"type": "success"})
        else:
            send(conn, {"type": "success"})
