// std/svg on the web: the icon's layers (the compiler's encoding:
// internal/svgparse's Encode) as an <svg> of <path>s the browser draws, fitted to
// the view (xMidYMid meet); currentColor is the view's color, a semantic
// one a variable that follows light and dark.
const NS = "http://www.w3.org/2000/svg";

/** decode reads an icon's data: its viewBox, size and layers (each path as SVG's path data). */
function decode(b) {
  const v = new DataView(b.buffer, b.byteOffset, b.byteLength);
  let at = 0;
  const u8 = () => (at < b.length ? b[at++] : 0);
  const u32 = () => { const x = at + 4 <= b.length ? v.getUint32(at, true) : 0; at += 4; return x; };
  const f32 = () => { const x = at + 4 <= b.length ? v.getFloat32(at, true) : 0; at += 4; return x; };
  const out = { viewBox: [0, 0, 0, 0], width: 0, height: 0, layers: [] };
  if (b.length < 24) return out;
  out.viewBox = [f32(), f32(), f32(), f32()];
  out.width = f32();
  out.height = f32();
  const paint = () => {
    switch (u8()) {
      case 1: { const r = u8(), g = u8(), bl = u8(), a = u8(); return { color: `rgb(${r} ${g} ${bl} / ${(a / 255).toFixed(3)})` }; }
      case 2: return { current: true, opacity: f32() };
      default: return null;
    }
  };
  const n = u32();
  for (let i = 0; i < n && at < b.length; i++) {
    const fill = paint(), stroke = paint(), width = f32();
    const cap = ["butt", "round", "square"][Math.min(u8(), 2)], join = ["miter", "round", "bevel"][Math.min(u8(), 2)];
    const len = u32(), end = at + len; // the path's length, then the path: at is after the length
    const d = [];
    const nums = (k) => { const a = []; for (let j = 0; j < k; j++) a.push(+f32().toFixed(4)); return a; };
    while (at < Math.min(end, b.length)) {
      switch (u8()) {
        case 1: d.push("M", ...nums(2)); break;
        case 2: d.push("L", ...nums(2)); break;
        case 3: d.push("Q", ...nums(4)); break;
        case 4: d.push("C", ...nums(6)); break;
        case 5: d.push("Z"); break;
        case 6: { const [x, y, w, h] = nums(4); d.push("M", x, y, "h", w, "v", h, "h", -w, "Z"); break; }
        case 7: {
          const [x, y, w, h, r0] = nums(5), r = Math.min(r0, w / 2, h / 2);
          d.push("M", x + r, y, "H", x + w - r, "A", r, r, 0, 0, 1, x + w, y + r, "V", y + h - r, "A", r, r, 0, 0, 1, x + w - r, y + h,
            "H", x + r, "A", r, r, 0, 0, 1, x, y + h - r, "V", y + r, "A", r, r, 0, 0, 1, x + r, y, "Z");
          break;
        }
        case 8: {
          const [x, y, w, h] = nums(4), rx = w / 2, ry = h / 2;
          d.push("M", x + w, y + ry, "A", rx, ry, 0, 1, 1, x, y + ry, "A", rx, ry, 0, 1, 1, x + w, y + ry, "Z");
          break;
        }
        default: at = end;
      }
    }
    at = end;
    out.layers.push({ fill, stroke, width, cap, join, d: d.join(" ") });
  }
  return out;
}

const paintOf = (p) => (p ? (p.current ? "currentColor" : p.color) : "none");

export function makeView(events) {
  const el = document.createElement("div");
  el.style.cssText = "display:flex;line-height:0";
  const svg = document.createElementNS(NS, "svg");
  svg.setAttribute("preserveAspectRatio", "xMidYMid meet");
  svg.style.cssText = "width:100%;height:100%;overflow:visible";
  el.appendChild(svg);
  let size = [0, 0];
  return {
    el,
    setIcon(icon) {
      const ic = decode(icon.data);
      size = [ic.width, ic.height];
      svg.setAttribute("viewBox", ic.viewBox.join(" "));
      svg.textContent = "";
      for (const l of ic.layers) {
        const p = document.createElementNS(NS, "path");
        p.setAttribute("d", l.d);
        p.setAttribute("fill", paintOf(l.fill));
        if (l.fill?.current && l.fill.opacity < 1) p.setAttribute("fill-opacity", l.fill.opacity);
        p.setAttribute("stroke", paintOf(l.stroke));
        if (l.stroke) {
          p.setAttribute("stroke-width", l.width);
          p.setAttribute("stroke-linecap", l.cap);
          p.setAttribute("stroke-linejoin", l.join);
          if (l.stroke.current && l.stroke.opacity < 1) p.setAttribute("stroke-opacity", l.stroke.opacity);
        }
        svg.appendChild(p);
      }
    },
    setColor(c) { el.style.color = globalThis.asuka.native.color(c) || "var(--asuka-label)"; },
    measure() { return size; },
  };
}

