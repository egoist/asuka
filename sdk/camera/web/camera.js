// platform/camera on the web: camera.View is a <video> of the camera
// (getUserMedia), front or back; capture takes a JPEG of what it shows,
// record records it with a MediaRecorder (MP4 where the browser records
// it, else WebM); scan reads codes with the browser's BarcodeDetector,
// where there is one (Chrome on Android and macOS). take is a phone's
// camera app (<input capture>, where the screen is touch), else a dialog
// of the page's own: the camera's preview and a shutter. Photos, videos
// and frames go to the app's files' /tmp/asuka-camera.
//
// What the track can do, it does (Image Capture's constraints: zoom,
// torch, exposureCompensation, pointsOfInterest and focusMode, and
// ImageCapture.takePhoto's fillLightMode for the flash). A camera
// without a zoom of its own (most webcams, a browser that exposes none)
// zooms digitally, up to 4×: the preview is scaled from its middle, and
// photos, frames and videos are cropped to the same middle (a video
// through a canvas, as a mirrored front camera's is). photoQuality has
// nothing to choose on the web.
import { Facing, Mode, Flash, VideoQuality, CameraError } from "./camera.asuka.js";
import { Recording, mediaError, recordingType, uuid } from "./media.js";

const fs = () => globalThis.asuka.native.fs;
const facingMode = (f) => (f === Facing.front ? "user" : "environment");
const clamp = (x, lo, hi) => Math.min(Math.max(x, lo), hi);

/** The digital zoom's most, where the camera has none of its own. */
export const DIGITAL_MAX_ZOOM = 4;
/** How many frame files a View writes in turn. */
const FRAME_RING = 4;
/** How long a tap's focus holds before the camera's own (continuous) autofocus resumes: CameraX's default. */
const FOCUS_HOLD_MS = 5000;

/** videoSize is the width and height a View in video mode asks for, at a quality. */
export function videoSize(q) {
  switch (q) {
    case VideoQuality.sd: return [640, 480];
    case VideoQuality.hd: return [1280, 720];
    case VideoQuality.uhd: return [3840, 2160];
  }
  return [1920, 1080];
}

/**
 * zoomRange is how a track zooms, from its capabilities: hw, whether it
 * zooms itself (else digitally); base, its zoom value for Asuka's 1 (a
 * webcam's units start anywhere: 100 to 500 is 1× to 5×; a phone's are a
 * ratio, below 1 an ultra wide); min and max, Asuka's.
 */
export function zoomRange(caps) {
  const z = caps?.zoom;
  if (z && z.max > z.min && z.min > 0) {
    const base = z.min < 1 ? 1 : z.min;
    return { hw: true, base, lo: z.min, hi: z.max, min: z.min / base, max: z.max / base };
  }
  return { hw: false, base: 1, lo: 1, hi: 1, min: 1, max: DIGITAL_MAX_ZOOM };
}

/** cropRect is the middle of a w×h frame a digital zoom of z shows. */
export function cropRect(w, h, z) {
  const f = z > 1 ? z : 1;
  const sw = w / f, sh = h / f;
  return { sx: (w - sw) / 2, sy: (h - sh) / 2, sw, sh };
}

/** outSize is a w×h scaled down (never up) to at most max on its long side (0: as it is). */
export function outSize(w, h, max) {
  const s = max > 0 ? Math.min(1, max / Math.max(w, h)) : 1;
  return [Math.max(1, Math.round(w * s)), Math.max(1, Math.round(h * s))];
}

/**
 * framePoint is where a point of the view (fractions of it, as the user
 * sees it) is in the camera's frame (fractions of it): the preview
 * fills the view (object-fit: cover), is scaled by the digital zoom from
 * its middle, and is mirrored for the front camera.
 */
export function framePoint(viewW, viewH, frameW, frameH, x, y, zoom, mirrored) {
  if (!viewW || !viewH || !frameW || !frameH) return { x, y };
  let px = viewW / 2 + (x * viewW - viewW / 2) / zoom;
  const py = viewH / 2 + (y * viewH - viewH / 2) / zoom;
  if (mirrored) px = viewW - px;
  const s = Math.max(viewW / frameW, viewH / frameH);
  const dw = frameW * s, dh = frameH * s;
  return { x: clamp((px - (viewW - dw) / 2) / dw, 0, 1), y: clamp((py - (viewH - dh) / 2) / dh, 0, 1) };
}

