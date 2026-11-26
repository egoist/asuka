// The "view" module's "capture" (imageedit.capture): a view drawn into an
// image file in the app's caches (/tmp). A browser has no call that draws
// an element, so the element is copied with its computed styles inlined,
// its images and canvases as data URLs, into an SVG's foreignObject, which
// an image draws onto a canvas, as html-to-image does. An image of another
// site that doesn't allow it (CORS) draws blank; the page's web fonts are
// the system's there.
//
// args: u32 the view, f32 x, y, width, height of what's drawn in it (a
// width below 0: all of it), u8 format (0 JPEG, 1 PNG), f32 quality, f32
// scale (pixels per CSS pixel; 0: the screen's). Answers the file's path.

import { Reader } from "../ops.js";
import * as fs from "../fs.js";

export function modules(h) {
  return { view: { capture: (b) => capture(h, b) } };
}

async function capture(h, b) {
  const r = new Reader(b);
  const id = r.u32();
  let [x, y, w, hgt] = [r.f32(), r.f32(), r.f32(), r.f32()];
  const png = r.u8() === 1;
  const quality = r.f32();
  const scale = r.f32() || devicePixelRatio || 1;
  const el = h.nodes.get(id)?.el;
  if (!el?.isConnected) throw new Error("capture: the view isn't showing");
  const box = el.getBoundingClientRect();
  if (w < 0 || hgt < 0) [x, y, w, hgt] = [0, 0, box.width, box.height];
  if (w < 1 || hgt < 1) throw new Error("capture: the view has no size");

  const copy = await inline(el);
  copy.style.margin = "0";
  copy.style.position = "relative";
  copy.style.left = copy.style.top = "0";
  copy.style.transform = "none";
  const wrap = document.createElementNS("http://www.w3.org/1999/xhtml", "div");
  wrap.style.cssText = `width:${box.width}px;height:${box.height}px;overflow:hidden`;
  wrap.append(copy);
  const xhtml = new XMLSerializer().serializeToString(wrap);
  const svg = `<svg xmlns="http://www.w3.org/2000/svg" width="${w}" height="${hgt}" viewBox="${x} ${y} ${w} ${hgt}">` +
    `<foreignObject x="0" y="0" width="${box.width}" height="${box.height}">${xhtml}</foreignObject></svg>`;
  const img = new Image();
  img.decoding = "sync";
  const loaded = new Promise((ok, fail) => { img.onload = ok; img.onerror = () => fail(new Error("capture: the view couldn't be drawn")); });
  img.src = "data:image/svg+xml;charset=utf-8," + encodeURIComponent(svg);
  await loaded;

  const canvas = document.createElement("canvas");
  canvas.width = Math.max(1, Math.round(w * scale));
  canvas.height = Math.max(1, Math.round(hgt * scale));
  const ctx = canvas.getContext("2d");
  if (!png) {
    // JPEG has no transparency: what's under the view shows as white.
    ctx.fillStyle = "#fff";
    ctx.fillRect(0, 0, canvas.width, canvas.height);
  }
  ctx.drawImage(img, 0, 0, canvas.width, canvas.height);
  const blob = await new Promise((ok) => canvas.toBlob(ok, png ? "image/png" : "image/jpeg", quality));
  if (!blob) throw new Error("capture: the image couldn't be written");
  const path = `/tmp/asuka-capture/${crypto.randomUUID()}.${png ? "png" : "jpg"}`;
  fs.writeFile(path, new Uint8Array(await blob.arrayBuffer()));
  return path;
}

// inline copies el and what's in it, each element's computed style
// written on it, form fields' values as they are, images and canvases as
// data URLs.
async function inline(el) {
  const copy = el.cloneNode(false);
  if (el.nodeType === Node.TEXT_NODE) return copy;
  if (el.nodeType !== Node.ELEMENT_NODE) return document.createTextNode("");
  const cs = getComputedStyle(el);
  let css = "";
  for (const p of cs) css += `${p}:${cs.getPropertyValue(p)};`;
  copy.setAttribute("style", css);
  copy.removeAttribute("id");
  if (el instanceof HTMLCanvasElement) {
    const img = document.createElement("img");
    img.setAttribute("style", css);
    try { img.src = el.toDataURL(); } catch {}
    return img;
  }
  if (el instanceof HTMLImageElement && el.currentSrc) copy.src = await dataURL(el.currentSrc);
  if (el instanceof HTMLInputElement || el instanceof HTMLTextAreaElement) {
    copy.setAttribute("value", el.value);
    if (el instanceof HTMLTextAreaElement) copy.textContent = el.value;
    if (el.checked) copy.setAttribute("checked", "");
  }
  // A shadow root's or the light DOM's children, as they show.
  const kids = el.shadowRoot ? el.shadowRoot.childNodes : el.childNodes;
  for (const k of kids) {
    if (k.nodeType === Node.ELEMENT_NODE && (k.tagName === "SCRIPT" || k.tagName === "STYLE")) continue;
    copy.append(await inline(k));
  }
  return copy;
}

async function dataURL(src) {
  if (src.startsWith("data:")) return src;
  try {
    const blob = await (await fetch(src)).blob();
    return await new Promise((ok) => {
      const fr = new FileReader();
      fr.onload = () => ok(fr.result);
      fr.onerror = () => ok("");
      fr.readAsDataURL(blob);
    });
  } catch {
    return ""; // another site's, without CORS: blank
  }
}
