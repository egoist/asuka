// Typed native modules (//asuka:native) and views (//asuka:view) on the
// web: what the JavaScript that asuka native generates for a Go package
// calls (through globalThis.asuka.native), and what Go's calls reach,
// through its host (Host's nativeNew, nativeCall...).
//
// The wire format is core/src/native's: values in declaration order,
// little-endian. int64 and uint64 are BigInts; Go's int and uint, and the
// smaller integers, are numbers; a time is a Date; a duration is
// milliseconds; a []byte is a Uint8Array; a map is an object.

import * as appFS from "./fs.js";
import { pickFiles, download, typeOf } from "./files.js";
import { css } from "./color.js";

/** fs is the app's files (the page's own file system): readFile, writeFile, blobURL... for a package's JavaScript. */
export const fs = appFS;

/** color is a Color that crossed to a package's JavaScript (a bigint, the core's encoding) as CSS: a semantic one a variable that follows light and dark; "" for none. */
export function color(v) { return css(null, Number(v)); }

let assets = "assets";

/** setAssets is for the host: the URL the app's assets are served from. */
// The host's views by id (setViews): what a Ref passed to a package's
// JavaScript is.
let viewOf = () => null;
export function setViews(f) { viewOf = f; }

export function setAssets(url) { assets = (url || "assets").replace(/\/$/, ""); }

