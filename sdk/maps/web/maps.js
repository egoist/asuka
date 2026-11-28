// platform/maps on the web: OpenStreetMap's tiles, drawn by the package
// (as on Windows and Linux): a Web Mercator view of the tiles, panned,
// zoomed and turned with the pointer, the wheel and the keyboard; the
// overlays (lines, shapes, circles) in an SVG over the tiles; the markers
// (pins, images, clusters) and the user's location over those. Its
// stylesheet is put in the page once.
//
// The tiles come from tile.openstreetmap.org (the start option mapTiles:
// another server's URL, with {z}, {x} and {y}) as its usage policy asks
// of a web page: the browser sends its Referer and keeps the tiles as the
// server says (its HTTP cache), only what's on screen is asked for (a
// tile that leaves the view before it comes is let go), and the
// attribution "© OpenStreetMap contributors", linking to its copyright
// page, is shown on the map. While a tile comes, one above it that's
// here is drawn bigger.
//
// Satellite and Hybrid have no free imagery source: they show the
// streets; Muted shows them in quieter colors. The map turns (heading:
// two fingers, Shift and a drag, the compass to face north again) but
// doesn't tilt: its pitch is always 0. The user moves it with a drag,
// zooms with the wheel (a trackpad's pinch comes as Ctrl and the wheel),
// a finger's pinch, a double click (Shift: out), and + and -; the arrow
// keys move it. A right click or a long touch is a long press. It has no
// size of its own: its style gives it one.
//
// Clustering is the same greedy grid the desktop's C (maps_geo.c) does,
// so a map clusters alike on Windows, Linux and the web: cluster below.
import { MapType } from "./maps.decl.asuka.js";

const styles = `
.asuka-map { position: relative; overflow: hidden; background: #e8e4de; touch-action: none; user-select: none; -webkit-user-select: none; cursor: grab; outline: none; contain: strict; }
.asuka-map:focus-visible { box-shadow: inset 0 0 0 2px var(--asuka-accent); }
.asuka-map.asuka-map-static { cursor: default; touch-action: auto; }
.asuka-map-stage { position: absolute; left: 0; top: 0; pointer-events: none; transform-origin: 50% 50%; }
.asuka-map-tiles, .asuka-map-shapes, .asuka-map-pins { position: absolute; inset: 0; pointer-events: none; }
.asuka-map-shapes { overflow: visible; }
.asuka-map-muted .asuka-map-tiles { filter: saturate(0.35) brightness(1.04); }
.asuka-map-tile { position: absolute; overflow: hidden; }
.asuka-map-img, .asuka-map-fallback { position: absolute; display: block; max-width: none; -webkit-user-drag: none; }
.asuka-map-img { inset: 0; width: 100%; height: 100%; opacity: 0; transition: opacity 0.15s; }
.asuka-map-img.asuka-map-loaded { opacity: 1; }
.asuka-map-pin { position: absolute; left: 0; top: 0; padding: 0; border: 0; background: none; cursor: pointer; pointer-events: auto; touch-action: none; }
.asuka-map-pin.asuka-map-drag { cursor: move; }
.asuka-map-pin.asuka-map-lifted { filter: drop-shadow(0 6px 4px rgb(0 0 0 / 0.35)); }
.asuka-map-pin svg { display: block; filter: drop-shadow(0 1px 1px rgb(0 0 0 / 0.3)); }
.asuka-map-pin img { display: block; width: 100%; height: 100%; -webkit-user-drag: none; pointer-events: none; }
.asuka-map-pin:focus-visible { outline: 2px solid var(--asuka-accent); outline-offset: 2px; border-radius: 6px; }
.asuka-map-cluster { position: absolute; left: 0; top: 0; padding: 0; border: 3px solid #fff; border-radius: 50%; box-sizing: border-box; color: #fff; font: 600 13px/1 system-ui, sans-serif; cursor: pointer; pointer-events: auto; box-shadow: 0 1px 3px rgb(0 0 0 / 0.35); display: flex; align-items: center; justify-content: center; }
.asuka-map-cluster:focus-visible { outline: 2px solid var(--asuka-accent); outline-offset: 2px; }
.asuka-map-callout { position: absolute; left: 0; top: 0; max-width: 240px; padding: 8px 12px; border-radius: 10px; background: var(--asuka-card); color: var(--asuka-label); box-shadow: var(--asuka-shadow); font: 13px/1.35 system-ui, sans-serif; pointer-events: auto; cursor: pointer; white-space: normal; z-index: 1000000; }
.asuka-map-callout-title { font-weight: 600; }
.asuka-map-callout-subtitle { color: var(--asuka-secondary-label); }
.asuka-map-user { position: absolute; left: 0; top: 0; width: 0; height: 0; z-index: 999999; }
.asuka-map-dot { position: absolute; left: -8px; top: -8px; width: 16px; height: 16px; border-radius: 50%; background: var(--asuka-blue); border: 3px solid #fff; box-sizing: border-box; box-shadow: 0 1px 3px rgb(0 0 0 / 0.35); }
.asuka-map-accuracy { position: absolute; transform: translate(-50%, -50%); border-radius: 50%; background: rgb(0 122 255 / 0.14); border: 1px solid rgb(0 122 255 / 0.35); box-sizing: border-box; }
.asuka-map-compass { position: absolute; right: 8px; top: 8px; width: 34px; height: 34px; padding: 0; border: 0; border-radius: 50%; background: rgb(255 255 255 / 0.9); box-shadow: 0 1px 3px rgb(0 0 0 / 0.3); cursor: pointer; }
.asuka-map-compass svg { display: block; width: 100%; height: 100%; }
.asuka-map-attribution { position: absolute; right: 0; bottom: 0; padding: 1px 6px; border-top-left-radius: 4px; background: rgb(255 255 255 / 0.8); color: #333; font: 11px/1.45 system-ui, sans-serif; text-decoration: none; cursor: pointer; }
.asuka-map-attribution:hover { text-decoration: underline; }
`;

let styled = false;
function style() {
  if (styled || typeof document === "undefined") return;
  styled = true;
  const s = document.createElement("style");
  s.textContent = styles;
  document.head.appendChild(s);
}

const native = () => globalThis.asuka?.native;

/** colorCSS is a Color (the core's encoding) as CSS: the runtime's, which knows the semantic ones; "" for none. */
export function colorCSS(v) {
  if (v == null) return "";
  const f = native()?.color;
  if (f) return f(v);
  const n = Number(v), kind = Math.floor(n / 4294967296), lo = n % 4294967296;
  if (kind !== 2) return "";
  const r = (lo >>> 24) & 255, g = (lo >>> 16) & 255, b = (lo >>> 8) & 255, a = lo & 255;
  return `rgb(${r} ${g} ${b} / ${(a / 255).toFixed(3)})`;
}

function imageURL(src) {
  const f = native()?.assetURL;
  return f ? f(src) : src;
}

// The maps on the page, by their elements: what a Ref passed to Maps'
// methods finds.
const views = new WeakMap();

function find(el) {
  if (!el) return null;
  let v = views.get(el);
  if (v) return v;
  const inner = el.querySelector?.(".asuka-map");
  if (inner && (v = views.get(inner))) return v;
  const outer = el.closest?.(".asuka-map");
  return outer ? views.get(outer) ?? null : null;
}

/** makeMaps is the Maps interface: what moves the map a Ref holds. */
export function makeMaps() {
  return {
    moveTo(view, region, animated) { find(view)?.showRegion(region, animated); },
    moveCamera(view, camera, animated) { find(view)?.showCamera(camera, animated); },
    fit(view, coordinates, padding, animated) { find(view)?.fit(coordinates, padding, animated); },
    camera(view) {
      return find(view)?.camera() ?? { center: { lat: 0, lng: 0 }, zoom: 0, heading: 0, pitch: 0 };
    },
  };
}

