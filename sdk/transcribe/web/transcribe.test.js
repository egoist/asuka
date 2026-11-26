// The web recognizer against a browser's, stubbed: on the device only,
// its language pack installed, the recording fed in as a track.
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
    fs: { readFile: (p) => (p === "/data/hi.m4a" ? new Uint8Array([1, 2, 3]) : null) },
    assetURL: (p) => p,
  },
};
globalThis.navigator ??= { language: "en-US" };

const { makeRecognizer } = await import("./transcribe.js");

// A browser whose recognizer is on the device as status says, and which
// hears words in the track it's given.
function browser(status, words = [], { trackless = false } = {}) {
  const seen = { installed: null, started: null, processLocally: null, lang: null };
  globalThis.SpeechRecognition = class {
    static async available(o) {
      assert.equal(o.processLocally, true);
      return status;
    }
    static async install(o) {
      seen.installed = o.langs;
      return true;
    }
    start(track) {
      if (trackless && track) throw new TypeError("no track");
      seen.started = track;
      seen.processLocally = this.processLocally;
      seen.lang = this.lang;
      const results = words.map((w) => Object.assign([{ transcript: w }], { isFinal: true }));
      queueMicrotask(() => this.onresult?.({ resultIndex: 0, results }));
    }
    stop() {
      queueMicrotask(() => this.onend?.());
    }
    abort() {
      this.stop();
    }
  };
  SpeechRecognition.prototype.processLocally = false;
  globalThis.AudioContext = class {
    async decodeAudioData(b) {
      assert.equal(b.byteLength, 3);
      return { duration: 1 };
    }
    createBufferSource() {
      return { connect() {}, start() { setTimeout(() => this.onended(), 0); } };
    }
    createMediaStreamDestination() {
      return { stream: { getAudioTracks: () => ["track"] } };
    }
    async close() {}
  };
  return seen;
}

test("a browser without an on-device recognizer says so", async () => {
  delete globalThis.SpeechRecognition;
  const r = makeRecognizer();
  assert.deepEqual(await r.check(null, "fr-FR"), { onDevice: false, language: "fr-FR" });
  await assert.rejects(r.file(null, "/data/hi.m4a", ""), /not available/);
});

test("a language pack not yet installed is installed, and file says downloading", async () => {
  const seen = browser("downloadable");
  const r = makeRecognizer();
  assert.equal((await r.check(null, "")).onDevice, false);
  await assert.rejects(r.file(null, "/data/hi.m4a", "en-GB"), /downloading/);
  assert.deepEqual(seen.installed, ["en-GB"]);
});

test("a recording is heard on the device", async () => {
  const seen = browser("available", ["hello", " world "]);
  const r = makeRecognizer();
  assert.equal((await r.check(null, "")).onDevice, true);
  const res = await r.file(null, "/data/hi.m4a", "en-US");
  assert.deepEqual(res, { text: "hello world", segments: [], language: "en-US" });
  assert.equal(seen.started, "track");
  assert.equal(seen.processLocally, true);
  assert.equal(seen.lang, "en-US");
  await assert.rejects(r.file(null, "/data/none.m4a", ""), /no file/);
});

test("a recognizer that takes only the microphone isn't one for files", async () => {
  browser("available", [], { trackless: true });
  await assert.rejects(makeRecognizer().file(null, "/data/hi.m4a", ""), /not available/);
});
