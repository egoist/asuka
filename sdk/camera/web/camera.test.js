// The camera's permission against a browser's, stubbed: access follows
// navigator.permissions' "camera" (its query and its change event), and
// requestAccess opens the camera and closes it at once. The View's zoom
// (the track's, in its units, or digital), torch, exposure and focus
// against a stubbed track and DOM; and a recording's maxBytes.
import { test } from "node:test";
import assert from "node:assert/strict";

class DeclaredError extends Error {
  constructor(index, message) {
    super(message);
    this.index = index;
  }
}
globalThis.asuka = {
  native: {
    DeclaredError, UnsupportedError: Error, NativeWriter: class {}, NativeReader: class {},
    registerModule() {}, retain() {}, object() {},
  },
};

// A browser whose camera permission is status's, and whose getUserMedia
// does what media says.
const status = Object.assign(new EventTarget(), { state: "prompt" });
const asked = [];
let media = null; // (constraints) => a stream, or throws
Object.defineProperty(globalThis, "navigator", {
  configurable: true,
  writable: true,
  value: {
    permissions: { async query(d) { asked.push(d.name); return status; } },
    mediaDevices: { async getUserMedia(c) { return media(c); }, getSupportedConstraints: () => ({ pointsOfInterest: true }) },
  },
});

// Enough of a DOM for a View: elements with a style, listeners and children.
class El extends EventTarget {
  constructor(tag) {
    super();
    this.tag = tag;
    this.style = {};
    this.children = [];
  }
  appendChild(c) { this.children.push(c); return c; }
  play() { return Promise.resolve(); }
  setPointerCapture() {}
  getBoundingClientRect() { return { left: 0, top: 0, width: 200, height: 100 }; }
}
globalThis.document = { createElement: (t) => new El(t) };

function stream() {
  const track = { stopped: false, stop() { this.stopped = true; } };
  return { track, getTracks: () => [track] };
}

const named = (name) => Object.assign(new Error(name), { name });

const { makeCamera, makeView, zoomRange, cropRect, outSize, framePoint, videoSize, DIGITAL_MAX_ZOOM } = await import("./camera.js");
const { Recording } = await import("./media.js");
const { Facing, Mode, VideoQuality } = await import("./camera.decl.asuka.js");

test("access follows the permission, requestAccess asks", async () => {
  const cam = makeCamera();
  assert.equal(cam.access(), "notAsked");
  await new Promise((r) => setImmediate(r));
  assert.deepEqual(asked, ["camera"]);
  assert.equal(cam.access(), "notAsked");

  status.state = "granted";
  status.dispatchEvent(new Event("change"));
  assert.equal(cam.access(), "allowed");

  let s;
  media = (c) => {
    assert.deepEqual(c, { video: true });
    return (s = stream());
  };
  assert.equal(await cam.requestAccess(new AbortController().signal), "allowed");
  assert.ok(s.track.stopped, "the camera's closed at once");

  media = () => { throw named("NotAllowedError"); };
  assert.equal(await cam.requestAccess(), "denied");
  assert.equal(cam.access(), "denied");

  // No camera: nothing learned, the permission as it was.
  status.state = "granted";
  status.dispatchEvent(new Event("change"));
  media = () => { throw named("NotFoundError"); };
  assert.equal(await cam.requestAccess(), "allowed");

  status.state = "denied";
  status.dispatchEvent(new Event("change"));
  assert.equal(cam.access(), "denied");
});

test("no mediaDevices (an insecure page): denied", async () => {
  const { mediaDevices } = navigator;
  delete navigator.mediaDevices;
  try {
    assert.equal(await makeCamera().requestAccess(), "denied");
  } finally {
    navigator.mediaDevices = mediaDevices;
  }
});

