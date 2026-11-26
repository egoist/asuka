// platform/pdf on the web: the browser's own PDF viewer, in an <iframe>
// that fills the view. The web has no other way to draw a PDF's pages
// without a library of its own, and the browser's viewer is the one its
// users know: its toolbar, zoom, search, print and download.
//
// The source is loaded as the browser loads any page: an asset or an
// http(s) URL as it is, a file of the app's (a path) as a Blob typed
// application/pdf (the app's files have no type, and a frame shows an
// untyped Blob as text). The page shown is the URL's #page=N, from 1,
// which the viewers read as a document loads (Chrome's and Firefox's
// do; Safari's may open at the first page). Chrome's doesn't turn to the
// page when only the fragment changes, so setting page loads the
// document again, in a new frame under the old one, which is taken away
// once the new one has loaded (from the browser's cache where the server
// lets it; a Blob is in memory).
//
// The viewer is another document, often another process's: it tells the
// page neither how many pages it has nor where the user scrolled. So
// loaded says 0 pages, once the first frame of a source has loaded, and
// no pageChange comes. What the view can learn is said with error: a
// source of the app's site that isn't there (a HEAD request asks), one of
// another site that can't be reached (a no-cors request), a file of the
// app's that isn't there or isn't a PDF.
//
// A browser with no viewer of its own (navigator.pdfViewerEnabled is
// false: most phones' browsers, which download a PDF or hand it to an
// app) shows a button in the view, "Open PDF", a link that opens it in a
// new tab, and says so with error.

const styles = `
.asuka-pdf { position: relative; width: 100%; height: 100%; overflow: hidden; }
.asuka-pdf-frame { position: absolute; inset: 0; width: 100%; height: 100%; border: 0; display: block; background: transparent; }
.asuka-pdf-fallback { position: absolute; inset: 0; display: flex; align-items: center; justify-content: center; padding: 16px; box-sizing: border-box; }
.asuka-pdf-open { display: inline-flex; align-items: center; padding: 7px 14px; border-radius: 9px; font: inherit; color: var(--asuka-accent, #0a66e0); background: color-mix(in srgb, var(--asuka-accent, #0a66e0) 15%, transparent); text-decoration: none; cursor: pointer; transition: background-color 0.15s; }
.asuka-pdf-open:hover { background: color-mix(in srgb, var(--asuka-accent, #0a66e0) 22%, transparent); }
.asuka-pdf-open:focus-visible { outline: 2px solid var(--asuka-accent, #0a66e0); outline-offset: 2px; }
`;

let styled = false;
function style() {
  if (styled || typeof document === "undefined") return;
  styled = true;
  const s = document.createElement("style");
  s.textContent = styles;
  document.head.appendChild(s);
}

const native = () => globalThis.asuka.native;

/** isFile is whether a source is a file of the app's (a path, or a file: URL) rather than an asset or a URL. */
function isFile(src) {
  return src.startsWith("/") || src.startsWith("file://");
}

