// The app's files offline (src/offline.js) and the service worker that
// keeps them (src/sw.js), run in a context of their own with fakes for
// Cache Storage, the network, the app's pages and its registration:
// installs whole or not at all, a page's load and the files from the
// cache, the version before kept for its open pages, a new version taking
// over as the only page reloads or as the app asks, and what isn't the
// worker's left to the network.
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import vm from "node:vm";

const src = (n) => readFileSync(new URL("../src/" + n, import.meta.url), "utf8");
const scope = "https://app.example/app/";

/** fakeCaches is Cache Storage in memory: each cache a map of URL to [body, status, headers]. */
function fakeCaches() {
  const all = new Map();
  const keyOf = (r) => (typeof r === "string" ? r : r.url);
  const open = async (name) => {
    if (!all.has(name)) all.set(name, new Map());
    const m = all.get(name);
    return {
      async match(r) {
        const e = m.get(keyOf(r));
        return e ? new Response(e.body, { status: e.status, headers: e.headers }) : undefined;
      },
      async put(r, res) {
        m.set(keyOf(r), { body: new Uint8Array(await res.arrayBuffer()), status: res.status, headers: [...res.headers] });
      },
    };
  };
  return {
    all, open,
    async keys() { return [...all.keys()]; },
    async delete(name) { return all.delete(name); },
    text: (name, url) => new TextDecoder().decode(all.get(name)?.get(url)?.body ?? new Uint8Array()),
  };
}

/** fakeNetwork serves site (path under the scope → its text), counting what's asked; down, it fails. */
function fakeNetwork(site) {
  const net = {
    site, asked: [], down: false,
    async fetch(req) {
      const r = typeof req === "string" ? new Request(req) : req;
      net.asked.push(r.url);
      if (net.down) throw new TypeError("Failed to fetch");
      const path = r.url.slice(scope.length);
      if (!(path in net.site)) return new Response("missing", { status: 404 });
      return new Response(net.site[path], { status: 200, headers: { "Content-Type": "text/plain" } });
    },
  };
  return net;
}

function context(extra = {}) {
  const ctx = { console, URL, Request, Response, Headers, Blob, setTimeout, clearTimeout, Promise, MessageChannel, ...extra };
  vm.createContext(ctx);
  vm.runInContext(src("offline.js"), ctx);
  return ctx;
}

const manifest = (version, files) => ({ version, files });
const v1 = manifest("v1", [["index.html", "r1"], ["asuka/aaaaaaaaaaaa/asuka.js", ""], ["asuka.aaaaaaaaaaaa.wasm", ""], ["assets/a.txt", "ra"], ["assets/movie.mp4", "rm"]]);
const site1 = { "index.html": "<p>v1</p>", "asuka/aaaaaaaaaaaa/asuka.js": "js1", "asuka.aaaaaaaaaaaa.wasm": "wasm1", "assets/a.txt": "A", "assets/movie.mp4": "0123456789" };
const v2 = manifest("v2", [["index.html", "r2"], ["asuka/bbbbbbbbbbbb/asuka.js", ""], ["asuka.bbbbbbbbbbbb.wasm", ""], ["assets/a.txt", "ra"], ["assets/movie.mp4", "rm"]]);
const site2 = { "index.html": "<p>v2</p>", "asuka/bbbbbbbbbbbb/asuka.js": "js2", "asuka.bbbbbbbbbbbb.wasm": "wasm2", "assets/a.txt": "A", "assets/movie.mp4": "0123456789" };

function offline(m, { caches, net, clients = [], registration = {} }) {
  const ctx = context();
  return ctx.asukaOffline.create({
    manifest: m, scope, caches, fetch: (r) => net.fetch(r), registration,
    clients: { async matchAll() { return clients; } },
  });
}

const nav = (path, headers = {}) => ({ method: "GET", url: scope + path, mode: "navigate", headers: new Headers(headers) });
const get = (path, headers = {}) => ({ method: "GET", url: path.startsWith("http") ? path : scope + path, mode: "cors", headers: new Headers(headers) });
const text = async (r) => (r ? await (await r).text() : null);

