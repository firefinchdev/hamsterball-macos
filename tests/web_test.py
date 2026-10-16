#!/usr/bin/env python3
"""Run the browser build in headless Chrome and capture its console and screenshots.

Usage: build/venv/bin/python tests/web_test.py [--dir dist/web] [--script FILE] [--env VAR=value ...]
                                                 [--shot SECONDS:PATH ...] [--seconds N]
Needs Google Chrome and the websocket-client package (installed in build/venv).
"""
import argparse
import base64
import functools
import http.server
import json
import os
import subprocess
import tempfile
import threading
import time
import urllib.parse
import urllib.request

import websocket

CHROME = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"


def serve(directory, port):
    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=directory)
    handler.log_message = lambda self, *a: None
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", port), handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default="dist/web")
    ap.add_argument("--script")
    ap.add_argument("--env", action="append", default=[])
    ap.add_argument("--shot", action="append", default=[])
    ap.add_argument("--seconds", type=float, default=30)
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--log", default="tests/out/web.log")
    ap.add_argument("--profile", help="START:STOP seconds: CPU-profile that window, print the top functions")
    ap.add_argument("--user-data", help="Chrome profile directory to reuse (saves persist between runs)")
    ap.add_argument("--gpu", action="store_true", help="render on the GPU (Metal) instead of SwiftShader")
    a = ap.parse_args()

    srv = serve(os.path.abspath(a.dir), a.port)
    q = [("autoplay", "1")] + [("hbenv", e) for e in a.env]
    if a.script:
        q.append(("hbscript", open(a.script).read()))
    url = "http://127.0.0.1:%d/index.html?%s" % (a.port, urllib.parse.urlencode(q))
    profile = a.user_data or tempfile.mkdtemp(prefix="hbchrome")
    chrome = subprocess.Popen([CHROME, "--headless=new", "--remote-debugging-port=9333", "--remote-allow-origins=http://127.0.0.1:9333", "--user-data-dir=" + profile,
                               "--window-size=1280,1000"] + (["--use-angle=metal", "--enable-gpu", "--ignore-gpu-blocklist"] if a.gpu else
                              ["--use-angle=swiftshader", "--enable-unsafe-swiftshader"]) + [
                               "--autoplay-policy=no-user-gesture-required", "--no-first-run", "about:blank"],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(100):
            try:
                tabs = json.load(urllib.request.urlopen("http://127.0.0.1:9333/json"))
                break
            except Exception:
                time.sleep(0.1)
        page = [t for t in tabs if t["type"] == "page"][0]
        ws = websocket.create_connection(page["webSocketDebuggerUrl"], timeout=1, origin="http://127.0.0.1:9333")
        msg_id = [0]

        def send(method, **params):
            msg_id[0] += 1
            ws.send(json.dumps({"id": msg_id[0], "method": method, "params": params}))
            return msg_id[0]

        os.makedirs(os.path.dirname(a.log), exist_ok=True)
        log = open(a.log, "w")
        send("Runtime.enable")
        send("Log.enable")
        send("Page.enable")
        send("Page.navigate", url=url)
        shots = sorted((float(s.split(":", 1)[0]), s.split(":", 1)[1]) for s in a.shot)
        pending = {}
        prof = [float(x) for x in a.profile.split(":")] if a.profile else None
        prof_state, prof_id = 0, None
        t0 = time.time()
        while time.time() - t0 < a.seconds:
            if prof and prof_state == 0 and time.time() - t0 >= prof[0]:
                send("Profiler.enable"); send("Profiler.setSamplingInterval", interval=200); send("Profiler.start"); prof_state = 1
            if prof and prof_state == 1 and time.time() - t0 >= prof[1]:
                prof_id = send("Profiler.stop"); prof_state = 2
            while shots and time.time() - t0 >= shots[0][0]:
                pending[send("Page.captureScreenshot", format="png")] = shots.pop(0)[1]
            try:
                m = json.loads(ws.recv())
            except websocket.WebSocketTimeoutException:
                continue
            if m.get("method") == "Runtime.consoleAPICalled":
                text = " ".join(str(x.get("value", x.get("description", ""))) for x in m["params"]["args"])
                log.write(text + "\n")
                log.flush()
            elif m.get("method") == "Log.entryAdded":
                e = m["params"]["entry"]
                log.write("[%s] %s\n" % (e.get("source"), e.get("text")))
                log.flush()
            elif m.get("method") == "Runtime.exceptionThrown":
                d = m["params"]["exceptionDetails"]
                log.write("EXCEPTION: %s\n" % d.get("exception", {}).get("description", d.get("text")))
                log.flush()
            elif prof_id is not None and m.get("id") == prof_id:
                nodes = m["result"]["profile"]["nodes"]
                samples = m["result"]["profile"].get("samples", [])
                cnt = {}
                for sid in samples:
                    cnt[sid] = cnt.get(sid, 0) + 1
                by_name = {}
                for n in nodes:
                    name = n["callFrame"]["functionName"] or "(anon %s)" % n["callFrame"]["url"][-20:]
                    by_name[name] = by_name.get(name, 0) + cnt.get(n["id"], 0)
                total = max(1, len(samples))
                top = sorted(by_name.items(), key=lambda x: -x[1])[:40]
                log.write("[profile] %d samples\n" % total)
                for name, c in top:
                    log.write("[profile] %5.1f%%  %s\n" % (100.0 * c / total, name[:110]))
            elif m.get("id") in pending:
                path = pending.pop(m["id"])
                open(path, "wb").write(base64.b64decode(m["result"]["data"]))
                log.write("[test] screenshot %s\n" % path)
        log.close()
    finally:
        chrome.kill()
        srv.shutdown()
    print(open(a.log).read()[-3000:])


if __name__ == "__main__":
    main()
