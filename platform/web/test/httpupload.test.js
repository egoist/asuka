// std/http's web uploads (std/http/web/http.js): parts sent as one Blob
// (the app's files by path), through XMLHttpRequest for its progress
// where there's one, else fetch; a Sender's pieces streamed as fetch's
// ReadableStream body (duplex: "half"), each taken before the next is
// asked for, or kept as Blobs and sent at finish where fetch can't
// stream one: node --test platform/web.
import { test } from "node:test";
import assert from "node:assert/strict";
import http from "node:http";
import { makeTransport, makeSending } from "../../../std/http/web/http.js";

const enc = new TextEncoder();
const dec = new TextDecoder();

function serve(handler) {
  return new Promise((resolve) => {
    const s = http.createServer(handler);
    s.listen(0, "127.0.0.1", () => resolve(s));
  });
}

/** collect reads a request's body: its pieces as they came, and what the head said. */
function collect(req) {
  return new Promise((resolve) => {
    const pieces = [];
    req.on("data", (d) => pieces.push(Buffer.from(d)));
    req.on("end", () => resolve({ body: Buffer.concat(pieces), pieces, length: req.headers["content-length"], chunked: req.headers["transfer-encoding"] === "chunked", type: req.headers["content-type"] }));
  });
}

const echo = (seen) => async (req, res) => {
  const got = await collect(req);
  seen.push(got);
  res.writeHead(201, { "Content-Type": "text/plain", "X-Asuka": "yes" });
  res.end(`got ${got.body.length}`);
};

test("upload: bytes and the app's files, in order, the progress ending with all sent", async () => {
  globalThis.asuka = { native: { fs: { readFile: (p) => (p === "/data/a.bin" ? enc.encode("FILE") : null) } } };
  const seen = [];
  const s = await serve(echo(seen));
  const url = `http://127.0.0.1:${s.address().port}/up`;
  const reports = [];
  const parts = [{ data: enc.encode("head-"), path: "" }, { data: new Uint8Array(0), path: "/data/a.bin" }, { data: enc.encode("-tail"), path: "" }];
  const r = await makeTransport().upload(new AbortController().signal, "POST", url, { "content-type": "text/x-test" }, parts, 14, 5000, (a, b) => reports.push([a, b]), false, "");
  assert.equal(r.status, 201);
  assert.equal(r.headers["x-asuka"], "yes");
  assert.equal(dec.decode(r.body), "got 14");
  assert.equal(seen[0].body.toString(), "head-FILE-tail");
  assert.equal(seen[0].length, "14");
  assert.equal(seen[0].type, "text/x-test");
  assert.deepEqual(reports.at(-1), [14, 14]);
  // A file that isn't there throws before anything's sent.
  await assert.rejects(makeTransport().upload(new AbortController().signal, "POST", url, {}, [{ data: new Uint8Array(0), path: "/data/none" }], 0, 5000, () => {}, false, ""), /no file at \/data\/none/);
  assert.equal(seen.length, 1);
  assert.deepEqual(await makeTransport().finished(new AbortController().signal), []);
  s.close();
});

/** FakeXHR is XMLHttpRequest over Node's http: its upload's progress as the body's pieces are written. */
class FakeXHR {
  constructor() {
    this.upload = {};
    this.headers = {};
    this.status = 0;
  }
  open(method, url) { this.method = method; this.url = url; }
  setRequestHeader(k, v) { this.headers[k] = v; }
  getAllResponseHeaders() { return Object.entries(this.res.headers).map(([k, v]) => `${k}: ${v}`).join("\r\n"); }
  abort() {
    this.req?.destroy();
    this.onabort?.();
  }
  async send(body) {
    const b = new Uint8Array(await body.arrayBuffer());
    const u = new URL(this.url);
    this.req = http.request({ host: u.hostname, port: u.port, path: u.pathname, method: this.method, headers: { ...this.headers, "content-length": b.length } }, (res) => {
      this.res = res;
      const pieces = [];
      res.on("data", (d) => { pieces.push(d); this.onprogress?.(); });
      res.on("end", () => {
        this.status = res.statusCode;
        this.responseURL = this.url;
        this.response = new Uint8Array(Buffer.concat(pieces)).buffer;
        this.onload?.();
      });
    });
    this.req.on("error", () => this.onerror?.());
    for (let at = 0; at < b.length; at += 65536) {
      const piece = b.subarray(at, at + 65536);
      await new Promise((r) => this.req.write(piece, r));
      this.upload.onprogress?.({ loaded: at + piece.length, total: b.length });
      await new Promise((r) => setTimeout(r, 30));
    }
    this.req.end();
    this.upload.onload?.();
  }
}

