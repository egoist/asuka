// Images the host fetches itself, and the memory cache they're kept in:
// an Image with Headers (an <img> sends none of the app's) or with a
// Cache other than Default (an <img> reads and writes the browser's HTTP
// cache), and the "image" module's prefetch. Each is fetched (CORS: a
// site of its own answers for headers it's sent) into an object URL for
// the <img>; Default and Memory keep it in memory, by its URL alone, and
// one load in flight serves every Image of it; None loads it each time.
// Memory and None fetch with cache: "no-store", never reading nor writing
// the HTTP cache. A plain Image (Default, no headers) is the <img>'s own,
// unless its URL is kept here (prefetched): then it shows at once.
//
// clearCache forgets what's kept here; what an Image shows stays until
// it lets go of it (its object URL is revoked then). The browser's HTTP
// cache (the disk's) can't be cleared from a page.

import { ImageCache } from "./ops.js";
import { decodeStrings } from "./webview-util.js";

/** headerPairs are Headers' strings (name, value, in turn) as fetch's [name, value] pairs. */
export function headerPairs(strings) {
  const out = [];
  for (let i = 0; i + 1 < strings.length; i += 2) out.push([strings[i], strings[i + 1]]);
  return out;
}

/** decodeHeaders reads an Image's Headers prop (or Video's). */
export function decodeHeaders(b) { return headerPairs(decodeStrings(b)); }

/** decodePrefetch reads prefetch's arguments: u32 n, n u32-length URLs, u32 m (twice the headers), m strings. */
export function decodePrefetch(b) {
  const v = new DataView(b.buffer, b.byteOffset, b.byteLength);
  const dec = new TextDecoder();
  let p = 0;
  const strings = () => {
    const out = [];
    if (p + 4 > b.length) return out;
    const n = v.getUint32(p, true);
    p += 4;
    for (let i = 0; i < n && p + 4 <= b.length; i++) {
      const l = v.getUint32(p, true);
      p += 4;
      if (p + l > b.length) break;
      out.push(dec.decode(b.subarray(p, p + l)));
      p += l;
    }
    return out;
  };
  const urls = strings();
  return { urls, headers: headerPairs(strings()) };
}

/** web says a URL is fetched over http(s): what the caches keep. */
export const web = (url) => /^https?:/i.test(url);

/** absolute is a URL as the page resolves it (an asset's relative path), the caches' key. */
export function absolute(url) {
  try { return new URL(url, globalThis.document?.baseURI ?? globalThis.location?.href).href; } catch { return url; }
}

/** fetches says an Image's load is the host's fetch, not its <img>'s: headers to send, or a cache other than the HTTP cache. */
export function fetches(url, headers, cache) {
  return web(url) && (headers.length > 0 || cache === ImageCache.Memory || cache === ImageCache.None);
}

/** decodeImage decodes an image into memory, so it shows without waiting (prefetch). */
async function decodeImage(url) {
  const img = new Image();
  img.decoding = "async";
  img.src = url;
  await img.decode();
  return img;
}

export class ImageStore {
  /** env: the page's fetch, URL.createObjectURL and revokeObjectURL, and an image decoder (tests give their own). */
  constructor(env = {}) {
    this.fetch = env.fetch || ((u, o) => globalThis.fetch(u, o));
    this.createURL = env.createURL || ((blob) => URL.createObjectURL(blob));
    this.revokeURL = env.revokeURL || ((u) => URL.revokeObjectURL(u));
    this.decode = env.decode || decodeImage;
    this.kept = new Map(); // url → entry: the memory cache
    this.loading = new Map(); // url → a promise of its entry, in flight
    this.gen = 0; // clear's: a load from before one isn't kept
  }

  /** cached is url's entry in memory, held for its Image (null: none). */
  cached(url) {
    const e = this.kept.get(url);
    if (!e) return null;
    e.refs++;
    return e;
  }

  /**
   * load is a promise of url's entry ({ objectURL }), held for its Image
   * until release: kept in memory, or fetched with headers (pairs) under
   * cache (ImageCache), and kept unless None.
   */
  async load(url, headers = [], cache = ImageCache.Default) {
    let e;
    if (cache === ImageCache.None) {
      e = await this.get(url, headers, cache, false);
    } else {
      e = this.kept.get(url);
      if (!e) {
        let p = this.loading.get(url);
        if (!p) {
          p = this.get(url, headers, cache, true);
          this.loading.set(url, p);
          const done = () => { if (this.loading.get(url) === p) this.loading.delete(url); };
          p.then(done, done);
        }
        e = await p;
      }
    }
    e.refs++;
    return e;
  }

  async get(url, headers, cache, keep) {
    const gen = this.gen;
    const r = await this.fetch(url, { headers, cache: cache === ImageCache.Default ? "default" : "no-store" });
    if (!r.ok) throw new Error(`HTTP ${r.status}${r.statusText ? " " + r.statusText : ""}`);
    const blob = await r.blob();
    const e = { url, objectURL: this.createURL(blob), refs: 0, kept: false, decoded: null };
    if (keep && gen === this.gen) {
      e.kept = true;
      this.kept.set(url, e);
    }
    return e;
  }

  /** release lets go of an Image's entry: its object URL is revoked once nothing shows it and it isn't kept. */
  release(e) {
    if (!e) return;
    e.refs--;
    if (e.refs <= 0 && !e.kept) this.revokeURL(e.objectURL);
  }

  /**
   * prefetch loads each of urls (absolute) into memory as an Image of it
   * would, decoded, without waiting; one that fails is left out. It
   * resolves once all have settled.
   */
  prefetch(urls, headers = []) {
    return Promise.allSettled([...new Set(urls)].map(async (url) => {
      if (!web(url)) {
        await this.decode(url); // an app's file (a blob URL): decoded, as the browser keeps it
        return;
      }
      const e = await this.load(url, headers, ImageCache.Default);
      try {
        if (!e.decoded) e.decoded = await this.decode(e.objectURL);
      } finally {
        this.release(e);
      }
    }));
  }

  /** clear forgets what's kept; what Images show stays until they let go of it. */
  clear() {
    this.gen++;
    for (const e of this.kept.values()) {
      e.kept = false;
      e.decoded = null;
      if (e.refs <= 0) this.revokeURL(e.objectURL);
    }
    this.kept.clear();
    this.loading.clear();
  }
}

/** imageStore is h's store, made on its first use. */
export function imageStore(h) { return (h.imageStore ??= new ImageStore()); }
