// platform/camera on the web: camera.View is a <video> of the camera
// (getUserMedia), front or back; capture takes a JPEG of what it shows,
// record records it with a MediaRecorder (MP4 where the browser records
// it, else WebM); scan reads codes with the browser's BarcodeDetector,
// where there is one (Chrome on Android and macOS). take is a phone's
// camera app (<input capture>, where the screen is touch), else a dialog
// of the page's own: the camera's preview and a shutter. Photos and
// videos go to the app's files' /tmp/asuka-camera.
import { Facing, Mode, CameraError } from "./camera.asuka.js";
import { Recording, mediaError, recordingType, uuid } from "./media.js";

const fs = () => globalThis.asuka.native.fs;
const facingMode = (f) => (f === Facing.front ? "user" : "environment");

function canvas(w, h) {
  if (typeof OffscreenCanvas !== "undefined") return new OffscreenCanvas(w, h);
  const c = document.createElement("canvas");
  c.width = w;
  c.height = h;
  return c;
}

/** writeJPEG writes a drawable to the app's files, as a JPEG on white: its path. */
async function writeJPEG(src, w, h) {
  const c = canvas(w, h);
  const g = c.getContext("2d");
  g.fillStyle = "#fff";
  g.fillRect(0, 0, w, h);
  g.drawImage(src, 0, 0, w, h);
  const blob = c.convertToBlob
    ? await c.convertToBlob({ type: "image/jpeg", quality: 0.92 })
    : await new Promise((resolve) => c.toBlob(resolve, "image/jpeg", 0.92));
  if (!blob) throw new Error("can't write the image");
  const path = `/tmp/asuka-camera/${uuid()}.jpg`;
  fs().writeFile(path, new Uint8Array(await blob.arrayBuffer()));
  return path;
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

/** The camera views on the page, newest last: capture and record use the newest. */
const views = [];
const current = () => [...views].reverse().find((v) => v.alive && v.stream);

export function makeView(events) {
  const el = document.createElement("div");
  el.style.cssText = "position:relative;overflow:hidden;background:#000";
  const video = document.createElement("video");
  video.muted = true;
  video.playsInline = true;
  video.autoplay = true;
  video.style.cssText = "width:100%;height:100%;object-fit:cover;display:block";
  el.appendChild(video);
  const v = { video, stream: null, props: { facing: Facing.back, mode: Mode.photo, scan: false, codes: [] }, recording: null, alive: true };
  views.push(v);
  let timer = 0, scanTimer = 0;
  const restart = () => {
    clearTimeout(timer);
    timer = setTimeout(start, 0); // once this batch's props are all set
  };
  const stopStream = () => {
    v.stream?.getTracks().forEach((t) => t.stop());
    v.stream = null;
  };
  const start = async () => {
    if (!v.alive) return;
    stopStream();
    if (!navigator.mediaDevices?.getUserMedia) {
      events.error("there's no camera");
      return;
    }
    try {
      const s = await navigator.mediaDevices.getUserMedia({
        video: { facingMode: facingMode(v.props.facing), width: { ideal: 1920 }, height: { ideal: 1080 } },
        audio: v.props.mode === Mode.video,
      });
      if (!v.alive) { s.getTracks().forEach((t) => t.stop()); return; }
      v.stream = s;
      video.srcObject = s;
      video.style.transform = v.props.facing === Facing.front ? "scaleX(-1)" : "";
      video.play().catch(() => {});
      scan();
    } catch (e) {
      const err = mediaError(e);
      events.error(err ? (err.message === "not allowed" ? "the camera isn't allowed" : err.message) : "there's no camera");
    }
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
  restart();
  const prop = (name, value, again) => {
    const old = v.props[name];
    v.props[name] = value;
    if (again && old !== value) restart();
    else if (!again) scan();
  };
  return {
    el,
    setFacing(f) { prop("facing", f, true); },
    setMode(m) { prop("mode", m, true); },
    setScan(on) { prop("scan", on, false); },
    setCodes(list) { prop("codes", list, false); },
    destroy() {
      v.alive = false;
      clearTimeout(timer);
      clearTimeout(scanTimer);
      v.recording?.stop();
      stopStream();
      views.splice(views.indexOf(v), 1);
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
      try { end(await writeJPEG(video, video.videoWidth, video.videoHeight)); } catch (e) { end("", e); }
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
    async capture() {
      const v = current();
      if (!v) throw new CameraError("noView");
      const w = v.video.videoWidth, h = v.video.videoHeight;
      if (!w || !h) throw new Error("the camera isn't ready");
      return writeJPEG(v.video, w, h);
    },
    async record(signal, mute, maxMs) {
      const v = current();
      if (!v) throw new CameraError("noView");
      if (v.recording) throw new Error("already recording");
      if (typeof MediaRecorder === "undefined") throw new globalThis.asuka.native.UnsupportedError("recording isn't in this browser");
      const tracks = v.stream.getVideoTracks().concat(mute ? [] : v.stream.getAudioTracks());
      const { type, ext } = recordingType((t) => MediaRecorder.isTypeSupported(t), true);
      return new Promise((resolve, reject) => {
        v.recording = new Recording(new MediaStream(tracks), { type, path: `/tmp/asuka-camera/${uuid()}.${ext}`, video: true, maxMs }, (err, p) => {
          v.recording = null;
          err ? reject(err) : resolve(p);
        });
      });
    },
    stopRecording() { for (const v of views) v.recording?.stop(); },
    access() { return accessOf(known); },
    requestAccess,
  };
}
