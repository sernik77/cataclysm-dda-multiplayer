#!/usr/bin/env python3
"""Minimal text client for testing a Cataclysm co-op host.

Speaks the multiplayer wire protocol (doc/c++/MULTIPLAYER.md, section 5):
4-byte big-endian length + JSON object. Uses only the standard library.

    ./cataclysm --mp-host 7777 --mp-loopback      # host, then load a game
    python3 tools/mp/mp_probe.py --port 7777       # this client

The host tells you its game version and mod fingerprint if they do not
match; pass them with --game-version and --mods-hash, or use --auto to copy
them from the host's rejection (development only).

Keys (type one per line, then Enter):
    h j k l y u b n   move west/south/north/east/NW/NE/SW/SE (vi keys)
    .                 wait one turn
    r / R             vote ready (skip time together) / take back the vote
    t / T             tactical mode on / off
    say <text>        chat
    q                 quit
"""

import argparse
import json
import re
import selectors
import socket
import struct
import sys
import uuid

PROTOCOL_VERSION = 1
MAX_FRAME = 16 * 1024 * 1024

TOGGLES = {
    "r": "ready", "R": "unready", "t": "tactical_on", "T": "tactical_off",
}

MOVES = {
    "h": (-1, 0), "j": (0, 1), "k": (0, -1), "l": (1, 0),
    "y": (-1, -1), "u": (1, -1), "b": (-1, 1), "n": (1, 1),
}


class Connection:
    def __init__(self, host, port, timeout):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.setblocking(False)
        self.buf = b""

    def send(self, obj):
        payload = json.dumps(obj).encode("utf-8")
        self.sock.sendall(struct.pack(">I", len(payload)) + payload)

    def read_available(self):
        """Returns decoded messages; raises ConnectionError when closed."""
        try:
            chunk = self.sock.recv(65536)
        except BlockingIOError:
            return []
        if not chunk:
            raise ConnectionError("host closed the connection")
        self.buf += chunk
        out = []
        while len(self.buf) >= 4:
            (length,) = struct.unpack(">I", self.buf[:4])
            if length > MAX_FRAME:
                raise ConnectionError("host sent an oversized frame")
            if len(self.buf) < 4 + length:
                break
            out.append(json.loads(self.buf[4:4 + length].decode("utf-8")))
            self.buf = self.buf[4 + length:]
        return out


def describe(msg):
    t = msg.get("t")
    if t == "tick":
        waiting = ",".join(str(i) for i in msg["waiting_for"]) or "-"
        return "turn %d  mode %-12s waiting for %s" % (
            msg["turn"], msg["mode"], waiting)
    if t == "self":
        return "you: (%d,%d,%d) hp %d/%d moves %d%s%s" % (
            msg["x"], msg["y"], msg["z"], msg["hp"], msg["hp_max"],
            msg["moves"],
            "  activity " + msg["activity"] if msg["activity"] else "",
            "  DANGER" if msg["danger"] else "")
    if t == "cmd_ack":
        if msg["ok"]:
            return None
        return "command %d refused: %s" % (msg["seq"], msg.get("reason", ""))
    if t == "chat":
        return "<%d> %s" % (msg["from"], msg["text"])
    if t == "bye":
        return "host says bye: %s" % msg["reason"]
    return json.dumps(msg)


def handshake(args, game_version, mods_hash, player_uuid):
    conn = Connection(args.host, args.port, args.timeout)
    hello = {
        "t": "hello", "proto": PROTOCOL_VERSION,
        "game_version": game_version, "mods_hash": mods_hash,
        "name": args.name, "player_uuid": player_uuid,
    }
    if args.password:
        hello["password"] = args.password
    conn.send(hello)
    sel = selectors.DefaultSelector()
    sel.register(conn.sock, selectors.EVENT_READ)
    while True:
        if not sel.select(args.timeout):
            raise ConnectionError("no answer from host")
        for msg in conn.read_available():
            if msg.get("t") == "welcome":
                return conn, msg
            if msg.get("t") == "reject":
                return None, msg


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=7777)
    ap.add_argument("--name", default="Probe")
    ap.add_argument("--password", default="")
    ap.add_argument("--game-version", default="")
    ap.add_argument("--mods-hash", default="")
    ap.add_argument("--uuid", default="",
                    help="stable player id; reuse it to get your "
                    "character back")
    ap.add_argument("--auto", action="store_true",
                    help="copy version and mod fingerprint from the "
                    "host's rejection")
    ap.add_argument("--timeout", type=float, default=5.0)
    args = ap.parse_args()

    player_uuid = args.uuid or str(uuid.uuid4())
    version, mods = args.game_version, args.mods_hash
    for _ in range(3):
        conn, msg = handshake(args, version, mods, player_uuid)
        if conn:
            break
        reason = msg.get("reason", "")
        print("rejected: " + reason)
        if not args.auto:
            return 1
        m = re.match(r"game version mismatch: host (.*), client", reason)
        if m:
            version = m.group(1)
            continue
        m = re.match(r"mod list mismatch: host ([0-9a-f]+)", reason)
        if m:
            mods = m.group(1)
            continue
        return 1
    else:
        return 1

    print("joined as player %d in world '%s' (pace %d ms, grace %d ms)" % (
        msg["player_id"], msg["world_name"], msg["pace_ms"],
        msg["tactical_grace_ms"]))
    print("uuid %s  (pass --uuid to rejoin as the same character)"
          % player_uuid)

    sel = selectors.DefaultSelector()
    sel.register(conn.sock, selectors.EVENT_READ, "net")
    sel.register(sys.stdin, selectors.EVENT_READ, "stdin")
    seq = 0
    last_tick = None
    last_self = None
    while True:
        for key, _ in sel.select():
            if key.data == "net":
                try:
                    messages = conn.read_available()
                except ConnectionError as e:
                    print(e)
                    return 0
                for m in messages:
                    if m.get("t") == "tick":
                        # Ticks arrive every turn; print only changes.
                        summary = (m["mode"], tuple(m["waiting_for"]))
                        if summary == last_tick:
                            continue
                        last_tick = summary
                    if m.get("t") == "self":
                        # Sent every turn; print only when it changes.
                        state = {k: v for k, v in m.items() if k != "turn"}
                        if state == last_self:
                            continue
                        last_self = state
                    text = describe(m)
                    if text:
                        print(text)
                continue
            line = sys.stdin.readline()
            if not line:
                return 0
            line = line.strip()
            cmd = None
            if line in MOVES:
                dx, dy = MOVES[line]
                cmd = {"action": "move", "dx": dx, "dy": dy, "dz": 0}
            elif line == ".":
                cmd = {"action": "wait"}
            elif line in ("r", "R", "t", "T"):
                cmd = {"action": TOGGLES[line]}
            elif line.startswith("say "):
                conn.send({"t": "chat", "from": 0, "text": line[4:]})
            elif line == "q":
                conn.send({"t": "bye", "reason": "quit"})
                return 0
            elif line:
                print("unknown key; see --help")
            if cmd:
                seq += 1
                cmd.update({"t": "cmd", "seq": seq, "turn": 0})
                conn.send(cmd)


if __name__ == "__main__":
    sys.exit(main())
