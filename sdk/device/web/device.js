// platform/device on the web: the browser's platform (User-Agent Client
// Hints where there are, else the user agent), a new tab for browse, the
// browser's file picker (its files copied into the app's caches, /tmp, as
// iOS and Android give them), the async Clipboard API (browsers read the
// clipboard only for a focused page, and ask the user first; images are
// PNG, what browsers put there and take; rich text, HTML), the Screen
// Wake Lock.

const native = () => globalThis.asuka.native;
const urlish = /^\s*[a-z][a-z0-9+.-]*:\/\/\S+\s*$/i;

/** info is the browser's platform, as Info. */
export async function info(nav = navigator) {
  const ua = nav.userAgent || "";
  let system = "", version = "", model = "";
  if (/iPhone|iPod/.test(ua)) { system = "iOS"; model = "iPhone"; }
  else if (/iPad/.test(ua) || (/Macintosh/.test(ua) && nav.maxTouchPoints > 1)) { system = "iPadOS"; model = "iPad"; }
  else if (/Android/.test(ua)) system = "Android";
  else if (/CrOS/.test(ua)) { system = "ChromeOS"; model = "Chromebook"; }
  else if (/Mac/.test(ua)) { system = "macOS"; model = "Mac"; }
  else if (/Win/.test(ua)) { system = "Windows"; model = "PC"; }
  else if (/Linux/.test(ua)) { system = "Linux"; model = "PC"; }
  const m = ua.match(/OS (\d+(?:_\d+)*) like Mac OS X/) || ua.match(/Android (\d+(?:\.\d+)*)/) || ua.match(/Mac OS X (\d+(?:[._]\d+)*)/) || ua.match(/Windows NT (\d+\.\d+)/);
  if (m) version = m[1].replace(/_/g, ".");
  if (/Version\/(\d+(?:\.\d+)*).*Safari/.test(ua) && (system === "iOS" || system === "iPadOS")) version = ua.match(/Version\/(\d+(?:\.\d+)*)/)[1];
  // User-Agent Client Hints (Chromium): the real version, and an Android phone's model.
  const hints = nav.userAgentData;
  if (hints?.getHighEntropyValues) {
    try {
      const v = await hints.getHighEntropyValues(["platformVersion", "model"]);
      const p = hints.platform || "";
      if (p === "Windows" && v.platformVersion) {
        // 13 and later: Windows 11; 1 to 10: Windows 10.
        const major = parseInt(v.platformVersion, 10);
        version = major >= 13 ? "11" : major > 0 ? "10" : version;
      } else if (v.platformVersion) version = v.platformVersion;
      if (v.model) model = v.model;
    } catch {}
  }
  if (system === "Android" && !model) model = "Android";
  const app = globalThis.asuka?.native?.app ?? {};
  return {
    model: model || "Browser", system: system || nav.platform || "Web", systemVersion: version,
    appVersion: app.version || "", build: app.build || "",
  };
}

// readable reports whether the page may read the clipboard without
// asking (Chrome's permission, granted).
async function readable() {
  try {
    return (await navigator.permissions?.query({ name: "clipboard-read" }))?.state === "granted";
  } catch {
    return false;
  }
}

// denied is what a refused read throws: only when the page had the
// focus (a page without it can't read, and has nothing to say).
function denied(e) {
  if (e?.name === "NotAllowedError" && document.hasFocus()) throw new Error("device: the browser didn't allow reading the clipboard");
}

async function kinds() {
  const out = { text: false, image: false, url: false, html: false };
  try {
    if (navigator.clipboard.read) {
      for (const it of await navigator.clipboard.read()) {
        if (it.types.some((t) => t.startsWith("image/"))) out.image = true;
        if (it.types.includes("text/uri-list")) out.url = true;
        if (it.types.includes("text/html")) out.html = true;
        if (it.types.includes("text/plain")) {
          out.text = true;
          if (urlish.test(await (await it.getType("text/plain")).text())) out.url = true;
        }
      }
    } else {
      const t = await navigator.clipboard.readText();
      if (t) { out.text = true; out.url = urlish.test(t); }
    }
  } catch (e) {
    denied(e);
  }
  return out;
}

/** pngOf is a PNG of an image's bytes (as they are if they're one). */
async function pngOf(bytes, type) {
  if (type === "image/png" || (bytes[0] === 0x89 && bytes[1] === 0x50 && bytes[2] === 0x4e && bytes[3] === 0x47)) {
    return new Blob([bytes], { type: "image/png" });
  }
  let bmp;
  try {
    bmp = await createImageBitmap(new Blob([bytes], { type: type || "" }));
  } catch {
    throw new Error("device: not an image");
  }
  const c = document.createElement("canvas");
  c.width = bmp.width;
  c.height = bmp.height;
  c.getContext("2d").drawImage(bmp, 0, 0);
  bmp.close?.();
  return new Promise((resolve, reject) => c.toBlob((b) => (b ? resolve(b) : reject(new Error("device: not an image"))), "image/png"));
}

let names = 0;
let wakeLock = null, wantAwake = false, awakeWatch = false;

async function acquire() {
  const l = await navigator.wakeLock.request("screen");
  if (!wantAwake) { l.release(); return; }
  wakeLock = l;
  l.addEventListener("release", () => { if (wakeLock === l) wakeLock = null; });
}