test("zoom: a track's in its own units from its least, else digital", () => {
  assert.deepEqual(zoomRange({ zoom: { min: 100, max: 400, step: 1 } }), { hw: true, base: 100, lo: 100, hi: 400, min: 1, max: 4 });
  // A phone's ratio, with an ultra wide: 1 is the main camera's.
  assert.deepEqual(zoomRange({ zoom: { min: 0.5, max: 10 } }), { hw: true, base: 1, lo: 0.5, hi: 10, min: 0.5, max: 10 });
  assert.equal(zoomRange({}).hw, false);
  assert.equal(zoomRange({ zoom: { min: 1, max: 1 } }).max, DIGITAL_MAX_ZOOM);
  assert.deepEqual(cropRect(1920, 1080, 2), { sx: 480, sy: 270, sw: 960, sh: 540 });
  assert.deepEqual(cropRect(100, 50, 0.5), { sx: 0, sy: 0, sw: 100, sh: 50 });
  assert.deepEqual(outSize(1920, 1080, 480), [480, 270]);
  assert.deepEqual(outSize(640, 480, 2048), [640, 480], "never up");
  assert.deepEqual(outSize(640, 480, 0), [640, 480]);
  assert.deepEqual(videoSize(VideoQuality.sd), [640, 480]);
  assert.deepEqual(videoSize(VideoQuality.uhd), [3840, 2160]);
});

test("a point of the view in the camera's frame: cover, digital zoom, mirror", () => {
  // A 200×100 view of a 100×100 frame: its middle half shows.
  assert.deepEqual(framePoint(200, 100, 100, 100, 0.5, 0.5, 1, false), { x: 0.5, y: 0.5 });
  assert.deepEqual(framePoint(200, 100, 100, 100, 0.5, 0, 1, false), { x: 0.5, y: 0.25 });
  assert.deepEqual(framePoint(200, 100, 100, 100, 0, 0.5, 1, false), { x: 0, y: 0.5 });
  // At 2× its middle's middle; mirrored, the other side.
  assert.deepEqual(framePoint(100, 100, 100, 100, 0, 0, 2, false), { x: 0.25, y: 0.25 });
  assert.deepEqual(framePoint(100, 100, 100, 100, 0, 0, 2, true), { x: 0.75, y: 0.25 });
});

const tick = () => new Promise((r) => setTimeout(r, 5));

function cameraTrack(caps, settings = {}) {
  return {
    applied: [], stopped: false,
    getCapabilities: () => caps,
    getSettings: () => ({ width: 1280, height: 720, ...settings }),
    async applyConstraints(c) { this.applied.push(c.advanced[0]); },
    stop() { this.stopped = true; },
  };
}
const streamOf = (track) => ({ getTracks: () => [track], getVideoTracks: () => [track], getAudioTracks: () => [] });

function view(track) {
  const said = { ready: [], zoom: [], focus: [], error: [] };
  let asked;
  media = (c) => { asked = c; return streamOf(track); };
  const v = makeView({
    ready: (i) => said.ready.push(i), zoomChange: (z) => said.zoom.push(z), focusPoint: (p) => said.focus.push(p),
    error: (m) => said.error.push(m), code() {}, frame() {},
  });
  return { v, said, asked: () => asked };
}

const pointer = (type, id, x, y) => Object.assign(new Event(type), { pointerId: id, clientX: x, clientY: y });

test("a View on a camera that zooms: ready says what it can, the props reach the track", async () => {
  const track = cameraTrack(
    { zoom: { min: 100, max: 400, step: 10 }, torch: true, exposureCompensation: { min: -2, max: 2, step: 0.5 }, focusMode: ["continuous", "single-shot"] },
    { pointsOfInterest: [] },
  );
  const { v, said, asked } = view(track);
  v.setMode(Mode.video);
  v.setVideoQuality(VideoQuality.hd);
  v.setTorch(true);
  v.setExposure(5);
  v.setZoom(2.5);
  await tick();
  assert.equal(asked().video.width.ideal, 1280);
  assert.equal(asked().video.zoom, true);
  assert.deepEqual(said.ready, [{ minZoom: 1, maxZoom: 4, minExposure: -2, maxExposure: 2, torch: true, flash: false, focus: true }]);
  await tick();
  assert.deepEqual(track.applied.at(-1), { zoom: 250, torch: true, exposureCompensation: 2 });
  assert.equal(said.zoom.length, 0, "a prop zooms without zoomChange");

  // A pinch: from the zoom it's at, clamped, said.
  v.el.dispatchEvent(pointer("pointerdown", 1, 50, 50));
  v.el.dispatchEvent(pointer("pointerdown", 2, 70, 50));
  v.el.dispatchEvent(pointer("pointermove", 2, 90, 50));
  assert.deepEqual(said.zoom, [4]);
  v.el.dispatchEvent(pointer("pointerup", 1, 50, 50));
  v.el.dispatchEvent(pointer("pointerup", 2, 90, 50));
  assert.equal(said.focus.length, 0, "a pinch isn't a tap");
  await tick();
  assert.equal(track.applied.at(-1).zoom, 400);

  // A tap focuses there, once, and then the camera's own resumes.
  v.el.dispatchEvent(pointer("pointerdown", 3, 50, 25));
  v.el.dispatchEvent(pointer("pointerup", 3, 52, 25));
  assert.deepEqual(said.focus, [{ x: 0.26, y: 0.25 }]);
  await tick();
  assert.equal(track.applied.at(-1).focusMode, "single-shot");
  assert.equal(track.applied.at(-1).pointsOfInterest.length, 1);
  v.setTapToFocus(false);
  v.el.dispatchEvent(pointer("pointerdown", 4, 50, 25));
  v.el.dispatchEvent(pointer("pointerup", 4, 50, 25));
  assert.equal(said.focus.length, 1);
  v.destroy();
  assert.ok(track.stopped);
});

