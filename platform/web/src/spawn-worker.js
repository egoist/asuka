// A spawn worker (spawn.js): the core again, with the page's app, which
// runs the closures the page's core hands out. A worker's Web Worker
// (workers.js) is one too, whose start made its state, which its jobs
// (the worker's calls) take.
//
// Messages in: { init: { wasm, module, appDir, sqlite, crash, files } } (a fresh
// instance of the core, wasm a WebAssembly.Module, with the app's
// bytecode, module, or the app compiled in; the app's init run without a
// UI), { job: { id, bytes, files } }. Out: { ready, error } once init
// ran, { start: { bytes } } (a worker's: the job that makes its state, run
// at once), { done: { id, status, bytes, files, dead } } for each job (dead:
// the core broke on it, and the worker should go), { log } for what the
// core prints. files are the file system's changes (fs.js): the page's
// in, the job's out.

import { wasi } from "./wasi.js";
import { stackRoom } from "./stack.js";
import { zoneName } from "./zone.js";
import * as fs from "./fs.js";

const FAILED = 2;
const enc = new TextEncoder();
const noDB = enc.encode("sqlite: the app's databases are the page's, not spawn's workers'");
const u32 = (n) => [n & 255, (n >> 8) & 255, (n >> 16) & 255, (n >>> 24) & 255];

/** serve answers port's messages (the worker's own scope in a browser; Node's tests give theirs). */
export function serve(port) {
  let core = null;
  const say = (m, transfer) => port.postMessage(m, transfer ?? []);
  const log = (fd, line) => say({ log: line });

  let alive = new Set();

  async function init({ wasm, module, appDir, sqlite, crash, files }) {
    fs.replica(files);
    let instance;
    const memory = () => instance.exports.memory;
    const timers = new Map();
    let posted = false;
    const run = (f) => {
      try {
        f();
      } catch (e) {
        log(2, `spawn's worker: ${e?.stack ?? e}`);
      }
    };
    const imports = {
      asuka: {
        apply() {},
        measure() {},
        post() {
          if (posted) return;
          posted = true;
          setTimeout(() => { posted = false; run(() => instance.exports.asuka_run_posted()); }, 0);
        },
        timer_set(id, ms) {
          timers.set(id, setTimeout(() => {
            timers.delete(id);
            run(() => { instance.exports.asuka_timer_fire(id); instance.exports.asuka_run_posted(); });
          }, ms));
        },
        timer_clear(id) { clearTimeout(timers.get(id)); timers.delete(id); },
        zone_offset: (s) => -new Date(s * 1000).getTimezoneOffset() * 60,
        zone_name: (p, cap) => zoneName(memory(), p, cap),
        stack_room: stackRoom(),
        spawn() { throw new Error("asuka: spawn in a worker"); }, // web.c runs a worker's in place
        spawn_cancel() {},
        // A worker is made on the page (the compiler keeps it so).
        worker_start() { throw new Error("asuka: a worker made in a worker"); },
        worker_call() { throw new Error("asuka: a worker called from a worker"); },
        worker_stop() {},
        // Native modules are the page's: a worker's core has none (web.c
        // gives it no native calls), so these are never reached.
        native_new: () => 0,
        native_call: () => 0,
        native_answer() {},
        native_start() {},
        native_cancel() {},
        native_release() {},
        // The app's databases are the page's: what the init sends without
        // waiting goes nowhere, and a statement fails (spawn's closures
        // can't await one anyway).
        // The page's crash record's runs: those whose pages are open.
        crash_alive: (p, n) => (alive.has(new TextDecoder().decode(new Uint8Array(memory().buffer, p, n))) ? 1 : 0),
        // asuka dev's frames are the page's core's.
        dev_send: () => 0,
        sqlite_available: () => (sqlite ? 1 : 0),
        sqlite_send(id) {
          if (!id) return;
          const answer = new Uint8Array([0, ...u32(noDB.length), ...noDB]);
          setTimeout(() => run(() => {
            const e = instance.exports;
            const p = e.asuka_alloc(answer.length);
            new Uint8Array(memory().buffer, p, answer.length).set(answer);
            e.asuka_sqlite_done(id, p, answer.length);
            e.asuka_free(p);
          }), 0);
        },
      },
      wasi_snapshot_preview1: wasi(memory, log),
    };
    try {
      instance = await WebAssembly.instantiate(wasm, imports);
      const e = instance.exports;
      e._initialize?.();
      const withBytes = (b, f) => {
        const p = e.asuka_alloc(b.length);
        new Uint8Array(memory().buffer, p, b.length).set(b);
        try { return f(p, b.length); } finally { e.asuka_free(p); }
      };
      if (appDir) withBytes(enc.encode(appDir + "\0"), (p) => e.asuka_set_app_dir(p));
      // platform/crash: reports are read here (crash.reports() spawns it), from the page's record.
      if (crash?.where) withBytes(enc.encode(crash.where + "\0"), (p) => e.asuka_crash_read(p));
      alive = new Set(crash?.alive ?? []);
      const ok = module ? withBytes(module, (p, n) => e.asuka_worker_start(p, n)) : e.asuka_worker_start(0, 0);
      if (ok !== 0) {
        say({ ready: false, error: answer(e, memory) });
        return;
      }
      core = { e, memory, withBytes };
      say({ ready: true });
    } catch (err) {
      say({ ready: false, error: `${err?.message ?? err}` });
    }
  }

  function job({ id, bytes, files, crash }) {
    fs.replica(files);
    if (crash) alive = new Set(crash.alive);
    const mark = fs.mark();
    let status, out, dead = false;
    if (!core) {
      status = FAILED;
      out = enc.encode("the worker has no app");
    } else {
      try {
        status = core.withBytes(new Uint8Array(bytes), (p, n) => core.e.asuka_worker_call(p, n));
        out = answerBytes(core.e, core.memory);
        core.e.asuka_worker_answered();
      } catch (err) {
        // A trap (the core panicked): this instance can't go on.
        status = FAILED;
        out = enc.encode(`the work failed: ${err?.message ?? err}`);
        dead = true;
        core = null;
      }
    }
    const changes = fs.since(mark);
    const transfer = [out.buffer];
    for (const [, e] of changes) if (e?.data) transfer.push(e.data.buffer);
    say({ done: { id, status, bytes: out.buffer, files: changes, dead } }, transfer);
  }

  // start: this instance becomes a worker's, its state made by the job's
  // closure. Its error, if it threw, is each call's answer.
  let starting = null;
  function start({ bytes }) {
    if (!core) return;
    try {
      core.withBytes(new Uint8Array(bytes), (p, n) => core.e.asuka_worker_new(p, n));
    } catch (err) {
      log(2, `a worker's init: ${err?.message ?? err}`);
      core = null;
    }
  }

  port.onmessage = (ev) => {
    const m = ev.data;
    if (m.init) starting = init(m.init);
    else if (m.start) starting = starting.then(() => start(m.start));
    else if (m.job) {
      if (starting) starting.then(() => job(m.job));
      else job(m.job);
    }
  };
}

// The answer the core left in its io buffer: its bytes, copied out.
function answerBytes(e, memory) {
  const io = new Uint32Array(memory().buffer, e.asuka_io(), 8);
  return new Uint8Array(memory().buffer, io[0], io[1]).slice();
}

function answer(e, memory) {
  return new TextDecoder().decode(answerBytes(e, memory));
}

// In a browser's worker: its own scope.
if (typeof WorkerGlobalScope !== "undefined" && globalThis instanceof WorkerGlobalScope) serve(globalThis);
