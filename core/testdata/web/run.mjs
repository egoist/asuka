// Runs an app's bytecode on the core as WebAssembly under Node, as the
// web host does (platform/web/src/core.js), with Node's worker threads
// as spawn's Web Workers: web_test.go's. It prints what the app prints,
// and ends once the app prints "done" (or after a while).
//
// With asqlite.wasm (std/sqlite's module), the page's SQLite worker runs
// on a worker thread too.
//
// A native package's JavaScript (native.mjs's: a module that registers
// providers) is imported before the core starts, as the page imports
// packages' web/ code.
//
// With "-" for the module, the app is the one compiled into asuka.wasm
// (a release's, its C: asuka_run starts it).
//
//	node run.mjs <platform/web/src> <asuka.wasm> <app.akb | -> [asqlite.wasm | native.mjs]
import { readFile } from "node:fs/promises";
import { pathToFileURL, fileURLToPath } from "node:url";
import { Worker } from "node:worker_threads";

const [src, wasm, akb, extra] = process.argv.slice(2);
const asqlite = extra?.endsWith(".wasm") ? extra : null;
const { startCore } = await import(pathToFileURL(src + "/core.js"));
const native = await import(pathToFileURL(src + "/native.js"));
globalThis.asuka = { native };
if (extra?.endsWith(".mjs")) await import(pathToFileURL(extra));
const fs = await import(pathToFileURL(src + "/fs.js"));

// fetch of files: the core and the module.
globalThis.fetch = async (u) => new Response(await readFile(fileURLToPath(u)), { headers: { "content-type": "application/wasm" } });

// Node's worker threads, as the pool wants Web Workers.
const workers = [];
let jobs = 0;
function makeWorker(url) {
  const w = new Worker(new URL("./worker.mjs", import.meta.url), { workerData: { src: url.href, serve: url.href.endsWith("sqlite-worker.js") ? "sqlite" : "spawn" } });
  workers.push(w);
  const port = {
    postMessage: (m, transfer) => {
      if (m.job) jobs++;
      w.postMessage(m, transfer);
    },
    terminate: () => w.terminate(),
    onmessage: null,
    onerror: null,
  };
  w.on("message", (data) => port.onmessage?.({ data }));
  w.on("error", (e) => port.onerror?.(e));
  return port;
}

// The host: nothing drawn; calls into the core wait while it runs, as host.js's.
const host = {
  inGo: 0,
  pending: [],
  attach() {},
  connect(core) { this.core = core; },
  apply() {},
  measure() {},
  post() { setTimeout(() => this.call(() => this.core.runPosted()), 0); },
  call(f) {
    if (this.inGo) {
      this.pending.push(f);
      return;
    }
    this.inGo++;
    try { return f(); } finally { this.inGo--; this.drain(); }
  },
  drain() { while (!this.inGo && this.pending.length) this.call(this.pending.shift()); },
  started() { this.inGo = 0; this.drain(); },
  // Typed native modules, as host.js's.
  get nativeBack() {
    return (this._back ??= {
      done: (call, b) => this.call(() => this.core.nativeDone(call, b)),
      invoke: (id, b) => {
        this.inGo++;
        try { return this.core.nativeInvoke(id, b); } finally { this.inGo--; this.drain(); }
      },
    });
  },
  nativeNew(id, iface) { return native.makeObject(id, iface, this.nativeBack); },
  nativeCall(h, method, input) { return native.call(h, method, input, this.nativeBack); },
  nativeStart(h, method, input, call) { native.start(h, method, input, call, this.nativeBack); },
  nativeCancel(call) { native.cancel(call); },
  nativeRelease(h) { native.release(h); },
};

const end = (code) => {
  for (const w of workers) w.terminate();
  process.exit(code);
};
const timeout = setTimeout(() => { console.log("(timed out)"); end(1); }, 60000);
const log = (fd, line) => {
  if (fd === 2) {
    console.log("stderr: " + line);
    return;
  }
  if (line === "done") console.log(`(${jobs} jobs on workers)`);
  console.log(line);
  if (line === "done") {
    clearTimeout(timeout);
    end(0);
  }
};
const dir = "/data/spawntest";
await fs.mount({ name: "spawntest", dir });
let sqlite = null;
if (asqlite) {
  const { Client } = await import(pathToFileURL(src + "/sqlite.js"));
  sqlite = async () => new Client(pathToFileURL(asqlite).href, makeWorker);
}
// The run's crash record (crash.js), the page's side.
const { Crash } = await import(pathToFileURL(src + "/crash.js"));
const crash = new Crash({ afterCore: (f) => f() });
await startCore(host, { wasm: pathToFileURL(wasm), module: akb === "-" ? undefined : pathToFileURL(akb), appDir: dir, log, makeWorker, sqlite, crash });
