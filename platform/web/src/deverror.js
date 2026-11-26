// deverror.js: asuka dev's error overlay (the DevError command, to the
// root; ops/deverror.go): an error the app didn't catch, a module that
// didn't load, a build that failed. It's a modal <dialog> in the page's
// own elements, not the app's views (the app's tree may be what's
// broken), in the top layer over the app and its own dialogs: the title,
// the message, the lines of the source where it went wrong and the stack
// (selectable, scrolling when long), with Dismiss and, when the core
// offers it, Reload. They send DevErrorAction; the core removes it
// (DevErrorClear). A trap, which leaves no core to ask, shows in it too.

import { Reader, Events, DevErrorAction } from "./ops.js";

/** DevErrorFlags (ops/deverror.go). */
export const DevErrorReload = 1;
export const DevErrorBuild = 2;

/** decodeDevError reads a DevError command's arguments. */
export function decodeDevError(args) {
  const r = new Reader(args);
  const flags = r.u8();
  return { flags, title: r.string(), message: r.string(), source: r.string(), details: r.string() };
}

/**
 * showDevError shows e ({ flags, title, message, source, details }) in
 * container's overlay, replacing what it showed; actions maps
 * "dismiss" and "reload" (shown when there's one) to what they do. It
 * returns the overlay's <dialog>.
 */
export function showDevError(container, e, actions) {
  let d = container.querySelector(":scope > dialog.asuka-error");
  if (!d) {
    d = document.createElement("dialog");
    d.className = "asuka-error";
    d.setAttribute("aria-labelledby", "asuka-error-title");
    container.appendChild(d);
  }
  d.classList.toggle("asuka-error-build", !!(e.flags & DevErrorBuild));
  const el = (tag, cls, text) => {
    const x = document.createElement(tag);
    x.className = cls;
    if (text != null) x.textContent = text;
    return x;
  };
  const body = el("div", "asuka-error-body");
  const title = el("h2", "asuka-error-title", e.title);
  title.id = "asuka-error-title";
  body.append(title, el("p", "asuka-error-message", e.message));
  if (e.source) body.append(el("pre", "asuka-error-source", e.source));
  if (e.details) body.append(el("pre", "asuka-error-details", e.details));
  const buttons = el("div", "asuka-error-buttons");
  const button = (label, f, primary) => {
    const b = el("button", primary ? "asuka-error-primary" : "", label);
    b.type = "button";
    b.addEventListener("click", f);
    buttons.append(b);
    return b;
  };
  button("Dismiss", actions.dismiss);
  if (actions.reload) button("Reload", actions.reload, true);
  d.replaceChildren(body, buttons);
  // Escape dismisses it, as Dismiss does (the core says when it goes).
  d.oncancel = (ev) => {
    ev.preventDefault();
    actions.dismiss();
  };
  // Shown again after the app's own dialogs: the top layer's last is on top.
  if (d.open) d.close();
  d.showModal();
  return d;
}

/** hideDevError removes container's overlay, if it shows one. */
export function hideDevError(container) {
  const d = container.querySelector(":scope > dialog.asuka-error");
  if (!d) return;
  if (d.open) d.close();
  d.remove();
}

/** DevErrors is a host's overlay: DevError and DevErrorClear, its actions to the root. */
export class DevErrors {
  constructor(host) {
    this.h = host;
  }

  show(root, args) {
    const e = decodeDevError(args);
    const act = (a) => () => this.h.dispatch(Events.devErrorAction(root.id, a));
    showDevError(this.h.container, e, {
      dismiss: act(DevErrorAction.Dismiss),
      reload: e.flags & DevErrorReload ? act(DevErrorAction.Reload) : null,
    });
  }

  clear() {
    hideDevError(this.h.container);
  }
}

/**
 * showHighlight shows asuka dev's inspector's highlight (DevHighlight)
 * over a rect of el (x, y, width, height from its top left; a width below
 * 0: all of it), in the top layer so it's over the app's dialogs too, and
 * taking no clicks. It replaces the one it showed before.
 */
export function showHighlight(container, el, rect) {
  hideHighlight(container);
  if (!el?.getBoundingClientRect) return;
  const b = el.getBoundingClientRect();
  let [x, y, w, h] = rect;
  if (w < 0 || h < 0) [x, y, w, h] = [0, 0, b.width, b.height];
  const d = document.createElement("div");
  d.className = "asuka-highlight";
  d.setAttribute("aria-hidden", "true");
  d.popover = "manual";
  Object.assign(d.style, {
    position: "fixed", inset: "auto", margin: "0", padding: "0", boxSizing: "border-box",
    left: `${b.left + x}px`, top: `${b.top + y}px`, width: `${w}px`, height: `${h}px`,
    background: "rgba(10, 132, 255, 0.18)", border: "1px solid rgb(10, 132, 255)",
    pointerEvents: "none", overflow: "visible",
  });
  container.append(d);
  try { d.showPopover(); } catch { d.style.zIndex = "2147483647"; }
}

/** hideHighlight removes the highlight showHighlight shows, if any. */
export function hideHighlight(container) {
  container.querySelector(":scope > .asuka-highlight")?.remove();
}

/**
 * showPerf shows asuka dev's performance overlay (DevPerf): the core's
 * numbers (frames a second, flushes, the last flush's phases, memory) in a
 * small box over the app's top end corner, monospaced, light on a dark
 * translucent background, in the top layer (over the app's dialogs, raised
 * again at each update), taking no clicks and hidden from screen readers.
 * No text removes it.
 */
export function showPerf(container, text) {
  let d = container.querySelector(":scope > .asuka-perf");
  if (!text) {
    d?.remove();
    return;
  }
  if (!d) {
    d = document.createElement("div");
    d.className = "asuka-perf";
    d.setAttribute("aria-hidden", "true");
    d.popover = "manual";
    Object.assign(d.style, {
      position: "fixed", inset: "auto", margin: "0", boxSizing: "border-box", padding: "4px 6px",
      background: "rgba(0, 0, 0, 0.72)", color: "#fff", borderRadius: "6px", border: "0",
      font: "500 10px/1.3 ui-monospace, SFMono-Regular, Menlo, Consolas, monospace", whiteSpace: "pre",
      pointerEvents: "none", overflow: "visible", userSelect: "none",
    });
    container.append(d);
  }
  if (d.textContent !== text) d.textContent = text;
  // The container's top end corner, below the safe area's top.
  const b = container.getBoundingClientRect();
  const rtl = getComputedStyle(container).direction === "rtl";
  d.style.top = `calc(${Math.max(b.top, 0)}px + env(safe-area-inset-top, 0px) + 6px)`;
  if (rtl) {
    d.style.left = `${Math.max(b.left, 0) + 8}px`;
    d.style.right = "auto";
  } else {
    d.style.right = `${Math.max(innerWidth - b.right, 0) + 8}px`;
    d.style.left = "auto";
  }
  // Over what the app opened since.
  try {
    if (d.matches(":popover-open")) d.hidePopover();
    d.showPopover();
  } catch {
    d.style.zIndex = "2147483647";
  }
}
