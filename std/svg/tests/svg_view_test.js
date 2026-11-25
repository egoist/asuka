// svg_view_test.js: std/svg's web views (web/svg.js). An icon in the
// compiler's encoding (internal/svgparse's Encode) is drawn as an <svg> of
// <path>s, its fills, strokes and currentColor as given, sized as the icon
// says; a Shape's paths are resolved in its box, again each time the box
// changes, a fade a gradient down it. e2e's TestStdWebViews runs it in
// headless Chrome: it ends by writing "PASS ..." or "FAIL ..." into the
// page's #result.
import { makeView, makeShape } from "../web/svg.js";

const failures = [];
const check = (ok, what) => {
  if (!ok) failures.push(what);
};

// What the generated glue gives a native view: a color as a CSS color.
globalThis.asuka = { native: { color: (c) => (c === 1 ? "rgb(255 0 0)" : c === 2 ? "rgb(0 0 255)" : "") } };

/** icon encodes layers as the compiler does: little-endian, a viewBox, a size, then each layer. */
function icon(viewBox, size, layers) {
  const b = [];
  const f32 = (x) => { const a = new DataView(new ArrayBuffer(4)); a.setFloat32(0, x, true); b.push(...new Uint8Array(a.buffer)); };
  const u32 = (x) => { const a = new DataView(new ArrayBuffer(4)); a.setUint32(0, x, true); b.push(...new Uint8Array(a.buffer)); };
  [...viewBox, ...size].forEach(f32);
  u32(layers.length);
  for (const l of layers) {
    for (const p of [l.fill, l.stroke]) {
      if (!p) b.push(0);
      else if (p.current) { b.push(2); f32(p.opacity); }
      else b.push(1, ...p.rgba);
    }
    f32(l.width ?? 0);
    b.push(l.cap ?? 0, l.join ?? 0);
    const ops = [];
    for (const [op, ...nums] of l.ops) {
      ops.push(op);
      for (const n of nums) { const a = new DataView(new ArrayBuffer(4)); a.setFloat32(0, n, true); ops.push(...new Uint8Array(a.buffer)); }
    }
    u32(ops.length);
    b.push(...ops);
  }
  return { data: new Uint8Array(b) };
}

// Two frames: a ResizeObserver's callback runs in the first, after layout.
const frame = () => new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r)));

async function run() {
  // An icon: a red square, then a circle stroked in currentColor at half
  // its opacity, round capped and joined.
  const v = makeView({});
  document.body.appendChild(v.el);
  v.setIcon(icon([0, 0, 24, 24], [24, 24], [
    { fill: { rgba: [255, 0, 0, 255] }, ops: [[1, 2, 2], [2, 10, 2], [2, 10, 10], [2, 2, 10], [5]] },
    { stroke: { current: true, opacity: 0.5 }, width: 2, cap: 1, join: 1, ops: [[8, 12, 12, 10, 10]] },
  ]));
  const svg = v.el.querySelector("svg");
  const paths = [...svg.querySelectorAll("path")];
  check(svg.getAttribute("viewBox") === "0 0 24 24", `viewBox ${svg.getAttribute("viewBox")}`);
  check(paths.length === 2, `${paths.length} paths, want 2`);
  check(paths[0]?.getAttribute("d") === "M 2 2 L 10 2 L 10 10 L 2 10 Z", `the square's path: ${paths[0]?.getAttribute("d")}`);
  check(paths[0]?.getAttribute("fill") === "rgb(255 0 0 / 1.000)", `the square's fill: ${paths[0]?.getAttribute("fill")}`);
  check(paths[0]?.getAttribute("stroke") === "none", `the square's stroke: ${paths[0]?.getAttribute("stroke")}`);
  check(paths[1]?.getAttribute("fill") === "none", `the circle's fill: ${paths[1]?.getAttribute("fill")}`);
  check(paths[1]?.getAttribute("stroke") === "currentColor", `the circle's stroke: ${paths[1]?.getAttribute("stroke")}`);
  check(paths[1]?.getAttribute("stroke-opacity") === "0.5", `the circle's stroke opacity: ${paths[1]?.getAttribute("stroke-opacity")}`);
  check(paths[1]?.getAttribute("stroke-linecap") === "round" && paths[1]?.getAttribute("stroke-linejoin") === "round", "the circle's cap and join aren't round");
  check(JSON.stringify(v.measure()) === "[24,24]", `measured ${JSON.stringify(v.measure())}, want the icon's 24 by 24`);
  v.setColor(1);
  check(v.el.style.color === "rgb(255, 0, 0)", `currentColor is ${v.el.style.color}, want the view's red`);
  // A new icon replaces the old one's paths.
  v.setIcon(icon([0, 0, 16, 16], [16, 16], [{ fill: { current: true, opacity: 1 }, ops: [[6, 0, 0, 16, 16]] }]));
  check(svg.querySelectorAll("path").length === 1, `${svg.querySelectorAll("path").length} paths after a new icon, want 1`);
  check(svg.querySelector("path")?.getAttribute("fill") === "currentColor", "the new icon's fill isn't currentColor");

  // A Shape: a line from the top left to 8 points in from the bottom
  // right, filled red fading to blue, in a 100 by 50 box, then 200 by 80.
  const s = makeShape({});
  s.el.style.width = "100px";
  s.el.style.height = "50px";
  document.body.appendChild(s.el);
  s.setLayers([{ path: { data: [1, 0, 0, 0, 0, 2, 1, -8, 1, -8, 5] }, fill: 1, fade: 2, stroke: null, width: 0, cap: 0n, join: 0n }]);
  await frame();
  const d = () => s.el.querySelector("path")?.getAttribute("d");
  check(d() === "M 0 0 L 92 42 Z", `in 100 by 50: ${d()}`);
  const grad = s.el.querySelector("linearGradient");
  check(grad && s.el.querySelector("path").style.fill.includes(grad.id), "the fade isn't a gradient the path fills with");
  check(grad?.getAttribute("y2") === "50", `the gradient runs to ${grad?.getAttribute("y2")}, want the box's bottom, 50`);
  s.el.style.width = "200px";
  s.el.style.height = "80px";
  await frame();
  check(d() === "M 0 0 L 192 72 Z", `in 200 by 80: ${d()}`);
  check(s.el.querySelector("linearGradient")?.getAttribute("y2") === "80", "the gradient didn't follow the box");
  s.destroy();
}

const result = document.getElementById("result");
run().then(
  () => { result.textContent = failures.length ? "FAIL\n" + failures.join("\n") : "PASS std/svg's web views"; },
  (e) => { result.textContent = "FAIL " + (e.stack || e); },
);
