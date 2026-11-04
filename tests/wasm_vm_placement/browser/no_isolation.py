#!/usr/bin/env python3
"""Same wasm64 probe, but served WITHOUT COOP/COEP (no cross-origin isolation).
Answers: is memory64 itself gated on isolation, or only SharedArrayBuffer?"""
import asyncio, json, socket, subprocess, threading, time, urllib.request
import http.server, socketserver
import websockets
CHROME = "/root/.cache/ms-playwright/chromium-1248/chrome-linux64/chrome"
class H(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **kw): super().__init__(*a, directory=kw.pop("directory","."), **kw)
    def log_message(self,*a): pass
def free_port():
    s=socket.socket(); s.bind(("127.0.0.1",0)); p=s.getsockname()[1]; s.close(); return p
async def main():
    port=free_port()
    srv=socketserver.TCPServer(("127.0.0.1",port), lambda *a,**kw: H(*a,directory=".",**kw))
    threading.Thread(target=srv.serve_forever,daemon=True).start()
    d=free_port()
    proc=subprocess.Popen([CHROME,"--headless=new","--no-sandbox","--disable-dev-shm-usage",
        "--remote-debugging-port=%d"%d,"--user-data-dir=/tmp/cdp-noiso-%d"%d,
        "--no-first-run","--no-default-browser-check","about:blank"],
        stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    ws_url=None
    for _ in range(300):
        try:
            for t in json.load(urllib.request.urlopen("http://127.0.0.1:%d/json/list"%d,timeout=1)):
                if t.get("type")=="page": ws_url=t["webSocketDebuggerUrl"]
            if ws_url: break
        except Exception: pass
        time.sleep(0.1)
    assert ws_url
    async with websockets.connect(ws_url, max_size=2**26) as ws:
        mid=[0]
        async def send(m,p=None):
            mid[0]+=1; await ws.send(json.dumps({"id":mid[0],"method":m,"params":p or {}}))
        await send("Runtime.enable"); await send("Page.enable")
        await send("Page.navigate",{"url":"http://127.0.0.1:%d/probe_matrix.html"%port})
        t0=time.time(); done=False
        while time.time()-t0<90 and not done:
            try: raw=await asyncio.wait_for(ws.recv(),timeout=0.4)
            except asyncio.TimeoutError: continue
            m=json.loads(raw)
            if m.get("method")=="Runtime.consoleAPICalled":
                txt=" ".join(str(a.get("value",a.get("description",""))) for a in m["params"]["args"])
                print("[page]",txt); done=True
    proc.terminate()
    try: proc.wait(timeout=5)
    except Exception: proc.kill()
asyncio.run(main())