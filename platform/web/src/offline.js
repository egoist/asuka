// The app's files offline, for its service worker (sw.js, which the CLI
// ships as one classic script: this before it). asuka build --platform
// web lists the site's files in the worker (globalThis.asukaApp.precache:
// { version, files: [[path, revision]] }, paths under the worker's scope,
// a revision "" for a file named by its digest, else its content's), so
// a new build is a new worker, which the browser installs:
//
//   - install: every file into a cache of its own version (the files an
//     older version has, by path and revision, copied from it; the rest
//     from the network, past the browser's HTTP cache): all of them or
//     none, so a version is whole or isn't.
//   - the new version takes over once no page of the old one is open, or
//     as one reloads (the only page of the app: its load becomes the new
//     version's), or as the app asks (platform/update's relaunch:
//     { asuka: "skipWaiting" }).
//   - activate: the version before is kept (a page of it still open loads
//     its files from it), older ones deleted.
//   - fetch: a page's load (a navigation, any route's path) is index.html
//     from the cache, and the listed files are the cache's; a file of the
//     version before is its; anything else (std/http's requests, images
//     from the network, version.json) isn't the worker's to answer.
//
// The pieces take what they use (caches, fetch, clients, registration), so
// node --test runs them with fakes.

(() => {
  const g = globalThis;

  // A file of a version, named by its digest: what a page of the version
  // before may still ask for.
  const hashed = /\/asuka\/[0-9a-f]{12}\/|\/asuka\.[0-9a-f]{12}\.wasm$/;

  /** prefixOf is the start of the caches' names of the app at scope (several apps may share an origin). */
  const prefixOf = (scope) => "asuka-precache:" + scope + ":";
  const metaOf = (scope) => "asuka-meta:" + scope;

  /**
   * create is the offline part of the worker of the app at scope (its
   * registration's), with manifest's files.
   */
  function create({ manifest, scope, caches = g.caches, fetch = (r) => g.fetch(r), clients = g.clients, registration = g.registration }) {
    const prefix = prefixOf(scope);
    const name = prefix + manifest.version;
    const origin = new URL(scope).origin;
    const metaURL = new URL("__asuka/precache.json", scope).href;
    // A file's URL (without its query) → its key in the cache: the URL,
    // and its revision for a file whose name doesn't change with it.
    const keys = new Map();
    const files = [];
    for (const [path, rev] of manifest.files) {
      const url = new URL(path, scope).href;
      const key = rev ? url + "?__asuka=" + rev : url;
      keys.set(url, key);
      files.push({ url, key, rev });
    }
    const shell = keys.get(new URL("index.html", scope).href);

    async function readMeta() {
      try {
        const r = await (await caches.open(metaOf(scope))).match(metaURL);
        return r ? await r.json() : {};
      } catch {
        return {};
      }
    }

    async function writeMeta(m) {
      const c = await caches.open(metaOf(scope));
      await c.put(metaURL, new Response(JSON.stringify(m), { headers: { "Content-Type": "application/json" } }));
    }

    /** The versions' caches of this app other than this one. */
    async function others() {
      return (await caches.keys()).filter((k) => k.startsWith(prefix) && k !== name);
    }

    /** install puts every file in this version's cache, or fails (the version isn't installed). */
    async function install() {
      const cache = await caches.open(name);
      const older = await Promise.all((await others()).map((k) => caches.open(k)));
      const one = async (f) => {
        if (await cache.match(f.key)) return;
        for (const c of older) {
          const r = await c.match(f.key);
          if (r) {
            await cache.put(f.key, r);
            return;
          }
        }
        // A file named by its digest may come from the HTTP cache; the
        // others are asked for again.
        const res = await fetch(new Request(f.url, { cache: f.rev ? "reload" : "default", credentials: "same-origin" }));
        if (!res.ok) throw new Error(`asuka: the app's ${f.url} for offline: ${res.status}`);
        await cache.put(f.key, await plain(res));
      };
      // A few at a time.
      let next = 0;
      const lanes = Array.from({ length: Math.min(8, files.length) }, async () => {
        while (next < files.length) await one(files[next++]);
      });
      try {
        await Promise.all(lanes);
      } catch (e) {
        await caches.delete(name);
        throw e;
      }
    }

    /** Whether a version of the app works offline already (its worker answers loads): a new one then waits its turn. */
    async function installed() {
      const m = await readMeta();
      return !!m.current && m.current !== name && (await caches.keys()).includes(m.current);
    }

    /** activate makes this version the current one, keeps the one before and deletes the rest. */
    async function activate() {
      const m = await readMeta();
      const previous = m.current && m.current !== name ? m.current : m.previous || "";
      await writeMeta({ current: name, previous });
      for (const k of await others()) {
        if (k !== previous) await caches.delete(k);
      }
    }

    async function fromCache(key, request) {
      const r = await (await caches.open(name)).match(key);
      return r ? ranged(r, request) : null;
    }

    /**
     * respond is the answer to request, or null for what isn't the
     * worker's (the network has it, untouched).
     */
    function respond(request) {
      if (request.method !== "GET") return null;
      const u = new URL(request.url);
      if (u.origin !== origin || !u.href.startsWith(scope)) return null;
      const bare = u.origin + u.pathname;
      const key = keys.get(bare);
      if (request.mode === "navigate") {
        // A file's own address (an asset opened in a tab) is the file;
        // one that isn't listed, with an extension (another page of the
        // site's, a document), isn't the app's page.
        if (!key && /\.[A-Za-z0-9]+$/.test(u.pathname)) return null;
        return navigate(request, key || shell);
      }
      if (key) return (async () => (await fromCache(key, request)) || fetch(request))();
      if (hashed.test(u.pathname)) return older(request, bare);
      return null;
    }

    async function navigate(request, key) {
      // A reload of the app's only page, with a new version waiting: the
      // new version takes over, and the page loads again as it.
      const waiting = registration?.waiting;
      if (waiting && key === shell) {
        const ws = await clients.matchAll({ type: "window" });
        if (ws.length < 2) {
          // (Its activation waits for this load's answer: a load again
          // that comes first is answered so again, until it's active.)
          waiting.postMessage({ asuka: "skipWaiting" });
          return new Response("", { headers: { Refresh: "0" } });
        }
      }
      try {
        const r = await fromCache(key, request);
        if (r) return r;
      } catch {}
      return fetch(request);
    }

    // A page of the version before asks for a file of it.
    async function older(request, bare) {
      const m = await readMeta();
      for (const k of [m.previous, m.current]) {
        if (!k || k === name) continue;
        try {
          const r = await (await caches.open(k)).match(bare);
          if (r) return ranged(r, request);
        } catch {}
      }
      return fetch(request);
    }

    return { name, install, installed, activate, respond };
  }

  /** clear deletes what an offline version of the app at scope kept (the app turned it off). */
  async function clear(scope, caches = g.caches) {
    const prefix = prefixOf(scope);
    for (const k of await caches.keys()) {
      if (k.startsWith(prefix) || k === metaOf(scope)) await caches.delete(k);
    }
  }

  /** plain is res as a cache answers a page's load with it: not redirected (a page's load can't take one that was). */
  async function plain(res) {
    if (!res.redirected) return res;
    return new Response(await res.blob(), { status: res.status, statusText: res.statusText, headers: res.headers });
  }

  /** ranged is r, or the part of it request's Range asks for (a video's, an audio's), as a 206. */
  async function ranged(r, request) {
    const range = request.headers?.get?.("range");
    const m = range && /^bytes=(\d*)-(\d*)$/.exec(range.trim());
    if (!m || r.status !== 200) return r;
    const blob = await r.blob();
    const size = blob.size;
    let start = m[1] === "" ? size - Number(m[2]) : Number(m[1]);
    let end = m[1] === "" || m[2] === "" ? size - 1 : Math.min(Number(m[2]), size - 1);
    if (start < 0) start = 0;
    if (start >= size || end < start) {
      return new Response("", { status: 416, headers: { "Content-Range": `bytes */${size}` } });
    }
    const headers = new Headers(r.headers);
    headers.set("Content-Range", `bytes ${start}-${end}/${size}`);
    headers.set("Content-Length", String(end - start + 1));
    return new Response(blob.slice(start, end + 1), { status: 206, statusText: "Partial Content", headers });
  }

  g.asukaOffline = { create, clear, ranged };
})();
