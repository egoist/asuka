// Asuka's web loader: it starts an app (the C core as WebAssembly, the
// app compiled into it or loaded as bytecode) with the web host on a page.
//
//	import { start } from "./asuka/asuka.js";
//	start({ core: "asuka.wasm", container: document.body });
//
// Options: container (the element the app fills; the page's body by
// default, the app then filling the page), core (the URL of the C core,
// asuka.wasm, with the app compiled in) and module (an app's bytecode to
// load into it: asuka dev's, which with dev loads each save's in place,
// keeping the app's state), assets (where its assets are served:
// "assets"), name (the app's: its stored data is kept under it), fonts
// (the app's font files, assets/fonts), sqlite (the URL of SQLite's
// module, asqlite.wasm, which an app that imports std/sqlite has: SQLite's
// worker starts with it), serviceWorker (the
// URL of the app's service worker, sw.js with the app's settings, which
// the page registers: its notifications and pushes), vapidPublicKey (the
// app's VAPID key, for platform/push), icon (a notification's icon),
// mapTiles (the URL of platform/maps' map tiles, with {z}, {x} and {y}:
// OpenStreetMap's by default), workers (false: spawn runs on the page's
// thread, not its workers), dev (asuka dev's: the page reloads with each
// build, keeping the app's state, and the app's logs go to the
// terminal).

import { Host } from "./host.js";
import * as fs from "./fs.js";
import * as worker from "./worker.js";
import { notifyFor } from "./modules/system-notify.js";
import { startCore } from "./core.js";
import { Crash } from "./crash.js";
import { app as nativeApp, savedIcon } from "./native.js";
import { showDevError, hideDevError } from "./deverror.js";
// What packages' generated JavaScript (asuka native) builds on: globalThis.asuka.native.
export * as native from "./native.js";

let hosts = 0;
const natives = new Map();
const modules = new Map();

/** registerNative gives the page a native view kind (ui.Native): factory(ctx) returns { el, set(name, value), measure?(c), destroy?() }; ctx.emit(name, values) sends its events. */
export function registerNative(kind, factory) { natives.set(kind, factory); }

/** registerModule gives the page a native module (ui.Call): methods by name, each taking the call's bytes and returning (or resolving to) the result's. */
export function registerModule(name, methods) { modules.set(name, methods); }

/** mapTiles is the start option platform/maps' view (sdk/maps/web) reads its tiles' URL from. */
export let mapTiles = "";

export async function start(options = {}) {
  const fullPage = !options.container;
  const container = options.container || document.body.appendChild(document.createElement("div"));
  if (fullPage) container.classList.add("asuka-page");
  const name = options.name || "app";
  nativeApp.name = name;
  mapTiles = options.mapTiles || "";
  const dev = options.dev ? devClient(options.dev) : null;
  // The app's service worker, listened to before the app starts: a tap
  // on a notification may be what opened the page.
  worker.register(options.serviceWorker);
  // SQLite's worker starts beside the app (std/sqlite), loading its module meanwhile.
  const sqliteWasm = options.sqlite;
  const sqlite = sqliteWasm ? import("./sqlite.js").then((m) => m.load(new URL(sqliteWasm, document.baseURI))) : null;
  sqlite?.catch(() => {});
  const safe = name.replace(/[^\w.-]/g, "_");
  const data = "/data/" + safe;
  // The app's files, and its caches (std/fs's cacheDir: the core's
  // /cache/<the data directory's name>) kept apart, each in its own
  // database.
  await Promise.all([fs.mount({ name, dir: data, output: dev?.log }), fs.mount({ name: name + "#cache", dir: "/cache/" + safe })]);
  const host = new Host(container, { ...options, fullPage, name });
  nativeApp.assets = host.assets;
  nativeApp.version = options.version || "";
  nativeApp.build = String(options.build || "");
  nativeApp.dev = !!options.dev;
  // The alternate icons (platform/appicon); the one the app chose shows from the start.
  nativeApp.icons = options.icons || {};
  if (savedIcon()) nativeApp.showIcon(savedIcon());
  // The app's own native views and modules (registerNative, registerModule).
  for (const [k, f] of natives) host.natives.set(k, f);
  for (const [k, m] of modules) host.modules.registered.set(k, m);
  worker.attach(host);
  // Files an installed app is opened with (asuka.json's fileTypes).
  worker.openLaunchFiles(host, fs.saveUpload);
  const hostName = "asukaHost" + (hosts++ || "");
  globalThis[hostName] = host;
  let core;
  try {
    core = await startCore(host, {
      wasm: options.core || "asuka.wasm",
      module: options.module,
      dev: !!dev,
      // The core's lines come without their line ending.
      log: dev ? (fd, line) => dev.log(fd, line + "\n") : undefined,
      devSend: dev?.send,
      appDir: data,
      workers: options.workers,
      sqlite: sqlite ? () => sqlite : null,
      crash: (host.crash = new Crash(host)),
    });
  } catch (e) {
    showError(container, "The app couldn't load", e.message);
    throw e;
  }
  const trapped = watchTrap(container, host);
  if (dev) dev.attach(host, core, trapped);
  try { notifyFor(host); } catch (e) { console.warn("platform/notify:", e); }
  return host;
}