function canvas(w, h) {
  if (typeof OffscreenCanvas !== "undefined") return new OffscreenCanvas(w, h);
  const c = document.createElement("canvas");
  c.width = w;
  c.height = h;
  return c;
}

/** draw draws src (srcW×srcH) to g's w×h: its middle at a digital zoom, mirrored if asked. */
function draw(g, src, srcW, srcH, zoom, mirror, w, h) {
  const { sx, sy, sw, sh } = cropRect(srcW, srcH, zoom);
  g.save();
  if (mirror) {
    g.translate(w, 0);
    g.scale(-1, 1);
  }
  g.drawImage(src, sx, sy, sw, sh, 0, 0, w, h);
  g.restore();
}

async function jpeg(c, quality) {
  const blob = c.convertToBlob
    ? await c.convertToBlob({ type: "image/jpeg", quality })
    : await new Promise((resolve) => c.toBlob(resolve, "image/jpeg", quality));
  if (!blob) throw new Error("can't write the image");
  return new Uint8Array(await blob.arrayBuffer());
}

/**
 * writeJPEG writes a drawable (srcW×srcH) to the app's files as a JPEG on
 * white, its middle at a digital zoom, at most maxSize on its long side,
 * mirrored if asked: its path.
 */
async function writeJPEG(src, srcW, srcH, { zoom = 1, maxSize = 0, mirror = false, quality = 0.92, path } = {}) {
  const { sw, sh } = cropRect(srcW, srcH, zoom);
  const [w, h] = outSize(sw, sh, maxSize);
  const c = canvas(w, h);
  const g = c.getContext("2d");
  g.fillStyle = "#fff";
  g.fillRect(0, 0, w, h);
  draw(g, src, srcW, srcH, zoom, mirror, w, h);
  const bytes = await jpeg(c, quality);
  path ??= `/tmp/asuka-camera/${uuid()}.jpg`;
  fs().writeFile(path, bytes);
  return { path, width: w, height: h };
}

// platform/scan's kinds and the Shape Detection API's formats.
const formats = {
  qr: "qr_code", aztec: "aztec", datamatrix: "data_matrix", pdf417: "pdf417", ean13: "ean_13", ean8: "ean_8",
  upca: "upc_a", upce: "upc_e", code128: "code_128", code39: "code_39", code93: "code_93", codabar: "codabar", itf: "itf",
};
const kinds = Object.fromEntries(Object.entries(formats).map(([k, f]) => [f, k]));
const kindOf = (format) => kinds[format] || format;

async function barcodeDetector(list) {
  const supported = await BarcodeDetector.getSupportedFormats?.() ?? [];
  const want = (list.length ? list : Object.keys(formats)).map((k) => formats[k]).filter((f) => f && (!supported.length || supported.includes(f)));
  return new BarcodeDetector(want.length ? { formats: want } : undefined);
}

/** The camera views on the page, newest last: capture, record and focus use the newest. */
const views = [];
const current = () => [...views].reverse().find((v) => v.alive && v.stream);
let viewIDs = 0;

