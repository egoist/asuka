// platform/transcribe on the web: the Web Speech API's recognizer, asked
// to stay on the device (processLocally, Chrome 139 and later), fed the
// recording as an audio track: its sound decoded and played, silently,
// into the recognizer, so a file takes about its own length. A browser
// without an on-device recognizer for the language says so (check's
// onDevice false, file's notAvailable) rather than sending the sound to
// a server, as the Web Speech API otherwise does. One whose language pack
// isn't installed yet starts installing it, and file says downloading.
// The browser gives no timing: segments are empty. listen hears the
// microphone with the same recognizer, on the device, as it's spoken.
import { TranscribeError } from "./transcribe.asuka.js";

const native = () => globalThis.asuka.native;
const Recognition = () => globalThis.SpeechRecognition || globalThis.webkitSpeechRecognition;

const lang = (language) => language || navigator.language || "en-US";

// The on-device recognizer's status for a language: "available",
// "downloadable", "downloading" or "unavailable" (a browser without one).
async function status(language) {
  const R = Recognition();
  if (!R || typeof R.available !== "function" || !("processLocally" in R.prototype)) return "unavailable";
  try {
    return await R.available({ langs: [language], processLocally: true });
  } catch {
    return "unavailable";
  }
}

async function readAudio(path) {
  if (path.startsWith("file://")) path = decodeURIComponent(path.slice(7));
  if (path.startsWith("/")) {
    const b = native().fs.readFile(path);
    if (!b) throw new Error(`transcribe: no file at ${path}`);
    return b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength);
  }
  const r = await fetch(native().assetURL(path)).catch(() => null);
  if (!r?.ok) throw new Error(`transcribe: no file at ${path}`);
  return r.arrayBuffer();
}

function recognize(audio, language, signal) {
  return new Promise((resolve, reject) => {
    const R = Recognition();
    const ctx = new AudioContext();
    const done = (err, text) => {
      ctx.close().catch(() => {});
      signal?.removeEventListener("abort", abort);
      if (err) reject(err);
      else resolve(text);
    };
    const abort = () => {
      try { rec.abort(); } catch {}
      done(signal.reason ?? new DOMException("cancelled", "AbortError"));
    };
    const rec = new R();
    rec.lang = language;
    rec.continuous = true;
    rec.interimResults = false;
    rec.processLocally = true;
    const parts = [];
    let failed = null;
    rec.onresult = (e) => {
      for (let i = e.resultIndex; i < e.results.length; i++) {
        if (e.results[i].isFinal) parts.push(e.results[i][0].transcript.trim());
      }
    };
    rec.onerror = (e) => {
      // no-speech: a recording with nothing said, an empty result.
      if (e.error === "no-speech" || e.error === "aborted") return;
      failed = new TranscribeError(e.error === "not-allowed" ? "notAllowed" : "notAvailable");
    };
    rec.onend = () => done(failed, parts.filter(Boolean).join(" "));
    signal?.addEventListener("abort", abort, { once: true });
    ctx.decodeAudioData(audio).then((buffer) => {
      // Played into a track the recognizer hears, not the speakers.
      const src = ctx.createBufferSource();
      src.buffer = buffer;
      const dest = ctx.createMediaStreamDestination();
      src.connect(dest);
      try {
        rec.start(dest.stream.getAudioTracks()[0]);
      } catch {
        // A browser whose recognizer takes only the microphone.
        done(new TranscribeError("notAvailable"));
        return;
      }
      // What it heard last is given once the sound has ended.
      src.onended = () => setTimeout(() => { try { rec.stop(); } catch {} }, 500);
      src.start();
    }, () => done(new Error("transcribe: not a recording the browser can decode")));
  });
}

// The recognition listening to the microphone, which finish stops.
let listening = null;

// listen hears the microphone on the device, calling heard with all it's
// heard so far (its final results, then the one it's still hearing).
function listenTo(language, heard, signal) {
  return new Promise((resolve, reject) => {
    const rec = new (Recognition())();
    rec.lang = language;
    rec.continuous = true;
    rec.interimResults = true;
    rec.processLocally = true;
    let text = "";
    let failed = null;
    const finished = () => {
      signal?.removeEventListener("abort", abort);
      if (listening === rec) listening = null;
      if (failed) return reject(failed);
      heard({ text, final: true });
      resolve();
    };
    const abort = () => {
      failed = signal.reason ?? new DOMException("cancelled", "AbortError");
      try { rec.abort(); } catch { finished(); }
    };
    rec.onresult = (e) => {
      let t = "";
      for (let i = 0; i < e.results.length; i++) t += e.results[i][0].transcript;
      text = t.trim().replace(/\s+/g, " ");
      heard({ text, final: false });
    };
    rec.onerror = (e) => {
      if (e.error === "no-speech" || e.error === "aborted") return;
      failed ??= new TranscribeError(e.error === "not-allowed" || e.error === "audio-capture" ? "notAllowed" : "notAvailable");
    };
    rec.onend = finished;
    signal?.addEventListener("abort", abort, { once: true });
    listening = rec;
    try {
      rec.start();
    } catch {
      listening = null;
      reject(new TranscribeError("notAvailable"));
    }
  });
}

async function ready(l) {
  switch (await status(l)) {
    case "available":
      return;
    case "downloadable":
      // Its language pack, from the browser (Permissions-Policy:
      // on-device-speech-recognition); asked again once it's in.
      Recognition().install?.({ langs: [l], processLocally: true }).catch(() => {});
      throw new TranscribeError("downloading");
    case "downloading":
      throw new TranscribeError("downloading");
    default:
      throw new TranscribeError("notAvailable");
  }
}

export function makeRecognizer() {
  return {
    async check(signal, language) {
      const l = lang(language);
      return { onDevice: (await status(l)) === "available", language: l };
    },
    async file(signal, path, language) {
      const l = lang(language);
      await ready(l);
      const text = await recognize(await readAudio(path), l, signal);
      return { text, segments: [], language: l };
    },
    async listen(signal, language, heard) {
      const l = lang(language);
      await ready(l);
      if (listening) throw new Error("transcribe: already listening");
      await listenTo(l, heard, signal);
    },
    finish() {
      try { listening?.stop(); } catch {}
    },
  };
}
