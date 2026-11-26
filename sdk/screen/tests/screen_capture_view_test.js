// screen_capture_view_test.js: platform/screen's capture on the web
// (web/screen.js), called as the core calls it (Screens.capture through
// the generated glue, in core/src/native's wire format). getDisplayMedia
// is stood in for by a canvas's stream (headless Chrome has no picker to
// answer): one frame of it is a PNG in the app's files at
// /tmp/asuka-screen, of the canvas's size and color, taken with
// ImageCapture and, without it, from a <video>; the stream's tracks are
// stopped once it's taken; the picker cancelled or refused is
// CaptureError.notAllowed, no getDisplayMedia or no source notAvailable;
// a capture cancelled while the picker is up is done at once, and what
// the user then picks is stopped. e2e's TestStdWebViews runs it in
// headless Chrome: it ends by writing "PASS ..." or "FAIL ..." into the
// page's #result.
import * as native from "/platform/web/src/native.js";

globalThis.asuka = { native };
await import("../web/screen.asuka.js");

const failures = [];
const check = (ok, what) => {
  if (!ok) failures.push(what);
};

const md = navigator.mediaDevices;
const realGetDisplayMedia = md?.getDisplayMedia;
const RealImageCapture = globalThis.ImageCapture;

// The core's side: a call's answer, once it's done.
const answers = new Map();
const back = { done: (id, b) => answers.get(id)?.(b), invoke: () => null };
const made = new native.NativeReader(native.makeObject("platform/screen", 0, back));
const h = made.uint8() === 0 ? Number(made.uint64()) : 0;
let calls = 0;

/** capture calls Screens.capture (method 9) as the core does: { path } or { error: its index } or { failed: why }. */
function capture(display = "", cancelAfter = -1) {
  const id = ++calls;
  const done = new Promise((resolve) => answers.set(id, resolve));
  native.start(h, 9, new native.NativeWriter().string(display).bytes(), id, back);
  if (cancelAfter >= 0) setTimeout(() => native.cancel(id), cancelAfter);
  return done.then((b) => {
    const r = new native.NativeReader(b);
    switch (r.uint8()) {
      case 0: return { path: r.string() };
      case 1: return { error: r.uint32() };
      case 2: return { failed: `${r.string()}: ${(r.int64(), r.string())}` };
      case 3: return { failed: "unsupported: " + r.string() };
      case 4: return { cancelled: true };
    }
    return { failed: "a bad answer" };
  });
}
const notAllowed = 0, notAvailable = 1;

/** stand makes getDisplayMedia answer with f(options). */
function stand(f) {
  Object.defineProperty(md, "getDisplayMedia", { value: f, configurable: true, writable: true });
}

/** colored is a w by h canvas's stream, painted rgb all the while (a canvas's stream has frames only as it's painted), and the tracks it gave. */
function colored(w, h, rgb) {
  const c = document.createElement("canvas");
  c.width = w;
  c.height = h;
  const g = c.getContext("2d");
  let on = true;
  const paint = () => {
    if (!on) return;
    g.fillStyle = `rgb(${rgb.join(",")})`;
    g.fillRect(0, 0, w, h);
    requestAnimationFrame(paint);
  };
  paint();
  const stream = c.captureStream(30);
  return { stream, tracks: stream.getTracks(), stop: () => { on = false; } };
}

/** pixel is a PNG's size and its pixel at x, y, decoded by the browser. */
async function pixel(bytes, x, y) {
  const bmp = await createImageBitmap(new Blob([bytes], { type: "image/png" }));
  const c = new OffscreenCanvas(bmp.width, bmp.height);
  const g = c.getContext("2d");
  g.drawImage(bmp, 0, 0);
  return { w: bmp.width, h: bmp.height, rgba: [...g.getImageData(x, y, 1, 1).data] };
}

const near = (a, b) => a.every((v, i) => Math.abs(v - b[i]) <= 3);

/** shows checks a capture's answer is a PNG of the app's, w by h, all rgb, and that the stream it took is stopped. */
async function shows(what, got, s, w, h, rgb) {
  check(got.path, `${what}: ${JSON.stringify(got)}, want a path`);
  if (!got.path) return;
  check(/^\/tmp\/asuka-screen\/[^/]+\.png$/.test(got.path), `${what}: the path ${got.path}, want a PNG in /tmp/asuka-screen`);
  const bytes = native.files.fs.readFile(got.path);
  check(bytes && bytes.length > 8, `${what}: no file at ${got.path}`);
  if (!bytes) return;
  check([137, 80, 78, 71, 13, 10, 26, 10].every((b, i) => bytes[i] === b), `${what}: the file isn't a PNG`);
  check(native.files.typeOf(got.path) === "image/png", `${what}: its type is ${native.files.typeOf(got.path)}`);
  // What Image shows a file of the app's from.
  check(native.assetURL(got.path).startsWith("blob:"), `${what}: Image's URL for it is ${native.assetURL(got.path)}`);
  const p = await pixel(bytes.slice(), w >> 1, h >> 1);
  check(p.w === w && p.h === h, `${what}: ${p.w} by ${p.h}, want ${w} by ${h}`);
  check(near(p.rgba, [...rgb, 255]), `${what}: its middle is ${p.rgba}, want ${rgb},255`);
  const c = await pixel(bytes.slice(), 0, 0);
  check(near(c.rgba, [...rgb, 255]), `${what}: its corner is ${c.rgba}, want ${rgb},255`);
  check(s.tracks.every((t) => t.readyState === "ended"), `${what}: the stream's tracks are ${s.tracks.map((t) => t.readyState)}, want ended`);
}

