// The app's files: a file system in memory, answered at once. The core
// reaches it through WASI (wasi.js: std/fs, crash records, what the C
// library opens), the host's modules directly (a camera's photo, a
// dropped file, a Save's file). What's under the app's data directory
// (/data/<the app's name>, the core's app directory) is kept in
// IndexedDB: loaded before the core starts, written back shortly after
// each change and as the page hides. /tmp is the app's caches, kept for
// the page's life: what's dropped or pasted into it, files it makes to
// share.
//
// IndexedDB keeps a small file whole, and a large one in chunks of 64
// KiB: a write stores only the chunks it changed.
//
// Several tabs of an app share its data directory: a tab writes its
// changes holding the directory's Web Lock, then tells the others on a
// BroadcastChannel which files it wrote, and they read those again (a
// file a tab changed and hasn't written yet stays its own: written whole
// once it's written, the last tab's to write it).
//
// spawn's workers (spawn.js) each have a copy (replica): the page sends
// a worker what changed since its last job with the next one, and takes
// back what the job changed, as one more writer (the last to write a
// file wins). Every change is numbered for it (the journal).

const S_IFDIR = 0o040000, S_IFREG = 0o100000;

const files = new Map(); // path -> { dir, data, size, mode, mtime, ino, chunked }
// The data directories kept in IndexedDB: an app's each (several apps
// may run on one page, each with its own), and its database.
let flushOnHide = false; // the page's listeners that write every store as it goes, added once
const stores = []; // { dir, name, db, channel, dirty: Map<path, Set<chunk> | true (all of it)>, writing: Set<path> }
const CHUNK = 64 * 1024;
let flushTimer = 0;
let onOutput = null;

const now = () => Date.now();

/** fsError is an Error with a POSIX code ("ENOENT"), as the file system's calls throw. */
export function fsError(code) {
  const e = new Error(code);
  e.code = code;
  return e;
}

/** clean is path made absolute, without . and .. and doubled slashes. */
export function clean(p) {
  if (!p.startsWith("/")) p = "/" + p;
  const parts = [];
  for (const s of p.split("/")) {
    if (!s || s === ".") continue;
    if (s === "..") parts.pop();
    else parts.push(s);
  }
  return "/" + parts.join("/");
}

const parent = (p) => p.slice(0, p.lastIndexOf("/")) || "/";

// lastInode numbers the entries, each its own inode kept as it's
// renamed, as a disk's is.
let lastInode = 0;
const newEntry = (dir, mode, data = null) => ({ dir, mode, mtime: now(), size: data ? data.length : 0, data: dir ? null : data ?? new Uint8Array(0), ino: ++lastInode });

// --- The journal: what changed, for the workers' copies ---

let version = 0;
const journal = new Map(); // path -> { v, origin }, in the order they changed
let origin = 0; // who's changing files now: 0 the page (or a worker itself)

function note(path) {
  version++;
  journal.delete(path);
  journal.set(path, { v: version, origin });
}

// --- Changing ---

function put(path, e) {
  files.set(path, e);
  changed(path);
}

// changed notes that path changed (its bytes from..to, or all of it): for
// IndexedDB, shortly, and for the workers.
function changed(path, from, to) {
  note(path);
  markDirty(path, from, to);
}

function mkdirs(p) {
  if (files.has(p)) return;
  if (p !== "/") mkdirs(parent(p));
  put(p, newEntry(true, S_IFDIR | 0o755));
}

function grow(e, need) {
  if (e.data.length >= need) return;
  const b = new Uint8Array(Math.max(need, e.data.length * 2, 64));
  b.set(e.data.subarray(0, e.size));
  e.data = b;
}

function remove(path) {
  files.delete(path);
  changed(path);
}

/**
 * fsys is what the core's WASI calls (wasi.js) do with files: paths are
 * absolute and clean; a failure throws an fsError.
 */
