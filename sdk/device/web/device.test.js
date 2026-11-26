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