/** nameOf is a source's file name, the frame's title: what a screen reader says the frame is. */
function nameOf(src) {
  const path = src.split(/[?#]/)[0].replace(/\/+$/, "");
  try {
    return decodeURIComponent(path.slice(path.lastIndexOf("/") + 1)) || "PDF";
  } catch {
    return path.slice(path.lastIndexOf("/") + 1) || "PDF";
  }
}

/** fileBlob is a file of the app's as a Blob typed application/pdf; it throws when the file isn't there or isn't a PDF. */
async function fileBlob(src) {
  const blob = await native().files.blob(src, "application/pdf");
  const head = new Uint8Array(await blob.slice(0, 1024).arrayBuffer());
  if (!new TextDecoder("latin1").decode(head).includes("%PDF-")) throw new Error("not a PDF");
  return blob;
}

/** probe asks whether url can be had: a HEAD request on the app's own site, a no-cors one on another (which only fails when it can't be reached). */
async function probe(url, signal) {
  const same = new URL(url, document.baseURI).origin === location.origin;
  if (same) {
    const r = await fetch(url, { method: "HEAD", signal });
    // A server that answers no HEAD request says nothing of the file.
    if (!r.ok && r.status !== 405 && r.status !== 501) throw new Error(`${r.status} ${r.statusText}`.trim());
  } else {
    await fetch(url, { method: "HEAD", mode: "no-cors", signal });
  }
}

export function makeView(events) {
  style();
  const el = document.createElement("div");
  el.className = "asuka-pdf";

  let source = "";
  let page = 0;
  let url = ""; // the document's URL, without the page
  let blobURL = ""; // a file of the app's, made here: revoked when it's let go
  let frame = null; // the frame shown
  let next = null; // a frame loading a new page, under the one shown
  let loaded = false; // whether loaded was said for this source
  let gen = 0; // the source's generation: what an answer that comes late was for
  let abort = null;
  let ready = Promise.resolve(); // the probe of the source: loaded waits for it

  const fail = (why) => events.error(`platform/pdf: ${source}: ${why}`);

  /** clear lets the source go: its frames, its Blob, what's still being asked. */
  function clear() {
    gen++;
    abort?.abort();
    abort = null;
    ready = Promise.resolve();
    el.replaceChildren();
    frame = next = null;
    url = "";
    loaded = false;
    if (blobURL) URL.revokeObjectURL(blobURL);
    blobURL = "";
  }

  /** show loads the document at the page in a new frame, under the one shown, which goes once it has loaded. */
  function show() {
    if (!url) return;
    const f = document.createElement("iframe");
    f.className = "asuka-pdf-frame";
    f.title = nameOf(source);
    const g = gen;
    f.addEventListener("load", () => {
      if (g !== gen || (f !== next && f !== frame)) return;
      if (f === next) {
        frame?.remove();
        frame = f;
        next = null;
      }
      ready.then(() => {
        if (g !== gen || loaded) return;
        loaded = true;
        events.loaded(0);
      }, () => {});
    });
    f.src = `${url}#page=${page + 1}`;
    if (!frame) {
      frame = f;
      el.appendChild(f);
    } else {
      next?.remove();
      next = f;
      el.insertBefore(f, frame);
    }
  }

  /** fallback is the view where the browser has no viewer: a link that opens the PDF in a new tab. */
  function fallback(href) {
    const box = document.createElement("div");
    box.className = "asuka-pdf-fallback";
    const a = document.createElement("a");
    a.className = "asuka-pdf-open";
    a.href = href;
    a.target = "_blank";
    a.rel = "noopener";
    a.textContent = "Open PDF";
    a.title = nameOf(source);
    box.appendChild(a);
    el.replaceChildren(box);
    events.error("platform/pdf: this browser has no PDF viewer: the link opens it");
  }

  async function load() {
    clear();
    if (!source) return;
    const g = gen;
    abort = new AbortController();
    const { signal } = abort;
    let href;
    if (isFile(source)) {
      try {
        const blob = await fileBlob(source);
        if (g !== gen) return;
        href = blobURL = URL.createObjectURL(blob);
      } catch (e) {
        if (g === gen) fail(/^no file/.test(e.message) ? "there's no such file" : e.message);
        return;
      }
    } else {
      href = new URL(native().assetURL(source), document.baseURI).href;
    }
    if (navigator.pdfViewerEnabled === false) {
      fallback(href);
      return;
    }
    url = href;
    if (!blobURL) {
      ready = probe(href, signal);
      ready.catch((e) => {
        if (g !== gen || signal.aborted) return;
        el.replaceChildren();
        frame = next = null;
        url = "";
        fail(e instanceof TypeError ? "it can't be reached" : e.message);
      });
    }
    show();
  }

  return {
    el,
    setSource(s) {
      if (s === source) return;
      source = s;
      load();
    },
    setPage(p) {
      p = Math.max(0, Math.trunc(Number(p)) || 0);
      if (p === page) return;
      page = p;
      show();
    },
    destroy() {
      clear();
    },
  };
}