export function makeView(events) {
  const el = document.createElement("div");
  // touch-action: the view's own pinch and taps, not the page's zoom.
  el.style.cssText = "position:relative;overflow:hidden;background:#000;touch-action:none";
  const video = document.createElement("video");
  video.muted = true;
  video.playsInline = true;
  video.autoplay = true;
  video.style.cssText = "width:100%;height:100%;object-fit:cover;display:block;transform-origin:50% 50%";
  el.appendChild(video);
  const v = {
    id: ++viewIDs, el, video, stream: null, track: null, imageCapture: null, range: zoomRange(null),
    caps: {}, canFocus: false, canPoint: false, flash: false, zoom: 1, wanted: 1, focus: null,
    props: {
      facing: Facing.back, mode: Mode.photo, scan: false, codes: [], zoom: 1, pinchToZoom: true, torch: false,
      flash: Flash.off, exposure: 0, tapToFocus: true, videoQuality: VideoQuality.fullHD, mirror: false, frameRate: 0, frameSize: 480,
    },
    recording: null, alive: true, started: performance.now(),
  };
  views.push(v);
  let timer = 0, scanTimer = 0, frameTimer = 0, focusTimer = 0, starts = 0;

  // The digital zoom (1 where the camera zooms itself), and whether what it
  // gives is mirrored (the front camera's, with mirror).
  v.digital = () => (v.range.hw ? 1 : v.zoom);
  v.mirrored = () => v.props.facing === Facing.front && v.props.mirror;

  const preview = () => {
    const t = [];
    if (v.props.facing === Facing.front) t.push("scaleX(-1)");
    if (v.digital() !== 1) t.push(`scale(${v.digital()})`);
    video.style.transform = t.join(" ");
  };

  // The track's settings, all in one advanced set (Chrome takes Image
  // Capture's constraints only on their own, and a set's values together):
  // applied one at a time, the latest after the one in flight.
  let applying = false, again = false;
  const apply = async () => {
    const track = v.track;
    if (!track?.applyConstraints) return;
    if (applying) { again = true; return; }
    const c = {};
    if (v.range.hw) {
      let z = clamp(v.zoom * v.range.base, v.range.lo, v.range.hi);
      const step = v.caps.zoom?.step;
      if (step > 0) z = clamp(v.range.lo + Math.round((z - v.range.lo) / step) * step, v.range.lo, v.range.hi);
      c.zoom = z;
    }
    if (v.caps.torch) c.torch = !!v.props.torch;
    const ev = v.caps.exposureCompensation;
    if (ev && ev.max > ev.min) c.exposureCompensation = clamp(v.props.exposure, ev.min, ev.max);
    if (v.focus) {
      if (v.canPoint) c.pointsOfInterest = [v.focus.point];
      if (v.focus.mode) c.focusMode = v.focus.mode;
    }
    if (!Object.keys(c).length) return;
    applying = true;
    try {
      await track.applyConstraints({ advanced: [c] });
    } catch { /* what the camera turned down stays as it was */ }
    applying = false;
    if (again && v.track === track) {
      again = false;
      apply();
    }
  };

  const setZoom = (z, user) => {
    const was = v.zoom;
    v.wanted = Number(z) || 1;
    v.zoom = clamp(v.wanted, v.range.min, v.range.max);
    if (user) v.wanted = v.zoom;
    if (v.zoom === was) return;
    preview();
    apply();
    if (user) events.zoomChange(v.zoom);
  };

  const stopStream = () => {
    v.stream?.getTracks().forEach((t) => t.stop());
    v.stream = null;
    v.track = null;
    v.imageCapture = null;
  };

  const restart = () => {
    clearTimeout(timer);
    timer = setTimeout(start, 0); // once this batch's props are all set
  };

  const start = async () => {
    if (!v.alive) return;
    const mine = ++starts;
    stopStream();
    if (!navigator.mediaDevices?.getUserMedia) {
      events.error("there's no camera");
      return;
    }
    const [width, height] = v.props.mode === Mode.video ? videoSize(v.props.videoQuality) : [1920, 1080];
    // zoom: true asks for the camera's zoom too (Chrome: pan-tilt-zoom's
    // permission, for a camera that has one).
    const video0 = { facingMode: facingMode(v.props.facing), width: { ideal: width }, height: { ideal: height }, zoom: true };
    let s;
    try {
      s = await navigator.mediaDevices.getUserMedia({ video: video0, audio: v.props.mode === Mode.video });
    } catch (e) {
      const err = mediaError(e);
      if (mine === starts) events.error(err ? (err.message === "not allowed" ? "the camera isn't allowed" : err.message) : "there's no camera");
      return;
    }
    if (!v.alive || mine !== starts) { s.getTracks().forEach((t) => t.stop()); return; }
    v.stream = s;
    v.track = s.getVideoTracks()[0] ?? null;
    try { v.caps = v.track?.getCapabilities?.() ?? {}; } catch { v.caps = {}; }
    const supported = navigator.mediaDevices.getSupportedConstraints?.() ?? {};
    const modes = v.caps.focusMode ?? [];
    // A point to focus and meter at: where the browser takes one and the
    // camera says it has one (Chrome's settings name it once set) or
    // focuses once (single-shot: Chrome on Android's cameras).
    v.canPoint = !!supported.pointsOfInterest && ("pointsOfInterest" in (v.track?.getSettings?.() ?? {}) || modes.includes("single-shot"));
    v.canFocus = v.canPoint && (modes.includes("single-shot") || modes.includes("continuous"));
    v.range = zoomRange(v.caps);
    v.zoom = clamp(v.wanted, v.range.min, v.range.max); // the zoom last asked for (or pinched to), on this camera
    v.focus = null;
    v.flash = false;
    if (typeof ImageCapture !== "undefined" && v.track) {
      try {
        v.imageCapture = new ImageCapture(v.track);
        const pc = await v.imageCapture.getPhotoCapabilities();
        v.flash = (pc?.fillLightMode ?? []).includes("flash");
      } catch { /* no photos of its own: the preview's frames */ }
      if (!v.alive || mine !== starts) return;
    }
    video.srcObject = s;
    preview();
    video.play().catch(() => {});
    apply();
    scan();
    frames();
    const ev = v.caps.exposureCompensation;
    const exp = ev && ev.max > ev.min;
    events.ready({
      minZoom: v.range.min, maxZoom: v.range.max,
      minExposure: exp ? ev.min : 0, maxExposure: exp ? ev.max : 0,
      torch: !!v.caps.torch, flash: v.flash, focus: v.canFocus,
    });
  };

  // Codes: read every quarter second while the view scans; each code once
  // until it's out of sight.
  const seen = new Set();
  const scan = () => {
    clearTimeout(scanTimer);
    if (!v.props.scan || !v.alive || typeof BarcodeDetector === "undefined") return;
    const detector = barcodeDetector(v.props.codes);
    const tick = async () => {
      if (!v.alive || !v.props.scan) return;
      if (video.readyState >= 2) {
        try {
          const found = await (await detector).detect(video);
          const now = new Set();
          for (const c of found) {
            const kind = kindOf(c.format), key = kind + "\n" + c.rawValue;
            now.add(key);
            if (!seen.has(key)) events.code({ kind, value: c.rawValue });
          }
          seen.clear();
          for (const k of now) seen.add(k);
        } catch {}
      }
      scanTimer = setTimeout(tick, 250);
    };
    tick();
  };

  // Frames: frameRate a second (at most 15), each a JPEG of the preview's
  // frame (its digital zoom's middle, mirrored with mirror), written in
  // turn to a ring of files of the view's; a frame comes only after the
  // last is written (the ones between are dropped).
  let frameCanvas = null, frameBusy = false, frameN = 0;
  const frames = () => {
    clearInterval(frameTimer);
    frameTimer = 0;
    const rate = Math.min(Number(v.props.frameRate) || 0, 15);
    if (rate <= 0 || !v.alive || !v.stream) return;
    frameTimer = setInterval(async () => {
      if (frameBusy || !v.alive || video.readyState < 2 || !video.videoWidth) return;
      frameBusy = true;
      try {
        const time = performance.now() - v.started;
        const fw = video.videoWidth, fh = video.videoHeight, z = v.digital();
        const { sw, sh } = cropRect(fw, fh, z);
        const [w, h] = outSize(sw, sh, Math.max(16, v.props.frameSize | 0));
        if (!frameCanvas || frameCanvas.width !== w || frameCanvas.height !== h) frameCanvas = canvas(w, h);
        draw(frameCanvas.getContext("2d"), video, fw, fh, z, v.mirrored(), w, h);
        const bytes = await jpeg(frameCanvas, 0.8);
        if (!v.alive) return;
        const path = `/tmp/asuka-camera/frame-${v.id}-${frameN++ % FRAME_RING}.jpg`;
        fs().writeFile(path, bytes);
        events.frame({ path, width: w, height: h, time });
      } catch { /* this frame's dropped */ } finally {
        frameBusy = false;
      }
    }, 1000 / rate);
  };

  // Focus and metering at a point of the view (fractions of it): the
  // camera's single-shot focus there (else its continuous one, aimed
  // there), until the camera's own continuous autofocus resumes at the
  // middle a few seconds on, as CameraX's does.
  v.focusAt = (x, y, tapped) => {
    if (!v.track || !v.canPoint) return;
    const r = el.getBoundingClientRect?.() ?? { width: 0, height: 0 };
    const point = framePoint(r.width, r.height, video.videoWidth, video.videoHeight, x, y, v.digital(), v.props.facing === Facing.front);
    const modes = v.caps.focusMode ?? [];
    v.focus = { point, mode: modes.includes("single-shot") ? "single-shot" : modes.includes("continuous") ? "continuous" : null };
    apply();
    clearTimeout(focusTimer);
    if (modes.includes("continuous")) {
      focusTimer = setTimeout(() => {
        if (!v.alive) return;
        v.focus = { point: { x: 0.5, y: 0.5 }, mode: "continuous" };
        apply();
      }, FOCUS_HOLD_MS);
    }
    if (tapped) events.focusPoint({ x, y });
  };

  // Gestures: two pointers pinch (from the zoom it's at), ctrl and the
  // wheel (a trackpad's pinch) too; a pointer that goes up where it went
  // down is a tap.
  const pointers = new Map();
  let pinch = null, several = false;
  const spread = () => {
    const [a, b] = [...pointers.values()];
    return Math.hypot(a.x - b.x, a.y - b.y);
  };
  el.addEventListener("pointerdown", (e) => {
    pointers.set(e.pointerId, { x: e.clientX, y: e.clientY, x0: e.clientX, y0: e.clientY });
    try { el.setPointerCapture(e.pointerId); } catch {}
    if (pointers.size >= 2) {
      several = true;
      if (pointers.size === 2 && v.props.pinchToZoom) pinch = { d: spread() || 1, zoom: v.zoom };
    }
  });
  el.addEventListener("pointermove", (e) => {
    const p = pointers.get(e.pointerId);
    if (!p) return;
    p.x = e.clientX;
    p.y = e.clientY;
    if (pinch && pointers.size === 2 && v.props.pinchToZoom) setZoom(pinch.zoom * (spread() / pinch.d), true);
  });
  const up = (e, tap) => {
    const p = pointers.get(e.pointerId);
    if (!p) return;
    pointers.delete(e.pointerId);
    if (pointers.size < 2) pinch = null;
    if (tap && !several && v.props.tapToFocus && Math.hypot(e.clientX - p.x0, e.clientY - p.y0) < 10) {
      const r = el.getBoundingClientRect();
      if (r.width && r.height) v.focusAt(clamp((e.clientX - r.left) / r.width, 0, 1), clamp((e.clientY - r.top) / r.height, 0, 1), true);
    }
    if (!pointers.size) several = false;
  };
  el.addEventListener("pointerup", (e) => up(e, true));
  el.addEventListener("pointercancel", (e) => up(e, false));
  el.addEventListener("wheel", (e) => {
    if (!e.ctrlKey || !v.props.pinchToZoom) return;
    e.preventDefault();
    setZoom(v.zoom * Math.exp(-e.deltaY / 100), true);
  }, { passive: false });

  restart();
  const set = (name, value) => {
    const old = v.props[name];
    v.props[name] = value;
    return old !== value;
  };
  return {
    el,
    setFacing(f) { if (set("facing", f)) restart(); },
    setMode(m) { if (set("mode", m)) restart(); },
    setVideoQuality(q) { if (set("videoQuality", q) && v.props.mode === Mode.video) restart(); },
    setScan(on) { set("scan", on); scan(); },
    setCodes(list) { set("codes", list); scan(); },
    setZoom(z) { set("zoom", z); setZoom(z, false); },
    setPinchToZoom(on) { set("pinchToZoom", on); },
    setTorch(on) { if (set("torch", on)) apply(); },
    setFlash(f) { set("flash", f); },
    setExposure(ev) { if (set("exposure", ev)) apply(); },
    setTapToFocus(on) { set("tapToFocus", on); },
    setPhotoQuality() {}, // the web's photos have no quality to favor
    setMirror(on) { set("mirror", on); },
    setFrameRate(r) { if (set("frameRate", r)) frames(); },
    setFrameSize(n) { set("frameSize", n); },
    destroy() {
      v.alive = false;
      clearTimeout(timer);
      clearTimeout(scanTimer);
      clearInterval(frameTimer);
      clearTimeout(focusTimer);
      v.recording?.stop();
      stopStream();
      views.splice(views.indexOf(v), 1);
    },
  };
}