export function makeView(events) {
  style();
  const v = new MapView(events, globalThis.asuka?.mapTiles);
  return {
    el: v.el,
    setRegion(r) { v.setRegion(r); },
    setCamera(c) { v.setCamera(c); },
    setMarkers(ms) { v.setMarkers(ms); },
    setPolylines(ls) { v.setShapes("polylines", ls); },
    setPolygons(ps) { v.setShapes("polygons", ps); },
    setCircles(cs) { v.setShapes("circles", cs); },
    setSelected(id) { v.setSelected(id); },
    setClustering(on) { v.setClustering(on); },
    setClusterColor(c) { v.setClusterColor(c); },
    setMapType(t) { v.el.classList.toggle("asuka-map-muted", t === MapType.muted); },
    setShowsUser(on) { v.setShowsUser(on); },
    setInteractive(on) { v.setInteractive(on); },
    destroy() { v.destroy(); },
  };
}

// ---------------------------------------------------------------------
// Geometry: what's worked out without a page (and tested so).

const tileSize = 256;
const maxZoom = 19;
const earthRadius = 6371008.8; // meters: the mean radius
const rad = Math.PI / 180;

export function mercY(lat) {
  lat = Math.max(Math.min(lat, 85.05112878), -85.05112878) * rad;
  return (1 - Math.log(Math.tan(lat) + 1 / Math.cos(lat)) / Math.PI) / 2;
}

export function mercLat(y) {
  return Math.atan(Math.sinh(Math.PI * (1 - 2 * y))) / rad;
}

/** normalLng is a longitude in [-180, 180). */
export function normalLng(lng) {
  lng = ((lng + 180) % 360 + 360) % 360 - 180;
  return lng === 180 ? -180 : lng;
}

/** zoomFor is the zoom that shows region r in a view w×h points wide (at most maxZoom). */
export function zoomFor(r, w, h) {
  let z = maxZoom;
  if (r.lngDelta > 0) z = Math.min(z, Math.log2((w * 360) / (tileSize * r.lngDelta)));
  if (r.latDelta > 0) {
    const span = Math.abs(mercY(r.center.lat + r.latDelta / 2) - mercY(r.center.lat - r.latDelta / 2));
    if (span > 0) z = Math.min(z, Math.log2(h / (tileSize * span)));
  }
  return Math.max(z, 0);
}

/** tileURL is a tile's URL from a template ({z}, {x}, {y}). */
export function tileURL(template, z, x, y) {
  return template.replace("{z}", z).replace("{x}", x).replace("{y}", y);
}

/**
 * cluster groups points ({id, lat, lng}, in their order) as the map shows
 * them at zoom: at its whole level L, each point not yet taken takes the
 * ones after it within 40 points across and down (round the world the
 * short way). A group of one is the point alone ({ids: [id], count: 1,
 * lat, lng} as it is); of more, a cluster at their mean (in the level's
 * pixels), its longitude in [-180, 180). The groups come in the order of
 * their first points. maps_geo.c's maps_cluster is the same, to the bit.
 */
export function cluster(points, zoom) {
  const L = Math.max(0, Math.min(22, Math.floor(zoom + 1e-6)));
  const W = tileSize * 2 ** L;
  const R = 40;
  const n = points.length;
  const X = new Float64Array(n), Y = new Float64Array(n);
  for (let i = 0; i < n; i++) {
    X[i] = ((points[i].lng + 180) / 360) * W;
    Y[i] = mercY(points[i].lat) * W;
  }
  // A grid of R-sized cells, for the points near one: those in the 3×3
  // cells around it, taken in their order.
  const cell = new Map();
  const cols = Math.max(1, Math.floor(W / R));
  const key = (cx, cy) => cy * (cols + 2) + ((cx % cols) + cols) % cols;
  for (let i = 0; i < n; i++) {
    const k = key(Math.floor(X[i] / R), Math.floor(Y[i] / R));
    let c = cell.get(k);
    if (!c) cell.set(k, (c = []));
    c.push(i);
  }
  const taken = new Uint8Array(n);
  const out = [];
  for (let i = 0; i < n; i++) {
    if (taken[i]) continue;
    taken[i] = 1;
    const members = [i];
    const dxs = [0];
    const cx = Math.floor(X[i] / R), cy = Math.floor(Y[i] / R);
    const near = [];
    const seen = new Set();
    for (let oy = -1; oy <= 1; oy++) {
      for (let ox = -1; ox <= 1; ox++) {
        const k = key(cx + ox, cy + oy);
        if (seen.has(k)) continue;
        seen.add(k);
        const c = cell.get(k);
        if (c) for (const j of c) if (j > i && !taken[j]) near.push(j);
      }
    }
    // A level of fewer than 3 cells across: the grid can't say who's near.
    if (cols < 3) for (let j = i + 1; j < n; j++) if (!taken[j] && !near.includes(j)) near.push(j);
    near.sort((a, b) => a - b);
    for (const j of near) {
      let dx = X[j] - X[i];
      dx -= W * Math.round(dx / W);
      const dy = Y[j] - Y[i];
      if (Math.abs(dx) <= R && Math.abs(dy) <= R) {
        taken[j] = 1;
        members.push(j);
        dxs.push(dx);
      }
    }
    if (members.length === 1) {
      out.push({ ids: [points[i].id], count: 1, lat: points[i].lat, lng: points[i].lng, index: i });
      continue;
    }
    let sx = 0, sy = 0;
    for (let m = 0; m < members.length; m++) {
      sx += X[i] + dxs[m];
      sy += Y[members[m]];
    }
    const mx = sx / members.length, my = sy / members.length;
    out.push({
      ids: members.map((m) => points[m].id),
      count: members.length,
      lat: mercLat(my / W),
      lng: normalLng((mx / W) * 360 - 180),
      index: i,
      members,
    });
  }
  return out;
}

function unit(c) {
  const la = c.lat * rad, lo = c.lng * rad;
  return [Math.cos(la) * Math.cos(lo), Math.cos(la) * Math.sin(lo), Math.sin(la)];
}

/** geodesic is a line's points along great circles: each segment split into a piece a degree of arc (at least one). */
export function geodesic(points) {
  if (points.length < 2) return points.slice();
  const out = [points[0]];
  for (let s = 1; s < points.length; s++) {
    const a = points[s - 1], b = points[s];
    const u = unit(a), v = unit(b);
    const dot = Math.max(-1, Math.min(1, u[0] * v[0] + u[1] * v[1] + u[2] * v[2]));
    const arc = Math.acos(dot);
    const n = Math.max(1, Math.ceil(arc / rad));
    if (arc < 1e-12 || Math.abs(Math.sin(arc)) < 1e-12) {
      out.push(b);
      continue;
    }
    for (let k = 1; k <= n; k++) {
      if (k === n) {
        out.push(b);
        break;
      }
      const t = k / n;
      const fa = Math.sin((1 - t) * arc) / Math.sin(arc), fb = Math.sin(t * arc) / Math.sin(arc);
      const x = fa * u[0] + fb * v[0], y = fa * u[1] + fb * v[1], z = fa * u[2] + fb * v[2];
      out.push({ lat: Math.atan2(z, Math.hypot(x, y)) / rad, lng: Math.atan2(y, x) / rad });
    }
  }
  return out;
}

/** circlePoints is a circle on the ground as 72 points, a bearing 5° apart from north. */
export function circlePoints(center, radius) {
  const d = radius / earthRadius;
  const la = center.lat * rad, lo = center.lng * rad;
  const out = [];
  for (let k = 0; k < 72; k++) {
    const t = k * 5 * rad;
    const lat = Math.asin(Math.sin(la) * Math.cos(d) + Math.cos(la) * Math.sin(d) * Math.cos(t));
    const lng = lo + Math.atan2(Math.sin(t) * Math.sin(d) * Math.cos(la), Math.cos(d) - Math.sin(la) * Math.sin(lat));
    out.push({ lat: lat / rad, lng: normalLng(lng / rad) });
  }
  return out;
}

