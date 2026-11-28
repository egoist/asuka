// The map's geometry, worked out without a page: clustering (the same
// cases as maps_geo_test.c, which the desktop's C must answer alike),
// great circles, circles on the ground, fitting coordinates, hit tests,
// the overlays' order and the turned map's points.
import { test } from "node:test";
import assert from "node:assert/strict";

globalThis.asuka = { native: {} };
const m = await import("./maps.js");

const near = (a, b, eps = 1e-9) => assert.ok(Math.abs(a - b) <= eps, `${a} isn't ${b}`);
const points = [["A", 0, 0], ["B", 0, 0.1], ["C", 10, 10], ["D", 10.05, 10.05], ["E", 0, 179.99], ["F", 0, -179.99]]
  .map(([id, lat, lng]) => ({ id, lat, lng }));

test("clusters at zoom 5: neighbors together, round the 180th meridian too", () => {
  const g = m.cluster(points, 5);
  assert.deepEqual(g.map((c) => c.ids), [["A", "B"], ["C", "D"], ["E", "F"]]);
  assert.deepEqual(g.map((c) => c.count), [2, 2, 2]);
  near(g[0].lat, 0);
  near(g[0].lng, 0.05, 1e-9);
  near(g[1].lat, 10.025000964168566);
  near(g[1].lng, 10.025, 1e-9);
  near(g[2].lat, 0);
  assert.equal(g[2].lng, -180); // the mean, at 180, is normalized to -180
});

test("at zoom 12 every marker is on its own, as it is", () => {
  const g = m.cluster(points, 12);
  assert.deepEqual(g.map((c) => c.ids), [["A"], ["B"], ["C"], ["D"], ["E"], ["F"]]);
  assert.deepEqual(g.map((c) => [c.lat, c.lng]), points.map((p) => [p.lat, p.lng]));
});

test("the level is the zoom's whole part (a hair under counts), and zoom 0 gathers more", () => {
  assert.equal(m.cluster(points, 3.9999995).length, 3); // level 4
  assert.deepEqual(m.cluster(points, 0).map((c) => c.ids), [["A", "B", "C", "D"], ["E", "F"]]);
  assert.deepEqual(m.cluster(points, 30).map((c) => c.count), [1, 1, 1, 1, 1, 1]); // level 22
});

test("order matters: the first takes the one 30 points on, not the one 60 on", () => {
  const W = 256 * 2 ** 10, d = (30 / W) * 360;
  const row = [0, 1, 2].map((i) => ({ id: "r" + i, lat: 0, lng: i * d }));
  const g = m.cluster(row, 10);
  assert.deepEqual(g.map((c) => c.ids), [["r0", "r1"], ["r2"]]);
  near(g[0].lng, 0.020599365234375);
  near(g[1].lng, 0.0823974609375);
  // From the other end, the last two join instead.
  assert.deepEqual(m.cluster(row.slice().reverse(), 10).map((c) => c.ids), [["r2", "r1"], ["r0"]]);
});

test("many markers: the grid finds what a pass over all would", () => {
  const ps = [];
  let s = 7;
  const rnd = () => ((s = (s * 1103515245 + 12345) % 2147483648) / 2147483648);
  for (let i = 0; i < 400; i++) ps.push({ id: "p" + i, lat: 37.7 + rnd() * 0.1, lng: -122.5 + rnd() * 0.1 });
  for (const z of [8, 11, 13, 15]) {
    const got = m.cluster(ps, z).map((c) => c.ids.join(","));
    // The spec's loop, plainly.
    const W = 256 * 2 ** Math.floor(z), taken = new Set(), want = [];
    const X = ps.map((p) => ((p.lng + 180) / 360) * W), Y = ps.map((p) => m.mercY(p.lat) * W);
    for (let i = 0; i < ps.length; i++) {
      if (taken.has(i)) continue;
      const g = [i];
      for (let j = i + 1; j < ps.length; j++) {
        if (taken.has(j)) continue;
        let dx = X[j] - X[i];
        dx -= W * Math.round(dx / W);
        if (Math.abs(dx) <= 40 && Math.abs(Y[j] - Y[i]) <= 40) {
          taken.add(j);
          g.push(j);
        }
      }
      want.push(g.map((k) => ps[k].id).join(","));
    }
    assert.deepEqual(got, want, `zoom ${z}`);
  }
});