test("install keeps every file, by its revision; a page's load at any route is index.html, offline too", async () => {
  const caches = fakeCaches();
  const net = fakeNetwork(site1);
  const o = offline(v1, { caches, net });
  assert.equal(await o.installed(), false);
  await o.install();
  const name = "asuka-precache:" + scope + ":v1";
  assert.equal(caches.text(name, scope + "index.html?__asuka=r1"), "<p>v1</p>");
  assert.equal(caches.text(name, scope + "asuka.aaaaaaaaaaaa.wasm"), "wasm1");
  await o.activate();
  net.down = true;
  assert.equal(await text(o.respond(nav(""))), "<p>v1</p>");
  assert.equal(await text(o.respond(nav("chat/7?x=1"))), "<p>v1</p>");
  assert.equal(await text(o.respond(nav("assets/a.txt"))), "A");
  assert.equal(await text(o.respond(get("asuka/aaaaaaaaaaaa/asuka.js"))), "js1");
  assert.equal(await text(o.respond(get("assets/a.txt?v=3"))), "A");
  // Not the worker's: the network has them, untouched.
  assert.equal(o.respond(nav("report.pdf")), null);
  assert.equal(o.respond(nav("embed.html")), null);
  assert.equal(o.respond(get("version.json?t=1")), null);
  assert.equal(o.respond(get("api/notes")), null);
  assert.equal(o.respond(get("https://api.example/notes")), null);
  assert.equal(o.respond(get("https://app.example/other/")), null);
  assert.equal(o.respond({ ...get("assets/a.txt"), method: "POST" }), null);
});

test("a range of a kept file is a 206", async () => {
  const caches = fakeCaches();
  const net = fakeNetwork(site1);
  const o = offline(v1, { caches, net });
  await o.install();
  const r = await o.respond(get("assets/movie.mp4", { Range: "bytes=2-5" }));
  assert.equal(r.status, 206);
  assert.equal(r.headers.get("content-range"), "bytes 2-5/10");
  assert.equal(await r.text(), "2345");
  const tail = await o.respond(get("assets/movie.mp4", { Range: "bytes=-3" }));
  assert.equal(await tail.text(), "789");
  const past = await o.respond(get("assets/movie.mp4", { Range: "bytes=20-" }));
  assert.equal(past.status, 416);
});

test("a version installs whole or not at all", async () => {
  const caches = fakeCaches();
  const net = fakeNetwork({ ...site1 });
  delete net.site["assets/a.txt"];
  const o = offline(v1, { caches, net });
  await assert.rejects(o.install(), /assets\/a\.txt.*404/);
  assert.deepEqual(await caches.keys(), []);
});

test("a file the server dropped or couldn't answer is asked for again; one it hasn't isn't", async () => {
  const caches = fakeCaches();
  const net = fakeNetwork(site1);
  // A server that takes a few connections at a time (python3 -m http.server):
  // the wasm's request reset twice, the script's answered 503 once.
  const failing = { "asuka.aaaaaaaaaaaa.wasm": 2, "asuka/aaaaaaaaaaaa/asuka.js": 1 };
  const flaky = async (r) => {
    const path = r.url.slice(scope.length);
    if (failing[path] > 0) {
      failing[path]--;
      net.asked.push(r.url);
      if (path.endsWith(".wasm")) throw new TypeError("Failed to fetch");
      return new Response("busy", { status: 503 });
    }
    return net.fetch(r);
  };
  const waited = [];
  const o = context().asukaOffline.create({ manifest: v1, scope, caches, fetch: flaky, wait: async (ms) => { waited.push(ms); } });
  await o.install();
  const name = "asuka-precache:" + scope + ":v1";
  assert.equal(caches.text(name, scope + "asuka.aaaaaaaaaaaa.wasm"), "wasm1");
  assert.equal(caches.text(name, scope + "asuka/aaaaaaaaaaaa/asuka.js"), "js1");
  assert.equal(net.asked.filter((u) => u.endsWith(".wasm")).length, 3);
  assert.deepEqual(waited.sort((a, b) => a - b), [250, 250, 500]);
  // A network that stays down fails the install after its attempts; a 404 at once.
  net.down = true;
  net.asked = [];
  const down = context().asukaOffline.create({ manifest: manifest("v9", [["assets/a.txt", "x9"]]), scope, caches, fetch: (r) => net.fetch(r), wait: async () => {} });
  await assert.rejects(down.install(), /Failed to fetch/);
  assert.equal(net.asked.length, 4);
  net.down = false;
  net.asked = [];
  const missing = context().asukaOffline.create({ manifest: manifest("v8", [["assets/none.txt", "x8"]]), scope, caches, fetch: (r) => net.fetch(r), wait: async () => {} });
  await assert.rejects(missing.install(), /none\.txt.*404/);
  assert.equal(net.asked.length, 1);
});

test("a new version copies the files it shares with the old, keeps the old for its pages, and deletes older ones", async () => {
  const caches = fakeCaches();
  const net = fakeNetwork(site1);
  const a = offline(v1, { caches, net });
  await a.install();
  await a.activate();
  net.site = site2;
  net.asked = [];
  const b = offline(v2, { caches, net });
  await b.install();
  assert.equal(await b.installed(), true, "an older version answers loads: this one waits");
  assert.deepEqual(net.asked.map((u) => u.slice(scope.length)).sort(), ["asuka.bbbbbbbbbbbb.wasm", "asuka/bbbbbbbbbbbb/asuka.js", "index.html"]);
  await b.activate();
  net.down = true;
  assert.equal(await text(b.respond(nav(""))), "<p>v2</p>");
  // A page of v1 still open loads a file of its own.
  assert.equal(await text(b.respond(get("asuka/aaaaaaaaaaaa/asuka.js"))), "js1");
  const c = offline(manifest("v3", v2.files.map(([p, r]) => [p, r === "r2" ? "r3" : r])), { caches, net: fakeNetwork({ ...site2, "index.html": "<p>v3</p>" }) });
  await c.install();
  await c.activate();
  const names = (await caches.keys()).filter((k) => k.startsWith("asuka-precache:"));
  assert.deepEqual(names.sort(), ["asuka-precache:" + scope + ":v2", "asuka-precache:" + scope + ":v3"]);
});