/** bounds is the smallest region holding the coordinates, across the 180th meridian when that's shorter (maps.asuka's bounds). */
export function bounds(cs) {
  if (!cs.length) return { center: { lat: 0, lng: 0 }, latDelta: 0, lngDelta: 0 };
  let south = cs[0].lat, north = south;
  for (const c of cs) {
    south = Math.min(south, c.lat);
    north = Math.max(north, c.lat);
  }
  const lngs = cs.map((c) => normalLng(c.lng)).sort((a, b) => a - b);
  let gap = lngs[0] + 360 - lngs[lngs.length - 1], west = lngs[0];
  for (let i = 1; i < lngs.length; i++) {
    const g = lngs[i] - lngs[i - 1];
    if (g > gap) {
      gap = g;
      west = lngs[i];
    }
  }
  const span = 360 - gap;
  return { center: { lat: (south + north) / 2, lng: normalLng(west + span / 2) }, latDelta: north - south, lngDelta: span, south, north, west };
}

/**
 * fitCamera is the camera that shows coordinates in a w×h view with
 * padding's room at its edges ({top, left, bottom, right}), turned to
 * heading: as near as they fit (at most maxZoom; one place: zoom 15),
 * centered in what the padding leaves.
 */
export function fitCamera(cs, w, h, padding, heading = 0) {
  const b = bounds(cs);
  const p = { top: 0, left: 0, bottom: 0, right: 0, ...padding };
  const aw = Math.max(w - p.left - p.right, 1), ah = Math.max(h - p.top - p.bottom, 1);
  // The places in the world at zoom 0 (east of the bounds' west edge, so
  // across the 180th meridian too), as the turned map shows them.
  const west = ((b.west + 180) / 360) * tileSize;
  let u0 = Infinity, u1 = -Infinity, v0 = Infinity, v1 = -Infinity;
  for (const c of cs) {
    const x = west + ((((c.lng - b.west) % 360) + 360) % 360 / 360) * tileSize;
    const [u, v] = turn(x, mercY(c.lat) * tileSize, heading);
    u0 = Math.min(u0, u);
    u1 = Math.max(u1, u);
    v0 = Math.min(v0, v);
    v1 = Math.max(v1, v);
  }
  const sx = u1 - u0, sy = v1 - v0;
  let zoom = maxZoom;
  if (sx > 1e-12) zoom = Math.min(zoom, Math.log2(aw / sx));
  if (sy > 1e-12) zoom = Math.min(zoom, Math.log2(ah / sy));
  if (sx <= 1e-12 && sy <= 1e-12) zoom = 15;
  zoom = Math.max(zoom, 0);
  const k = 2 ** zoom;
  // The padded area's middle is off the view's by this, on screen: the
  // view's center is that much the other way from the places' middle.
  const ox = (p.left - p.right) / 2, oy = (p.top - p.bottom) / 2;
  const [x, y] = unturn((u0 + u1) / 2 - ox / k, (v0 + v1) / 2 - oy / k, heading);
  const cx = x / tileSize, cy = y / tileSize;
  return { center: { lat: mercLat(cy), lng: normalLng(cx * 360 - 180) }, zoom, heading, pitch: 0 };
}

/** turn is a world offset as it shows on screen with the map's top facing heading. */
export function turn(x, y, heading) {
  if (!heading) return [x, y];
  const c = Math.cos(heading * rad), s = Math.sin(heading * rad);
  return [c * x + s * y, -s * x + c * y];
}

/** unturn is a screen offset as it is in the world. */
export function unturn(x, y, heading) {
  if (!heading) return [x, y];
  const c = Math.cos(heading * rad), s = Math.sin(heading * rad);
  return [c * x - s * y, s * x + c * y];
}

/** inRings reports whether (x, y) is inside the rings ([[x, y]...] each), even-odd: a hole's inside isn't. */
export function inRings(rings, x, y) {
  let inside = false;
  for (const ring of rings) {
    for (let i = 0, j = ring.length - 1; i < ring.length; j = i++) {
      const [xi, yi] = ring[i], [xj, yj] = ring[j];
      if (yi > y !== yj > y && x < ((xj - xi) * (y - yi)) / (yj - yi) + xi) inside = !inside;
    }
  }
  return inside;
}

/** nearLine is the distance from (x, y) to a line through pts ([x, y] each). */
export function nearLine(pts, x, y) {
  let best = Infinity;
  for (let i = 1; i < pts.length; i++) {
    const [ax, ay] = pts[i - 1], [bx, by] = pts[i];
    const dx = bx - ax, dy = by - ay;
    const l = dx * dx + dy * dy;
    const t = l ? Math.max(0, Math.min(1, ((x - ax) * dx + (y - ay) * dy) / l)) : 0;
    best = Math.min(best, Math.hypot(x - ax - t * dx, y - ay - t * dy));
  }
  if (pts.length === 1) best = Math.hypot(x - pts[0][0], y - pts[0][1]);
  return best;
}

/** overlays is the overlays in the order they're drawn: by zIndex, then polygons, circles, lines, each in its list's order. */
export function overlayOrder(polygons, circles, polylines) {
  const all = [];
  polygons.forEach((o, i) => all.push({ kind: "polygon", o, z: o.zIndex || 0, k: 0, i }));
  circles.forEach((o, i) => all.push({ kind: "circle", o, z: o.zIndex || 0, k: 1, i }));
  polylines.forEach((o, i) => all.push({ kind: "polyline", o, z: o.zIndex || 0, k: 2, i }));
  return all.sort((a, b) => a.z - b.z || a.k - b.k || a.i - b.i);
}

// ---------------------------------------------------------------------
// The view.

const retryAfter = 30_000; // a tile that failed is asked for again after this
const osm = "https://tile.openstreetmap.org/{z}/{x}/{y}.png";
const blank = "data:image/gif;base64,R0lGODlhAQABAAAAACH5BAEKAAEALAAAAAABAAEAAAICTAEAOw==";
const svgNS = "http://www.w3.org/2000/svg";

// What the page knows of each tile's URL: "loaded" (in the browser's
// cache now), or when it failed.
const tiles = new Map();
// The images' own sizes, by URL, once they've come.
const sizes = new Map();

class MapView {
  constructor(events, template) {
    this.events = events;
    this.template = template || osm;
    // The view: its center in the world's unit square (Web Mercator), its
    // zoom (the world is tileSize·2^zoom points wide), the compass
    // direction its top faces.
    this.cx = 0.5;
    this.cy = 0.5;
    this.zoom = 1;
    this.heading = 0;
    this.w = 0;
    this.h = 0;
    this.pending = null; // a region or a camera waiting for the view's size
    this.shown = false; // where it is was shown: the next move animates there
    this.markers = [];
    this.dragged = new Map(); // id -> where the user put it, until markers changes
    this.shapes = { polylines: [], polygons: [], circles: [] };
    this.paths = new WeakMap(); // an overlay -> its points (a geodesic's, a circle's), once
    this.hits = []; // the overlays with ids as last drawn, topmost last
    this.clustering = false;
    this.clusterColor = "";
    this.clusters = null; // { level, groups } for the markers as they are
    this.selected = "";
    this.interactive = true;
    this.showsUser = false;
    this.user = null; // { lat, lng, acc }
    this.watch = null;
    this.moved = false; // the user moved it since the last report
    this.failed = false;
    this.alive = true;
    this.frame = 0;
    this.anim = null;
    this.idleTimer = 0;
    this.moveKey = "";
    this.idleKey = "";
    this.pointers = new Map(); // id -> { x, y }
    this.drag = null; // the map's gesture
    this.pin = null; // a marker being dragged: { id, pointer, start, off, lifted, at }
    this.press = 0; // a touch's long press timer
    this.suppressClick = false;
    this.pinEls = new Map(); // key -> element
    this.calloutEl = null;

    const el = (this.el = document.createElement("div"));
    el.className = "asuka-map";
    el.tabIndex = 0;
    el.setAttribute("role", "application");
    el.setAttribute("aria-roledescription", "map");
    this.stage = div("asuka-map-stage");
    this.layer = div("asuka-map-tiles");
    this.svg = document.createElementNS(svgNS, "svg");
    this.svg.setAttribute("class", "asuka-map-shapes");
    this.stage.append(this.layer, this.svg);
    this.pins = div("asuka-map-pins");
    this.compass = document.createElement("button");
    this.compass.type = "button";
    this.compass.className = "asuka-map-compass";
    this.compass.setAttribute("aria-label", "Face north");
    this.compass.innerHTML = compassSVG;
    this.compass.hidden = true;
    this.attribution = document.createElement("a");
    this.attribution.className = "asuka-map-attribution";
    this.attribution.href = "https://www.openstreetmap.org/copyright";
    this.attribution.target = "_blank";
    this.attribution.rel = "noopener";
    this.attribution.textContent = "© OpenStreetMap contributors";
    el.append(this.stage, this.pins, this.compass, this.attribution);
    this.tileEls = new Map(); // key -> { el, img, url, fallback }
    views.set(el, this);

    this.resize = new ResizeObserver(() => this.sized());
    this.resize.observe(el);
    this.listen();
  }