/** assetURL is a source as the hosts take one (a URL, or a file in the app's assets) as a URL a package can load. */
export function assetURL(src) {
  if (!src) return "";
  if (/^(https?|blob|data):/.test(src)) return src;
  if (src.startsWith("file://")) src = decodeURIComponent(src.slice(7));
  if (src.startsWith("/")) return appFS.blobURL(src);
  return assets + "/" + src.replace(/^\.?\//, "");
}

const enc = new TextEncoder();
const dec = new TextDecoder();

/** Encodes values (core/src/native's writer). */
export class NativeWriter {
  constructor() {
    this.b = new Uint8Array(64);
    this.v = new DataView(this.b.buffer);
    this.n = 0;
  }

  /** An answer's writer: its status, ok, first. */
  static ok() { return new NativeWriter().uint8(0); }

  room(k) {
    if (this.n + k <= this.b.length) return;
    const b = new Uint8Array(Math.max(this.b.length * 2, this.n + k));
    b.set(this.b.subarray(0, this.n));
    this.b = b;
    this.v = new DataView(b.buffer);
  }

  bool(x) { return this.uint8(x ? 1 : 0); }
  int8(x) { this.room(1); this.v.setInt8(this.n, x); this.n += 1; return this; }
  uint8(x) { this.room(1); this.v.setUint8(this.n, x); this.n += 1; return this; }
  int16(x) { this.room(2); this.v.setInt16(this.n, x, true); this.n += 2; return this; }
  uint16(x) { this.room(2); this.v.setUint16(this.n, x, true); this.n += 2; return this; }
  int32(x) { this.room(4); this.v.setInt32(this.n, x, true); this.n += 4; return this; }
  uint32(x) { this.room(4); this.v.setUint32(this.n, x, true); this.n += 4; return this; }
  int64(x) { this.room(8); this.v.setBigInt64(this.n, BigInt(x), true); this.n += 8; return this; }
  uint64(x) { this.room(8); this.v.setBigUint64(this.n, BigInt(x), true); this.n += 8; return this; }
  int(x) { return this.int64(Math.trunc(x)); }
  uint(x) { return this.uint64(Math.trunc(x)); }
  float32(x) { this.room(4); this.v.setFloat32(this.n, x, true); this.n += 4; return this; }
  float64(x) { this.room(8); this.v.setFloat64(this.n, x, true); this.n += 8; return this; }
  string(x) { return this.data(enc.encode(String(x))); }

  data(x) {
    this.uint32(x.length);
    this.room(x.length);
    this.b.set(x, this.n);
    this.n += x.length;
    return this;
  }

  /** A time: seconds and nanoseconds since 1970. */
  time(d) {
    const ms = d.getTime();
    const s = Math.floor(ms / 1000);
    return this.int64(s).int32(Math.round((ms - s * 1000) * 1e6));
  }

  /** A duration, in milliseconds (Go's: nanoseconds). */
  duration(ms) { return this.int64(BigInt(Math.round(ms * 1e6))); }
  /** A Decimal (std/decimal's), as its exact text ("19.99", "-1.5e3", or
   *  a number): its coefficient without trailing zeros and its exponent;
   *  more than 18 significant digits are rounded half to even. */
  decimal(x) {
    const [c, e] = decimalParts(x);
    return this.int64(c).int64(e);
  }
  len(k) { return this.uint32(k); }
  present(x) { return this.bool(x); }
  bytes() { return this.b.slice(0, this.n); }
}

/** decimalParts is a decimal's text (or a number) as std/decimal keeps it:
 *  [coefficient, exponent] as BigInts, the coefficient without trailing
 *  zeros, rounded half to even to 18 significant digits. */
export function decimalParts(x) {
  const m = /^([+-]?)(\d*)(?:\.(\d*))?(?:[eE]([+-]?\d+))?$/.exec(String(x).trim());
  if (!m || (m[2] + (m[3] ?? "")) === "") throw new TypeError(`not a decimal number: ${x}`);
  let digits = (m[2] + (m[3] ?? "")).replace(/^0+/, "");
  let exp = BigInt(m[4] ?? 0) - BigInt((m[3] ?? "").length);
  if (digits.length > 18) {
    const rest = digits.slice(18);
    let kept = BigInt(digits.slice(0, 18));
    exp += BigInt(rest.length);
    const more = /[1-9]/.test(rest.slice(1));
    if (rest[0] > "5" || (rest[0] === "5" && (more || kept % 2n === 1n))) kept += 1n;
    digits = kept.toString();
  }
  if (digits === "") return [0n, 0n];
  const t = digits.replace(/0+$/, "");
  exp += BigInt(digits.length - t.length);
  const c = BigInt(t);
  return [m[1] === "-" ? -c : c, exp];
}

/** decimalText is coefficient × 10^exponent in plain digits: "19.99". */
export function decimalText(c, e) {
  if (c === 0n) return "0";
  const sign = c < 0n ? "-" : "";
  const d = (c < 0n ? -c : c).toString();
  if (e >= 0) return sign + d + "0".repeat(e);
  const point = d.length + e;
  if (point > 0) return sign + d.slice(0, point) + "." + d.slice(point);
  return sign + "0." + "0".repeat(-point) + d;
}

/** Decodes values (core/src/native's reader); past what's there, every read is its zero value and failed is true. */
export class NativeReader {
  constructor(b, back) {
    this.b = b instanceof Uint8Array ? b : new Uint8Array(b);
    this.v = new DataView(this.b.buffer, this.b.byteOffset, this.b.byteLength);
    this.p = 0;
    this.failed = false;
    this.back = back; // the Go it's from: its callbacks' way back
  }

  take(k) {
    if (this.failed || this.b.length - this.p < k) {
      this.failed = true;
      return -1;
    }
    const at = this.p;
    this.p += k;
    return at;
  }

  bool() { return this.uint8() !== 0; }
  int8() { const at = this.take(1); return at < 0 ? 0 : this.v.getInt8(at); }
  uint8() { const at = this.take(1); return at < 0 ? 0 : this.v.getUint8(at); }
  int16() { const at = this.take(2); return at < 0 ? 0 : this.v.getInt16(at, true); }
  uint16() { const at = this.take(2); return at < 0 ? 0 : this.v.getUint16(at, true); }
  int32() { const at = this.take(4); return at < 0 ? 0 : this.v.getInt32(at, true); }
  uint32() { const at = this.take(4); return at < 0 ? 0 : this.v.getUint32(at, true); }
  int64() { const at = this.take(8); return at < 0 ? 0n : this.v.getBigInt64(at, true); }
  uint64() { const at = this.take(8); return at < 0 ? 0n : this.v.getBigUint64(at, true); }
  /** A view the app shows (an Asuka Ref), by its id: its element, or null. */
  viewRef() { const id = Number(this.uint64()); return id ? viewOf(id) : null; }
  int() { return Number(this.int64()); }
  uint() { return Number(this.uint64()); }
  float32() { const at = this.take(4); return at < 0 ? 0 : this.v.getFloat32(at, true); }
  float64() { const at = this.take(8); return at < 0 ? 0 : this.v.getFloat64(at, true); }
  string() { return dec.decode(this.data()); }

  data() {
    const k = this.uint32();
    const at = this.take(k);
    return at < 0 ? new Uint8Array(0) : this.b.slice(at, at + k);
  }

  time() {
    const s = this.int64(), ns = this.int32();
    return new Date(Number(s) * 1000 + ns / 1e6);
  }

  duration() { return Number(this.int64()) / 1e6; }

  /** A Decimal (std/decimal's), as its exact text: "19.99". */
  decimal() {
    const c = this.int64(), e = Number(this.int64());
    return decimalText(c, e);
  }

  /** A slice's or a map's length, bounded by what's left. */
  len() {
    const k = this.uint32();
    if (k > this.b.length - this.p) {
      this.failed = true;
      return 0;
    }
    return k;
  }

  present() { return this.bool(); }
  /** A callback's id (0: none). */
  callback() { return Number(this.uint64()); }

  /** Calls a Go callback that answers nothing (it runs on Go's main thread). */
  notify(id, w) { if (id) this.back.invoke(id, w.bytes()); }

  /** Calls a Go callback and reads its answer. */
  ask(id, w) { return new NativeReader(id ? (this.back.invoke(id, w.bytes()) ?? new Uint8Array(0)) : new Uint8Array(0), this.back); }

  /** Throws when a read went past what's there. */
  check() { if (this.failed) throw new Error("asuka: a malformed call"); }
}

/** An error a package declares (its enum ...: Error): the app gets that value. Generated code subclasses it. */
export class DeclaredError extends Error {
  constructor(index, message) {
    super(message);
    this.asukaIndex = index;
  }
}

/**
 * The app the page runs: its name (asuka.toml's, the host's options), for
 * a package to keep its data under, as the host keeps the app's; and
 * where its assets are served from; its version (asuka.toml's), and
 * whether it's asuka dev's build. Set as the host starts.
 */
export const app = { name: "app", assets: "assets", version: "", build: "", dev: false, icons: {}, showIcon, savedIcon };

// The page's icon (its tab's): the app's, or one of its alternate icons
// (platform/appicon), kept in localStorage for the next visit.
let ownIcon; // the page's own icon's link and address, as the page had them
const iconKey = () => "asuka." + app.name + ".appicon";

/** savedIcon is the alternate icon the app chose last, if the build still has it; "" for its own. */
export function savedIcon() {
  let n = "";
  try { n = localStorage.getItem(iconKey()) || ""; } catch {}
  return Object.hasOwn(app.icons, n) ? n : "";
}

/** showIcon shows the alternate icon named name as the page's icon ("": the app's own) and keeps the choice. */
export function showIcon(name) {
  if (name && !Object.hasOwn(app.icons, name)) return false;
  try {
    if (name) localStorage.setItem(iconKey(), name);
    else localStorage.removeItem(iconKey());
  } catch {}
  if (typeof document === "undefined") return true;
  let link = document.querySelector('link[rel="icon"]');
  ownIcon ??= { link, href: link?.getAttribute("href") ?? null, type: link?.getAttribute("type") ?? null };
  if (!name) {
    // The app's own again: the page's link as it was, or none.
    if (ownIcon.href == null) link?.remove();
    else if (link) {
      link.setAttribute("href", ownIcon.href);
      if (ownIcon.type) link.setAttribute("type", ownIcon.type);
    }
    return true;
  }
  if (!link) {
    link = document.createElement("link");
    link.rel = "icon";
    document.head.appendChild(link);
  }
  link.type = "image/png";
  link.href = app.icons[name];
  return true;
}

/**
 * The app's files, for a package that hands it some: a browser gives a
 * page files, never their paths, so what the user picks is copied into
 * the app's own file system (fs.js), whose paths the app opens.
 */
export const files = {
  /** The app's file system: readFile, writeFile, makeDir, removeFile, saveUpload, onWritten, blobURL, fileOf, clean. */
  fs: appFS,
  /** Asks for files with the browser's picker (limit 0: any number): their copies, { path, name, type, size }. */
  pick: pickFiles,
  /** Saves a File with the browser's download. */
  download,
  /** A file name's MIME type, from its extension ("" if not known). */
  typeOf,
  /**
   * Where a source loads from: an http(s), blob: or data: URL as it is,
   * an asset's path under the app's assets; null for a file of the app's
   * (a path: readFile has it).
   */
  sourceURL(src) {
    if (/^(https?|blob|data):/.test(src)) return src;
    if (src.startsWith("file://")) src = decodeURIComponent(src.slice(7));
    if (src.startsWith("/")) return null;
    return new URL(app.assets + "/" + src.replace(/^\.?\//, ""), location.href).href;
  },
  /** A source's bytes as a Blob of type: a file of the app's, an asset, a URL. */
  async blob(src, type) {
    const url = files.sourceURL(src);
    if (url == null) {
      const path = src.startsWith("file://") ? decodeURIComponent(src.slice(7)) : src;
      const b = appFS.readFile(path);
      if (!b) throw new Error("no file at " + path);
      return new Blob([b.slice()], { type });
    }
    const r = await fetch(url);
    if (!r.ok) throw new Error(`${url}: ${r.status} ${r.statusText}`);
    const blob = await r.blob();
    return type && blob.type !== type ? new Blob([blob], { type }) : blob;
  },
};

/** What the platform doesn't have: Go gets errors.ErrUnsupported. */
export class UnsupportedError extends Error {}

const providers = new Map(); // module id → provider
const objects = new Map(); // handle → { obj, provider, iface }
const calls = new Map(); // call → AbortController
let nextHandle = 0;

/** Registers a package's native code (its generated JavaScript does, as it loads). */
export function registerModule(id, provider) { providers.set(id, provider); }

/** Keeps an object for Go: its handle. */
export function retain(obj, provider, iface) {
  objects.set(++nextHandle, { obj, provider, iface });
  return nextHandle;
}

/** The object of a handle Go passed (null for 0, or one let go). */
export function object(h) { return objects.get(Number(h))?.obj ?? null; }

/** An error's answer: declared (its index), unsupported, canceled, or its name, 0 and message. */
export function answer(e) {
  const w = new NativeWriter();
  if (e instanceof DeclaredError) w.uint8(1).uint32(e.asukaIndex);
  else if (e instanceof UnsupportedError) w.uint8(3).string(e.message);
  else if (e?.name === "AbortError") w.uint8(4);
  else w.uint8(2).string(e?.name ?? "Error").int64(0).string(e?.message ?? String(e));
  return w.bytes();
}

// What Go calls, through its host; back is the host's way back to its Go.

export function makeObject(id, iface, back) {
  const p = providers.get(id);
  if (!p) return answer(new UnsupportedError(`no JavaScript ${id}: build the app with its packages' native code`));
  try {
    const obj = p.make(iface);
    return NativeWriter.ok().uint64(retain(obj, p, iface)).bytes();
  } catch (e) {
    return answer(e);
  }
}

export function call(h, method, input, back) {
  const e = objects.get(h);
  if (!e) return answer(new Error("asuka: the object is closed"));
  const w = NativeWriter.ok();
  try {
    e.provider.call(e.obj, e.iface, method, new NativeReader(input, back), w);
    return w.bytes();
  } catch (err) {
    return answer(err);
  }
}

export function start(h, method, input, id, back) {
  const e = objects.get(h);
  if (!e) {
    back.done(id, answer(new Error("asuka: the object is closed")));
    return;
  }
  const abort = new AbortController();
  calls.set(id, abort);
  // Started now, so a cancel after this returns reaches it.
  let p;
  try {
    p = Promise.resolve(e.provider.start(e.obj, e.iface, method, new NativeReader(input, back), abort.signal));
  } catch (err) {
    p = Promise.reject(err);
  }
  p.then((w) => w.bytes(), (err) => answer(abort.signal.aborted ? Object.assign(new Error("canceled"), { name: "AbortError" }) : err))
    .then((b) => {
      calls.delete(id);
      back.done(id, b);
    });
}

export function cancel(id) { calls.get(id)?.abort(); }
export function release(h) {
  const o = objects.get(h)?.obj;
  objects.delete(h);
  // An object that frees what it holds as it's let go (Swift's deinit).
  o?.released?.();
}

/** A typed view of a kind (its module's id # its name). */
export function makeView(kind, ctx) {
  const i = kind.lastIndexOf("#");
  if (i < 0) return null;
  return providers.get(kind.slice(0, i))?.makeView(kind.slice(i + 1), ctx) ?? null;
}

