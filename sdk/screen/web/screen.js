// platform/screen on the web: window.screen, or, where the user granted
// window management, getScreenDetails' screens (never asked for here);
// the Screen Orientation API's lock (only for a full-screen page or an
// installed app); hiding the bars is full screen. A page can't keep
// itself out of screenshots, and isn't told of them.

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
  };
}
