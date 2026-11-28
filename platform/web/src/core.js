// The C core as WebAssembly (asuka.wasm: the core, its VM, and an app
// compiled in or loaded as bytecode), driven by the web host (host.js):
// batches and measurements through a shared buffer, events and the
// lists' calls into the core's C ABI (asuka.h). The C library's system
// calls are WASI's, answered by wasi.js (the app's files: fs.js); spawn
// runs on the page's workers (spawn.js), each with the same core.
//
//	const core = await startCore(host, { wasm: "asuka.wasm", module: "app.akb", dev: true, appDir: "/data/app" });

import { wasi, logLine, logLevels } from "./wasi.js";
import { Pool } from "./spawn.js";
import { Workers } from "./workers.js";
import { stackRoom } from "./stack.js";
import { zoneName } from "./zone.js";

/**
 * startCore loads the core into the page and connects it to host. Options: wasm (the
 * core's URL), module (bytecode to run: asuka dev's app), dev (asuka dev's page: the module
 * loads as its reloads do, and core.reload loads the next one keeping the app's state and
 * views), appDir (the app's data directory, std/fs's appDir), workers (false: spawn runs
 * in place, on the page's thread), sqlite (a function resolving to the page's SQLite
 * client, sqlite.js's), crash (crash.js's Crash: the run's record), makeWorker (makes a worker of a URL: Node's tests'),
 * log (where the core's output goes).
 */