/**
 * canvasTrack is a video track of v's preview drawn to a canvas, its
 * digital zoom's middle, mirrored if asked: what a recording records
 * where the camera's own track can't give that. It draws each frame the
 * <video> shows.
 */
function canvasTrack(v, mirror) {
  const { video } = v;
  const s = v.track?.getSettings?.() ?? {};
  const c = document.createElement("canvas");
  c.width = video.videoWidth || s.width || 1280;
  c.height = video.videoHeight || s.height || 720;
  const g = c.getContext("2d");
  let on = true, handle = 0;
  const rvfc = typeof video.requestVideoFrameCallback === "function";
  const loop = () => {
    if (!on) return;
    const w = video.videoWidth, h = video.videoHeight;
    if (w && h) draw(g, video, w, h, v.digital(), mirror, c.width, c.height);
    handle = rvfc ? video.requestVideoFrameCallback(loop) : setTimeout(loop, 1000 / 30);
  };
  loop();
  const track = c.captureStream().getVideoTracks()[0];
  return {
    track,
    stop() {
      on = false;
      if (rvfc) video.cancelVideoFrameCallback?.(handle);
      else clearTimeout(handle);
      track.stop();
    },
  };
}

/** takeWithDialog shows the camera's preview in a dialog of the page's own, with a shutter. */
async function takeWithDialog() {
  if (!navigator.mediaDevices?.getUserMedia) throw new CameraError("noCamera");
  let stream;
  try {
    stream = await navigator.mediaDevices.getUserMedia({ video: { facingMode: "environment", width: { ideal: 1920 }, height: { ideal: 1080 } } });
  } catch (e) {
    const err = mediaError(e);
    throw err?.message === "not allowed" ? new CameraError("notAllowed") : err || new CameraError("noCamera");
  }
  return new Promise((resolve, reject) => {
    const d = document.createElement("dialog");
    d.className = "asuka-camera-dialog";
    d.style.cssText = "padding:0;border:0;background:#000;width:min(100vw,960px);max-width:100vw;max-height:100dvh;overflow:hidden";
    const video = document.createElement("video");
    video.muted = video.playsInline = video.autoplay = true;
    video.style.cssText = "display:block;width:100%;max-height:calc(100dvh - 96px);object-fit:contain;background:#000";
    video.srcObject = stream;
    const bar = document.createElement("div");
    bar.style.cssText = "display:flex;align-items:center;justify-content:center;gap:48px;height:96px";
    const shutter = document.createElement("button");
    shutter.setAttribute("aria-label", "Take photo");
    shutter.style.cssText = "width:64px;height:64px;border-radius:50%;border:4px solid #fff;background:#fff;box-shadow:inset 0 0 0 3px #000;cursor:pointer";
    const cancel = document.createElement("button");
    cancel.setAttribute("aria-label", "Cancel");
    cancel.textContent = "✕";
    cancel.style.cssText = "width:44px;height:44px;border-radius:50%;border:0;background:rgba(255,255,255,.2);color:#fff;font-size:20px;cursor:pointer";
    bar.append(cancel, shutter);
    d.append(video, bar);
    document.body.appendChild(d);
    let done = false;
    const end = (path, err) => {
      if (done) return;
      done = true;
      stream.getTracks().forEach((t) => t.stop());
      d.close();
      d.remove();
      err ? reject(err) : resolve(path);
    };
    shutter.onclick = async () => {
      try { end((await writeJPEG(video, video.videoWidth, video.videoHeight)).path); } catch (e) { end("", e); }
    };
    cancel.onclick = () => end("");
    d.addEventListener("cancel", (e) => { e.preventDefault(); end(""); });
    d.showModal();
    video.play().catch(() => {});
  });
}