function keepAwake(on) {
  wantAwake = on;
  if (!on) {
    const l = wakeLock;
    wakeLock = null;
    l?.release().catch(() => {});
    return;
  }
  if (!navigator.wakeLock) return; // this browser can't keep the screen on
  if (!awakeWatch) {
    awakeWatch = true;
    // The browser lets the lock go as the page hides: taken again as it shows.
    document.addEventListener("visibilitychange", () => {
      if (wantAwake && !document.hidden && !wakeLock) acquire().catch(() => {});
    });
  }
  if (!wakeLock && !document.hidden) acquire().catch(() => {});
}

export function makePlatform() {
  return {
    info: (_signal) => info(),
    async browse(_signal, url) {
      const u = new URL(url, location.href);
      if (u.protocol !== "https:" && u.protocol !== "http:") throw new Error("device: not a web URL: " + url);
      const w = window.open(u.href, "_blank");
      if (w) w.opener = null;
    },
    async pickPhotos(_signal, max) {
      const n = Number(max);
      const files = await native().files.pick(["image/*"], n);
      return files.map((f) => f.path);
    },
    async pickFiles(_signal, types, multiple) {
      const files = await native().files.pick(types, multiple ? 0 : 1);
      return files.map((f) => ({ path: f.path, name: f.name, type: f.type || "", size: f.size || 0 }));
    },
    async clipboardText(_signal) {
      try {
        const t = await navigator.clipboard.readText();
        return t === "" ? null : t;
      } catch (e) {
        denied(e);
        return null;
      }
    },
    clipboardHas: (_signal) => kinds(),
    async clipboardImage(_signal) {
      if (!navigator.clipboard.read) throw new native().UnsupportedError("device: this browser reads only text from the clipboard");
      let items;
      try {
        items = await navigator.clipboard.read();
      } catch (e) {
        denied(e);
        return null;
      }
      for (const it of items) {
        const t = it.types.find((t) => t.startsWith("image/"));
        if (!t) continue;
        const blob = await it.getType(t);
        const ext = { "image/png": "png", "image/jpeg": "jpg", "image/gif": "gif", "image/webp": "webp", "image/svg+xml": "svg" }[t] || "png";
        const path = `/tmp/asuka-clipboard/${Date.now().toString(36)}-${++names}.${ext}`;
        native().files.fs.writeFile(path, new Uint8Array(await blob.arrayBuffer()));
        return path;
      }
      return null;
    },
    async copyImage(_signal, path) {
      const bytes = native().files.fs.readFile(path);
      if (!bytes) throw new Error("device: no file at " + path);
      if (!navigator.clipboard?.write || !globalThis.ClipboardItem) throw new native().UnsupportedError("device: this browser puts only text on the clipboard");
      // A promise, as Safari wants it, so the write keeps the user's gesture.
      await navigator.clipboard.write([new ClipboardItem({ "image/png": pngOf(bytes.slice(), native().files.typeOf(path)) })]);
    },
    async clipboardHTML(_signal) {
      if (!navigator.clipboard.read) throw new native().UnsupportedError("device: this browser reads only text from the clipboard");
      let items;
      try {
        items = await navigator.clipboard.read();
      } catch (e) {
        denied(e);
        return null;
      }
      const it = items.find((it) => it.types.includes("text/html"));
      return it ? (await it.getType("text/html")).text() : null;
    },
    async copyHTML(_signal, html, text) {
      if (!navigator.clipboard?.write || !globalThis.ClipboardItem) {
        await navigator.clipboard.writeText(text); // this browser puts only text there
        return;
      }
      await navigator.clipboard.write([new ClipboardItem({
        "text/html": new Blob([html], { type: "text/html" }),
        "text/plain": new Blob([text], { type: "text/plain" }),
      })]);
    },
    // What changes it is seen, as browsers tell a page: copies and cuts in
    // it, and, where the user has let the page read the clipboard
    // (Chrome's permission), another app's copy, read again as the page is
    // focused and every second while it is.
    async watchClipboard(signal, changed) {
      let last = null;
      const signature = async () => {
        try {
          if (navigator.clipboard.read) {
            let s = "";
            for (const it of await navigator.clipboard.read()) {
              s += it.types.join(",") + ";";
              if (it.types.includes("text/plain")) {
                s += await (await it.getType("text/plain")).text();
                if (it.types.includes("text/html")) s += ";" + await (await it.getType("text/html")).text();
              } else for (const t of it.types) s += (await it.getType(t)).size + ";";
            }
            return s;
          }
          return await navigator.clipboard.readText();
        } catch {
          return null;
        }
      };
      const report = async () => changed(await kinds());
      const check = async () => {
        if (!document.hasFocus()) return;
        const s = await signature();
        if (s == null) return;
        if (last != null && s !== last) report();
        last = s;
      };
      const copied = () => setTimeout(report, 0);
      const shown = () => { if (!document.hidden) check(); };
      for (const e of ["copy", "cut"]) document.addEventListener(e, copied, true);
      addEventListener("focus", check);
      document.addEventListener("visibilitychange", shown);
      let timer = 0;
      if (await readable()) {
        last = await signature();
        timer = setInterval(check, 1000);
      }
      await new Promise((resolve) => signal.addEventListener("abort", resolve, { once: true }));
      clearInterval(timer);
      for (const e of ["copy", "cut"]) document.removeEventListener(e, copied, true);
      removeEventListener("focus", check);
      document.removeEventListener("visibilitychange", shown);
    },
    keepAwake,
    // A page has no settings of the system's to open (a site's are the
    // browser's, behind its address bar).
    openSettings: () => false,
  };
}