export async function startCore(host, options = {}) {
  let instance, pool = null, workers = null;
  // std/sqlite: the page's SQLite worker (sqlite.js), when the app imports std/sqlite.
  let sqlite = null;
  if (options.sqlite) {
    try {
      sqlite = await options.sqlite();
      sqlite.root(options.appDir);
    } catch (err) {
      console.error("std/sqlite: SQLite didn't load:", err);
    }
  }
  const memory = () => instance.exports.memory;
  // A native call's answer, until the core copies it (native_answer).
  let nativeAnswer = null;
  const answerLength = (b) => {
    nativeAnswer = b instanceof Uint8Array ? b : new Uint8Array(0);
    return nativeAnswer.length;
  };
  const timers = new Map();
  const log = options.log || ((fd, line) => (fd === 2 ? console.error : console.log)(line));
  const imports = {
    asuka: {
      apply: () => host.apply(),
      measure: () => host.measure(),
      post: () => host.post(),
      timer_set: (id, ms) => {
        timers.set(id, setTimeout(() => {
          timers.delete(id);
          host.call(() => { e.asuka_timer_fire(id); e.asuka_run_posted(); });
        }, ms));
      },
      timer_clear: (id) => { clearTimeout(timers.get(id)); timers.delete(id); },
      // The zone's offset from UTC at a time (Unix seconds), in seconds east:
      // dates, and std/time's local zone.
      zone_offset: (s) => -new Date(s * 1000).getTimezoneOffset() * 60,
      // The zone's IANA name (std/time's Zone.local.name()), written at p:
      // its length; 0 when there's none or it doesn't fit cap bytes.
      zone_name: (p, cap) => zoneName(memory(), p, cap),
      // Whether the engine's stack has room for the core to go deeper (stack.js).
      stack_room: stackRoom(),
      // spawn's jobs (core/src/abi/web.c): its bytes copied out, for a worker.
      spawn: (id, p, n) => pool.run(id, new Uint8Array(memory().buffer, p, n).slice()),
      spawn_cancel: (id) => pool?.cancel(id),
      // Workers (workers.js), a Web Worker each: their init's job, their calls, their end.
      worker_start: (id, p, n) => workers.start(id, new Uint8Array(memory().buffer, p, n).slice()),
      worker_call: (id, job, p, n) => workers.call(id, job, new Uint8Array(memory().buffer, p, n).slice()),
      worker_stop: (id, cancel) => workers?.stop(id, cancel !== 0),
      // Typed native modules (native.js, through the host): an answer's
      // length, then its bytes copied where the core wants them.
      native_new: (p, n, iface) => answerLength(host.nativeNew(new TextDecoder().decode(new Uint8Array(memory().buffer, p, n)), iface)),
      native_call: (h, method, p, n) => answerLength(host.nativeCall(Number(h), method, new Uint8Array(memory().buffer, p, n).slice())),
      native_answer: (dst) => {
        new Uint8Array(memory().buffer, dst, nativeAnswer.length).set(nativeAnswer);
        nativeAnswer = null;
      },
      native_start: (h, method, p, n, call) => host.nativeStart(Number(h), method, new Uint8Array(memory().buffer, p, n).slice(), Number(call)),
      native_cancel: (call) => host.nativeCancel(Number(call)),
      native_release: (h) => host.nativeRelease(Number(h)),
      // std/sqlite's calls (core/src/sqlite/sqlite.c): to the page's
      // SQLite worker, answered into the core once it isn't running.
      // platform/crash's record: whether a run's page is open (crash.js's marks).
      crash_alive: (p, n) => (options.crash?.alive(new TextDecoder().decode(new Uint8Array(memory().buffer, p, n))) ? 1 : 0),
      // asuka dev's frames from the core (core/src/abi/dev.h): the page's
      // dev client sends them on (1), or there's none (0).
      dev_send: (kind, p, n) => (options.devSend ? (options.devSend(kind, new Uint8Array(memory().buffer, p, n).slice()), 1) : 0),
      // std/log's lines (core/src/std/log.c): the console's method for
      // their level (0 debug … 3 error), and asuka dev's terminal.
      log_write: (level, p, n) => {
        const line = new TextDecoder().decode(new Uint8Array(memory().buffer, p, n));
        logLine(level, line);
        if (options.log) options.log(level >= 2 ? 2 : 1, `${logLevels[level].toUpperCase().padEnd(5)} ${line}`);
      },
      sqlite_available: () => (sqlite ? 1 : 0),
      sqlite_send: (id, op, conn, p, n) => {
        const input = new Uint8Array(memory().buffer, p, n).slice();
        sqlite.send(id, op, conn, input, (answer) => host.call(() => withBytes(answer, (q, k) => e.asuka_sqlite_done(id, q, k))));
      },
    },
    wasi_snapshot_preview1: wasi(memory, log),
  };
  // Compiled once: the page's instance and each worker's.
  const url = options.wasm || "asuka.wasm";
  const wasmModule = await WebAssembly.compileStreaming(fetch(url)).catch(async () => {
    // A server that doesn't send application/wasm.
    return WebAssembly.compile(await (await fetch(url)).arrayBuffer());
  });
  instance = await WebAssembly.instantiate(wasmModule, imports);
  const e = instance.exports;
  e._initialize?.();

  // Bytes into the core's memory for a call, freed after it.
  const withBytes = (bytes, f) => {
    const p = e.asuka_alloc(bytes.length);
    new Uint8Array(memory().buffer, p, bytes.length).set(bytes);
    try { return f(p, bytes.length); } finally { e.asuka_free(p); }
  };
  // asuka dev's loads: the state cells kept (0 for the first, and a restart's, which keeps
  // none) and the microseconds taken; failed when the module didn't load or start (the
  // error overlay says why).
  const devLoad = (bytes, restart) => {
    const micros = e.asuka_alloc(4);
    try {
      const kept = withBytes(bytes, (p, n) => (restart ? e.asuka_dev_restart : e.asuka_dev_reload)(p, n, micros, 0));
      return { kept: Math.max(kept, 0), micros: new DataView(memory().buffer).getUint32(micros, true), failed: kept < 0 };
    } finally {
      e.asuka_free(micros);
    }
  };
  const devLoaded = (r) => {
    if (r.failed) throw new Error("asuka: the app's module didn't load (the error overlay says why)");
    return r;
  };
  const core = {
    /** reload loads asuka dev's next module, keeping the app's state and views: { kept, micros }. */
    reload: (bytes) => new Promise((ok, fail) => host.call(() => {
      try {
        ok(devLoaded(devLoad(bytes, false)));
        pool?.reload(bytes);
        workers?.reload(bytes);
      } catch (err) { fail(err); }
    })),
    /** restart loads asuka dev's module keeping none of the app's state: { kept: 0, micros }. */
    restart: (bytes) => new Promise((ok, fail) => host.call(() => {
      try {
        ok(devLoaded(devLoad(bytes, true)));
        pool?.reload(bytes);
        workers?.reload(bytes);
      } catch (err) { fail(err); }
    })),
    /** frame gives the core a frame of asuka dev's (core/src/abi/dev.h): a build that failed, its fix, an error's source. */
    frame: (kind, bytes) => host.call(() => { withBytes(bytes, (p, n) => e.asuka_dev_frame(kind, p, n)); e.asuka_run_posted(); }),
    event: (bytes) => withBytes(bytes, (p, n) => e.asuka_event(p, n)),
    listCount: (id) => Number(e.asuka_list_count(id)),
    listBind: (id, index, cell) => e.asuka_list_bind(id, BigInt(index), cell),
    listUnbind: (id, cell) => e.asuka_list_unbind(id, cell),
    runPosted: () => e.asuka_run_posted(),
    nativeDone: (call, bytes) => withBytes(bytes, (p, n) => e.asuka_native_done(BigInt(call), p, n)),
    // A native package's call of an Asuka function (a callback): its
    // answer's bytes (empty for none).
    nativeInvoke: (id, bytes) => {
      const out = e.asuka_alloc(4);
      try {
        const n = withBytes(bytes, (p, k) => e.asuka_native_invoke(BigInt(id), p, k, out));
        const q = new DataView(memory().buffer).getUint32(out, true);
        if (!q) return new Uint8Array(0);
        const b = new Uint8Array(memory().buffer, q, n).slice();
        e.asuka_free(q);
        return b;
      } finally {
        e.asuka_free(out);
      }
    },
    saveState: () => null,
    // SVG an Image shows, read and drawn by the core (core/src/ui/svg.c):
    // the document bytes hold (null: not SVG), its size in points, and the
    // commands that draw it in a box of w by h fitted by mode (an
    // ImageMode), every color tint's when tint (a core color) isn't 0.
    svgParse: (bytes) => {
      const out = e.asuka_alloc(4);
      try {
        const n = withBytes(bytes, (p, k) => e.asuka_svg_parse(p, k, out));
        const q = new DataView(memory().buffer).getUint32(out, true);
        if (!n || !q) return null;
        const b = new Uint8Array(memory().buffer, q, n).slice();
        e.asuka_free(q);
        return b;
      } finally {
        e.asuka_free(out);
      }
    },
    svgSize: (doc) => {
      const out = e.asuka_alloc(8);
      try {
        withBytes(doc, (p, k) => e.asuka_svg_size(p, k, out, out + 4));
        const v = new DataView(memory().buffer);
        return [v.getFloat32(out, true), v.getFloat32(out + 4, true)];
      } finally {
        e.asuka_free(out);
      }
    },
    svgDraw: (doc, w, h, mode, tint) => {
      const out = e.asuka_alloc(4);
      try {
        const n = withBytes(doc, (p, k) => e.asuka_svg_draw(p, k, w, h, mode, 0n, BigInt(tint || 0), out));
        const q = new DataView(memory().buffer).getUint32(out, true);
        if (!n || !q) return null;
        const b = new Uint8Array(memory().buffer, q, n).slice();
        e.asuka_free(q);
        return b;
      } finally {
        e.asuka_free(out);
      }
    },
  };
  host.attach(memory());
  host.connect(core, e.asuka_io());
  host.inGo = 1;
  if (options.appDir) withBytes(new TextEncoder().encode(options.appDir + "\0"), (p) => e.asuka_set_app_dir(p));
  // Where the Router starts: the page's address (a reload, a link), or
  // where the main window was left.
  const start = host.location?.("")?.[0];
  if (start) withBytes(new TextEncoder().encode(start + "\0"), (p) => e.asuka_set_start(p));
  // The run's crash record, before the app's init (crash.js).
  options.crash?.start(e, withBytes, (p) => {
    const b = new Uint8Array(memory().buffer, p);
    return new TextDecoder().decode(b.subarray(0, b.indexOf(0)));
  });
  e.asuka_web_start();
  let bytes = null;
  if (options.module) {
    // asuka dev: the app's bytecode.
    const res = await fetch(options.module);
    if (!res.ok) throw new Error("asuka: the app's module: " + (await res.text()));
    bytes = new Uint8Array(await res.arrayBuffer());
  }
  if (options.workers !== false && (options.makeWorker || typeof Worker !== "undefined")) {
    const shared = {
      wasm: wasmModule,
      module: bytes,
      appDir: options.appDir,
      sqlite: !!sqlite,
      // Where the page's crash record is, and which runs' pages are open.
      crash: options.crash ? () => ({ where: options.crash.where(), alive: options.crash.aliveRuns() }) : null,
      log: (line) => log(2, line),
      ...(options.makeWorker ? { makeWorker: options.makeWorker } : {}),
      // A job's answer: into the core, once it isn't running.
      done: (id, status, answer) => host.call(() => withBytes(answer, (p, n) => e.asuka_spawn_done(id, status, p, n))),
    };
    pool = new Pool(shared);
    workers = new Workers(shared);
    e.asuka_web_workers(1);
  }
  if (bytes) {
    if (options.dev) {
      core.loaded = devLoad(bytes);
    } else if (withBytes(bytes, (p, n) => e.asuka_load(p, n, 0)) !== 0) {
      throw new Error("asuka: the app's module didn't load (see the console)");
    }
  } else if (e.asuka_run(0) !== 0) {
    // The app compiled into asuka.wasm.
    throw new Error("asuka: the app didn't start (see the console)");
  }
  host.started();
  return core;
}
