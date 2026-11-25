// A double-click's second press on what its first press opened (a row that
// opens a sheet as it's pressed) goes nowhere, so it never presses that
// sheet's close or enlarge button or its backdrop, closing or enlarging what
// it just opened. A press within a double-click's time and reach of the
// last one, on a dialog or window that wasn't open at the last one, is
// swallowed: its pointerdown, pointerup, mousedown, mouseup, click and
// dblclick stop at the window, before any of the page's own listeners. A
// click with no pointer (the keyboard's, detail 0) is never swallowed, and
// only the primary pointer's presses count, so a second finger's press is
// its own. nav.js loads it for the page.

const doubleTime = 500, doubleReach = 8;
let lastPress = null; // the last press: { t, x, y, open: the dialogs and windows open then }
let swallowing = false; // the press under way goes nowhere

/** secondPress: whether press p, on layer (a dialog or window, or null), is a double-click's second press on what its first opened. */
export function secondPress(last, p, layer) {
  return !!(last && layer && !last.open.has(layer) && p.t - last.t < doubleTime && Math.hypot(p.x - last.x, p.y - last.y) < doubleReach);
}

if (typeof addEventListener === "function") {
  // A window hidden stays in the page, and isn't open
  const layers = () => new Set(document.querySelectorAll("dialog[open], .asuka-window:not([hidden])"));
  const swallow = (e) => { e.stopImmediatePropagation(); e.preventDefault(); };
  addEventListener("pointerdown", (e) => {
    if (!e.isPrimary) return;
    const p = { t: e.timeStamp, x: e.clientX, y: e.clientY };
    const layer = e.target instanceof Element ? e.target.closest("dialog, .asuka-window") : null;
    swallowing = secondPress(lastPress, p, layer);
    lastPress = { ...p, open: layers() };
    if (swallowing) swallow(e);
  }, true);
  for (const type of ["pointerup", "mousedown", "mouseup", "click", "dblclick"]) {
    addEventListener(type, (e) => {
      if (!swallowing || (type === "pointerup" && !e.isPrimary) || (type === "click" && e.detail === 0)) return;
      swallow(e);
      if (type === "dblclick") swallowing = false;
    }, true);
  }
}
