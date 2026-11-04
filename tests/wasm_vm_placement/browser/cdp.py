#!/usr/bin/env python3
"""Minimal Chrome DevTools Protocol driver: launch headless Chromium, serve files
with COOP/COEP, evaluate JS in a page, print console output."""
import argparse, asyncio, http.server, json, os, socket, socketserver, subprocess, sys, threading, time, urllib.request
import websockets

CHROME = "/root/.cache/ms-playwright/chromium-1248/chrome-linux64/chrome"

class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **kw):
        super().__init__(*a, directory=kw.pop("directory", "."), **kw)
    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cross-Origin-Resource-Policy", "cross-origin")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()
    def log_message(self, *a):
        pass

def serve(directory, port):
    srv = socketserver.TCPServer(("127.0.0.1", port), lambda *a, **kw: Handler(*a, directory=directory, **kw))
    srv.allow_reuse_address = True
    t = threading.Thread(target=srv.serve_forever, daemon=True)
    t.start()
    return srv

def free_port():
    s = socket.socket(); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close(); return p

async def run(url, extra_flags, timeout, page_html_js=None, delay=0.0):
    port = free_port()
    args = [CHROME, "--headless=new", "--no-sandbox", "--disable-gpu-sandbox",
            "--remote-debugging-port=%d" % port, "--user-data-dir=/tmp/cdp-profile-%d" % port,
            "--no-first-run", "--no-default-browser-check", "--disable-dev-shm-usage",
            "--enable-logging=stderr", "--v=0"] + extra_flags + ["about:blank"]
    proc = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    ws_url = None
    for _ in range(200):
        try:
            data = json.load(urllib.request.urlopen("http://127.0.0.1:%d/json/list" % port, timeout=1))
            for t in data:
                if t.get("type") == "page":
                    ws_url = t["webSocketDebuggerUrl"]
            if ws_url: break
        except Exception:
            pass
        time.sleep(0.1)
    if not ws_url:
        proc.kill()
        raise SystemExit("could not reach DevTools")

    logs = []
    msgid = [0]
    async with websockets.connect(ws_url, max_size=64*1024*1024) as ws:
        pending = {}
        async def send(method, params=None):
            msgid[0] += 1
            i = msgid[0]
            await ws.send(json.dumps({"id": i, "method": method, "params": params or {}}))
            return i
        async def reader():
            async for raw in ws:
                m = json.loads(raw)
                if "id" in m:
                    pending[m["id"]] = m
                elif m.get("method") in ("Runtime.consoleAPICalled", "Runtime.exceptionThrown", "Log.entryAdded"):
                    logs.append(m)
        task = asyncio.create_task(reader())
        for m in ("Runtime.enable", "Log.enable", "Page.enable"):
            await send(m)
        await send("Page.navigate", {"url": url})
        t0 = time.time()
        result = None
        while time.time() - t0 < timeout:
            await asyncio.sleep(0.25)
            if page_html_js:
                i = await send("Runtime.evaluate", {"expression": page_html_js, "returnByValue": True, "awaitPromise": True})
                for _ in range(80):
                    if i in pending: break
                    await asyncio.sleep(0.05)
                result = pending.get(i)
                if result: break
        task.cancel()
        await send("Browser.close") if False else None
    proc.terminate()
    try: proc.wait(timeout=5)
    except Exception: proc.kill()
    return result, logs

def fmt_logs(logs):
    out = []
    for m in logs:
        meth = m.get("method")
        p = m.get("params", {})
        if meth == "Runtime.consoleAPICalled":
            args = []
            for a in p.get("args", []):
                if "value" in a: args.append(str(a["value"]))
                elif "description" in a: args.append(a["description"])
                else: args.append(a.get("type"))
            out.append("[console.%s] %s" % (p.get("type"), " ".join(args)))
        elif meth == "Runtime.exceptionThrown":
            d = p.get("exceptionDetails", {})
            out.append("[exception] %s" % (d.get("text") or d.get("exception", {}).get("description")))
        elif meth == "Log.entryAdded":
            e = p.get("entry", {})
            out.append("[log.%s] %s" % (e.get("level"), e.get("text")))
    return out

if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", required=True)
    ap.add_argument("--dir", default=".")
    ap.add_argument("--flags", default="")
    ap.add_argument("--timeout", type=float, default=60)
    ap.add_argument("--eval", default=None)
    a = ap.parse_args()
    port = free_port()
    serve(a.dir, port)
    url = a.url.replace("{port}", str(port))
    result, logs = asyncio.run(run(url, [f for f in a.flags.split() if f], a.timeout, a.eval))
    for l in fmt_logs(logs): print(l)
    if result:
        r = result.get("result", {}).get("result", {})
        print("EVAL RESULT:", json.dumps(r)[:4000])
        if result.get("result", {}).get("exceptionDetails"):
            print("EVAL EXCEPTION:", json.dumps(result["result"]["exceptionDetails"])[:2000])
