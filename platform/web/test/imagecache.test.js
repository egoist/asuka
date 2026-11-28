// Images the host fetches (src/imagecache.js): Headers and prefetch's
// arguments decoded; which loads are the host's fetch (headers, Memory,
// None) and how (cache: "no-store" but for Default); the memory cache by
// URL, one load in flight for all, None never kept; object URLs revoked
// once nothing shows them; prefetch decoded and deduplicated; clearCache
// leaving what shows until it's let go.
import { test } from "node:test";
import assert from "node:assert/strict";
import { Writer, ImageCache } from "../src/ops.js";
import { ImageStore, decodeHeaders, decodePrefetch, fetches } from "../src/imagecache.js";

const strings = (w, list) => { w.u32(list.length); for (const s of list) w.string(s); return w; };

test("Headers and prefetch's arguments", () => {
  assert.deepEqual(decodeHeaders(strings(new Writer(), ["Authorization", "Bearer x", "X-Id", "7"]).done()), [["Authorization", "Bearer x"], ["X-Id", "7"]]);
  assert.deepEqual(decodeHeaders(new Uint8Array(0)), []);
  const b = strings(strings(new Writer(), ["https://a/1.jpg", "pin.png"]), ["Authorization", "Bearer x"]).done();
  assert.deepEqual(decodePrefetch(b), { urls: ["https://a/1.jpg", "pin.png"], headers: [["Authorization", "Bearer x"]] });
  // No headers (count 0), or cut off.
  assert.deepEqual(decodePrefetch(strings(new Writer(), ["https://a/1.jpg"]).u32(0).done()).headers, []);
  assert.deepEqual(decodePrefetch(strings(new Writer(), ["https://a/1.jpg"]).done()).headers, []);
});

test("the host fetches an image with headers, or kept out of the HTTP cache; else it's the <img>'s", () => {
  const h = [["Authorization", "x"]];
  assert.equal(fetches("https://a/1.jpg", [], ImageCache.Default), false);
  assert.equal(fetches("https://a/1.jpg", h, ImageCache.Default), true);
  assert.equal(fetches("https://a/1.jpg", [], ImageCache.Memory), true);
  assert.equal(fetches("http://a/1.jpg", [], ImageCache.None), true);
  assert.equal(fetches("blob:https://a/x", h, ImageCache.None), false); // an app's file
  assert.equal(fetches("data:image/png;base64,AA", h, ImageCache.Memory), false);
});

/** page is a fake page's fetch and object URLs: what was fetched, how, and what's revoked. */
function page() {
  const p = { fetched: [], made: 0, revoked: [], decoded: [], fail: new Set(), waits: [] };
  p.store = new ImageStore({
    fetch: (url, opts) => {
      p.fetched.push({ url, ...opts });
      return new Promise((resolve) => {
        const done = () => resolve(p.fail.has(url)
          ? { ok: false, status: 401, statusText: "Unauthorized" }
          : { ok: true, status: 200, blob: async () => ({ url }) });
        if (p.hold) p.waits.push(done); else done();
      });
    },
    createURL: (blob) => `blob:${blob.url}#${++p.made}`,
    revokeURL: (u) => p.revoked.push(u),
    decode: async (u) => { p.decoded.push(u); return { u }; },
  });
  return p;
}

test("fetched with its headers; Default from the HTTP cache, Memory and None not", async () => {
  const p = page();
  const h = [["Authorization", "Bearer demo"]];
  await p.store.load("https://a/1.jpg", h, ImageCache.Default);
  await p.store.load("https://a/2.jpg", [], ImageCache.Memory);
  await p.store.load("https://a/3.jpg", [], ImageCache.None);
  assert.deepEqual(p.fetched, [
    { url: "https://a/1.jpg", headers: h, cache: "default" },
    { url: "https://a/2.jpg", headers: [], cache: "no-store" },
    { url: "https://a/3.jpg", headers: [], cache: "no-store" },
  ]);
  assert.deepEqual([...p.store.kept.keys()], ["https://a/1.jpg", "https://a/2.jpg"]); // None isn't kept
});