// Shape: the app's paths, as <path>s whose data is resolved in the
// view's size (a point is a fraction of the box plus points), again
// each time the box changes.

/** shapeData is a Shape path's data as SVG's, in a box of w by h. */
function shapeData(d, w, h) {
  const out = [];
  let i = 0;
  const pt = () => {
    if (i + 4 > d.length) { i = d.length; return [0, 0]; }
    const p = [+(d[i] * w + d[i + 1]).toFixed(3), +(d[i + 2] * h + d[i + 3]).toFixed(3)];
    i += 4;
    return p;
  };
  while (i < d.length) {
    const op = d[i++];
    switch (op) {
      case 1: out.push("M", ...pt()); break;
      case 2: out.push("L", ...pt()); break;
      case 3: out.push("Q", ...pt(), ...pt()); break;
      case 4: out.push("C", ...pt(), ...pt(), ...pt()); break;
      case 5: out.push("Z"); break;
      case 6: case 7: {
        const [ax, ay] = pt(), [bx, by] = pt();
        const x = Math.min(ax, bx), y = Math.min(ay, by), rw = Math.abs(bx - ax), rh = Math.abs(by - ay);
        if (op === 6) {
          const r = Math.max(0, Math.min(i < d.length ? d[i] : 0, rw / 2, rh / 2));
          i++;
          out.push("M", x + r, y, "H", x + rw - r, "A", r, r, 0, 0, 1, x + rw, y + r, "V", y + rh - r, "A", r, r, 0, 0, 1, x + rw - r, y + rh,
            "H", x + r, "A", r, r, 0, 0, 1, x, y + rh - r, "V", y + r, "A", r, r, 0, 0, 1, x + r, y, "Z");
        } else {
          const rx = rw / 2, ry = rh / 2;
          out.push("M", x + rw, y + ry, "A", rx, ry, 0, 1, 1, x, y + ry, "A", rx, ry, 0, 1, 1, x + rw, y + ry, "Z");
        }
        break;
      }
      default: return out.join(" ");
    }
  }
  return out.join(" ");
}

let shapes = 0;

export function makeShape(events) {
  const el = document.createElement("div");
  el.style.cssText = "display:flex;line-height:0";
  const svg = document.createElementNS(NS, "svg");
  svg.style.cssText = "width:100%;height:100%;overflow:visible";
  el.appendChild(svg);
  const id = `asuka-shape-${++shapes}`;
  let layers = [];
  let size = [0, 0];
  const color = (c) => globalThis.asuka.native.color(c) || "currentColor";
  const draw = () => {
    const [w, h] = size;
    svg.textContent = "";
    svg.setAttribute("viewBox", `0 0 ${w || 1} ${h || 1}`);
    const defs = document.createElementNS(NS, "defs");
    layers.forEach((l, n) => {
      const p = document.createElementNS(NS, "path");
      p.setAttribute("d", shapeData(l.path.data, w, h));
      if (l.fill != null && l.fade != null) {
        const g = document.createElementNS(NS, "linearGradient");
        g.id = `${id}-${n}`;
        g.setAttribute("gradientUnits", "userSpaceOnUse");
        for (const [k, v] of [["x1", 0], ["y1", 0], ["x2", 0], ["y2", h]]) g.setAttribute(k, v);
        for (const [off, c] of [[0, l.fill], [1, l.fade]]) {
          const s = document.createElementNS(NS, "stop");
          s.setAttribute("offset", off);
          s.style.stopColor = color(c); // a style: a semantic color is a CSS variable
          g.appendChild(s);
        }
        defs.appendChild(g);
        p.style.fill = `url(#${g.id})`;
      } else {
        p.style.fill = l.fill != null ? color(l.fill) : "none";
      }
      if (l.stroke != null && l.width > 0) {
        p.style.stroke = color(l.stroke);
        p.setAttribute("stroke-width", l.width);
        p.setAttribute("stroke-linecap", ["butt", "round", "square"][Math.min(Number(l.cap), 2)]);
        p.setAttribute("stroke-linejoin", ["miter", "round", "bevel"][Math.min(Number(l.join), 2)]);
      }
      svg.appendChild(p);
    });
    if (defs.firstChild) svg.prepend(defs);
  };
  const ro = new ResizeObserver(() => {
    const next = [el.clientWidth, el.clientHeight];
    if (next[0] !== size[0] || next[1] !== size[1]) {
      size = next;
      draw();
    }
  });
  ro.observe(el);
  return {
    el,
    setLayers(v) { layers = v; draw(); },
    destroy() { ro.disconnect(); },
  };
}
