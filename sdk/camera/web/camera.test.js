// The camera's permission against a browser's, stubbed: access follows
// navigator.permissions' "camera" (its query and its change event), and
// requestAccess opens the camera and closes it at once.
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
    mediaDevices: { async getUserMedia(c) { return media(c); } },
  },
});

function stream() {
  const track = { stopped: false, stop() { this.stopped = true; } };
  return { track, getTracks: () => [track] };
}

const named = (name) => Object.assign(new Error(name), { name });

const { makeCamera } = await import("./camera.js");

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