test("kept by URL: a second Image of it shows it from memory; one load in flight serves both", async () => {
  const p = page();
  p.hold = true;
  const a = p.store.load("https://a/1.jpg", [], ImageCache.Memory);
  const b = p.store.load("https://a/1.jpg", [["X", "other"]], ImageCache.Default); // the same URL: the same load
  assert.equal(p.fetched.length, 1);
  p.waits.forEach((f) => f());
  const [ea, eb] = await Promise.all([a, b]);
  assert.equal(ea, eb);
  assert.equal(ea.refs, 2);
  assert.equal(p.store.cached("https://a/1.jpg"), ea);
  assert.equal(ea.refs, 3);
  assert.equal(p.store.cached("https://a/9.jpg"), null);
  // Kept: let go of by all, it isn't revoked.
  for (let i = 0; i < 3; i++) p.store.release(ea);
  assert.deepEqual(p.revoked, []);
  // None loads every time, and its URL goes when its Image lets go.
  p.hold = false;
  const n1 = await p.store.load("https://a/1.jpg", [], ImageCache.None);
  const n2 = await p.store.load("https://a/1.jpg", [], ImageCache.None);
  assert.notEqual(n1.objectURL, n2.objectURL);
  assert.equal(p.fetched.length, 3);
  p.store.release(n1);
  assert.deepEqual(p.revoked, [n1.objectURL]);
});

test("a failed load says why and isn't kept", async () => {
  const p = page();
  p.fail.add("https://a/private.jpg");
  await assert.rejects(p.store.load("https://a/private.jpg", [], ImageCache.Default), /HTTP 401 Unauthorized/);
  assert.equal(p.store.kept.size, 0);
  assert.equal(p.store.loading.size, 0);
});

test("prefetch: each URL once, with its headers, decoded in memory; a failure left out", async () => {
  const p = page();
  p.fail.add("https://a/bad.jpg");
  const h = [["Authorization", "Bearer demo"]];
  const r = await p.store.prefetch(["https://a/1.jpg", "https://a/1.jpg", "https://a/bad.jpg", "blob:https://site/file"], h);
  assert.deepEqual(r.map((x) => x.status), ["fulfilled", "rejected", "fulfilled"]);
  assert.deepEqual(p.fetched.map((f) => [f.url, f.headers]), [["https://a/1.jpg", h], ["https://a/bad.jpg", h]]);
  const e = p.store.kept.get("https://a/1.jpg");
  assert.ok(e.decoded);
  assert.equal(e.refs, 0);
  assert.deepEqual(p.decoded.sort(), [e.objectURL, "blob:https://site/file"].sort());
  // Again: nothing fetched, nothing decoded.
  await p.store.prefetch(["https://a/1.jpg"]);
  assert.equal(p.fetched.length, 2);
  assert.equal(p.decoded.length, 2);
});

test("clearCache forgets what's kept; what shows stays until its Image lets go", async () => {
  const p = page();
  const shown = await p.store.load("https://a/1.jpg", [], ImageCache.Default);
  await p.store.prefetch(["https://a/2.jpg"]);
  const idle = p.store.kept.get("https://a/2.jpg");
  // A load in flight while it clears isn't kept either.
  p.hold = true;
  const late = p.store.load("https://a/3.jpg", [], ImageCache.Default);
  p.store.clear();
  assert.deepEqual(p.revoked, [idle.objectURL]);
  assert.equal(p.store.cached("https://a/1.jpg"), null);
  p.waits.forEach((f) => f());
  const e3 = await late;
  assert.equal(p.store.kept.size, 0);
  p.store.release(shown);
  p.store.release(e3);
  assert.deepEqual(p.revoked, [idle.objectURL, shown.objectURL, e3.objectURL]);
  // Shown again: loaded again.
  p.hold = false;
  await p.store.load("https://a/1.jpg", [], ImageCache.Default);
  assert.equal(p.fetched.filter((f) => f.url === "https://a/1.jpg").length, 2);
});
