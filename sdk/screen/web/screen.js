// platform/screen on the web: window.screen, or, where the user granted
// window management, getScreenDetails' screens (never asked for here);
// the Screen Orientation API's lock (only for a full-screen page or an
// installed app); hiding the bars is full screen. A page can't keep
// itself out of screenshots, and isn't told of them. capture is
// getDisplayMedia's picker (the user picks a screen, a window or a tab;
// the display asked for can't be chosen by a page) and one frame of what
// was picked, as a PNG in the app's caches (/tmp/asuka-screen).

import { CaptureError } from "./screen.asuka.js";

const rect = (x, y, width, height) => ({ x: x ?? 0, y: y ?? 0, width: width ?? 0, height: height ?? 0 });

/** displaysOf is screens (window.screen, or getScreenDetails' screens) as Displays: the primary one first. Without details, the one screen is the primary one. */
export function displaysOf(screens, scale, detailed) {
  const ds = screens.map((s, i) => ({
    id: detailed ? (s.label || String(i)) : "screen",
    name: detailed ? (s.label ?? "") : "",
    bounds: rect(s.left, s.top, s.width, s.height),
    workArea: rect(s.availLeft ?? s.left, s.availTop ?? s.top, s.availWidth ?? s.width, s.availHeight ?? s.height),
    scale: (detailed ? s.devicePixelRatio : scale) || 1,
    refreshRate: 0,
    primary: detailed ? !!s.isPrimary : (s.isPrimary ?? true),
  }));
  ds.sort((a, b) => (b.primary ? 1 : 0) - (a.primary ? 1 : 0));
  return ds;
}

/** screenDetails is getScreenDetails() if the user granted it, else null; never asks. */
async function screenDetails() {
  if (typeof getScreenDetails !== "function" || !navigator.permissions?.query) return null;
  try {
    const p = await navigator.permissions.query({ name: "window-management" });
    if (p.state !== "granted") return null;
    return await getScreenDetails();
  } catch {
    return null;
  }
}

/** orientationLock is the Screen Orientation API's lock for an Orientation (null: unlock). */
export function orientationLock(o) {
  return [null, "portrait", "portrait-primary", "landscape", "landscape-primary", "landscape-secondary"][o] ?? null;
}

/** captureError is what getDisplayMedia's rejection means: the picker cancelled or refused is notAllowed, no source to capture notAvailable. */
export function captureError(e) {
  switch (e?.name) {
    case "NotAllowedError": // the user cancelled the picker, the system or a permissions policy said no
    case "AbortError":
      return new CaptureError("notAllowed");
    case "NotFoundError":
    case "NotSupportedError":
    case "NotReadableError":
      return new CaptureError("notAvailable");
    case "InvalidAccessError":
    case "InvalidStateError":
      // Safari's (and older Chrome's) "not from a user gesture".
      return new Error("screen: the browser captures the screen only from a press (" + (e.message || e.name) + ")");
  }
  return e instanceof Error ? e : new Error(String(e));
}

/** aborted is a promise rejected once signal aborts. */
const aborted = (signal) => new Promise((_, reject) => {
  const no = () => reject(signal.reason ?? new DOMException("cancelled", "AbortError"));
  if (signal.aborted) no();
  else signal.addEventListener("abort", no, { once: true });
});

/** videoFrame plays stream in a <video> until it has a frame: the video, to draw. */
async function videoFrame(stream, signal) {
  const v = document.createElement("video");
  v.muted = true;
  v.playsInline = true;
  v.srcObject = stream;
  const ready = new Promise((resolve, reject) => {
    const done = () => {
      if (!v.videoWidth || !v.videoHeight) return false;
      // A frame painted, where the browser says so; else the first one decoded.
      if (v.requestVideoFrameCallback) v.requestVideoFrameCallback(() => resolve(v));
      else resolve(v);
      return true;
    };
    const check = () => { if (done()) for (const ev of evs) v.removeEventListener(ev, check); };
    const evs = ["loadeddata", "playing", "resize", "timeupdate"];
    for (const ev of evs) v.addEventListener(ev, check);
    v.addEventListener("error", () => reject(new Error("screen: the capture didn't play")), { once: true });
  });
  v.play().catch(() => {}); // a muted video plays without a gesture; "ready" says if it doesn't
  try {
    return await Promise.race([ready, aborted(signal)]);
  } catch (e) {
    v.srcObject = null;
    throw e;
  }
}

/** drawn is a canvas w by h with a frame (an ImageBitmap, a video) drawn on it. */
function drawn(src, w, h) {
  let c;
  if (typeof OffscreenCanvas !== "undefined") c = new OffscreenCanvas(w, h);
  else {
    c = document.createElement("canvas");
    c.width = w;
    c.height = h;
  }
  c.getContext("2d").drawImage(src, 0, 0, w, h);
  return c;
}

/** png is a canvas's PNG's bytes. */
async function png(c) {
  const blob = c.convertToBlob
    ? await c.convertToBlob({ type: "image/png" })
    : await new Promise((resolve) => c.toBlob(resolve, "image/png"));
  if (!blob) throw new Error("screen: can't write the capture");
  return new Uint8Array(await blob.arrayBuffer());
}

