// The microphone's permission against a browser that can't query it
// (Firefox: navigator.permissions has no "microphone"), stubbed: access
// is "notAsked" until a request learns the answer.
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

let media = null; // (constraints) => a stream, or throws
Object.defineProperty(globalThis, "navigator", {
  configurable: true,
  writable: true,
  value: {
    permissions: { async query(d) { throw new TypeError(`'${d.name}' isn't a valid permission name`); } },
    mediaDevices: { async getUserMedia(c) { return media(c); } },
  },
});

const named = (name) => Object.assign(new Error(name), { name });

const { makeRecorder } = await import("./record.js");

test("access is what the last request learned", async () => {
  const r = makeRecorder();
  await new Promise((res) => setImmediate(res));
  assert.equal(r.access(), "notAsked");

  media = () => { throw named("NotFoundError"); }; // no microphone: nothing learned
  assert.equal(await r.requestAccess(), "notAsked");

  const track = { stopped: false, stop() { this.stopped = true; } };
  media = (c) => {
    assert.deepEqual(c, { audio: true });
    return { getTracks: () => [track] };
  };
  assert.equal(await r.requestAccess(new AbortController().signal), "allowed");
  assert.ok(track.stopped, "the microphone's closed at once");
  assert.equal(r.access(), "allowed");

  media = () => { throw named("NotAllowedError"); };
  assert.equal(await r.requestAccess(), "denied");
  assert.equal(r.access(), "denied");
});