  // --- Props ---

  setRegion(r) {
    if (!r?.center || !(r.latDelta > 0 || r.lngDelta > 0 || r.center.lat || r.center.lng)) return;
    this.pending = { region: r, animated: true };
    this.applyPending();
  }

  setCamera(c) {
    if (!c?.center || !(c.zoom > 0)) return;
    this.pending = { camera: c, animated: true };
    this.applyPending();
  }

  setMarkers(ms) {
    this.markers = Array.isArray(ms) ? ms : [];
    this.dragged.clear();
    this.clusters = null;
    this.draw();
  }

  setShapes(kind, list) {
    this.shapes[kind] = Array.isArray(list) ? list : [];
    this.draw();
  }

  setSelected(id) {
    this.selected = id || "";
    this.draw();
  }

  setClustering(on) {
    this.clustering = !!on;
    this.clusters = null;
    this.draw();
  }

  setClusterColor(c) {
    this.clusterColor = colorCSS(c);
    this.draw();
  }

  setShowsUser(on) {
    this.showsUser = !!on;
    this.followUser();
  }

  setInteractive(on) {
    this.interactive = !!on;
    this.el.tabIndex = this.interactive ? 0 : -1;
    this.el.classList.toggle("asuka-map-static", !this.interactive);
  }

  sized() {
    const w = this.el.clientWidth, h = this.el.clientHeight;
    if (w === this.w && h === this.h) return;
    this.w = w;
    this.h = h;
    if (this.pending) this.applyPending();
    else {
      this.clamp();
      this.draw();
    }
  }

  // --- Moving it ---

  showRegion(r, animated) {
    if (!r?.center) return;
    this.pending = { region: r, animated, force: true };
    this.applyPending();
  }

  showCamera(c, animated) {
    if (!c?.center) return;
    this.pending = { camera: c.zoom > 0 ? c : { ...c, zoom: this.zoom }, animated, force: true };
    this.applyPending();
  }

  fit(cs, padding, animated) {
    if (!cs?.length) return;
    this.pending = { fit: cs, padding, animated, force: true };
    this.applyPending();
  }

  /** applyPending shows the pending region or camera once the view has a size: animated there if it showed one before. */
  applyPending() {
    const p = this.pending;
    if (!p || this.w <= 0 || this.h <= 0) return;
    this.pending = null;
    let to;
    if (p.region) {
      const r = p.region;
      to = { cx: (r.center.lng + 180) / 360, cy: mercY(r.center.lat), zoom: zoomFor(r, this.w, this.h), heading: this.heading };
    } else {
      const c = p.camera ?? fitCamera(p.fit, this.w, this.h, p.padding, this.heading);
      to = { cx: (c.center.lng + 180) / 360, cy: mercY(c.center.lat), zoom: Math.min(c.zoom, maxZoom), heading: norm360(c.heading || 0) };
    }
    // Where it is already (the user's move, bound and given back).
    let ddx = to.cx - this.cx;
    ddx -= Math.round(ddx);
    if (!p.force && Math.abs(ddx) < 1e-9 && Math.abs(to.cy - this.cy) < 1e-9 && Math.abs(to.zoom - this.zoom) < 1e-3 && Math.abs(angle(to.heading - this.heading)) < 1e-3) return;
    const reduced = typeof matchMedia === "function" && matchMedia("(prefers-reduced-motion: reduce)").matches;
    if (!this.shown || p.animated === false || reduced) {
      this.shown = true;
      this.stopAnim();
      Object.assign(this, to);
      this.clamp();
      this.draw();
      return;
    }
    this.animateTo(to, 300);
  }

  animateTo(to, ms, done) {
    const from = { cx: this.cx, cy: this.cy, zoom: this.zoom, heading: this.heading };
    // The short way round the world, and round the compass.
    let dx = to.cx - from.cx;
    dx -= Math.round(dx);
    const dh = angle(to.heading - from.heading);
    this.animate(ms, (f) => {
      this.cx = from.cx + dx * f;
      this.cy = from.cy + (to.cy - from.cy) * f;
      this.zoom = from.zoom + (to.zoom - from.zoom) * f;
      this.heading = norm360(from.heading + dh * f);
      this.clamp();
    }, done);
  }

  // --- Geometry ---

  world() { return tileSize * 2 ** this.zoom; }

  /** The stage's size: the view's, or with the map turned the square that covers it. */
  stageSize() {
    if (!this.heading) return [this.w, this.h];
    const d = Math.ceil(Math.hypot(this.w, this.h));
    return [d, d];
  }

  /** clamp keeps the zoom in range and the poles out of view; the world wraps east and west. */
  clamp() {
    const [, lh] = this.stageSize();
    const least = lh > 0 ? Math.max(Math.log2(lh / tileSize), 0) : 0;
    this.zoom = Math.max(Math.min(this.zoom, maxZoom), least);
    this.cx -= Math.floor(this.cx);
    const half = lh / 2 / this.world();
    this.cy = Math.max(Math.min(this.cy, 1 - half), half);
    this.heading = norm360(this.heading);
  }

  region() {
    const W = this.world();
    const top = mercLat(this.cy - this.h / 2 / W), bottom = mercLat(this.cy + this.h / 2 / W);
    return {
      center: { lat: mercLat(this.cy), lng: this.cx * 360 - 180 },
      latDelta: Math.abs(top - bottom),
      lngDelta: Math.min((this.w / W) * 360, 360),
    };
  }

  camera() {
    return { center: { lat: mercLat(this.cy), lng: normalLng(this.cx * 360 - 180) }, zoom: this.zoom, heading: this.heading, pitch: 0 };
  }

  /** worldAt is the world's point (unwrapped, in the unit square) at a point of the view. */
  worldAt(x, y) {
    const W = this.world();
    const [wx, wy] = unturn(x - this.w / 2, y - this.h / 2, this.heading);
    return [this.cx + wx / W, this.cy + wy / W];
  }

  /** at is the place at a point of the view. */
  at(x, y) {
    let [px, py] = this.worldAt(x, y);
    px -= Math.floor(px);
    return { lat: mercLat(py), lng: px * 360 - 180 };
  }

  /** keepAt moves the center so the view's point (x, y) shows the world's (px, py). */
  keepAt(px, py, x, y) {
    const W = this.world();
    const [wx, wy] = unturn(x - this.w / 2, y - this.h / 2, this.heading);
    this.cx = px - wx / W;
    this.cy = py - wy / W;
  }

  /** offset is where a place is from the view's center, in the world's points: the copy of the world nearest the center. */
  offset(lat, lng) {
    const W = this.world();
    let dx = (lng + 180) / 360 - this.cx;
    dx -= Math.round(dx);
    return [dx * W, (mercY(lat) - this.cy) * W];
  }

  /** point is where a place is in the view. */
  point(lat, lng) {
    const [x, y] = this.offset(lat, lng);
    const [sx, sy] = turn(x, y, this.heading);
    return [this.w / 2 + sx, this.h / 2 + sy];
  }

  zoomBy(dz, x, y) {
    const [px, py] = this.worldAt(x, y);
    this.zoom += dz;
    this.clamp();
    this.keepAt(px, py, x, y);
    this.clamp();
    this.moved = true;
    this.draw();
  }

