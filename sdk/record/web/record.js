// platform/record on the web: the microphone (getUserMedia) through a
// MediaRecorder: AAC in MPEG-4 (.m4a) where the browser records it
// (Safari, Chrome), else Opus in WebM (.webm, Firefox), written to the
// app's files. The level is an analyser's RMS, on iOS's meter's scale
// (silence below -60 dB). Video is platform/camera's.
import { Quality, RecordError } from "./record.asuka.js";
import { Recording, mediaError, recordingType, uuid } from "./media.js";

let rec = null; // a Recording
let meter = null; // { ctx, analyser, data }

function level() {
  if (!rec?.recording || !meter) return 0;
  meter.analyser.getFloatTimeDomainData(meter.data);
  let sum = 0;
  for (const x of meter.data) sum += x * x;
  const rms = Math.sqrt(sum / meter.data.length);
  return rms > 0.001 ? Math.min(rms, 1) : 0;
}

// The microphone's permission: navigator.permissions' "microphone",
// queried when the recorder is made and followed by its change event.
// access is synchronous: the state as last known ("notAsked" until the
// query settles), and what the last request learned, which a browser
// without a "microphone" permission to query (Firefox, an older Safari)
// says nothing else of.
let permission = null; // the PermissionStatus, once queried
let known = "prompt";
const accessOf = (state) => ({ granted: "allowed", denied: "denied" })[state] ?? "notAsked";

async function queryPermission() {
  if (permission || !navigator.permissions?.query) return;
  try {
    permission = await navigator.permissions.query({ name: "microphone" });
    known = permission.state;
    permission.addEventListener?.("change", () => { known = permission.state; });
  } catch { /* no "microphone" permission's name here */ }
}

// requestAccess opens the microphone (the browser asks, if it hasn't) and
// closes it at once.
async function requestAccess() {
  if (!navigator.mediaDevices?.getUserMedia) return "denied"; // not a secure context
  let answer;
  try {
    const stream = await navigator.mediaDevices.getUserMedia({ audio: true });
    stream.getTracks().forEach((t) => t.stop());
    answer = "allowed";
  } catch (e) {
    // No microphone at all says nothing of the permission.
    answer = mediaError(e)?.message === "not allowed" ? "denied" : null;
  }
  if (answer) known = answer === "allowed" ? "granted" : "denied";
  else if (permission) known = permission.state;
  return answer ?? accessOf(known);
}

export function makeRecorder() {
  queryPermission();
  return {
    async record(signal, quality, maxMs, path, background, title) {
      if (rec) throw new RecordError("busy");
      const music = quality === Quality.music;
      if (!navigator.mediaDevices?.getUserMedia || typeof MediaRecorder === "undefined") {
        throw new globalThis.asuka.native.UnsupportedError("recording isn't in this browser");
      }
      let stream;
      try {
        stream = await navigator.mediaDevices.getUserMedia({
          audio: music
            ? { channelCount: { ideal: 2 }, echoCancellation: false, noiseSuppression: false, autoGainControl: false }
            : { channelCount: { ideal: 1 }, echoCancellation: true, noiseSuppression: true, autoGainControl: true },
        });
      } catch (e) {
        const err = mediaError(e);
        throw err?.message === "not allowed" ? new RecordError("notAllowed") : err || new Error("no microphone");
      }
      const { type, ext } = recordingType((t) => MediaRecorder.isTypeSupported(t));
      const file = path || `/tmp/asuka-audio/${uuid()}.${ext}`;
      try {
        const ctx = new AudioContext();
        ctx.resume().catch(() => {});
        const analyser = ctx.createAnalyser();
        analyser.fftSize = 1024;
        ctx.createMediaStreamSource(stream).connect(analyser);
        meter = { ctx, analyser, data: new Float32Array(analyser.fftSize) };
      } catch {
        meter = null;
      }
      return new Promise((resolve, reject) => {
        rec = new Recording(stream, { type, path: file, bitrate: music ? 128000 : 64000, maxMs }, (err, p) => {
          stream.getTracks().forEach((t) => t.stop());
          meter?.ctx.close().catch(() => {});
          meter = null;
          rec = null;
          err ? reject(err) : resolve(p);
        });
        // The task that waits went: the recording ends.
        signal.addEventListener("abort", () => rec?.stop(), { once: true });
      });
    },
    stop() { rec?.stop(); },
    pause() { rec?.pause(); },
    resume() { rec?.resume(); },
    status() {
      return { level: level(), recording: Boolean(rec?.recording), elapsed: rec ? rec.ms() : 0 };
    },
    access() { return accessOf(known); },
    requestAccess,
  };
}