async function run() {
  check(h > 0, "no Screens object from the glue");
  check(typeof realGetDisplayMedia === "function", "this Chrome has no getDisplayMedia");
  const notes = [];

  // What the page asks for: video, no sound, a screen offered first.
  let asked = null;
  let s = colored(64, 48, [255, 0, 0]);
  stand((o) => { asked = o; return Promise.resolve(s.stream); });
  let got = await capture("display-2"); // the display is the user's to pick, not the page's
  check(asked && asked.video && asked.audio === false, `asked for ${JSON.stringify(asked)}, want video and no audio`);
  check(asked?.video?.displaySurface === "monitor", `asked for ${JSON.stringify(asked)}, want a screen offered first`);
  await shows("ImageCapture's frame", got, s, 64, 48, [255, 0, 0]);
  s.stop();
  check(typeof RealImageCapture === "function", "this Chrome has no ImageCapture: the <video>'s path was tested twice");

  // Without ImageCapture: a <video>'s frame.
  s = colored(40, 30, [0, 160, 0]);
  stand(() => Promise.resolve(s.stream));
  globalThis.ImageCapture = undefined;
  try {
    got = await capture();
  } finally {
    globalThis.ImageCapture = RealImageCapture;
  }
  await shows("a <video>'s frame", got, s, 40, 30, [0, 160, 0]);
  s.stop();

  // Two captures are two files.
  s = colored(16, 16, [0, 0, 255]);
  stand(() => Promise.resolve(s.stream));
  const again = await capture();
  check(again.path && again.path !== got.path, `a second capture's path is ${again.path}, the first's ${got.path}`);
  s.stop();

  // The picker cancelled (NotAllowedError), aborted, refused by a
  // permissions policy (NotAllowedError too); nothing to capture.
  for (const [name, want] of [["NotAllowedError", notAllowed], ["AbortError", notAllowed], ["NotFoundError", notAvailable], ["NotSupportedError", notAvailable], ["NotReadableError", notAvailable]]) {
    stand(() => Promise.reject(new DOMException("no", name)));
    got = await capture();
    check(got.error === want, `${name}: ${JSON.stringify(got)}, want CaptureError ${want === notAllowed ? "notAllowed" : "notAvailable"}`);
  }
  // Thrown, not rejected (an older browser's TypeError is an error, not a CaptureError).
  stand(() => { throw new DOMException("no", "NotAllowedError"); });
  got = await capture();
  check(got.error === notAllowed, `a thrown NotAllowedError: ${JSON.stringify(got)}, want notAllowed`);

  // No getDisplayMedia (a phone's browser, an insecure page).
  stand(undefined);
  got = await capture();
  check(got.error === notAvailable, `no getDisplayMedia: ${JSON.stringify(got)}, want notAvailable`);

  // Cancelled while the picker is up: what the user then picks is stopped.
  s = colored(16, 16, [255, 255, 0]);
  let pick;
  stand(() => new Promise((resolve) => { pick = resolve; }));
  got = await capture("", 0);
  check(got.cancelled, `cancelled while the picker was up: ${JSON.stringify(got)}, want cancelled`);
  pick(s.stream);
  await new Promise((r) => setTimeout(r, 0));
  check(s.tracks.every((t) => t.readyState === "ended"), `picked after a cancel: the tracks are ${s.tracks.map((t) => t.readyState)}, want ended`);
  s.stop();

  // Chrome's own getDisplayMedia, with no picker to answer in headless
  // Chrome: what it does is noted, not checked (it may be refused, or not
  // come back at all, without a user's press).
  stand(realGetDisplayMedia);
  const real = await Promise.race([capture(), new Promise((r) => setTimeout(() => r({ timeout: true }), 3000))]);
  notes.push(`Chrome's own picker: ${JSON.stringify(real)}`);

  document.getElementById("result").textContent = failures.length
    ? "FAIL " + failures.join("; ")
    : "PASS screen.capture's frames, files, errors and cancel (" + notes.join("; ") + ")";
}

run().catch((e) => {
  document.getElementById("result").textContent = "FAIL " + (e?.stack || e);
});