let captures = 0;

/**
 * capture asks with getDisplayMedia's picker, takes one frame of what the
 * user picked and stops sharing at once: a PNG of the app's (its path).
 * getDisplayMedia is called before anything is awaited, so a capture that
 * comes from a press asks within the press's user activation (Safari asks
 * for one; Chrome and Firefox give a press's for a few seconds).
 */
export async function capture(signal, md = globalThis.navigator?.mediaDevices) {
  if (typeof md?.getDisplayMedia !== "function") throw new CaptureError("notAvailable");
  let asked;
  try {
    // A screen, not a window or a tab, offered first: what a display shows.
    asked = md.getDisplayMedia({ video: { displaySurface: "monitor" }, audio: false, monitorTypeSurfaces: "include", surfaceSwitching: "exclude", systemAudio: "exclude" });
  } catch (e) {
    throw captureError(e);
  }
  const stopAll = (s) => { for (const t of s?.getTracks?.() ?? []) t.stop(); };
  let stream;
  try {
    // Cancelled while the picker is up: done now, and what the user then
    // picks is stopped as it comes.
    stream = await Promise.race([asked, aborted(signal)]);
  } catch (e) {
    if (signal.aborted) {
      Promise.resolve(asked).then(stopAll, () => {});
      throw e;
    }
    throw captureError(e);
  }
  const stop = () => stopAll(stream);
  // One frame, drawn before the tracks stop (a stopped track's video is
  // empty), then the sharing stops: the browser's "sharing" bar goes.
  let c;
  try {
    const track = stream.getVideoTracks()[0];
    if (!track) throw new CaptureError("notAvailable");
    let frame = null;
    if (typeof ImageCapture === "function") {
      try {
        frame = await Promise.race([new ImageCapture(track).grabFrame(), aborted(signal)]);
      } catch (e) {
        if (signal.aborted) throw e;
        frame = null; // a track ImageCapture can't grab from: the <video>'s frame
      }
    }
    if (frame) {
      try {
        c = drawn(frame, frame.width, frame.height);
      } finally {
        frame.close?.();
      }
    } else {
      const v = await videoFrame(stream, signal);
      try {
        c = drawn(v, v.videoWidth, v.videoHeight);
      } finally {
        v.srcObject = null;
      }
    }
  } finally {
    stop();
  }
  const bytes = await png(c);
  const path = `/tmp/asuka-screen/${Date.now().toString(36)}-${++captures}.png`;
  globalThis.asuka.native.files.fs.writeFile(path, bytes);
  return path;
}

let details; // a promise of ScreenDetails or null, once asked

export function makeScreens() {
  const now = async () => {
    details ??= screenDetails();
    const d = await details;
    return d ? displaysOf([...d.screens], 1, true) : displaysOf([screen], devicePixelRatio, false);
  };
  return {
    displays: () => displaysOf([screen], devicePixelRatio, false),
    async watchDisplays(signal, changed) {
      let last = "";
      const report = async () => {
        const ds = await now();
        const k = JSON.stringify(ds);
        if (k !== last) {
          last = k;
          changed(ds);
        }
      };
      await report();
      const listeners = [];
      const on = (t, e, f) => { t?.addEventListener?.(e, f); listeners.push(() => t?.removeEventListener?.(e, f)); };
      on(screen, "change", report);
      on(window, "resize", report);
      // The scale (a window moved to another display, the page zoomed).
      let q = null;
      const watchScale = () => {
        if (typeof matchMedia !== "function") return;
        q = matchMedia(`(resolution: ${devicePixelRatio}dppx)`);
        q.addEventListener?.("change", () => { report(); watchScale(); }, { once: true });
      };
      watchScale();
      const d = await details;
      if (d) {
        on(d, "screenschange", report);
        for (const s of d.screens) on(s, "change", report);
      }
      await new Promise((resolve) => signal.addEventListener("abort", resolve, { once: true }));
      for (const off of listeners) off();
    },
    lock(o) {
      const lock = orientationLock(o);
      const so = globalThis.screen?.orientation;
      if (!so?.lock) return;
      if (lock == null) so.unlock?.();
      else so.lock(lock).catch(() => {}); // a browser locks only a full-screen page, or an installed app
    },
    setBars(_style, hideStatus, hideNavigation) {
      const d = document;
      const hide = hideStatus || hideNavigation;
      if (hide && !d.fullscreenElement) d.documentElement.requestFullscreen?.({ navigationUI: "hide" })?.catch?.(() => {});
      else if (!hide && d.fullscreenElement) d.exitFullscreen?.()?.catch?.(() => {});
    },
    preventCapture(_on) {},
    // A page can't set or read the display's brightness.
    setBrightness(_level) {},
    brightness() { return -1; },
    async watchScreenshots(signal, _taken) {
      await new Promise((resolve) => signal.addEventListener("abort", resolve, { once: true }));
    },
    // The display isn't the page's to choose: the user picks in the browser's picker.
    capture: (signal, _display) => capture(signal),
  };
}