  turnBy(dh, x, y) {
    const [px, py] = this.worldAt(x, y);
    this.heading = norm360(this.heading + dh);
    this.keepAt(px, py, x, y);
    this.clamp();
    this.moved = true;
    this.draw();
  }

  panBy(dx, dy) {
    const W = this.world();
    const [wx, wy] = unturn(dx, dy, this.heading);
    this.cx -= wx / W;
    this.cy -= wy / W;
    this.clamp();
    this.moved = true;
    this.draw();
  }

  // --- Drawing ---

  draw() {
    if (this.frame || !this.alive) return;
    if (typeof requestAnimationFrame !== "function") return;
    this.frame = requestAnimationFrame(() => {
      this.frame = 0;
      if (this.w <= 0 || this.h <= 0) return;
      const [lw, lh] = this.stageSize();
      const s = this.stage.style;
      s.width = lw + "px";
      s.height = lh + "px";
      s.left = (this.w - lw) / 2 + "px";
      s.top = (this.h - lh) / 2 + "px";
      s.transform = this.heading ? `rotate(${-this.heading}deg)` : "";
      this.compass.hidden = !this.heading;
      if (this.heading) this.compass.firstChild.style.transform = `rotate(${-this.heading}deg)`;
      this.drawTiles(lw, lh);
      this.drawShapes(lw, lh);
      this.drawPins();
      this.moving();
    });
  }

  /** moving reports where the map is as it moves (once a frame), and once it stops. */
  moving() {
    const c = this.camera();
    const key = `${c.center.lat},${c.center.lng},${c.zoom},${c.heading},${this.w},${this.h}`;
    if (key === this.moveKey) return;
    this.moveKey = key;
    if (this.shown) this.events.cameraMove(c, this.region());
    clearTimeout(this.idleTimer);
    this.idleTimer = setTimeout(() => this.settle(), 150);
  }

  settle() {
    if (!this.alive) return;
    if (this.anim || this.pointers.size || this.pin) {
      this.idleTimer = setTimeout(() => this.settle(), 150);
      return;
    }
    if (this.moveKey === this.idleKey || !this.shown) return;
    this.idleKey = this.moveKey;
    const c = this.camera(), r = this.region();
    if (this.moved) {
      this.moved = false;
      this.events.regionChange(r);
      this.events.cameraChange(c);
    }
    this.events.cameraIdle(c, r);
  }

  /**
   * drawTiles places the tiles of the zoom level nearest the view's at the
   * screen's pixels (a 2× display: the next level's), each scaled to fit,
   * over the stage.
   */
  drawTiles(lw, lh) {
    const k = globalThis.devicePixelRatio || 1;
    const z = Math.max(0, Math.min(Math.round(this.zoom + Math.log2(k)), maxZoom));
    const n = 2 ** z;
    const W = this.world();
    const tw = W / n;
    const ox = this.cx * W - lw / 2, oy = this.cy * W - lh / 2;
    const tx0 = Math.floor(ox / tw), tx1 = Math.floor((ox + lw - 1) / tw);
    const ty0 = Math.max(Math.floor(oy / tw), 0), ty1 = Math.min(Math.floor((oy + lh - 1) / tw), n - 1);
    const keep = new Set();
    const now = Date.now();
    for (let ty = ty0; ty <= ty1; ty++) {
      for (let tx = tx0; tx <= tx1; tx++) {
        const key = `${z}/${tx}/${ty}`;
        keep.add(key);
        const wx = ((tx % n) + n) % n;
        let t = this.tileEls.get(key);
        if (!t) {
          t = this.tile(z, wx, ty, now);
          this.tileEls.set(key, t);
          this.layer.appendChild(t.el);
        } else if (t.failed && now - t.failed > retryAfter) {
          t.failed = 0;
          t.img.src = t.url;
        }
        const x0 = Math.floor(tx * tw - ox), x1 = Math.floor((tx + 1) * tw - ox);
        const y0 = Math.floor(ty * tw - oy), y1 = Math.floor((ty + 1) * tw - oy);
        const s = t.el.style;
        s.left = x0 + "px";
        s.top = y0 + "px";
        s.width = x1 - x0 + "px";
        s.height = y1 - y0 + "px";
      }
    }
    for (const [key, t] of this.tileEls) {
      if (keep.has(key)) continue;
      // One that hasn't come is let go: the browser stops asking.
      if (tiles.get(t.url) !== "loaded") t.img.src = blank;
      t.el.remove();
      this.tileEls.delete(key);
    }
  }

  /** tile makes a tile's element: its image, and while it comes one above it that's here. */
  tile(z, x, y, now) {
    const el = div("asuka-map-tile");
    const url = tileURL(this.template, z, x, y);
    const t = { el, url, img: document.createElement("img"), fallback: null, failed: 0 };
    const state = tiles.get(url);
    if (state !== "loaded") {
      for (let dz = 1; dz <= Math.min(z, 6); dz++) {
        const purl = tileURL(this.template, z - dz, x >> dz, y >> dz);
        if (tiles.get(purl) !== "loaded") continue;
        const f = (t.fallback = document.createElement("img"));
        f.alt = "";
        f.draggable = false;
        const parts = 2 ** dz;
        f.className = "asuka-map-fallback";
        f.style.width = f.style.height = parts * 100 + "%";
        f.style.left = -(x & (parts - 1)) * 100 + "%";
        f.style.top = -(y & (parts - 1)) * 100 + "%";
        f.src = purl;
        el.appendChild(f);
        break;
      }
    }
    const img = t.img;
    img.alt = "";
    img.draggable = false;
    img.decoding = "async";
    img.className = "asuka-map-img";
    img.onload = () => {
      if (img.src === blank) return;
      tiles.set(url, "loaded");
      img.classList.add("asuka-map-loaded");
      t.fallback?.remove();
      t.fallback = null;
    };
    img.onerror = () => {
      if (img.src === blank) return;
      t.failed = Date.now();
      tiles.set(url, t.failed);
      this.tileFailed();
    };
    const failedAt = typeof state === "number" ? state : 0;
    if (failedAt && now - failedAt < retryAfter) t.failed = failedAt;
    else img.src = url;
    el.appendChild(img);
    return t;
  }

  tileFailed() {
    if (this.failed || !this.alive) return;
    this.failed = true;
    const why = navigator.onLine === false ? "the device is offline" : "the tile server didn't answer";
    this.events.error("maps: the map's tiles can't be loaded: " + why);
  }

  /** points is an overlay's places: a geodesic line's, a circle's, worked out once. */
  points(kind, o) {
    let p = this.paths.get(o);
    if (p) return p;
    if (kind === "circle") p = [circlePoints(o.center, o.radius)];
    else if (kind === "polyline") p = [o.geodesic ? geodesic(o.points) : o.points];
    else p = [o.points, ...(o.holes || [])];
    this.paths.set(o, p);
    return p;
  }

  /** ring is places in the stage's points, each copy of the world the one next to the place before. */
  ring(places, lw, lh) {
    const W = this.world();
    const out = new Array(places.length);
    let prev = null;
    for (let i = 0; i < places.length; i++) {
      const c = places[i];
      let x = ((c.lng + 180) / 360) * W;
      if (prev == null) {
        let dx = x / W - this.cx;
        dx -= Math.round(dx);
        x = (this.cx + dx) * W;
      } else {
        let dx = x - prev;
        dx -= W * Math.round(dx / W);
        x = prev + dx;
      }
      prev = x;
      out[i] = [lw / 2 + x - this.cx * W, lh / 2 + (mercY(c.lat) - this.cy) * W];
    }
    return out;
  }

