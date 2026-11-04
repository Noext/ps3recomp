#!/usr/bin/env node
/*
 * ps3recomp - headless-Chromium driver for the canvas blit probe (Phase B, B2)
 *
 * Serves the probe directory, opens probe.html in the bundled Chromium over the
 * DevTools protocol, waits for window.__RESULT__, and writes a screenshot of the
 * canvas element. No playwright/puppeteer install: node 24's global WebSocket
 * and fetch are enough for the four CDP calls this needs, which keeps the check
 * runnable on a runner that has Chromium and nothing else.
 *
 * Usage:
 *   node tools/run_canvas_blit_probe.js <probe-dir> <screenshot.png> [chrome]
 *
 * Exit code 0 when the page reported a result and the screenshot was written.
 */
'use strict';

const [,, probeDir, shotPath, chromeArg] = process.argv;
if (!probeDir || !shotPath) {
  console.error('usage: run_canvas_blit_probe.js <probe-dir> <screenshot.png> [chrome]');
  process.exit(2);
}

const CHROME = chromeArg ||
  '/root/.cache/ms-playwright/chromium-1248/chrome-linux64/chrome';

const http = require('http');
const fs = require('fs');
const path = require('path');
const { spawn } = require('child_process');

const MIME = {
  '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm',
  '.png': 'image/png',
};

/* A dead-simple static server: the page needs same-origin fetch of the .wasm,
 * and file:// blocks that in Chromium. */
function serve(root) {
  return new Promise((resolve) => {
    const server = http.createServer((req, res) => {
      const rel = decodeURIComponent(req.url.split('?')[0]).replace(/^\/+/, '') || 'index.html';
      const file = path.join(root, rel);
      if (!file.startsWith(root)) { res.writeHead(403).end(); return; }
      fs.readFile(file, (err, buf) => {
        if (err) { res.writeHead(404).end('not found'); return; }
        res.writeHead(200, { 'Content-Type': MIME[path.extname(file)] || 'application/octet-stream' });
        res.end(buf);
      });
    });
    server.listen(0, '127.0.0.1', () => resolve(server));
  });
}

/* Minimal CDP over the WebSocket node 24 ships. Each send is one request;
 * responses are matched by id and events go to listeners. */
function connect(wsUrl) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(wsUrl);
    const pending = new Map();
    const listeners = [];
    let nextId = 1;

    ws.onopen = () => resolve({
      send(method, params, sessionId) {
        const id = nextId++;
        const msg = { id, method, params: params || {} };
        /* Flattened CDP: the session id is a top-level field on the command,
         * not a param -- putting it in params gets "X wasn't found" back. */
        if (sessionId) msg.sessionId = sessionId;
        return new Promise((res, rej) => {
          pending.set(id, { res, rej });
          ws.send(JSON.stringify(msg));
        });
      },
      on(fn) { listeners.push(fn); },
      close() { ws.close(); },
    });
    ws.onerror = (e) => reject(new Error('ws error: ' + (e.message || 'unknown')));
    ws.onmessage = (ev) => {
      const msg = JSON.parse(ev.data);
      if (msg.id && pending.has(msg.id)) {
        const { res, rej } = pending.get(msg.id);
        pending.delete(msg.id);
        if (msg.error) rej(new Error(msg.error.message));
        else res(msg.result);
      } else if (msg.method) {
        for (const fn of listeners) fn(msg);
      }
    };
  });
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

