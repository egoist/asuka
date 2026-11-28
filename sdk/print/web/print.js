// platform/print on the web: the browser's print dialog, for markup (a
// hidden frame), a file it can show (a PDF in its viewer, an image on a
// page) or a WebView's page (its frame); it doesn't say what the user
// did: unknown. A browser makes a PDF
// only through its print dialog (Save as PDF there): pdf is unsupported.
import { Outcome, PrintError } from "./print.asuka.js";

const native = () => globalThis.asuka.native;
const viewable = (t) => /^(image|video|audio|text)\//.test(t) || ["application/pdf", "application/json", "application/xml", "application/xhtml+xml"].includes(t);
const escapeHTML = (s) => String(s).replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" })[c]);

function pageStyle(job) {
  return `@page { size: ${job.letter ? "letter" : "A4"} ${job.landscape ? "landscape" : "portrait"}; }`;
}

/**
 * printDialog prints w's page and resolves once the print dialog is gone:
 * its afterprint; where print doesn't wait for the dialog (phones), the
 * page coming back in front; and at once where no dialog showed (no
 * beforeprint within a second: printing turned off).
 */
function printDialog(w) {
  return new Promise((resolve) => {
    let opened = false, done = false;
    const finish = () => {
      if (done) return;
      done = true;
      removeEventListener("focus", back);
      document.removeEventListener("visibilitychange", back);
      resolve();
    };
    const back = () => { if (opened && !document.hidden) setTimeout(finish, 300); };
    w.addEventListener("beforeprint", () => { opened = true; }, { once: true });
    w.addEventListener("afterprint", finish, { once: true });
    w.print();
    setTimeout(() => {
      if (!opened) finish();
      else {
        addEventListener("focus", back);
        document.addEventListener("visibilitychange", back);
      }
    }, 1000);
  });
}

/** printFrame prints a hidden iframe once it's loaded (load sets it up), and answers once the dialog is gone. */
function printFrame(load) {
  return new Promise((resolve, reject) => {
    const f = document.createElement("iframe");
    f.setAttribute("aria-hidden", "true");
    f.style.cssText = "position:fixed;right:0;bottom:0;width:0;height:0;border:0;visibility:hidden";
    let settled = false;
    const finish = (err) => {
      if (settled) return;
      settled = true;
      setTimeout(() => f.remove(), 1000);
      err ? reject(err) : resolve(Outcome.unknown);
    };
    f.addEventListener("load", async () => {
      const w = f.contentWindow;
      try {
        await w.document.fonts?.ready;
        await Promise.all([...w.document.images].map((i) => (i.complete ? null : new Promise((r) => { i.onload = i.onerror = r; }))));
      } catch {}
      try {
        printDialog(w).then(() => finish());
      } catch {
        finish(new PrintError("unavailable"));
      }
    }, { once: true });
    // What to print goes in before the frame is in the page: a frame put
    // in without it loads an empty document first, and Chrome fires its
    // load at once, so the listener would print that.
    load(f);
    document.body.appendChild(f);
  });
}

function printHTML(markup, job) {
  let html = String(markup || "");
  const style = `<style>${pageStyle(job)}</style>` + (job.name ? `<title>${escapeHTML(job.name)}</title>` : "");
  html = /<head[^>]*>/i.test(html) ? html.replace(/<head[^>]*>/i, (m) => m + style) : style + html;
  return printFrame((f) => { f.srcdoc = html; });
}

async function printFile(source, job) {
  const { files } = native();
  const type = files.typeOf(source);
  if (!viewable(type) || type.startsWith("video/") || type.startsWith("audio/")) throw new PrintError("unsupported");
  const blob = await files.blob(source, type);
  const title = job.name ? `<title>${escapeHTML(job.name)}</title>` : "";
  if (type === "application/pdf") {
    // The browser's PDF viewer prints it (a page of the same origin: a blob).
    const u = URL.createObjectURL(blob);
    try {
      return await printFrame((f) => { f.src = u; });
    } finally {
      setTimeout(() => URL.revokeObjectURL(u), 60000);
    }
  }
  if (type.startsWith("image/")) {
    const u = URL.createObjectURL(blob);
    const doc = `<!doctype html><html><head>${title}<style>${pageStyle(job)} html,body{margin:0;height:100%} img{display:block;max-width:100%;max-height:100vh;margin:auto;object-fit:contain}</style></head><body><img src="${u}"></body></html>`;
    try {
      return await printFrame((f) => { f.srcdoc = doc; });
    } finally {
      setTimeout(() => URL.revokeObjectURL(u), 60000);
    }
  }
  const text = await blob.text();
  if (type === "text/html" || type === "application/xhtml+xml") return printHTML(text, job);
  const doc = `<!doctype html><html><head>${title}<style>${pageStyle(job)} pre{white-space:pre-wrap;font:12px ui-monospace,monospace}</style></head><body><pre>${escapeHTML(text)}</pre></body></html>`;
  return printFrame((f) => { f.srcdoc = doc; });
}

/**
 * printView prints what a WebView shows (el: its element). Its page, when
 * it's the app's (an HTML string's, a same-origin URL's), prints whole,
 * paginated; another site's can't be reached from the app's page, so the
 * app's page prints with only the WebView on it, as it shows.
 */
async function printView(el, job) {
  const f = el?.tagName === "IFRAME" ? el : el?.querySelector?.("iframe");
  if (!f) throw new PrintError("unsupported");
  let w = null;
  try {
    w = f.contentWindow?.document ? f.contentWindow : null;
  } catch {}
  const doc = w ? w.document : document;
  const style = doc.createElement("style");
  style.textContent = pageStyle(job) + (w ? "" :
    " @media print { body * { visibility: hidden !important; } .asuka-printing, .asuka-printing * { visibility: visible !important; }" +
    " .asuka-printing { position: fixed !important; inset: 0 !important; width: 100% !important; height: 100% !important; transform: none !important; } }");
  const title = doc.title;
  if (job.name) doc.title = job.name;
  (doc.head || doc.documentElement).appendChild(style);
  if (!w) el.classList.add("asuka-printing");
  try {
    await printDialog(w || window);
  } catch {
    throw new PrintError("unavailable");
  } finally {
    style.remove();
    el.classList.remove("asuka-printing");
    if (job.name) doc.title = title;
  }
  return Outcome.unknown;
}

export function makePrinter() {
  return {
    file: (_signal, source, job) => printFile(source, job),
    html: (_signal, markup, job) => printHTML(markup, job),
    webView: (_signal, view, job) => printView(view, job),
    async pdf(_signal, _layout) {
      throw new (native().UnsupportedError)("print: a browser makes a PDF only through its print dialog (print.html, then Save as PDF there)");
    },
  };
}