  /** drawShapes draws the overlays in the SVG over the tiles, and keeps where those with ids are for a press. */
  drawShapes(lw, lh) {
    const svg = this.svg;
    svg.setAttribute("width", lw);
    svg.setAttribute("height", lh);
    svg.setAttribute("viewBox", `0 0 ${lw} ${lh}`);
    const order = overlayOrder(this.shapes.polygons, this.shapes.circles, this.shapes.polylines);
    let out = "";
    const hits = [];
    const f = (n) => Math.round(n * 10) / 10;
    for (const { kind, o } of order) {
      const rings = this.points(kind, o).filter((r) => r?.length).map((r) => this.ring(r, lw, lh));
      if (!rings.length) continue;
      let d = "";
      for (const r of rings) {
        d += "M" + r.map(([x, y]) => f(x) + " " + f(y)).join("L");
        if (kind !== "polyline") d += "Z";
      }
      if (kind === "polyline") {
        const w = Math.max(o.width || 0, 0.5);
        const color = colorCSS(o.color) || "var(--asuka-blue)";
        const dash = o.dashed ? `stroke-dasharray:${4 * w} ${2 * w};stroke-linecap:butt;` : "stroke-linecap:round;";
        out += `<path d="${d}" style="fill:none;stroke:${color};stroke-width:${w};stroke-linejoin:round;${dash}"/>`;
        if (o.id) hits.push({ id: o.id, line: rings[0], width: w });
      } else {
        const fill = colorCSS(o.fill) || "none";
        const sw = o.strokeWidth || 0;
        const stroke = sw > 0 ? colorCSS(o.stroke) || "none" : "none";
        out += `<path d="${d}" style="fill:${fill};fill-rule:evenodd;stroke:${stroke};stroke-width:${sw};stroke-linejoin:round"/>`;
        if (o.id) hits.push({ id: o.id, rings });
      }
    }
    svg.innerHTML = out;
    this.hits = hits;
  }

  /** overlayAt is the id of the topmost overlay with one at a point of the view, "" if none. */
  overlayAt(x, y) {
    if (!this.hits.length) return "";
    const [lw, lh] = this.stageSize();
    const [sx, sy] = unturn(x - this.w / 2, y - this.h / 2, this.heading);
    const px = lw / 2 + sx, py = lh / 2 + sy;
    for (let i = this.hits.length - 1; i >= 0; i--) {
      const h = this.hits[i];
      if (h.line ? nearLine(h.line, px, py) <= h.width / 2 + 6 : inRings(h.rings, px, py)) return h.id;
    }
    return "";
  }

  /** groups is what the markers show as: each on its own, or (clustering) in clusters for the zoom's level. */
  groups() {
    const level = Math.max(0, Math.min(22, Math.floor(this.zoom + 1e-6)));
    if (this.clusters && this.clusters.level === level) return this.clusters.groups;
    const groups = [];
    const pts = [];
    this.markers.forEach((m, i) => {
      const at = this.dragged.get(m.id) ?? m.at;
      if (!at || !Number.isFinite(at.lat) || !Number.isFinite(at.lng)) return;
      if (this.clustering && m.clusters) pts.push({ id: m.id, lat: at.lat, lng: at.lng, i });
      else groups.push({ marker: m, at, z: m.zIndex || 0, i });
    });
    if (pts.length) {
      for (const g of cluster(pts, this.zoom)) {
        const first = pts[g.index];
        if (g.count === 1) {
          const m = this.markers[first.i];
          groups.push({ marker: m, at: { lat: first.lat, lng: first.lng }, z: m.zIndex || 0, i: first.i });
        } else {
          const ms = g.members.map((k) => this.markers[pts[k].i]);
          groups.push({
            cluster: g,
            places: g.members.map((k) => ({ lat: pts[k].lat, lng: pts[k].lng })),
            at: { lat: g.lat, lng: g.lng },
            z: Math.max(...ms.map((m) => m.zIndex || 0)),
            i: first.i,
          });
        }
      }
    }
    groups.sort((a, b) => a.z - b.z || a.i - b.i);
    this.clusters = { level, groups };
    return groups;
  }

  /** size is a marker's image's size in points: its width and height, or its own (as it came), keeping its shape. */
  imageSize(m) {
    const url = imageURL(m.image);
    const own = sizes.get(url);
    let w = m.width || 0, h = m.height || 0;
    if (w && h) return [w, h];
    if (!own) return [w || h || 0, h || w || 0];
    if (w) return [w, (w * own[1]) / own[0]];
    if (h) return [(h * own[0]) / own[1], h];
    return own;
  }

  /** drawPins places the markers and clusters (keyed, so a drag or a focus survives), the selected one's callout, and the user's location. */
  drawPins() {
    if (!this.alive) return;
    const box = this.pins;
    const keep = new Set();
    let rank = 1;
    let callout = null;
    for (const g of this.groups()) {
      const [x, y] = this.point(g.at.lat, g.at.lng);
      if (x < -120 || x > this.w + 120 || y < -120 || y > this.h + 120) {
        if (!(this.pin && g.marker?.id === this.pin.id)) continue;
      }
      if (g.cluster) {
        const key = "c:" + g.cluster.ids[0] + ":" + g.cluster.count;
        keep.add(key);
        let b = this.pinEls.get(key);
        const n = g.cluster.count;
        const d = n < 10 ? 32 : n < 100 ? 38 : 44;
        if (!b) {
          b = document.createElement("button");
          b.type = "button";
          b.className = "asuka-map-cluster";
          b.dataset.key = key;
          b.textContent = n > 999 ? "999+" : String(n);
          b.setAttribute("aria-label", `${n} markers`);
          b.style.width = b.style.height = d + "px";
          this.pinEls.set(key, b);
          box.appendChild(b);
        }
        b.group = g;
        b.tabIndex = this.interactive ? 0 : -1;
        b.style.background = this.clusterColor || "var(--asuka-accent)";
        b.style.zIndex = rank++;
        b.style.transform = `translate(${x - d / 2}px, ${y - d / 2}px)`;
        continue;
      }
      const m = g.marker;
      const key = "m:" + m.id;
      keep.add(key);
      let b = this.pinEls.get(key);
      const look = m.image ? "i" + m.image + "|" + m.width + "|" + m.height : "p" + m.color;
      if (!b) {
        b = document.createElement("button");
        b.type = "button";
        b.className = "asuka-map-pin";
        b.dataset.key = key;
        this.pinEls.set(key, b);
        box.appendChild(b);
      }
      if (b.look !== look) {
        b.look = look;
        b.textContent = "";
        if (m.image) {
          const img = document.createElement("img");
          img.alt = "";
          img.draggable = false;
          const url = imageURL(m.image);
          img.onload = () => {
            if (!sizes.has(url)) {
              sizes.set(url, [img.naturalWidth / (globalThis.devicePixelRatio || 1) || img.naturalWidth, img.naturalHeight / (globalThis.devicePixelRatio || 1) || img.naturalHeight]);
              this.draw();
            }
          };
          img.src = url;
          b.appendChild(img);
        } else {
          b.innerHTML = pinSVG;
          b.firstChild.style.color = colorCSS(m.color) || "var(--asuka-red)";
          b.style.width = "26px";
          b.style.height = "36px";
        }
      }
      b.marker = m;
      b.classList.toggle("asuka-map-drag", !!m.draggable && this.interactive);
      b.classList.toggle("asuka-map-lifted", this.pin?.id === m.id && this.pin.started);
      b.tabIndex = this.interactive ? 0 : -1;
      b.setAttribute("aria-label", [m.title, m.subtitle].filter(Boolean).join(", ") || "Marker");
      let w = 26, h = 36, ax = 0.5, ay = 1;
      if (m.image) {
        [w, h] = this.imageSize(m);
        ax = m.anchor?.x ?? 0.5;
        ay = m.anchor?.y ?? 0.5;
        b.style.width = w + "px";
        b.style.height = h + "px";
        b.style.visibility = w && h ? "" : "hidden";
      }
      b.style.zIndex = rank++;
      b.style.transform = `translate(${x - ax * w}px, ${y - ay * h}px)`;
      b.anchorAt = [x, y];
      if (m.id === this.selected && (m.title || m.subtitle)) callout = { m, x, y: y - ay * h };
    }
    for (const [key, b] of this.pinEls) {
      if (keep.has(key)) continue;
      b.remove();
      this.pinEls.delete(key);
    }
    this.drawCallout(callout);
    this.drawUser();
  }