(async () => {
  const server = await serve(path.resolve(probeDir));
  const port = server.address().port;
  const url = `http://127.0.0.1:${port}/probe.html`;

  const userDir = fs.mkdtempSync(path.join(require('os').tmpdir(), 'rsx-blit-'));
  const proc = spawn(CHROME, [
    '--headless=new',
    '--remote-debugging-port=0',
    `--user-data-dir=${userDir}`,
    '--no-sandbox', '--disable-gpu', '--disable-dev-shm-usage',
    '--hide-scrollbars',
    '--window-size=1000,700',
    'about:blank',
  ], { stdio: ['ignore', 'ignore', 'pipe'] });

  /* Chromium writes "DevTools listening on ws://..." to stderr. */
  const wsBrowser = await new Promise((resolve, reject) => {
    let buf = '';
    const timer = setTimeout(() => reject(new Error('no devtools endpoint: ' + buf)), 20000);
    proc.stderr.on('data', (d) => {
      buf += d.toString();
      const m = buf.match(/ws:\/\/[^\s]+/);
      if (m) { clearTimeout(timer); resolve(m[0]); }
    });
    proc.on('exit', (c) => { clearTimeout(timer); reject(new Error('chrome exited ' + c)); });
  });

  const browser = await connect(wsBrowser);
  const { targetId } = await browser.send('Target.createTarget', { url: 'about:blank' });
  const { sessionId } = await browser.send('Target.attachToTarget', { targetId, flatten: true });

  /* Flattened sessions multiplex over the same socket; wrap send with the id. */
  const sessionSend = (method, params) => browser.send(method, params, sessionId);

  await sessionSend('Page.enable', {});
  await sessionSend('Runtime.enable', {});
  await sessionSend('Page.navigate', { url });

  /* Wait for the page to publish a result (or fail). */
  let result = null;
  const deadline = Date.now() + 60000;
  while (Date.now() < deadline) {
    const r = await sessionSend('Runtime.evaluate', {
      expression: 'window.__RESULT__ ? JSON.stringify(window.__RESULT__) : ""',
      returnByValue: true,
    });
    const v = r.result && r.result.value;
    if (v) { result = JSON.parse(v); break; }
    const err = await sessionSend('Runtime.evaluate', {
      expression: 'window.__ERROR__ || ""', returnByValue: true,
    });
    if (err.result && err.result.value) throw new Error('page error: ' + err.result.value);
    await sleep(200);
  }
  if (!result) throw new Error('timed out waiting for window.__RESULT__');

  /* Screenshot the canvas element itself, not the viewport, so the pixels in
   * the PNG are exactly what the blit produced (minus the CSS scale). */
  const shot = await sessionSend('Page.captureScreenshot', {
    format: 'png',
    clip: await (async () => {
      const box = await sessionSend('Runtime.evaluate', {
        expression: `(() => { const c = document.getElementById('screen');
            const r = c.getBoundingClientRect();
            return JSON.stringify({x: r.x, y: r.y, width: r.width, height: r.height});
        })()`,
        returnByValue: true,
      });
      return Object.assign(JSON.parse(box.result.value), { scale: 1 });
    })(),
    captureBeyondViewport: true,
  });

  fs.mkdirSync(path.dirname(path.resolve(shotPath)), { recursive: true });
  fs.writeFileSync(path.resolve(shotPath), Buffer.from(shot.data, 'base64'));

  /* The element screenshot above goes through the compositor and therefore the
   * CSS scale (960x540 for a 1280x720 backing store). canvas.toDataURL reads
   * the backing store itself, so this second file is exactly the pixels the
   * blit wrote -- the one to trust when checking a channel order pixel by
   * pixel. */
  const native = await sessionSend('Runtime.evaluate', {
    expression: `document.getElementById('screen').toDataURL('image/png')`,
    returnByValue: true,
  });
  const nativePath = path.resolve(shotPath).replace(/\.png$/i, '-native.png');
  fs.writeFileSync(nativePath,
    Buffer.from(String(native.result.value).split(',')[1], 'base64'));

  console.log(JSON.stringify(result, null, 2));
  console.log('screenshot (compositor, CSS-scaled): ' + path.resolve(shotPath));
  console.log('screenshot (canvas backing store, native 1280x720): ' + nativePath);

  browser.close();
  proc.kill('SIGKILL');
  server.close();
  process.exit(0);
})().catch((e) => {
  console.error('PROBE FAILED:', e.message);
  process.exit(1);
});
