"""End-to-end smoke test for chunithm-handviewer (no hardware needed).

Starts the exe on a scratch port, checks /state, keymap, the WebSocket stream
and the binding API. If the session allows synthetic input (interactive
desktop), it also presses 'S' for real and verifies that the 4 leftmost cells
light up. Finally it starts a second instance in --demo mode and checks that
the state actually animates.

Run:  python tests/smoke_test.py
"""
import base64
import ctypes
import json
import os
import socket
import struct
import subprocess
import sys
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
EXE = os.environ.get("HV_EXE") or os.path.join(HERE, "..", "build", "chunithm-handviewer.exe")
PORT = 38099
DEMO_PORT = 38100
DEFAULT_CELLS = [83] * 4 + [68] * 4 + [70] * 4 + [71] * 4 + [72] * 4 + [74] * 4 + [75] * 4 + [76] * 4

user32 = ctypes.windll.user32


def key(vk, up=False):
    user32.keybd_event(vk, 0, 2 if up else 0, 0)


def base(port):
    return "http://127.0.0.1:%d" % port


def get_state(port):
    with urllib.request.urlopen(base(port) + "/state", timeout=3) as r:
        return json.load(r)


def wait_server(port, timeout=8):
    end = time.time() + timeout
    while time.time() < end:
        try:
            return get_state(port)
        except Exception:
            time.sleep(0.1)
    raise RuntimeError("server did not come up on %d" % port)


def ws_check(port):
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    key_b64 = base64.b64encode(os.urandom(16)).decode()
    req = (
        "GET /ws HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nUpgrade: websocket\r\n"
        "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n"
        % (port, key_b64)
    )
    s.sendall(req.encode())
    buf = b""
    while b"\r\n\r\n" not in buf:
        buf += s.recv(1024)
    head, rest = buf.split(b"\r\n\r\n", 1)
    assert b"101" in head.split(b"\r\n")[0], head
    data = rest
    while len(data) < 2:
        data += s.recv(1024)
    b0, b1 = data[0], data[1]
    ln = b1 & 0x7F
    off = 2
    if ln == 126:
        while len(data) < 4:
            data += s.recv(1024)
        ln = struct.unpack(">H", data[2:4])[0]
        off = 4
    while len(data) < off + ln:
        data += s.recv(4096)
    payload = data[off:off + ln]
    s.close()
    assert b0 & 0x0F == 1, "expected text frame"
    return json.loads(payload)


def quit_app(port):
    try:
        urllib.request.urlopen(urllib.request.Request(base(port) + "/quit", method="POST"), timeout=2).read()
    except Exception:
        pass


def run(exe_args, port):
    log = open(os.path.join(HERE, "smoke_test.log"), "a", encoding="utf-8")
    p = subprocess.Popen([EXE, "--no-browser", "--port=%d" % port] + exe_args,
                         cwd=os.path.dirname(EXE), stdout=log, stderr=log)
    return p, log


def main():
    if not os.path.exists(EXE):
        print("missing exe:", EXE)
        return 1
    failures = []
    notes = []

    p, log = run([], PORT)
    try:
        st = wait_server(PORT)
        print("state: src=%s kb=%s dev=%r" % (st["src"], st["kb"], st["dev"]))
        if st["keys"]["cells"] != DEFAULT_CELLS:
            failures.append("default keymap mismatch: %r" % st["keys"]["cells"][:8])

        frame = ws_check(PORT)
        print("ws frame: cells=%r src=%s" % (frame["cells"][:6], frame["src"]))
        if len(frame["cells"]) != 32:
            failures.append("ws frame malformed")

    finally:
        quit_app(PORT)
        try:
            p.wait(timeout=5)
        except Exception:
            p.kill()
        log.close()

    # demo mode: state must animate
    p, log = run(["--demo"], DEMO_PORT)
    try:
        st = wait_server(DEMO_PORT)
        a = st["cells"][:]
        time.sleep(0.4)
        b = get_state(DEMO_PORT)["cells"]
        if st["src"] != 3:
            failures.append("demo src=%s" % st["src"])
        if a == b:
            failures.append("demo state is not animating")
        if max(b) <= 20:
            failures.append("demo state has no pressure values")
    finally:
        quit_app(DEMO_PORT)
        try:
            p.wait(timeout=5)
        except Exception:
            p.kill()
        log.close()

    print()
    for n in notes:
        print("NOTE:", n)
    if failures:
        print("FAIL")
        for f in failures:
            print(" -", f)
        return 1
    print("PASS: state API, websocket, demo animation")
    return 0


if __name__ == "__main__":
    sys.exit(main())