export const fsys = {
  /** entry is what's at path, or undefined. */
  entry: (path) => files.get(path),
  /** create makes an empty file at path (its directory must be there). */
  create(path, mode = 0o644) {
    const p = files.get(parent(path));
    if (!p) throw fsError("ENOENT");
    if (!p.dir) throw fsError("ENOTDIR");
    const e = newEntry(false, S_IFREG | (mode & 0o777));
    put(path, e);
    return e;
  },
  mkdir(path, mode = 0o755) {
    if (files.has(path)) throw fsError("EEXIST");
    const p = files.get(parent(path));
    if (!p) throw fsError("ENOENT");
    if (!p.dir) throw fsError("ENOTDIR");
    put(path, newEntry(true, S_IFDIR | (mode & 0o777)));
  },
  /** names are a directory's entries' names, sorted. */
  names(path) {
    const e = files.get(path);
    if (!e) throw fsError("ENOENT");
    if (!e.dir) throw fsError("ENOTDIR");
    const prefix = path === "/" ? "/" : path + "/";
    const out = [];
    for (const k of files.keys()) {
      if (k !== path && k.startsWith(prefix) && !k.slice(prefix.length).includes("/")) out.push(k.slice(prefix.length));
    }
    return out.sort();
  },
  unlink(path) {
    const e = files.get(path);
    if (!e) throw fsError("ENOENT");
    if (e.dir) throw fsError("EISDIR");
    remove(path);
  },
  rmdir(path) {
    const e = files.get(path);
    if (!e) throw fsError("ENOENT");
    if (!e.dir) throw fsError("ENOTDIR");
    if (path === "/") throw fsError("EBUSY");
    const prefix = path + "/";
    for (const k of files.keys()) if (k.startsWith(prefix)) throw fsError("ENOTEMPTY");
    remove(path);
  },
  /** rename moves from to to, replacing a file there (or an empty directory, for a directory), as POSIX's does. */
  rename(from, to) {
    const e = files.get(from);
    if (!e) throw fsError("ENOENT");
    if (from === to) return;
    const dst = files.get(to);
    const tp = files.get(parent(to));
    if (!tp) throw fsError("ENOENT");
    if (!tp.dir) throw fsError("ENOTDIR");
    if (e.dir && (to + "/").startsWith(from + "/")) throw fsError("EINVAL");
    if (dst) {
      if (e.dir && !dst.dir) throw fsError("ENOTDIR");
      if (!e.dir && dst.dir) throw fsError("EISDIR");
      if (dst.dir) for (const k of files.keys()) if (k.startsWith(to + "/")) throw fsError("ENOTEMPTY");
    }
    const moved = [[from, e]];
    if (e.dir) for (const [k, v] of files) if (k.startsWith(from + "/")) moved.push([k, v]);
    for (const [k] of moved) remove(k);
    for (const [k, v] of moved) put(to + k.slice(from.length), v);
    if (!e.dir) written(to);
  },
  /** write writes src into e (at path) at at, growing it: its length. */
  write(path, e, src, at) {
    grow(e, at + src.length);
    if (at > e.size) e.data.fill(0, e.size, at);
    e.data.set(src, at);
    e.size = Math.max(e.size, at + src.length);
    e.mtime = now();
    if (files.get(path) === e) changed(path, at, at + src.length);
    return src.length;
  },
  /** read reads e's bytes at at into dst: how many. */
  read(e, dst, at) {
    const n = Math.max(0, Math.min(dst.length, e.size - at));
    dst.set(e.data.subarray(at, at + n));
    return n;
  },
  resize(path, e, length) {
    grow(e, length);
    if (length > e.size) e.data.fill(0, e.size, length);
    const old = e.size;
    e.size = length;
    e.mtime = now();
    if (files.get(path) === e) changed(path, Math.min(old, length), Math.max(old, length));
  },
  /** touch sets e's modification time (milliseconds since 1970). */
  touch(path, e, mtime) {
    e.mtime = mtime;
    if (files.get(path) === e) changed(path);
  },
  /** closed says a file opened to write was closed: what's done with it once written runs (onWritten). */
  closed(path) { written(path); },
};

// --- Keeping the data directory ---

// markDirty notes that path changed (its bytes from..to, or all of it), to
// be written to IndexedDB shortly.
function markDirty(path, from, to) {
  const st = stores.find((st) => path.startsWith(st.dir + "/"));
  if (!st?.db) return;
  let d = st.dirty.get(path);
  if (from === undefined) d = true;
  else if (d !== true) {
    d ??= new Set();
    for (let i = Math.floor(from / CHUNK), last = Math.floor(Math.max(from, to - 1) / CHUNK); i <= last; i++) d.add(i);
  }
  st.dirty.set(path, d);
  if (!flushTimer) flushTimer = setTimeout(flush, 50);
}

const chunksOf = (path, from = 0) => IDBKeyRange.bound([path, from], [path, Infinity]);