/** takeWithInput asks a phone's camera app for a photo (an <input capture>). */
function takeWithInput() {
  return new Promise((resolve) => {
    const i = document.createElement("input");
    i.type = "file";
    i.accept = "image/*";
    i.capture = "environment";
    i.addEventListener("change", async () => {
      const f = i.files?.[0];
      if (!f) return resolve("");
      const ext = f.type === "image/png" ? "png" : f.type === "image/heic" ? "heic" : "jpg";
      const path = `/tmp/asuka-camera/${uuid()}.${ext}`;
      fs().writeFile(path, new Uint8Array(await f.arrayBuffer()));
      resolve(path);
    });
    i.addEventListener("cancel", () => resolve(""));
    i.click();
  });
}

// The camera's permission: navigator.permissions' "camera",
// queried when the camera is made and followed by its change event.
// access is synchronous: the state as last known ("notAsked" until the
// query settles), and what the last request learned, which a browser
// without a "camera" permission to query (Firefox, an older Safari)
// says nothing else of.
let permission = null; // the PermissionStatus, once queried
let known = "prompt";
const accessOf = (state) => ({ granted: "allowed", denied: "denied" })[state] ?? "notAsked";

async function queryPermission() {
  if (permission || !navigator.permissions?.query) return;
  try {
    permission = await navigator.permissions.query({ name: "camera" });
    known = permission.state;
    permission.addEventListener?.("change", () => { known = permission.state; });
  } catch { /* no "camera" permission's name here */ }
}

