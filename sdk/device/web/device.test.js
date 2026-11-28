// The clipboard's rich text against a stubbed async Clipboard API: what
// copyHTML writes, what clipboardHTML and clipboardHas read back, a
// browser without ClipboardItem, and a refused read.
import { test } from "node:test";
import assert from "node:assert/strict";

globalThis.asuka = { native: { UnsupportedError: class extends Error {} } };
globalThis.document = { focused: true, hasFocus() { return this.focused; } };

class ClipboardItem {
  constructor(parts) {
    this.parts = parts;
    this.types = Object.keys(parts);
  }
  async getType(t) {
    if (!(t in this.parts)) throw Object.assign(new Error(t), { name: "NotFoundError" });
    return this.parts[t];
  }
}

let items = [], refused = false;
const clipboard = {
  async read() {
    if (refused) throw Object.assign(new Error("denied"), { name: "NotAllowedError" });
    return items;
  },
  async write(list) { items = list; },
  async readText() {
    const it = items.find((it) => it.types.includes("text/plain"));
    return it ? (await it.getType("text/plain")).text() : "";
  },
  async writeText(t) { items = [new ClipboardItem({ "text/plain": new Blob([t], { type: "text/plain" }) })]; },
};
Object.defineProperty(globalThis, "navigator", { configurable: true, writable: true, value: { clipboard } });

const { makePlatform } = await import("./device.js");
const p = makePlatform();
const signal = new AbortController().signal;

test("copyHTML puts the HTML and the text; clipboardHTML reads the HTML", async () => {
  globalThis.ClipboardItem = ClipboardItem;
  await p.copyHTML(signal, "<b>Hi</b> there", "Hi there");
  assert.deepEqual(items[0].types, ["text/html", "text/plain"]);
  assert.equal(await p.clipboardHTML(signal), "<b>Hi</b> there");
  assert.equal(await p.clipboardText(signal), "Hi there");
  assert.deepEqual(await p.clipboardHas(signal), { text: true, image: false, url: false, html: true });
});

test("no HTML: none", async () => {
  await p.copyHTML(signal, "<i>x</i>", "x");
  await clipboard.writeText("plain");
  assert.equal(await p.clipboardHTML(signal), null);
  assert.equal((await p.clipboardHas(signal)).html, false);
});

test("a browser without ClipboardItem puts the text", async () => {
  delete globalThis.ClipboardItem;
  await p.copyHTML(signal, "<b>only</b> text", "only text");
  assert.deepEqual(items[0].types, ["text/plain"]);
  assert.equal(await p.clipboardHTML(signal), null);
  globalThis.ClipboardItem = ClipboardItem;
});

test("a refused read throws, as clipboardText's does, in a focused page", async () => {
  refused = true;
  try {
    await assert.rejects(p.clipboardHTML(signal), /didn't allow reading the clipboard/);
    document.focused = false;
    assert.equal(await p.clipboardHTML(signal), null);
  } finally {
    refused = false;
    document.focused = true;
  }
});

// pickMedia against a stubbed picker, decoder, canvas and <video>: sizes
// and durations, images scaled and written as JPEGs when asked, what
// isn't of the kind left out.
test("pickMedia measures, scales and filters what the picker gives", async () => {
  const files = new Map();
  const fs = {
    readFile: (p) => files.get(p) ?? null,
    writeFile: (p, b) => files.set(p, b),
    removeFile: (p) => files.delete(p),
  };
  let accepted, limit;
  const pick = [];
  const typeOf = (n) => ({ jpg: "image/jpeg", png: "image/png", mov: "video/quicktime", txt: "text/plain" })[n.split(".").pop()] ?? "";
  Object.assign(globalThis.asuka.native, {
    files: { fs, typeOf, async pick(types, n) { accepted = types; limit = n; return pick; } },
  });
  const give = (name, type, n) => {
    const path = `/tmp/shared/${pick.length + 1}/${name}`;
    files.set(path, new Uint8Array(n));
    pick.push({ path, name, type, size: n });
  };
  // Images: 4000×3000 (upright); a video: 1080×1920, 2.5 s.
  globalThis.createImageBitmap = async (blob) => {
    if (!blob.type.startsWith("image/")) throw new Error("not an image");
    return { width: 4000, height: 3000, close() {} };
  };
  const drawn = [];
  globalThis.OffscreenCanvas = class {
    constructor(w, h) { this.width = w; this.height = h; }
    getContext() { return { fillRect() {}, drawImage: (_, x, y, w, h) => drawn.push([w, h]), set fillStyle(_) {} }; }
    async convertToBlob(o) { this.quality = o.quality; return new Blob([new Uint8Array(123)], { type: o.type }); }
  };
  globalThis.URL.createObjectURL = () => "blob:x";
  globalThis.URL.revokeObjectURL = () => {};
  document.createElement = (tag) => {
    assert.equal(tag, "video");
    const el = new EventTarget();
    el.removeAttribute = () => {};
    Object.defineProperty(el, "src", {
      set() {
        Object.assign(el, { videoWidth: 1080, videoHeight: 1920, duration: 2.5 });
        setTimeout(() => el.dispatchEvent(new Event("loadedmetadata")));
      },
    });
    return el;
  };
  try {
    give("a.jpg", "image/jpeg", 5000);
    give("b.mov", "", 9000);
    give("c.txt", "text/plain", 10);

    let got = await p.pickMedia(signal, 2, 0, true, 0, 0);
    assert.deepEqual(accepted, ["image/*", "video/*"]);
    assert.equal(limit, 0);
    assert.deepEqual(got, [
      { path: "/tmp/shared/1/a.jpg", name: "a.jpg", type: "image/jpeg", video: false, width: 4000, height: 3000, durationMs: 0, size: 5000 },
      { path: "/tmp/shared/2/b.mov", name: "b.mov", type: "video/quicktime", video: true, width: 1080, height: 1920, durationMs: 2500, size: 9000 },
    ]);
    assert.ok(!files.has("/tmp/shared/3/c.txt"), "not a photo or a video: left out, and its copy gone");

    // Images only, scaled: a JPEG at most 1000 on its long side.
    pick.length = 0;
    give("a.jpg", "image/jpeg", 5000);
    got = await p.pickMedia(signal, 0, 1, false, 1000, 0.5);
    assert.deepEqual(accepted, ["image/*"]);
    assert.equal(limit, 1);
    assert.equal(got.length, 1);
    assert.match(got[0].path, /^\/tmp\/asuka-photos\/.*-a\.jpg$/);
    assert.deepEqual({ ...got[0], path: "" }, { path: "", name: "a.jpg", type: "image/jpeg", video: false, width: 1000, height: 750, durationMs: 0, size: 123 });
    assert.deepEqual(drawn, [[1000, 750]]);
    assert.ok(!files.has("/tmp/shared/1/a.jpg"), "the original's copy is replaced");

    // Videos only: given as they are, whatever maxSize says.
    pick.length = 0;
    give("b.mov", "video/quicktime", 9000);
    got = await p.pickMedia(signal, 1, 3, false, 1000, 0);
    assert.deepEqual(accepted, ["video/*"]);
    assert.equal(got[0].path, "/tmp/shared/1/b.mov");

    // Cancelled: none.
    pick.length = 0;
    assert.deepEqual(await p.pickMedia(signal, 0, 1, false, 0, 0), []);
  } finally {
    delete globalThis.createImageBitmap;
    delete globalThis.OffscreenCanvas;
  }
});