test("upload through XMLHttpRequest: progress as it's sent; a stop throws", async () => {
  globalThis.asuka = { native: { fs: { readFile: () => null } } };
  globalThis.XMLHttpRequest = FakeXHR;
  try {
    const seen = [];
    const s = await serve(echo(seen));
    const url = `http://127.0.0.1:${s.address().port}/up`;
    const reports = [];
    const big = new Uint8Array(6 * 65536).fill(7);
    const r = await makeTransport().upload(new AbortController().signal, "PUT", url, {}, [{ data: big, path: "" }], big.length, 5000, (a, b) => reports.push([a, b]), false, "");
    assert.equal(r.status, 201);
    assert.equal(r.headers["x-asuka"], "yes");
    assert.equal(seen[0].body.length, big.length);
    assert.ok(reports.length >= 2, `reports: ${JSON.stringify(reports)}`);
    assert.ok(reports.every(([a, b], i) => b === big.length && (i === 0 || a >= reports[i - 1][0])));
    assert.deepEqual(reports.at(-1), [big.length, big.length]);
    const ctl = new AbortController();
    const going = makeTransport().upload(ctl.signal, "PUT", url, {}, [{ data: big, path: "" }], big.length, 5000, () => {}, false, "");
    setTimeout(() => ctl.abort(), 50);
    await assert.rejects(going, /stopped|abort/i);
    s.close();
  } finally {
    delete globalThis.XMLHttpRequest;
  }
});

test("a sender streams its pieces chunked, each taken before the next is asked for", async () => {
  const seen = [];
  let firstAt = 0;
  const s = await serve(async (req, res) => {
    req.once("data", () => { firstAt = performance.now(); });
    await echo(seen)(req, res);
  });
  const url = `http://127.0.0.1:${s.address().port}/up`;
  const reports = [];
  const sig = new AbortController().signal;
  const snd = makeSending(sig, "POST", url, { "content-type": "application/x-ndjson" }, -1, 5000, (a, b) => reports.push([a, b]), true);
  assert.equal(await snd.write(sig, enc.encode("one\n")), true);
  await new Promise((r) => setTimeout(r, 150));
  const secondAt = performance.now();
  assert.equal(await snd.write(sig, enc.encode("two\n")), true);
  assert.equal(await snd.write(sig, enc.encode("three\n")), true);
  const r = await snd.finish(sig);
  assert.equal(r.status, 201);
  assert.equal(seen[0].body.toString(), "one\ntwo\nthree\n");
  assert.equal(seen[0].chunked, true);
  assert.equal(seen[0].length, undefined);
  assert.ok(firstAt > 0 && firstAt < secondAt, "the first piece reached the server before the second was written");
  assert.deepEqual(reports.at(-1), [14, -1]);
  s.close();
});

// A half-duplex fetch has its answer once the body's sent: a server that
// answers and hangs up first fails the request (the answer is lost, as
// in a browser over HTTP/1.1), which the writes say and finish throws.
test("a sender's writes end when the server hangs up first, and finish says how", async () => {
  const s = await serve((req, res) => {
    res.writeHead(413, { Connection: "close" });
    res.end("too big");
    req.socket.end();
  });
  const url = `http://127.0.0.1:${s.address().port}/up`;
  const sig = new AbortController().signal;
  const snd = makeSending(sig, "POST", url, {}, -1, 5000, () => {}, true);
  let ok = true;
  for (let i = 0; i < 200 && ok; i++) ok = await snd.write(sig, new Uint8Array(65536));
  assert.equal(ok, false, "a write said the request ended");
  await assert.rejects(snd.finish(sig), /fetch failed|network/i);
  s.close();
});

test("where fetch can't stream a body, a sender's pieces are sent together at finish, their length said", async () => {
  globalThis.asuka = { native: { fs: { readFile: () => null } } };
  const seen = [];
  const s = await serve(echo(seen));
  const url = `http://127.0.0.1:${s.address().port}/up`;
  const sig = new AbortController().signal;
  const reports = [];
  const snd = makeSending(sig, "POST", url, {}, 8, 5000, (a, b) => reports.push([a, b]), false);
  assert.equal(await snd.write(sig, enc.encode("abcd")), true);
  assert.equal(await snd.write(sig, enc.encode("efgh")), true);
  const r = await snd.finish(sig);
  assert.equal(r.status, 201);
  assert.equal(seen[0].body.toString(), "abcdefgh");
  assert.equal(seen[0].length, "8");
  assert.deepEqual(reports.at(-1), [8, 8]);
  s.close();
});

test("a sender closed (or its write's task cancelled) stops the request", async () => {
  let gone = 0;
  let arrived = null;
  const s = await serve((req) => {
    req.once("data", () => arrived?.());
    req.socket.on("close", () => { gone++; });
  });
  const url = `http://127.0.0.1:${s.address().port}/up`;
  const sig = new AbortController().signal;
  const reached = () => new Promise((r) => { arrived = r; });
  let at = reached();
  const a = makeSending(sig, "POST", url, {}, -1, 5000, () => {}, true);
  await a.write(sig, enc.encode("x"));
  await at;
  a.close();
  assert.equal(await a.write(sig, enc.encode("y")), false);
  await assert.rejects(a.finish(sig));
  at = reached();
  const b = makeSending(sig, "POST", url, {}, -1, 5000, () => {}, true);
  await b.write(sig, enc.encode("x"));
  await at;
  const cancel = new AbortController();
  const w = b.write(cancel.signal, enc.encode("y"));
  cancel.abort();
  await w;
  await assert.rejects(b.finish(sig));
  for (let i = 0; i < 100 && gone < 2; i++) await new Promise((r) => setTimeout(r, 20));
  assert.equal(gone, 2);
  s.close();
  s.closeAllConnections();
});