/**
 * flush writes the changed files of the data directories to IndexedDB,
 * each directory's in one transaction, holding the directory's Web Lock
 * (now, without it, when urgent: the page is going away); it resolves
 * once they're written (and those before them).
 */
export function flush(urgent) {
  clearTimeout(flushTimer);
  flushTimer = 0;
  const done = [];
  for (const st of stores) {
    if (st.saving) done.push(st.saving);
    if (!st.db || !st.dirty.size) continue;
    const dirty = st.dirty;
    st.dirty = new Map();
    for (const p of dirty.keys()) st.writing.add(p);
    const locks = globalThis.navigator?.locks;
    const write = () => writeFiles(st, dirty);
    const saving = urgent === true || !locks ? write() : locks.request("asuka-files:" + st.name, write);
    st.saving = Promise.all([st.saving, saving]).then(() => {
      for (const p of dirty.keys()) st.writing.delete(p);
    });
    done.push(st.saving);
  }
  return Promise.all(done).then(() => {});
}

// writeFiles writes dirty's files as they are now, in one transaction,
// then tells the other tabs which.
function writeFiles(st, dirty) {
  const tx = st.db.transaction(["files", "chunks"], "readwrite");
  const meta = tx.objectStore("files"), chunks = tx.objectStore("chunks");
  for (const [p, d] of dirty) {
    const e = files.get(p);
    if (!e) {
      meta.delete(p);
      chunks.delete(chunksOf(p));
    } else if (e.dir || e.size <= CHUNK) {
      meta.put({ dir: !!e.dir, mode: e.mode, mtime: e.mtime, data: e.dir ? null : e.data.slice(0, e.size) }, p);
      chunks.delete(chunksOf(p));
    } else {
      const n = Math.ceil(e.size / CHUNK);
      meta.put({ dir: false, mode: e.mode, mtime: e.mtime, size: e.size, chunked: true }, p);
      const put = (i) => chunks.put(e.data.slice(i * CHUNK, Math.min(e.size, (i + 1) * CHUNK)), [p, i]);
      // A file that was small has no chunks yet: all of them.
      if (d === true || !e.chunked) for (let i = 0; i < n; i++) put(i);
      else for (const i of d) if (i < n) put(i);
      chunks.delete(chunksOf(p, n));
    }
    if (e && !e.dir) e.chunked = e.size > CHUNK;
  }
  return new Promise((resolve) => {
    tx.oncomplete = () => {
      st.channel?.postMessage({ paths: [...dirty.keys()] });
      resolve();
    };
    tx.onerror = tx.onabort = () => {
      console.warn("asuka: the app's files weren't saved:", tx.error);
      resolve();
    };
  });
}

/**
 * reload reads files another tab wrote (paths) again from IndexedDB. One
 * this tab changed and hasn't written stays as it is, to be written
 * whole (the other tab's chunks of it aren't its).
 */
async function reload(st, paths) {
  const want = [];
  for (const p of paths) {
    if (st.dirty.has(p) || st.writing.has(p)) markDirty(p);
    else want.push(p);
  }
  if (!want.length || !st.db) return;
  const got = new Map();
  try {
    const tx = st.db.transaction(["files", "chunks"], "readonly");
    const meta = tx.objectStore("files"), chunks = tx.objectStore("chunks");
    await Promise.all(want.map(async (p) => {
      const v = await req(meta.get(p));
      let parts = null;
      if (v?.chunked) parts = await Promise.all([req(chunks.getAllKeys(chunksOf(p))), req(chunks.getAll(chunksOf(p)))]);
      got.set(p, [v, parts]);
    }));
  } catch (e) {
    console.warn("asuka: another tab's files couldn't be read:", e);
    return;
  }
  for (const [p, [v, parts]] of got) {
    if (st.dirty.has(p) || st.writing.has(p)) continue; // changed here meanwhile
    const old = files.get(p);
    if (!v) {
      if (old) {
        files.delete(p);
        note(p);
      }
      continue;
    }
    let e;
    if (v.dir) {
      if (old?.dir) continue;
      e = { dir: true, mode: v.mode, mtime: v.mtime, size: 0, data: null };
    } else if (v.chunked) {
      e = { dir: false, mode: v.mode, mtime: v.mtime, size: v.size, data: new Uint8Array(v.size), chunked: true };
      parts[0].forEach(([, i], j) => { if (i * CHUNK < v.size) e.data.set(parts[1][j].subarray(0, v.size - i * CHUNK), i * CHUNK); });
    } else e = { dir: false, mode: v.mode, mtime: v.mtime, size: v.data.length, data: v.data };
    e.ino = old?.ino ?? ++lastInode;
    for (let d = parent(p); !files.has(d); d = parent(d)) {
      files.set(d, { dir: true, mode: S_IFDIR | 0o755, mtime: v.mtime, size: 0, data: null, ino: ++lastInode });
      note(d);
    }
    files.set(p, e);
    note(p);
  }
}