// requestAccess opens the camera (the browser asks, if it hasn't) and
// closes it at once.
async function requestAccess() {
  if (!navigator.mediaDevices?.getUserMedia) return "denied"; // not a secure context
  let answer;
  try {
    const stream = await navigator.mediaDevices.getUserMedia({ video: true });
    stream.getTracks().forEach((t) => t.stop());
    answer = "allowed";
  } catch (e) {
    // No camera at all says nothing of the permission.
    answer = mediaError(e)?.message === "not allowed" ? "denied" : null;
  }
  if (answer) known = answer === "allowed" ? "granted" : "denied";
  else if (permission) known = permission.state;
  return answer ?? accessOf(known);
}

export function makeCamera() {
  queryPermission();
  return {
    async take() {
      const phone = matchMedia?.("(pointer: coarse)").matches && (navigator.maxTouchPoints || 0) > 0;
      return phone ? takeWithInput() : takeWithDialog();
    },
    // A frame of the preview, or, with the flash on (or auto) where the
    // camera has one, ImageCapture's photo with it.
    async capture(_signal, maxSize, quality) {
      const v = current();
      if (!v) throw new CameraError("noView");
      let src = v.video, w = v.video.videoWidth, h = v.video.videoHeight, bitmap = null;
      if (v.props.flash !== Flash.off && v.flash && v.imageCapture) {
        try {
          const blob = await v.imageCapture.takePhoto({ fillLightMode: v.props.flash === Flash.on ? "flash" : "auto" });
          bitmap = await createImageBitmap(blob);
          [src, w, h] = [bitmap, bitmap.width, bitmap.height];
        } catch { /* the preview's frame, then */ }
      }
      if (!w || !h) throw new Error("the camera isn't ready");
      try {
        const out = await writeJPEG(src, w, h, {
          zoom: v.digital(), maxSize: maxSize > 0 ? maxSize : 0, mirror: v.mirrored(), quality: quality > 0 && quality <= 1 ? quality : 0.92,
        });
        return out.path;
      } finally {
        bitmap?.close?.();
      }
    },
    async record(signal, mute, maxMs, maxBytes) {
      const v = current();
      if (!v) throw new CameraError("noView");
      if (v.recording) throw new Error("already recording");
      if (typeof MediaRecorder === "undefined") throw new globalThis.asuka.native.UnsupportedError("recording isn't in this browser");
      // The camera's own track, unless its zoom is digital or its video's
      // mirrored: then a canvas of the preview.
      let video = v.stream.getVideoTracks(), drawn = null;
      if ((!v.range.hw || v.mirrored()) && typeof HTMLCanvasElement !== "undefined" && HTMLCanvasElement.prototype.captureStream) {
        drawn = canvasTrack(v, v.mirrored());
        video = [drawn.track];
      }
      const tracks = video.concat(mute ? [] : v.stream.getAudioTracks());
      const { type, ext } = recordingType((t) => MediaRecorder.isTypeSupported(t), true);
      return new Promise((resolve, reject) => {
        const opts = { type, path: `/tmp/asuka-camera/${uuid()}.${ext}`, video: true, maxMs, maxBytes };
        const rec = new Recording(new MediaStream(tracks), opts, (err, p) => {
          if (v.recording === rec) v.recording = null;
          drawn?.stop();
          err ? reject(err) : resolve(p);
        });
        v.recording = rec;
        signal?.addEventListener?.("abort", () => rec.stop(), { once: true });
      });
    },
    stopRecording() { for (const v of views) v.recording?.stop(); },
    focus(x, y) { current()?.focusAt(clamp(x, 0, 1), clamp(y, 0, 1), false); },
    access() { return accessOf(known); },
    requestAccess,
  };
}