  drawCallout(c) {
    if (!c) {
      this.calloutEl?.remove();
      this.calloutEl = null;
      return;
    }
    let el = this.calloutEl;
    const text = c.m.title + "\n" + c.m.subtitle;
    if (!el || el.text !== text) {
      el?.remove();
      el = this.calloutEl = div("asuka-map-callout");
      el.text = text;
      el.setAttribute("role", "button");
      if (c.m.title) el.appendChild(div("asuka-map-callout-title")).textContent = c.m.title;
      if (c.m.subtitle) el.appendChild(div("asuka-map-callout-subtitle")).textContent = c.m.subtitle;
      this.pins.appendChild(el);
    }
    el.dataset.id = c.m.id;
    el.style.transform = `translate(${c.x}px, ${c.y - 6}px) translate(-50%, -100%)`;
  }

  drawUser() {
    this.userEl?.remove();
    this.userEl = null;
    if (!this.showsUser || !this.user) return;
    const [x, y] = this.point(this.user.lat, this.user.lng);
    const d = (this.userEl = div("asuka-map-user"));
    d.style.transform = `translate(${x}px, ${y}px)`;
    const acc = (this.user.acc / (40075016.686 * Math.cos(this.user.lat * rad))) * this.world();
    if (acc > 12) {
      const a = div("asuka-map-accuracy");
      a.style.width = a.style.height = 2 * acc + "px";
      d.appendChild(a);
    }
    d.appendChild(div("asuka-map-dot"));
    this.pins.appendChild(d);
  }

  // --- The user's location ---

  async followUser() {
    if (!this.showsUser) {
      if (this.watch != null) navigator.geolocation?.clearWatch(this.watch);
      this.watch = null;
      this.user = null;
      this.draw();
      return;
    }
    if (this.watch != null || !navigator.geolocation) return;
    // Only with the permission: the map never asks for it.
    try {
      const p = await navigator.permissions?.query({ name: "geolocation" });
      if (p && p.state !== "granted") {
        p.onchange = () => { if (p.state === "granted") this.followUser(); };
        return;
      }
    } catch {}
    if (!this.alive || !this.showsUser || this.watch != null) return;
    this.watch = navigator.geolocation.watchPosition(
      (pos) => {
        this.user = { lat: pos.coords.latitude, lng: pos.coords.longitude, acc: pos.coords.accuracy || 0 };
        this.draw();
      },
      () => {},
      { enableHighAccuracy: false, maximumAge: 30_000 },
    );
  }

  // --- Input ---

  listen() {
    const el = this.el;
    el.addEventListener("pointerdown", (e) => {
      if (!this.interactive || e.button > 0 || e.target === this.attribution || e.target.closest?.(".asuka-map-compass")) return;
      this.stopAnim();
      this.suppressClick = false;
      const p = this.local(e);
      const pinEl = e.target.closest?.(".asuka-map-pin");
      const m = pinEl?.marker;
      if (m?.draggable && !this.pin) {
        const [ax, ay] = pinEl.anchorAt || [p.x, p.y];
        const pin = { id: m.id, pointer: e.pointerId, start: p, off: [p.x - ax, p.y - ay], started: false, at: null };
        if (e.pointerType !== "touch") {
          this.pin = pin;
          return;
        }
        // A finger picks it up after holding it a moment; a move before
        // that pans the map.
        clearTimeout(pin.timer);
        this.pendingPin = pin;
        pin.timer = setTimeout(() => {
          if (this.pendingPin !== pin || this.pointers.size !== 1 || this.drag?.moved) return;
          this.pendingPin = null;
          this.pointers.delete(pin.pointer);
          this.drag = null;
          this.pin = pin;
          try { el.setPointerCapture(pin.pointer); } catch {}
          this.startPin();
        }, 300);
      }
      this.pointers.set(e.pointerId, p);
      this.drag = null;
      this.startGesture(e);
      clearTimeout(this.press);
      this.press = 0;
      if (e.pointerType === "touch" && this.pointers.size === 1 && !pinEl && !e.target.closest?.(".asuka-map-cluster, .asuka-map-callout")) {
        this.press = setTimeout(() => {
          this.press = 0;
          if (this.pointers.size !== 1 || this.drag?.moved) return;
          this.suppressClick = true;
          this.events.longPress(this.at(p.x, p.y));
        }, 500);
      }
    });
    el.addEventListener("pointermove", (e) => {
      if (this.pin && this.pin.pointer === e.pointerId) {
        this.movePin(this.local(e));
        return;
      }
      if (!this.pointers.has(e.pointerId)) return;
      this.pointers.set(e.pointerId, this.local(e));
      this.moveGesture(e);
      if (this.drag?.moved || this.pointers.size > 1) {
        clearTimeout(this.press);
        this.press = 0;
        if (this.pendingPin) {
          clearTimeout(this.pendingPin.timer);
          this.pendingPin = null;
        }
      }
    });
    const up = (e) => {
      if (this.pin && this.pin.pointer === e.pointerId) {
        const pin = this.pin;
        this.pin = null;
        if (el.hasPointerCapture?.(e.pointerId)) el.releasePointerCapture(e.pointerId);
        if (pin.started) {
          this.suppressClick = true;
          setTimeout(() => (this.suppressClick = false), 0);
          this.events.markerDragEnd(pin.id, pin.at);
          this.draw();
        }
        return;
      }
      if (!this.pointers.has(e.pointerId)) return;
      clearTimeout(this.press);
      this.press = 0;
      if (this.pendingPin) {
        clearTimeout(this.pendingPin.timer);
        this.pendingPin = null;
      }
      this.pointers.delete(e.pointerId);
      if (el.hasPointerCapture?.(e.pointerId)) el.releasePointerCapture(e.pointerId);
      if (this.drag?.moved) {
        // The click that follows a drag isn't one.
        this.suppressClick = true;
        setTimeout(() => (this.suppressClick = false), 0);
      }
      this.startGesture(e);
    };
    el.addEventListener("pointerup", up);
    el.addEventListener("pointercancel", up);
    el.addEventListener("click", (e) => {
      if (this.suppressClick || !this.interactive || e.target === this.attribution) return;
      if (e.target.closest?.(".asuka-map-compass")) {
        this.animateTo({ cx: this.cx, cy: this.cy, zoom: this.zoom, heading: 0 }, 300);
        this.moved = true;
        return;
      }
      const callout = e.target.closest?.(".asuka-map-callout");
      if (callout) {
        this.events.calloutPress(callout.dataset.id);
        return;
      }
      const c = e.target.closest?.(".asuka-map-cluster");
      if (c?.group) {
        const g = c.group;
        this.events.clusterPress(g.cluster.ids.slice());
        const b = bounds(g.places);
        if (b.latDelta > 1e-9 || b.lngDelta > 1e-9) this.fitAnimated(g.places, { top: 48, left: 48, bottom: 48, right: 48 });
        return;
      }
      const pin = e.target.closest?.(".asuka-map-pin");
      if (pin?.marker) {
        const id = pin.marker.id;
        this.events.markerPress(id);
        if (this.selected !== id) {
          this.selected = id;
          this.events.selectedChange(id);
          this.draw();
        }
        return;
      }
      if (e.detail > 1) return; // a double click's second: a zoom
      const { x, y } = this.local(e);
      if (this.selected) {
        this.selected = "";
        this.events.selectedChange("");
        this.draw();
      }
      const o = this.overlayAt(x, y);
      if (o) this.events.overlayPress(o);
      else this.events.press(this.at(x, y));
    });
    el.addEventListener("contextmenu", (e) => {
      if (!this.interactive || e.target === this.attribution) return;
      e.preventDefault();
      if (e.target.closest?.(".asuka-map-pin, .asuka-map-cluster, .asuka-map-callout, .asuka-map-compass")) return;
      // A touch's long press came as one already.
      if (e.pointerType === "touch" || this.suppressClick) return;
      const { x, y } = this.local(e);
      this.events.longPress(this.at(x, y));
    });
    el.addEventListener("dblclick", (e) => {
      if (!this.interactive || e.target.closest?.(".asuka-map-pin, .asuka-map-cluster, .asuka-map-callout, .asuka-map-compass") || e.target === this.attribution) return;
      e.preventDefault();
      const { x, y } = this.local(e);
      this.zoomTo(e.shiftKey ? -1 : 1, x, y);
    });
    el.addEventListener("wheel", (e) => {
      if (!this.interactive) return;
      e.preventDefault();
      this.stopAnim();
      const px = e.deltaY * (e.deltaMode === 1 ? 33 : e.deltaMode === 2 ? this.h : 1);
      // A notch (100 points) is half a level; a trackpad's pinch (Ctrl)
      // comes in small steps, as fine as it is.
      const { x, y } = this.local(e);
      this.zoomBy(-px / (e.ctrlKey ? 100 : 200), x, y);
    }, { passive: false });
    el.addEventListener("keydown", (e) => {
      if (!this.interactive || e.target !== el || e.metaKey || e.ctrlKey || e.altKey) return;
      const step = Math.min(this.w, this.h) / 8;
      switch (e.key) {
        case "+": case "=": this.zoomTo(1, this.w / 2, this.h / 2); break;
        case "-": case "_": this.zoomTo(-1, this.w / 2, this.h / 2); break;
        case "ArrowLeft": this.panBy(step, 0); break;
        case "ArrowRight": this.panBy(-step, 0); break;
        case "ArrowUp": this.panBy(0, step); break;
        case "ArrowDown": this.panBy(0, -step); break;
        default: return;
      }
      e.preventDefault();
    });
  }