test("a View on a camera that doesn't zoom: digitally, up to 4×, the preview scaled", async () => {
  const track = cameraTrack({});
  const { v, said } = view(track);
  v.setFacing(Facing.front);
  v.setZoom(9);
  await tick();
  assert.deepEqual(said.ready, [{ minZoom: 1, maxZoom: 4, minExposure: 0, maxExposure: 0, torch: false, flash: false, focus: false }]);
  const video = v.el.children[0];
  assert.equal(video.style.transform, "scaleX(-1) scale(4)");
  assert.deepEqual(track.applied, [], "nothing asked of the track");
  // ctrl and the wheel: a trackpad's pinch.
  const wheel = Object.assign(new Event("wheel", { cancelable: true }), { ctrlKey: true, deltaY: 100 * Math.log(2) });
  v.el.dispatchEvent(wheel);
  assert.ok(wheel.defaultPrevented);
  assert.ok(Math.abs(said.zoom[0] - 2) < 1e-9);
  assert.equal(video.style.transform, `scaleX(-1) scale(${said.zoom[0]})`);
  v.setPinchToZoom(false);
  v.el.dispatchEvent(Object.assign(new Event("wheel", { cancelable: true }), { ctrlKey: true, deltaY: -100 }));
  assert.equal(said.zoom.length, 1);
  v.destroy();
});

test("maxBytes stops a recording as its file nears it", async () => {
  const written = new Map();
  globalThis.asuka.native.fs = { writeFile: (p, b) => written.set(p, b) };
  const recorders = [];
  globalThis.MediaRecorder = class {
    constructor() { this.state = "inactive"; this.mimeType = "video/webm"; recorders.push(this); }
    start(slice) { this.slice = slice; this.state = "recording"; }
    stop() { this.state = "inactive"; this.ondataavailable({ data: new Blob([new Uint8Array(10)]) }); this.onstop(); }
    chunk(n) { this.ondataavailable({ data: new Blob([new Uint8Array(n)]) }); }
  };
  try {
    const done = new Promise((resolve, reject) => new Recording({}, { path: "/tmp/v.webm", video: true, maxBytes: 1000 }, (e, p) => (e ? reject(e) : resolve(p))));
    const r = recorders[0];
    assert.equal(r.slice, 250, "a quarter second's chunks");
    r.chunk(300);
    r.chunk(300);
    assert.equal(r.state, "recording");
    r.chunk(300); // 900, and another 300 would pass it
    assert.equal(r.state, "inactive");
    assert.equal(await done, "/tmp/v.webm");
    assert.equal(written.get("/tmp/v.webm").length, 910);
  } finally {
    delete globalThis.MediaRecorder;
  }
});

test("a zoom set before the camera starts is the camera's: an ultra wide's 0.5", async () => {
  const track = cameraTrack({ zoom: { min: 0.5, max: 8, step: 0.1 } });
  const { v, said } = view(track);
  v.setZoom(0.5);
  await tick();
  assert.equal(said.ready[0].minZoom, 0.5);
  await tick();
  assert.equal(track.applied.at(-1).zoom, 0.5);
  v.destroy();
});