test("the only page reloading with a new version waiting has it take over", async () => {
  const caches = fakeCaches();
  const net = fakeNetwork(site1);
  const posted = [];
  const registration = { waiting: { postMessage: (m) => posted.push(m) } };
  const clients = [{}];
  const o = offline(v1, { caches, net, clients, registration });
  await o.install();
  await o.activate();
  const r = await o.respond(nav("chat/7"));
  assert.equal(r.headers.get("refresh"), "0");
  assert.deepEqual(JSON.parse(JSON.stringify(posted)), [{ asuka: "skipWaiting" }]);
  // Another page of the app open: it waits; the load is this version's.
  clients.push({});
  assert.equal(await text(o.respond(nav("chat/7"))), "<p>v1</p>");
  assert.equal(posted.length, 1);
  // A file's load doesn't.
  clients.pop();
  assert.equal(await text(o.respond(nav("assets/a.txt"))), "A");
});

test("clear deletes what an app kept offline, and only its", async () => {
  const caches = fakeCaches();
  const o = offline(v1, { caches, net: fakeNetwork(site1) });
  await o.install();
  await o.activate();
  await caches.open("asuka-precache:https://app.example/other/:v1");
  await caches.open("someone-else");
  await context().asukaOffline.clear(scope, caches);
  assert.deepEqual((await caches.keys()).sort(), ["asuka-precache:https://app.example/other/:v1", "someone-else"]);
});

/** worker runs the whole service worker (notify-core.js, offline.js, sw.js) with app's settings. */
function worker(app, { caches = fakeCaches(), net = fakeNetwork(site1), active = null } = {}) {
  const listeners = {};
  let skipped = 0;
  const ctx = context({
    caches, fetch: (r) => net.fetch(r), navigator: {}, asukaApp: app,
    registration: { scope, active, async showNotification() {}, async getNotifications() { return []; } },
    clients: { async matchAll() { return []; }, async claim() {}, async openWindow() {} },
    skipWaiting() { skipped++; },
    addEventListener(type, f) { (listeners[type] ??= []).push(f); },
  });
  vm.runInContext(src("notify-core.js"), ctx);
  vm.runInContext(src("offline.js"), ctx);
  vm.runInContext(src("sw.js"), ctx);
  const dispatch = async (type, e = {}) => {
    const waits = [];
    let answer = null;
    for (const f of listeners[type] || []) f({ ...e, waitUntil: (p) => waits.push(p), respondWith: (r) => { answer = r; } });
    await Promise.all(waits);
    return answer;
  };
  return { listeners, dispatch, skipped: () => skipped, caches, net };
}

test("the worker: the first offline version takes over at once, the next waits; a page's message lets it", async () => {
  const caches = fakeCaches();
  const a = worker({ name: "com.example.app", precache: v1 }, { caches });
  assert.equal(a.listeners.fetch?.length, 1);
  await a.dispatch("install");
  assert.equal(a.skipped(), 1);
  await a.dispatch("activate");
  const answer = await a.dispatch("fetch", { request: nav("chat/7") });
  assert.equal(await (await answer).text(), "<p>v1</p>");
  assert.equal(await a.dispatch("fetch", { request: get("api/x") }), null);

  const b = worker({ name: "com.example.app", precache: v2 }, { caches, net: fakeNetwork(site2), active: {} });
  await b.dispatch("install");
  assert.equal(b.skipped(), 0, "waits for the old version's pages");
  await b.dispatch("message", { data: { asuka: "skipWaiting" } });
  assert.equal(b.skipped(), 1);
});

test("the worker without files to keep (asuka dev's, offline off): no fetch handler, what was kept deleted", async () => {
  const caches = fakeCaches();
  const a = worker({ name: "com.example.app", precache: v1 }, { caches });
  await a.dispatch("install");
  await a.dispatch("activate");
  assert.ok((await caches.keys()).some((k) => k.startsWith("asuka-precache:")));
  const off = worker({ name: "com.example.app" }, { caches });
  assert.equal(off.listeners.fetch, undefined);
  await off.dispatch("install");
  assert.equal(off.skipped(), 1);
  await off.dispatch("activate");
  // (The notifications' store stays.)
  assert.deepEqual((await caches.keys()).filter((k) => k.includes(scope)), []);
});