  fitAnimated(places, padding) {
    const c = fitCamera(places, this.w, this.h, padding, this.heading);
    this.moved = true;
    this.animateTo({ cx: (c.center.lng + 180) / 360, cy: mercY(c.center.lat), zoom: Math.min(c.zoom, maxZoom), heading: this.heading }, 300);
  }

  local(e) {
    const r = this.el.getBoundingClientRect();
    return { x: e.clientX - r.left, y: e.clientY - r.top };
  }

  // --- A marker dragged ---

  startPin() {
    const pin = this.pin;
    const m = this.markers.find((k) => k.id === pin.id);
    if (!m) return;
    pin.started = true;
    pin.at = this.dragged.get(m.id) ?? m.at;
    // It leaves its cluster, if it was in one, while it's dragged.
    this.events.markerDragStart(pin.id, pin.at);
    this.draw();
  }

  movePin(p) {
    const pin = this.pin;
    if (!pin.started) {
      if (Math.hypot(p.x - pin.start.x, p.y - pin.start.y) <= 3) return;
      this.startPin();
      if (!pin.started) return;
      try { this.el.setPointerCapture(pin.pointer); } catch {}
    }
    pin.at = this.at(p.x - pin.off[0], p.y - pin.off[1]);
    this.dragged.set(pin.id, pin.at);
    this.clusters = null;
    this.events.markerDrag(pin.id, pin.at);
    this.draw();
  }

  // --- The map's gestures ---

  /** startGesture begins a drag (one pointer; with Shift, a turn) or a pinch (two) from where the pointers are. */
  startGesture(e) {
    const ps = [...this.pointers.values()];
    if (!ps.length) {
      this.drag = null;
      return;
    }
    const moved = this.drag?.moved || false;
    const mid = midpoint(ps);
    this.drag = {
      start: mid,
      last: mid,
      dist: ps.length > 1 ? distance(ps[0], ps[1]) : 0,
      angle: ps.length > 1 ? Math.atan2(ps[1].y - ps[0].y, ps[1].x - ps[0].x) : 0,
      turned: 0,
      turning: false,
      moved,
      shift: ps.length === 1 && !!e?.shiftKey,
    };
  }

  moveGesture(e) {
    const d = this.drag;
    if (!d) return;
    const ps = [...this.pointers.values()];
    const mid = midpoint(ps);
    if (!d.moved) {
      if (Math.hypot(mid.x - d.start.x, mid.y - d.start.y) <= 3 && ps.length < 2) return;
      d.moved = true;
      for (const id of this.pointers.keys()) {
        try { this.el.setPointerCapture(id); } catch {}
      }
    }
    if (d.shift) {
      // Shift and a drag turns it around its middle.
      this.turnBy(-(mid.x - d.last.x) * 0.5, this.w / 2, this.h / 2);
      d.last = mid;
      return;
    }
    this.panBy(mid.x - d.last.x, mid.y - d.last.y);
    d.last = mid;
    if (ps.length > 1) {
      const dist = distance(ps[0], ps[1]);
      if (d.dist > 0 && dist > 0) this.zoomBy(Math.log2(dist / d.dist), mid.x, mid.y);
      d.dist = dist;
      // Two fingers turning it: once they've turned 8°, it follows them.
      const a = Math.atan2(ps[1].y - ps[0].y, ps[1].x - ps[0].x);
      let da = (a - d.angle) / rad;
      da = angle(da);
      d.angle = a;
      d.turned += da;
      if (!d.turning && Math.abs(d.turned) > 8) {
        d.turning = true;
        da = d.turned;
      }
      if (d.turning) this.turnBy(-da, mid.x, mid.y);
    }
  }

  /** zoomTo zooms by dz around a point over a fifth of a second. */
  zoomTo(dz, x, y) {
    const from = this.zoom, to = Math.max(Math.min(this.zoom + dz, maxZoom), 0);
    this.animate(200, (f) => this.zoomBy(from + (to - from) * f - this.zoom, x, y));
  }

  animate(ms, step, done) {
    this.stopAnim();
    const start = performance.now();
    const tick = (t) => {
      if (!this.alive) return;
      let f = Math.min((t - start) / ms, 1);
      f = 1 - (1 - f) * (1 - f); // easing out
      step(f);
      this.draw();
      if (f < 1) this.anim = requestAnimationFrame(tick);
      else {
        this.anim = null;
        done?.();
      }
    };
    this.anim = requestAnimationFrame(tick);
  }

  stopAnim() {
    if (this.anim) cancelAnimationFrame(this.anim);
    this.anim = null;
  }

  destroy() {
    this.alive = false;
    this.stopAnim();
    if (this.frame) cancelAnimationFrame(this.frame);
    clearTimeout(this.idleTimer);
    clearTimeout(this.press);
    this.resize.disconnect();
    views.delete(this.el);
    if (this.watch != null) navigator.geolocation?.clearWatch(this.watch);
    for (const t of this.tileEls.values()) if (tiles.get(t.url) !== "loaded") t.img.src = blank;
    this.tileEls.clear();
  }
}

function div(cls) {
  const d = document.createElement("div");
  d.className = cls;
  return d;
}

/** norm360 is degrees in [0, 360). */
function norm360(d) {
  d %= 360;
  return d < 0 ? d + 360 : d === 360 ? 0 : d;
}

/** angle is degrees in (-180, 180]: the short way round. */
function angle(d) {
  d = norm360(d);
  return d > 180 ? d - 360 : d;
}

const midpoint = (ps) => ({ x: ps.reduce((s, p) => s + p.x, 0) / ps.length, y: ps.reduce((s, p) => s + p.y, 0) / ps.length });
const distance = (a, b) => Math.hypot(a.x - b.x, a.y - b.y);

// A teardrop pin, 26×36, its tip at the bottom: currentColor, a white
// outline and dot.
const pinSVG = `<svg width="26" height="36" viewBox="0 0 26 36" aria-hidden="true"><path d="M13 35C13 35 2 21.5 2 13a11 11 0 0 1 22 0c0 8.5-11 22-11 22z" fill="currentColor" stroke="#fff" stroke-width="1.5"/><circle cx="13" cy="13" r="4.2" fill="#fff"/></svg>`;

// The compass: its red needle north, turned as the map is.
const compassSVG = `<svg viewBox="0 0 34 34" aria-hidden="true"><path d="M17 5l5 12h-10z" fill="#e53935"/><path d="M17 29l-5-12h10z" fill="#9e9e9e"/></svg>`;