/**
 * mount sets the file system up before the core starts: its
 * directories, and the data directory (dir) as the app last left it,
 * from the IndexedDB database name. output, if given, gets what the host
 * logs (log).
 */
export async function mount({ name, dir = "/data", output } = {}) {
  if (output) onOutput = output;
  name ||= globalThis.location?.pathname ?? "app";
  const st = { dir, name, db: null, channel: null, dirty: new Map(), writing: new Set() };
  stores.push(st);
  for (const d of ["/", "/tmp", "/home", dir]) mkdirs(d);
  if (!globalThis.indexedDB) return;
  try {
    const db = await new Promise((resolve, reject) => {
      const req = indexedDB.open("asuka-files:" + name, 2);
      req.onupgradeneeded = () => {
        const db = req.result;
        if (!db.objectStoreNames.contains("files")) db.createObjectStore("files");
        if (!db.objectStoreNames.contains("chunks")) db.createObjectStore("chunks");
      };
      req.onsuccess = () => resolve(req.result);
      req.onerror = () => reject(req.error);
    });
    const tx = db.transaction(["files", "chunks"], "readonly");
    const meta = tx.objectStore("files"), chunks = tx.objectStore("chunks");
    const [keys, values, ckeys, cvalues] = await Promise.all([req(meta.getAllKeys()), req(meta.getAll()), req(chunks.getAllKeys()), req(chunks.getAll())]);
    keys.forEach((k, i) => {
      const v = values[i];
      const ino = ++lastInode;
      if (v.dir) files.set(k, { dir: true, mode: v.mode, mtime: v.mtime, size: 0, data: null, ino });
      else if (v.chunked) files.set(k, { dir: false, mode: v.mode, mtime: v.mtime, size: v.size, data: new Uint8Array(v.size), chunked: true, ino });
      else files.set(k, { dir: false, mode: v.mode, mtime: v.mtime, size: v.data.length, data: v.data, ino });
      note(k);
    });
    ckeys.forEach(([p, i], j) => {
      const e = files.get(p);
      if (e?.chunked && i * CHUNK < e.size) e.data.set(cvalues[j].subarray(0, e.size - i * CHUNK), i * CHUNK);
    });
    for (const k of keys) mkdirs(parent(k));
    st.db = db;
    if (globalThis.BroadcastChannel) {
      st.channel = new BroadcastChannel("asuka-files:" + name);
      st.channel.onmessage = (e) => reload(st, e.data?.paths ?? []);
    }
  } catch (e) {
    console.warn("asuka: the app's files won't be kept:", e);
  }
  if (!flushOnHide && globalThis.document) {
    flushOnHide = true;
    // Written at once as the page goes (or may go: hidden).
    addEventListener("pagehide", () => flush(true));
    document.addEventListener("visibilitychange", () => { if (document.hidden) flush(true); });
  }
}

const req = (r) => new Promise((resolve, reject) => { r.onsuccess = () => resolve(r.result); r.onerror = () => reject(r.error); });

// --- spawn's workers' copies ---

// What a worker's copy is sent: [path, entry or null (gone)], each
// entry's bytes its own.
const copyOf = (e) => (e ? { dir: e.dir, mode: e.mode, mtime: e.mtime, size: e.size, data: e.dir ? null : e.data.slice(0, e.size), ino: e.ino } : null);

/**
 * changesFor is what a worker's copy (w: { id, synced }, synced the
 * journal's number it has) lacks: the files changed since, but those it
 * changed itself (null: none); w.synced moves on. A new copy (synced 0)
 * gets every file.
 */
export function changesFor(w) {
  if (w.synced === version) return null;
  const out = [];
  for (const [p, j] of journal) {
    if (j.v <= w.synced || j.origin === w.id) continue;
    out.push([p, copyOf(files.get(p))]);
  }
  w.synced = version;
  return out.length ? out : null;
}

