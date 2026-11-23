// A double-click's second press on what its first press opened (a row that
// opens a sheet as it's pressed) goes nowhere, so it never closes or
// enlarges what it just opened. nav.js loads it for the page.

/**
 * The second press of a double-click lands on what the first one opened (a
 * row that opens a sheet as it's pressed), where it would press the sheet's
 * close or enlarge button, or its backdrop: closing or enlarging what it
 * just opened. A press within a double-click's time and reach of the last
 * one, on a dialog or window that wasn't open at the last one, goes
 * nowhere: its pointer and mouse events and its click stop at the window.
 */
const doubleTime = 500, doubleReach = 8;
let lastPress = null; // the last press: { t, x, y, open: the dialogs and windows open then }
let swallowing = false; // the press under way goes nowhere

/** secondPress: whether press p, on layer (a dialog or window, or null), is a double-click's second press on what its first opened. */
export function secondPress(last, p, layer) {
  return !!(last && layer && !last.open.has(layer) && p.t - last.t < doubleTime && Math.hypot(p.x - last.x, p.y - last.y) < doubleReach);
}

if (typeof addEventListener === "function") {
  const layers = () => new Set(document.querySelectorAll("dialog[open], .asuka-window"));
  addEventListener("pointerdown", (e) => {
    const p = { t: e.timeStamp, x: e.clientX, y: e.clientY };
    const layer = e.target instanceof Element ? e.target.closest("dialog, .asuka-window") : null;
    swallowing = secondPress(lastPress, p, layer);
    lastPress = { ...p, open: layers() };
    if (swallowing) { e.stopPropagation(); e.preventDefault(); }
  }, true);
  for (const type of ["pointerup", "mousedown", "mouseup", "click", "dblclick"]) {
    addEventListener(type, (e) => {
      if (!swallowing) return;
      e.stopPropagation();
      e.preventDefault();
      if (type === "dblclick") swallowing = false;
    }, true);
  }
}