test("a geodesic line follows the great circle, a piece a degree", () => {
  const line = m.geodesic([{ lat: 0, lng: 0 }, { lat: 0, lng: 90 }]);
  assert.equal(line.length, 91);
  near(line[45].lat, 0, 1e-9);
  near(line[45].lng, 45, 1e-9);
  // New York to London bows north of both.
  const nl = m.geodesic([{ lat: 40.7, lng: -74 }, { lat: 51.5, lng: -0.1 }]);
  assert.ok(Math.max(...nl.map((p) => p.lat)) > 52);
  assert.deepEqual(nl.at(-1), { lat: 51.5, lng: -0.1 });
  assert.equal(m.geodesic([{ lat: 1, lng: 1 }, { lat: 1, lng: 1.5 }]).length, 2);
});

test("a circle is 72 points its radius from the center", () => {
  const c = { lat: 37.7793, lng: -122.4193 };
  const ps = m.circlePoints(c, 800);
  assert.equal(ps.length, 72);
  const R = 6371008.8, r = Math.PI / 180;
  for (const p of ps) {
    const a = Math.sin(((p.lat - c.lat) * r) / 2) ** 2 + Math.cos(c.lat * r) * Math.cos(p.lat * r) * Math.sin(((p.lng - c.lng) * r) / 2) ** 2;
    near(2 * R * Math.atan2(Math.sqrt(a), Math.sqrt(1 - a)), 800, 1e-6);
  }
  assert.ok(ps[0].lat > c.lat && Math.abs(ps[0].lng - c.lng) < 1e-12); // north first
  assert.ok(ps[18].lng > c.lng); // then east
});

test("fitting coordinates: as near as they fit in the padded view, centered there", () => {
  const cs = [{ lat: 37.8080, lng: -122.4177 }, { lat: 37.7694, lng: -122.4862 }];
  const c = m.fitCamera(cs, 400, 600, { top: 40, left: 40, bottom: 40, right: 40 });
  const b = m.bounds(cs);
  near(c.zoom, Math.log2(320 / ((b.lngDelta / 360) * 256)), 1e-9);
  near(c.center.lng, b.center.lng, 1e-9);
  near(c.center.lat, m.mercLat((m.mercY(b.north) + m.mercY(b.south)) / 2), 1e-9);
  assert.equal(c.pitch, 0);
  // Room at the bottom only: the center moves south of the bounds', so they sit higher.
  const s = m.fitCamera(cs, 400, 600, { bottom: 300 });
  assert.ok(s.center.lat < c.center.lat);
  // One place: zoom 15, on it.
  const one = m.fitCamera([cs[0]], 400, 600, {});
  assert.equal(one.zoom, 15);
  near(one.center.lat, cs[0].lat, 1e-9);
  // Across the 180th meridian, the short way.
  const fj = m.fitCamera([{ lat: -17, lng: 178 }, { lat: -18, lng: -179 }], 400, 400, {});
  assert.ok(fj.zoom > 7.5 && fj.zoom < 7.6);
  near(fj.center.lng, 179.5, 1e-9);
  assert.ok(m.fitCamera(cs, 400, 600, {}, 30).heading === 30);
});

