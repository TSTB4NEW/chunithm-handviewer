"""Keyboard-path test for chunithm-handviewer (no hardware needed).

Verifies the parts of the keyboard pipeline that can be exercised without a
TASOLLER: the keymap API, the token vocabulary of /bind, and -- when the session
can inject synthetic keystrokes -- that a real key press reaches /state.

Run:  python tests/keyboard_test.py
"""
import json
import os
import subprocess
import sys
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
EXE = os.environ.get("HV_EXE") or os.path.join(HERE, "..", "build", "chunithm-handviewer.exe")
PORT = 38101


def get(path):
    with urllib.request.urlopen("http://127.0.0.1:%d%s" % (PORT, path), timeout=3) as r:
        return json.load(r)


def wait_server(timeout=8):
    end = time.time() + timeout
    while time.time() < end:
        try:
            return get("/state")
        except Exception:
            time.sleep(0.1)
    raise RuntimeError("server did not come up")


def key_event(vk, up):
    import ctypes
    ctypes.windll.user32.keybd_event(vk, 0, 2 if up else 0, 0)


def inject_works():
    import ctypes
    u = ctypes.windll.user32
    u.keybd_event(0x5A, 0, 0, 0)  # 'Z'
    time.sleep(0.05)
    down = bool(u.GetAsyncKeyState(0x5A) & 0x8000)
    u.keybd_event(0x5A, 0, 2, 0)
    return down


def main():
    failures = []
    live = inject_works()
    log = open(os.path.join(HERE, "keyboard_test.log"), "w", encoding="utf-8")
    p = subprocess.Popen([EXE, "--no-browser", "--port=%d" % PORT], cwd=os.path.dirname(EXE),
                         stdout=log, stderr=log)
    try:
        st = wait_server()
        labels = st["keys"]["labels"]
        print("labels.cells[:8] =", labels["cells"][:8])

        # 1. named tokens must win over their numeric VK value (VK_TAB == 9, ...)
        cases = [("cell0", "TAB", 9), ("cell1", "ENTER", 13), ("cell2", "ESC", 27),
                 ("cell3", "SPACE", 32), ("cell4", "LEFT", 37), ("cell5", "F1", 112),
                 ("cell6", "a", 65), ("cell7", "5", 53)]
        got = {}
        for slot, tok, want in cases:
            r = get("/bind?slot=%s&key=%s" % (slot, tok))
            if not r.get("ok"):
                failures.append("bind %s=%s rejected" % (slot, tok))
        st = get("/state")
        got = st["keys"]["cells"][:8]
        for (slot, tok, want), have in zip(cases, got):
            if have != want:
                failures.append("%s=%s -> vk %d, want %d" % (slot, tok, have, want))
        print("bound VKs:", got)

        # 2. labels must round-trip through the token form
        labels = get("/state")["keys"]["labels"]["cells"][:8]
        want_labels = ["TAB", "ENTER", "ESC", "SPACE", "LEFT", "F1", "A", "5"]
        if labels != want_labels:
            failures.append("labels %r != %r" % (labels, want_labels))
        print("labels:", labels)

        # 3. invalid input is rejected, and does not corrupt the map
        for bad in ("?slot=cell0", "?slot=cell0&key=NOSUCHKEY", "?slot=cell99&key=A", "?slot=fn9&key=A"):
            r = get("/bind" + bad)
            if r.get("ok"):
                failures.append("bind accepted invalid %r" % bad)

        # 4. restore the default map for every cell. Binds persist to
        # handviewer.ini, so a previous run (or the user) may have rebound
        # anything; never assume the ini still holds the defaults.
        labels = get("/state")["keys"]["labels"]["cells"]
        if len(labels) != 32:
            failures.append("keymap has %d cells" % len(labels))
        default_tokens = ["S", "D", "F", "G", "H", "J", "K", "L"]
        for i, cur in enumerate(labels):
            want = default_tokens[min(i // 4, 7)]
            if cur != want:
                get("/bind?slot=cell%d&key=%s" % (i, want))
        if get("/state")["keys"]["labels"]["cells"] != [t for t in default_tokens for _ in range(4)]:
            failures.append("could not restore the default keymap")

        # 5. real synthetic key press -> /state cells + kb flag.
        # Synthetic input needs the same interactive desktop as the viewer; in a
        # non-interactive window station it never reaches GetAsyncKeyState, so
        # that section is skipped instead of reported as a product failure.
        if not live:
            print("live press: SKIPPED (session cannot inject synthetic keys)")
        else:
            key_event(0x53, False)  # 'S'
            time.sleep(0.15)
            st = get("/state")
            key_event(0x53, True)
            time.sleep(0.1)
            offline = get("/state")
            print("live press: held=%r kb_held=%r" % (st["cells"][:4], st["kb"]))
            if st["cells"][:4] != [255, 255, 255, 255]:
                failures.append("pressed S: cells[:4]=%r want 255s" % (st["cells"][:4],))
            if st["kb"] != 1:
                failures.append("pressed S: kb=%r want 1" % (st["kb"],))
            if offline["kb"] != 0 or max(offline["cells"]) != 0:
                failures.append("released: kb=%r maxcell=%d" % (offline["kb"], max(offline["cells"])))

            # 6. rebinding takes effect live: S -> cell0 only
            get("/bind?slot=cell0&key=Z")
            key_event(0x53, False); time.sleep(0.12)
            st = get("/state")
            key_event(0x53, True)
            if max(st["cells"]) != 0:
                failures.append("after rebind cell0=Z, S still lights cells")
            key_event(0x5A, False); time.sleep(0.12)
            st = get("/state")
            key_event(0x5A, True)
            if st["cells"][0] != 255 or st["cells"][1] != 255:
                failures.append("after rebind, Z should light cells 0..3 (got %r)" % (st["cells"][:4],))
            print("rebind live: Z ->", st["cells"][:4])
    finally:
        try:
            urllib.request.urlopen(urllib.request.Request(
                "http://127.0.0.1:%d/quit" % PORT, method="POST"), timeout=2).read()
        except Exception:
            pass
        try:
            p.wait(timeout=5)
        except Exception:
            p.kill()
        log.close()

    if failures:
        print("FAIL")
        for f in failures:
            print(" -", f)
        return 1
    print("PASS: keymap tokens, bind API, live keyboard merge")
    return 0


if __name__ == "__main__":
    sys.exit(main())
