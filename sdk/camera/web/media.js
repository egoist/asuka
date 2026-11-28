// The browser's media, as platform/camera uses it: recordings (a
// MediaRecorder writing a file of the app's), and what getUserMedia's
// errors mean. (platform/record has its own copy: each package's JavaScript
// is its own.)

const fs = () => globalThis.asuka.native.fs;

/** recordingType is the container and codec the browser records in, and the file's extension. */
export function recordingType(supported, video = false) {
  const kinds = video
    ? [["video/mp4;codecs=avc1,mp4a.40.2", "mp4"], ["video/mp4", "mp4"], ["video/webm;codecs=vp9,opus", "webm"], ["video/webm", "webm"]]
    : [["audio/mp4;codecs=mp4a.40.2", "m4a"], ["audio/mp4", "m4a"], ["audio/webm;codecs=opus", "webm"], ["audio/ogg;codecs=opus", "ogg"], ["audio/webm", "webm"]];
  for (const [type, ext] of kinds) if (supported(type)) return { type, ext };
  return { type: "", ext: "webm" }; // the browser's default
}

export const uuid = () => globalThis.crypto?.randomUUID?.() ?? Math.random().toString(36).slice(2) + Date.now().toString(36);

/**
 * Recording is a MediaRecorder writing a file of the app's (shared with
 * camera.View's video). maxBytes stops it as the file nears that size:
 * its chunks come as often as the browser hands them out then (a
 * quarter second's, or a second's fragment of an MP4), and it stops when
 * another as big as the biggest yet would pass it (the last, flushed as
 * it stops, is a fraction of one), so the file is whole and about
 * maxBytes, seldom over it.
 */
export class Recording {
  constructor(stream, { type, path, bitrate, video, maxMs, maxBytes = 0, stream: progressive }, done) {
    this.stream = stream;
    this.path = path;
    this.chunks = [];
    this.bytes = 0;
    this.biggest = 0;
    this.maxBytes = maxBytes > 0 ? maxBytes : 0;
    this.done = done;
    this.elapsed = 0;
    this.since = performance.now();
    const opts = {};
    if (type) opts.mimeType = type;
    if (bitrate) opts[video ? "bitsPerSecond" : "audioBitsPerSecond"] = bitrate;
    // Chrome's MP4 hands out its bytes a fragment at a time, a fragment
    // from a key frame to the next (every few seconds): with maxBytes,
    // one a second, so they're counted as they come.
    if (video && this.maxBytes) opts.videoKeyFrameIntervalDuration = 1000;
    this.rec = new MediaRecorder(stream, opts);
    this.rec.ondataavailable = (e) => {
      if (!e.data.size) return;
      this.chunks.push(e.data);
      this.bytes += e.data.size;
      this.biggest = Math.max(this.biggest, e.data.size);
      if (progressive) this.write();
      if (this.maxBytes && this.bytes + this.biggest >= this.maxBytes) this.stop();
    };
    // onstop comes after the last data.
    this.rec.onstop = () => this.finish(null);
    this.rec.onerror = (e) => this.finish(e.error || new Error("couldn't record"));
    this.rec.start(this.maxBytes ? 250 : 1000);
    if (maxMs > 0) this.timer = setTimeout(() => this.stop(), maxMs);
  }
  /** write writes what's recorded so far to the file (in order: each after the last). */
  write() {
    const blob = new Blob(this.chunks, { type: this.rec.mimeType });
    this.writing = (this.writing || Promise.resolve()).then(async () => {
      fs().writeFile(this.path, new Uint8Array(await blob.arrayBuffer()));
    });
    return this.writing;
  }
  get recording() { return this.rec.state === "recording"; }
  ms() { return Math.round(this.elapsed + (this.recording ? performance.now() - this.since : 0)); }
  pause() {
    if (!this.recording) return;
    this.elapsed += performance.now() - this.since;
    this.rec.pause();
  }
  resume() {
    if (this.rec.state !== "paused") return;
    this.since = performance.now();
    this.rec.resume();
  }
  stop() {
    clearTimeout(this.timer);
    if (this.rec.state !== "inactive") {
      if (this.recording) this.elapsed += performance.now() - this.since;
      this.rec.stop();
    }
  }
  finish(err) {
    if (!this.done) return;
    const done = this.done;
    this.done = null;
    clearTimeout(this.timer);
    if (err) done(err, "");
    else this.write().then(() => done(null, this.path), (e) => done(e, ""));
  }
}

export function mediaError(e) {
  switch (e?.name) {
    case "NotAllowedError":
    case "SecurityError":
      return new Error("not allowed");
    case "NotFoundError":
    case "OverconstrainedError":
      return null; // no such device: the caller says which
  }
  return e instanceof Error ? e : new Error(String(e));
}
