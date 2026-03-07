#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${RELAY_SMOKE_PORT:-41111}"
SECRET="${RELAY_SMOKE_SECRET:-smoke-secret}"

cleanup() {
  if [[ -n "${SERVER_PID:-}" ]]; then
    kill "${SERVER_PID}" >/dev/null 2>&1 || true
    wait "${SERVER_PID}" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

cd "$ROOT_DIR"
go run . --bind 127.0.0.1 --port "$PORT" --secret "$SECRET" --heartbeat-interval 100ms --heartbeat-timeout 3s >/tmp/relay-smoke.log 2>&1 &
SERVER_PID=$!
sleep 1

python3 - <<'PY'
import json
import socket
import struct
import time

import os
HOST = "127.0.0.1"
PORT = int(os.environ.get("RELAY_SMOKE_PORT", "41111"))
SECRET = os.environ.get("RELAY_SMOKE_SECRET", "smoke-secret")

def write_frame(sock, payload):
    b = payload.encode("utf-8")
    sock.sendall(struct.pack(">I", len(b)) + b)

def read_frame(sock):
    header = sock.recv(4)
    if len(header) < 4:
        raise RuntimeError("short frame header")
    n = struct.unpack(">I", header)[0]
    body = b""
    while len(body) < n:
        chunk = sock.recv(n - len(body))
        if not chunk:
            raise RuntimeError("connection closed")
        body += chunk
    return json.loads(body.decode("utf-8"))

c1 = socket.create_connection((HOST, PORT), timeout=2)
c2 = socket.create_connection((HOST, PORT), timeout=2)

write_frame(c1, json.dumps({"intent":"hello","protocolVersion":1,"clientSequence":0,"authToken":SECRET}))
assert read_frame(c1)["kind"] == "playerAssigned"
write_frame(c2, json.dumps({"intent":"hello","protocolVersion":1,"clientSequence":0,"authToken":SECRET}))
assert read_frame(c2)["kind"] == "playerAssigned"

write_frame(c1, json.dumps({"intent":"createRoom","clientSequence":1,"roomName":"smoke-room","maxPlayers":2}))
assert read_frame(c1)["kind"] == "playerAssigned"
assert read_frame(c1)["kind"] == "roomJoined"

write_frame(c2, json.dumps({"intent":"joinRoom","clientSequence":1,"roomId":"smoke-room"}))
assert read_frame(c2)["kind"] == "playerAssigned"
assert read_frame(c2)["kind"] == "roomJoined"

write_frame(c1, json.dumps({"intent":"publishLinkEvent","clientSequence":2,"event":{"sequence":1,"senderPlayerId":999,"tickMarker":1,"payload":"AQ=="}}))
a = read_frame(c1)
b = read_frame(c2)
assert a["kind"] == "inboundLinkEvent" and b["kind"] == "inboundLinkEvent"
assert a["serverSequence"] == 1 and b["serverSequence"] == 1

print("relay smoke passed: handshake/join/rebroadcast verified")

c1.close()
c2.close()
PY

echo "Smoke log: /tmp/relay-smoke.log"