// watchTrap shows that the app stopped when its core traps (its panic, or a
// fault: out of memory, its stack overflowed): the page can't go on, and
// says so rather than sitting still. The panic's message is the crash
// record's, when it keeps one. It returns what shows it, for a trap the page
// caught (asuka dev's reload).
function watchTrap(container, host) {
  let shown = false;
  const trapped = (err) => {
    if (shown || !(err instanceof WebAssembly.RuntimeError)) return;
    shown = true;
    showError(container, "The app stopped", host.crash?.panicMessage() || err.message);
  };
  globalThis.addEventListener?.("error", (e) => trapped(e.error));
  globalThis.addEventListener?.("unhandledrejection", (e) => trapped(e.reason));
  return trapped;
}

// showError shows what stopped the page (no core to ask: a trap, a core
// that didn't load) in the error overlay; Reload loads the page again.
function showError(container, title, msg) {
  showDevError(container, { flags: 1, title, message: msg, source: "", details: "" }, {
    dismiss: () => hideDevError(container),
    reload: () => location.reload(),
  });
}

/** devClient is asuka dev's side of the page: logs to the terminal, reloads with each build. */
function devClient(url) {
  const base = typeof url === "string" ? url : "/__asuka/";
  let pending = "";
  let timer = 0;
  let errorText = "";
  const send = () => {
    timer = 0;
    const body = pending;
    pending = "";
    if (body) fetch(base + "log", { method: "POST", body, keepalive: true }).catch(() => {});
  };
  const log = (fd, text) => {
    if (fd === 2) errorText += text;
    pending += (fd === 2 ? "\x02" : "\x01") + text;
    if (!timer) timer = setTimeout(send, 30);
  };
  addEventListener("error", (e) => log(2, `page: ${e.message} (${e.filename}:${e.lineno})\n${e.error?.stack || ""}\n`));
  addEventListener("unhandledrejection", (e) => log(2, `page: ${e.reason?.stack || e.reason}\n`));
  let host = null, core = null, trapped = null;
  const report = (r, first, restarted) => fetch(base + "reloaded", { method: "POST", body: JSON.stringify({ First: first, Restarted: restarted, Kept: r.kept, Micros: r.micros }) }).catch(() => {});
  // asuka dev's frames for the core (core/src/abi/dev.h's kinds), held until it's there.
  const Frame = { Reloaded: 5, Uncaught: 8, Source: 9, Failed: 10, Fixed: 11, Inspect: 12, Tree: 13, Highlight: 14, Automate: 15, Automated: 16 };
  let held = [];
  const frame = (kind, bytes) => {
    if (core?.frame) core.frame(kind, bytes);
    else held.push([kind, bytes]);
  };
  // The core's frames for asuka dev: a restart the error overlay's Reload made,
  // and an error the app didn't catch, which it prints and answers with its source.
  const toCLI = (kind, bytes) => {
    if (kind === Frame.Reloaded && bytes.length >= 8) {
      const v = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
      report({ kept: v.getUint32(0, true), micros: v.getUint32(4, true) }, false, (bytes[8] & 1) !== 0);
      return;
    }
    if (kind === Frame.Automated) {
      fetch(base + "automated", { method: "POST", body: bytes }).catch(() => {});
      return;
    }
    if (kind === Frame.Tree) {
      fetch(`${base}tree?page=${page}&name=${encodeURIComponent(browser())}`, { method: "POST", body: bytes }).catch(() => {});
      return;
    }
    if (kind !== Frame.Uncaught) return;
    fetch(base + "uncaught", { method: "POST", body: bytes })
      .then((res) => (res.ok ? res.arrayBuffer() : null))
      .then((b) => b && frame(Frame.Source, new Uint8Array(b)))
      .catch(() => {});
  };
  // This page's id, and its browser's name, for the inspector's list of apps.
  const page = Math.random().toString(36).slice(2, 10);
  const browser = () => {
    const ua = navigator.userAgent;
    for (const [name, re] of [["Edge", /Edg\//], ["Firefox", /Firefox\//], ["Chrome", /Chrome\//], ["Safari", /Safari\//]]) if (re.test(ua)) return name;
    return "browser";
  };
  const events = new EventSource(base + "events");
  // A new build of the app's module: loaded in place, its state and views kept. A
  // restart's: loaded keeping none of its state.
  let loading = Promise.resolve();
  const load = (stamp, restart) => {
    if (!core?.reload) return;
    loading = loading.then(async () => {
      try {
        const res = await fetch(base + "app.akb?" + stamp);
        if (!res.ok) throw new Error(await res.text());
        const bytes = new Uint8Array(await res.arrayBuffer());
        report(await (restart ? core.restart(bytes) : core.reload(bytes)), false, restart);
      } catch (err) {
        log(2, `page: the new module didn't load: ${err?.message || err}\n`);
        // The core trapped loading it (a panic): it can't go on, so the page
        // says the app stopped, with its Reload, rather than sitting still.
        trapped?.(err);
      }
      send();
    });
  };
  events.addEventListener("module", (e) => load(e.data, false));
  events.addEventListener("restart", (e) => load(e.data, true));
  events.addEventListener("reload", () => {
    host?.saveState();
    send();
    fs.flush().then(() => location.reload());
  });
  // A build that failed (its frame's payload, base64), and its fix: the overlay shows it.
  events.addEventListener("failed", (e) => frame(Frame.Failed, Uint8Array.from(atob(e.data), (c) => c.charCodeAt(0))));
  events.addEventListener("fixed", () => frame(Frame.Fixed, new Uint8Array(0)));
  // The inspector asks for the app's views: the core answers with a tree frame.
  events.addEventListener("inspect", () => core?.frame && frame(Frame.Inspect, new Uint8Array(0)));
  // asuka test --gui's steps ("<number>\n<commands>"): the core runs them, and answers.
  events.addEventListener("automate", (e) => {
    const i = e.data.indexOf("\n");
    const seq = Number(e.data.slice(0, i));
    const cmds = new TextEncoder().encode(e.data.slice(i + 1));
    const b = new Uint8Array(4 + cmds.length);
    new DataView(b.buffer).setUint32(0, seq, true);
    b.set(cmds, 4);
    if (core?.frame) frame(Frame.Automate, b);
  });
  // It selected a view ("<page> <path>"; no path: none): the core highlights it.
  events.addEventListener("highlight", (e) => {
    const [to, path = ""] = e.data.split(" ");
    if (to === page && core?.frame) frame(Frame.Highlight, new TextEncoder().encode(path));
  });
  // GUI tests' commands (e2e/), as the mobile hosts' debug builds take them.
  events.addEventListener("automation", async (e) => {
    const { id, commands } = JSON.parse(e.data);
    let out = "", error = "";
    try {
      const { run } = await import("./automation.js");
      out = await run(host, commands);
    } catch (err) {
      error = String(err?.message || err);
    }
    fetch(base + "automation/done", { method: "POST", body: JSON.stringify({ id, out, error }) }).catch(() => {});
  });
  return {
    log,
    send: toCLI,
    attach(h, c, onTrap) {
      host = h;
      core = c || null;
      trapped = onTrap || null;
      // A first module that didn't start: the overlay says why, and the CLI printed it.
      if (core?.loaded && !core.loaded.failed) report(core.loaded, true);
      const frames = held;
      held = [];
      for (const [kind, bytes] of frames) frame(kind, bytes);
    },
    lastError: () => errorText.trim().split("\n").slice(-30).join("\n"),
  };
}