/** take applies the changes a worker (id) made: its files are kept and written, as the page's own. */
export function take(id, changes) {
  if (!changes?.length) return;
  const was = origin;
  origin = id;
  try {
    for (const [p, e] of changes) {
      if (e) {
        for (let d = parent(p); !files.has(d); d = parent(d)) put(d, newEntry(true, S_IFDIR | 0o755));
        e.ino = files.get(p)?.ino ?? ++lastInode;
        if (!e.dir) e.data ??= new Uint8Array(0);
        put(p, e);
        if (!e.dir) written(p);
      } else if (files.has(p)) {
        remove(p);
      }
    }
  } finally {
    origin = was;
  }
}

/** replica makes this file system a worker's copy: applying changes (the page's) as they come, keeping nothing. */
export function replica(changes) {
  for (const [p, e] of changes ?? []) {
    if (e) {
      e.ino ??= ++lastInode;
      files.set(p, e);
    } else files.delete(p);
  }
}

/** mark is the journal's number now: the worker's changes after it are its job's (since). */
export const mark = () => version;

/** since is what changed after mark m, for the page: [path, entry or null], each entry's bytes a copy of their own. */
export function since(m) {
  const out = [];
  for (const [p, j] of journal) if (j.v > m) out.push([p, copyOf(files.get(p))]);
  return out;
}

// --- For the host ---

/** log prints a line of the host's to the console, and asuka dev's terminal. */
export function log(line) {
  console.error(line);
  onOutput?.(2, line + "\n");
}

/** readFile is a file's bytes (null if there's none). */
export function readFile(path) {
  const e = files.get(clean(path));
  return e && !e.dir ? e.data.subarray(0, e.size) : null;
}

/** writeFile makes a file (and its directories). */
export function writeFile(path, bytes) {
  path = clean(path);
  mkdirs(parent(path));
  const e = newEntry(false, S_IFREG | 0o644, bytes);
  e.ino = files.get(path)?.ino ?? e.ino;
  put(path, e);
}

/** removeFile removes a file, if there's one. */
export function removeFile(path) {
  path = clean(path);
  const e = files.get(path);
  if (e && !e.dir) remove(path);
}

/** makeDir makes a directory (and those it's in). */
export function makeDir(path) {
  mkdirs(clean(path));
}

/** fileOf is a File of a file's bytes, to drag or share. */
export function fileOf(path, name, type) {
  const b = readFile(path);
  if (!b) return null;
  return new File([b], name || path.slice(path.lastIndexOf("/") + 1), { type: type || "application/octet-stream" });
}

/** blobURL is a URL a page element loads a file from (an image the app saved). */
const urls = new Map();
export function blobURL(path) {
  path = clean(path);
  const e = files.get(path);
  if (!e || e.dir) return "";
  const old = urls.get(path);
  if (old && old.mtime === e.mtime && old.size === e.size) return old.url;
  if (old) URL.revokeObjectURL(old.url);
  const url = URL.createObjectURL(new Blob([e.data.subarray(0, e.size)]));
  urls.set(path, { url, mtime: e.mtime, size: e.size });
  return url;
}

// --- Files that go somewhere once written ---

// sinks are what's done with a file once the app has written it (closed
// a file it opened to write, or renamed one over it): platform/dialog's Save
// writes it to the file the user chose, or downloads it.
const sinks = new Map(); // path -> function(bytes)
const pending = new Set();

/** onWritten calls sink with a file's bytes each time the app has written it (null: no longer). */
export function onWritten(path, sink) {
  path = clean(path);
  if (sink) sinks.set(path, sink);
  else sinks.delete(path);
}

// written notes that the app has written path: its sink runs once the
// call's done (a write's several closes and renames run it once).
function written(path) {
  if (!sinks.has(path) || pending.has(path)) return;
  pending.add(path);
  setTimeout(() => {
    pending.delete(path);
    const b = readFile(path);
    if (b) sinks.get(path)?.(b.slice());
  }, 0);
}

let uploads = 0;

/** saveUpload copies a file the user gave the app (a drop, a paste, a picker) into its caches: its path there. */
export async function saveUpload(file) {
  const bytes = new Uint8Array(await file.arrayBuffer());
  const safe = (file.name || "file").replace(/[\/\\]/g, "_");
  const path = `/tmp/shared/${++uploads}/${safe}`;
  writeFile(path, bytes);
  return path;
}