test("fitting a turned map: every place in the padded view, one at each of two edges", () => {
  const cs = [{ lat: 37.8080, lng: -122.4177 }, { lat: 37.7955, lng: -122.3937 }, { lat: 37.7694, lng: -122.4862 }];
  const pad = { top: 40, left: 40, bottom: 40, right: 40 };
  const c = m.fitCamera(cs, 900, 700, pad, 45);
  const W = 256 * 2 ** c.zoom, cx = (c.center.lng + 180) / 360, cy = m.mercY(c.center.lat);
  const xs = [], ys = [];
  for (const p of cs) {
    const [x, y] = m.turn(((p.lng + 180) / 360 - cx) * W, (m.mercY(p.lat) - cy) * W, 45);
    xs.push(450 + x);
    ys.push(350 + y);
  }
  for (let i = 0; i < cs.length; i++) assert.ok(xs[i] >= 40 - 1e-6 && xs[i] <= 860 + 1e-6 && ys[i] >= 40 - 1e-6 && ys[i] <= 660 + 1e-6);
  const touches = (v, a, b) => Math.abs(Math.min(...v) - a) < 1e-6 && Math.abs(Math.max(...v) - b) < 1e-6;
  assert.ok(touches(xs, 40, 860) || touches(ys, 40, 660));
});

test("bounds go the short way round the world", () => {
  const b = m.bounds([{ lat: -17, lng: 178 }, { lat: -18, lng: -179 }]);
  near(b.lngDelta, 3, 1e-9);
  near(b.center.lng, 179.5, 1e-9);
  near(b.latDelta, 1, 1e-9);
});

test("hit tests: inside a polygon but not its hole; near a line", () => {
  const outer = [[0, 0], [100, 0], [100, 100], [0, 100]];
  const hole = [[40, 40], [60, 40], [60, 60], [40, 60]];
  assert.ok(m.inRings([outer, hole], 10, 10));
  assert.ok(!m.inRings([outer, hole], 50, 50));
  assert.ok(!m.inRings([outer, hole], 150, 50));
  near(m.nearLine([[0, 0], [100, 0]], 50, 7), 7);
  near(m.nearLine([[0, 0], [100, 0]], 110, 0), 10);
});

test("overlays draw by zIndex, then polygons, circles, lines", () => {
  const order = m.overlayOrder([{ id: "p1" }, { id: "p2", zIndex: 2 }], [{ id: "c1" }], [{ id: "l1" }, { id: "l2", zIndex: -1 }]);
  assert.deepEqual(order.map((o) => o.o.id), ["l2", "p1", "c1", "l1", "p2"]);
});

test("a turned map: east is up when the top faces east, and back", () => {
  const [x, y] = m.turn(1, 0, 90);
  near(x, 0, 1e-12);
  near(y, -1, 1e-12);
  const [a, b] = m.unturn(...m.turn(3, 4, 33), 33);
  near(a, 3, 1e-12);
  near(b, 4, 1e-12);
});

test("Mercator round trips, longitudes normalized", () => {
  near(m.mercY(0), 0.5);
  near(m.mercLat(m.mercY(51.5)), 51.5, 1e-9);
  assert.equal(m.normalLng(180), -180);
  assert.equal(m.normalLng(-181), 179);
  assert.equal(m.normalLng(540), -180);
});

test("colors: an rgba one without the runtime, the runtime's for the rest", () => {
  const rgba = 2 * 4294967296 + 0x1565c0ff;
  assert.equal(m.colorCSS(rgba), "rgb(21 101 192 / 1.000)");
  assert.equal(m.colorCSS(null), "");
  globalThis.asuka.native.color = (v) => "css:" + v;
  assert.equal(m.colorCSS(5), "css:5");
  delete globalThis.asuka.native.color;
});

test("Maps' methods with no map behind the Ref do nothing; camera is none", () => {
  const maps = m.makeMaps();
  maps.moveTo(null, { center: { lat: 1, lng: 2 }, latDelta: 1, lngDelta: 1 }, true);
  maps.fit(null, [{ lat: 1, lng: 2 }], {}, false);
  assert.deepEqual(maps.camera(null), { center: { lat: 0, lng: 0 }, zoom: 0, heading: 0, pitch: 0 });
});
