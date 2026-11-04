#!/usr/bin/env python3
"""Measure what a wasm64 16 GiB shared memory costs in a real browser,
using the kernel's own accounting of the whole Chromium process tree."""
import asyncio, json, os, socket, subprocess, threading, time, urllib.request
import http.server, socketserver
import websockets

CHROME = "/root/.cache/ms-playwright/chromium-1248/chrome-linux64/chrome"

class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **kw):
        super().__init__(*a, directory=kw.pop("directory", "."), **kw)
    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()
    def log_message(self, *a): pass

def free_port():
    s = socket.socket(); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close(); return p

def tree_mem(root):
    pids, kids, rss, vsz = set(), {}, 0, 0
    for d in os.listdir("/proc"):
        if not d.isdigit(): continue
        try:
            st = open("/proc/%s/status" % d).read()
        except Exception: continue
        if "PPid:" in st:
            kids.setdefault(int(st.split("PPid:")[1].split("\n")[0].strip()), []).append(int(d))
    stack = [root]
    while stack:
        p = stack.pop()
        if p in pids: continue
        pids.add(p)
        stack.extend(kids.get(p, []))
    for p in pids:
        try:
            for line in open("/proc/%d/status" % p):
                if line.startswith("VmRSS:"): rss += int(line.split()[1])
                elif line.startswith("VmSize:"): vsz += int(line.split()[1])
        except Exception: pass
    return len(pids), rss, vsz

async def main():
    port = free_port()
    srv = socketserver.TCPServer(("127.0.0.1", port), lambda *a, **kw: Handler(*a, directory=".", **kw))
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    dport = free_port()
    args = [CHROME, "--headless=new", "--no-sandbox", "--disable-dev-shm-usage",
            "--remote-debugging-port=%d" % dport,
            "--user-data-dir=/tmp/cdp-cost-%d" % dport,
            "--no-first-run", "--no-default-browser-check", "about:blank"]
    proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    ws_url = None
    for _ in range(300):
        try:
            data = json.load(urllib.request.urlopen("http://127.0.0.1:%d/json/list" % dport, timeout=1))
            for t in data:
                if t.get("type") == "page": ws_url = t["webSocketDebuggerUrl"]
            if ws_url: break
        except Exception: pass
        time.sleep(0.1)
    assert ws_url, "no devtools"
    async with websockets.connect(ws_url, max_size=2**26) as ws:
        mid = [0]
        async def send(method, params=None):
            mid[0] += 1
            await ws.send(json.dumps({"id": mid[0], "method": method, "params": params or {}}))
        await send("Runtime.enable")
        await send("Page.enable")
        await send("Page.navigate", {"url": "http://127.0.0.1:%d/probe_cost2.html" % port})
        t0 = time.time(); result = None
        while time.time() - t0 < 240:
            try:
                raw = await asyncio.wait_for(ws.recv(), timeout=0.3)
            except asyncio.TimeoutError:
                continue
            m = json.loads(raw)
            if m.get("method") == "Runtime.consoleAPICalled":
                txt = " ".join(str(a.get("value", a.get("description", ""))) for a in m["params"]["args"])
                print("[page]", txt)
                if txt.startswith("CKPT "):
                    n, rss, vsz = tree_mem(proc.pid)
                    print("[proc] %-30s procs=%d VmRSS=%.1f MiB VmSize=%.1f MiB" % (txt[5:], n, rss/1024.0, vsz/1024.0))
                    await send("Runtime.evaluate", {"expression": "window.__go=1"})
            if m.get("id") and m.get("result", {}).get("result", {}).get("value"):
                result = m["result"]["result"]["value"]
        print("\n=== page result ===\n" + str(result))
    proc.terminate()
    try: proc.wait(timeout=5)
    except Exception: proc.kill()

asyncio.run(main())