// std/http on the web: fetch, aborted when the call is cancelled or its
// timeout passes (a timeout throws TimeoutError: the call wasn't
// cancelled, it failed).
export function makeTransport() {
  return {
    async send(signal, method, url, headers, body, timeout) {
      const ctl = new AbortController();
      const g = guard(ctl, signal, timeout);
      try {
        const r = await fetch(url, request(method, headers, body, ctl));
        return { ...head(r, url), body: new Uint8Array(await r.arrayBuffer()) };
      } catch (e) {
        throw g.error(e);
      } finally {
        g.done();
      }
    },

    // download reads the body's stream as it comes, reporting progress (a
    // few times a second), then writes it into the app's files. timeout
    // is the longest wait for more.
    async download(signal, method, url, headers, body, timeout, path, progress) {
      const ctl = new AbortController();
      const g = guard(ctl, signal, timeout);
      try {
        const r = await fetch(url, request(method, headers, body, ctl));
        const length = Number(r.headers.get("content-length") ?? -1);
        // A compressed answer's length is its compressed size: unknown.
        const total = r.headers.get("content-encoding") ? -1 : length;
        const chunks = [];
        let got = 0, reported = 0;
        if (r.body) {
          const reader = r.body.getReader();
          for (;;) {
            g.restart();
            const { done, value } = await reader.read();
            if (done) break;
            chunks.push(value);
            got += value.length;
            const now = performance.now();
            if (now - reported > 100) {
              reported = now;
              progress(got, total);
            }
          }
        }
        const all = new Uint8Array(got);
        let at = 0;
        for (const c of chunks) {
          all.set(c, at);
          at += c.length;
        }
        globalThis.asuka.native.fs.writeFile(path, all);
        progress(got, got);
        return { ...head(r, url), body: new Uint8Array(0) };
      } catch (e) {
        throw g.error(e);
      } finally {
        g.done();
      }
    },

    // open answers once the response's head is in (fetch resolves then),
    // its body read by the Body as it comes. The request is the Body's
    // from then on: its close, or a read's cancel, aborts it.
    async open(signal, method, url, headers, body, timeout) {
      const ctl = new AbortController();
      const g = guard(ctl, signal, timeout);
      let r;
      try {
        r = await fetch(url, request(method, headers, body, ctl));
      } catch (e) {
        throw g.error(e);
      } finally {
        g.done();
      }
      return bodyOf(r, url, ctl, timeout);
    },

    // upload sends parts (bytes, and the app's files by path) as one
    // Blob: the app's files are in the page's memory already, and a Blob
    // is the browser's to send as it goes. XMLHttpRequest reports what's
    // sent (fetch can't); without it (a worker, Node), fetch, its
    // progress at the end. background changes nothing: a page's request
    // goes on while it's out of sight, and ends with it.
    async upload(signal, method, url, headers, parts, length, timeout, progress) {
      const blob = new Blob(parts.map(partBytes));
      return send(signal, method, url, headers, blob, timeout, progress);
    },

    // begin streams the pieces written as fetch's ReadableStream body
    // (duplex: "half"), each handed over as the network asks for the
    // next, where the browser can (Chrome over HTTP/2 or 3, Node); else
    // (Safari, Firefox; Chrome to an HTTP/1.1 server, which it refuses
    // before reading any) they're kept as Blobs and sent together at
    // finish.
    async begin(signal, method, url, headers, length, timeout, progress) {
      return makeSending(signal, method, url, headers, length, timeout, progress);
    },

    // A page has no background transfers to hear back from.
    async finished() {
      return [];
    },

    // A page's requests are its browser's: whom it trusts, and the
    // certificate it shows, aren't the page's to say.
    async secured() {
      throw new globalThis.asuka.native.UnsupportedError(untrusted);
    },
  };
}

// What a page's request with tls (std/http's or std/websocket's) throws.
const untrusted =
  "tls: a web page can't say whom to trust or which certificate to show: its browser decides (the system's authorities, and the certificates the user has); give tls only on the other platforms";

// A Sender comes from Transport.begin only.
export function makeSender() {
  throw new globalThis.asuka.native.UnsupportedError("a sender comes from Transport.begin");
}

/** partBytes is a part of an upload's body: its bytes, or the app's file's (a Blob part). */
function partBytes(p) {
  if (!p.path) return p.data;
  const b = globalThis.asuka.native.fs.readFile(p.path);
  if (!b) throw new Error(`no file at ${p.path}`);
  return b;
}

/** reporter calls progress at most every 100 ms, and always for the last (done). */
function reporter(progress) {
  let at = -Infinity;
  return (sent, total, done = false) => {
    const now = performance.now();
    if (!done && now - at < 100) return;
    at = now;
    progress(sent, total);
  };
}

/**
 * send sends body (a Blob) and answers with the response, the body read
 * whole: through XMLHttpRequest, which says what's sent, where there's
 * one, else fetch. timeout is the longest wait for more to go or come.
 */
function send(signal, method, url, headers, body, timeout, progress) {
  const report = reporter(progress);
  const total = body.size;
  const XHR = globalThis.XMLHttpRequest;
  if (!XHR) {
    const ctl = new AbortController();
    const g = guard(ctl, signal, timeout);
    return (async () => {
      try {
        const r = await fetch(url, { method, headers, body: total ? body : undefined, signal: ctl.signal });
        report(total, total, true);
        return { ...head(r, url), body: new Uint8Array(await r.arrayBuffer()) };
      } catch (e) {
        throw g.error(e);
      } finally {
        g.done();
      }
    })();
  }
  return new Promise((resolve, reject) => {
    const x = new XHR();
    let timer = 0, timedOut = false, finished = false;
    const end = () => {
      finished = true;
      clearTimeout(timer);
      signal.removeEventListener("abort", abort);
    };
    const restart = () => {
      clearTimeout(timer);
      if (timeout > 0) timer = setTimeout(() => { timedOut = true; x.abort(); }, timeout);
    };
    const abort = () => x.abort();
    x.open(method, url);
    x.responseType = "arraybuffer";
    for (const [k, v] of Object.entries(headers)) {
      try { x.setRequestHeader(k, v); } catch {}
    }
    x.upload.onprogress = (e) => {
      restart();
      report(e.loaded, total);
    };
    x.upload.onload = () => report(total, total, true);
    x.onprogress = restart;
    x.onload = () => {
      end();
      report(total, total, true);
      resolve({ status: x.status, url: x.responseURL || url, headers: headersOf(x.getAllResponseHeaders()), body: new Uint8Array(x.response || new ArrayBuffer(0)) });
    };
    x.onerror = () => { end(); reject(new TypeError(`${method} ${url}: the network failed`)); };
    x.onabort = () => {
      if (finished) return;
      end();
      reject(timedOut ? timedOutError(timeout) : signal.reason ?? new DOMException("the upload was stopped", "AbortError"));
    };
    if (signal.aborted) {
      end();
      reject(signal.reason ?? new DOMException("the upload was stopped", "AbortError"));
      return;
    }
    signal.addEventListener("abort", abort);
    restart();
    x.send(total || method !== "GET" ? body : null);
  });
}

/** headersOf is XMLHttpRequest's getAllResponseHeaders by lower-case name, a name's values joined. */
function headersOf(all) {
  const out = {};
  for (const line of (all || "").split(/\r?\n/)) {
    const i = line.indexOf(":");
    if (i <= 0) continue;
    const k = line.slice(0, i).trim().toLowerCase();
    const v = line.slice(i + 1).trim();
    out[k] = k in out ? out[k] + ", " + v : v;
  }
  return out;
}

/** Whether fetch sends a ReadableStream as a request's body (duplex: "half"): Chrome 105+, Node; not Safari or Firefox. */
export function streamsRequests() {
  try {
    let duplex = false;
    const typed = new Request("http://localhost/", {
      method: "POST",
      body: new ReadableStream(),
      get duplex() {
        duplex = true;
        return "half";
      },
    }).headers.has("Content-Type");
    return duplex && !typed;
  } catch {
    return false;
  }
}

/**
 * makeSending is a Sender: its pieces a ReadableStream's, each enqueued as
 * fetch pulls for more (so a write waits for the network to take the one
 * before), or, where fetch can't stream a body (or the server is
 * HTTP/1.1 to Chrome, which it says before reading any), Blobs sent
 * together at finish.
 */
export function makeSending(signal, method, url, headers, length, timeout, progress, streams = streamsRequests()) {
  const report = reporter(progress);
  const total = length;
  let sent = 0;
  let ended = false; // the request ended (answered or failed): writes are refused
  let kept = null; // the pieces as Blobs, once not streaming
  let answer = null; // fetch's promise
  let offer = null; // { piece, resolve } a write waiting for a pull, or { end: true }
  let wake = null; // a pull waiting for a write
  const ctl = new AbortController();
  const g = guard(ctl, signal, timeout);
  g.pause(); // until there's something to send

  const giveUp = () => {
    ended = true;
    if (offer && !offer.end) offer.resolve(false);
    offer = null;
  };

  const take = (controller, o) => {
    if (o.end) {
      controller.close();
      return;
    }
    controller.enqueue(o.piece);
    sent += o.piece.length;
    g.pause();
    report(sent, total);
    o.resolve(true);
  };

  const stream = () => new ReadableStream({
    pull(controller) {
      return new Promise((resolve) => {
        const give = () => {
          const o = offer;
          offer = null;
          take(controller, o);
          resolve();
        };
        if (offer) give();
        else wake = give;
      });
    },
    cancel() { giveUp(); },
  }, { highWaterMark: 0 });

  const start = () => {
    answer = fetch(url, { method, headers, body: stream(), duplex: "half", signal: ctl.signal }).then(
      (r) => {
        giveUp();
        return r;
      },
      (e) => {
        if (sent === 0 && !ctl.signal.aborted && !kept) {
          // No streams to this server (HTTP/1.1 to Chrome): kept as Blobs.
          kept = [];
          answer = null;
          if (offer && !offer.end) {
            kept.push(new Blob([offer.piece]));
            offer.resolve(true);
            offer = null;
          }
          wake = null;
          return null;
        }
        giveUp();
        throw g.error(e);
      },
    );
  };

  if (streams) start();
  else kept = [];

  // A write's or finish's call cancelled (its task went) stops the request.
  const follow = (sig) => {
    if (sig.aborted) ctl.abort(sig.reason);
    else sig.addEventListener("abort", () => ctl.abort(sig.reason), { once: true });
  };

  return {
    // Answers once the piece is taken (enqueued as fetch asked for more,
    // or kept): false once the request has ended, finish saying how.
    async write(sig, piece) {
      if (ended) return false;
      if (kept) {
        kept.push(new Blob([piece]));
        return true;
      }
      follow(sig);
      g.restart();
      const wrote = new Promise((resolve) => { offer = { piece, resolve }; });
      if (wake) {
        const w = wake;
        wake = null;
        w();
      }
      return wrote;
    },

    async finish(sig) {
      if (!kept && !ended) {
        follow(sig);
        g.restart();
        offer = { end: true };
        if (wake) {
          const w = wake;
          wake = null;
          w();
        }
      }
      try {
        const r = answer ? await answer : null;
        if (!r) {
          g.done();
          return await send(sig, method, url, headers, new Blob(kept), timeout, progress);
        }
        report(sent, total, true);
        return { ...head(r, url), body: new Uint8Array(await r.arrayBuffer()) };
      } catch (e) {
        throw g.error(e);
      } finally {
        g.done();
      }
    },

    close() {
      giveUp();
      ctl.abort();
      g.done();
    },

    // Let go, it stops the request.
    released() {
      this.close();
    },
  };
}

// A Body comes from Transport.open only.
export function makeBody() {
  throw new globalThis.asuka.native.UnsupportedError("a body comes from Transport.open");
}

// bodyOf reads r's body a piece at a time: each read waits for the next
// one the network delivers, at most timeout (0: no limit).
function bodyOf(r, url, ctl, timeout) {
  const h = { ...head(r, url), body: new Uint8Array(0) };
  const reader = r.body ? r.body.getReader() : null;
  let ended = !reader, closed = false;
  return {
    head: () => h,

    async read(signal) {
      if (closed) throw new Error("the body is closed");
      if (ended) return new Uint8Array(0);
      const g = guard(ctl, signal, timeout);
      try {
        for (;;) {
          const { done, value } = await reader.read();
          if (done) {
            ended = true;
            return new Uint8Array(0);
          }
          if (value.length) return value;
        }
      } catch (e) {
        throw g.error(e);
      } finally {
        g.done();
      }
    },

    close() {
      closed = true;
      ctl.abort();
    },

    // Let go, it stops the request.
    released() {
      this.close();
    },
  };
}

function request(method, headers, body, ctl) {
  return {
    method,
    headers,
    body: body.length && method !== "GET" && method !== "HEAD" ? body : undefined,
    signal: ctl.signal,
  };
}

// head is a response's status, URL and headers (by lower-case name).
function head(r, url) {
  const out = {};
  r.headers.forEach((v, k) => { out[k.toLowerCase()] = v; });
  return { status: r.status, url: r.url || url, headers: out };
}

// guard aborts ctl when the call's signal does, or when timeout passes
// (0: never) from its start or its last restart; error is what a failure
// was: TimeoutError when the timeout aborted it. done lets the call go.
function guard(ctl, signal, timeout) {
  let timer = 0, timedOut = false;
  const abort = () => ctl.abort(signal.reason);
  if (signal.aborted) abort();
  else signal.addEventListener("abort", abort);
  const g = {
    restart() {
      clearTimeout(timer);
      if (timeout > 0) {
        timer = setTimeout(() => {
          timedOut = true;
          ctl.abort(timedOutError(timeout));
        }, timeout);
      }
    },
    // pause stops the clock (while the app makes what's sent next).
    pause() {
      clearTimeout(timer);
    },
    error: (e) => (timedOut ? timedOutError(timeout) : e),
    done() {
      clearTimeout(timer);
      signal.removeEventListener("abort", abort);
    },
  };
  g.restart();
  return g;
}

function timedOutError(timeout) {
  return new DOMException(`no answer within ${timeout} ms`, "TimeoutError");
}

// A page can't listen: there's no server of the app's on the web (sign in
// with a redirect to the app's own site instead).
export function makeLocalServer() {
  throw new globalThis.asuka.native.UnsupportedError("a browser can't listen on the loopback: redirect to the app's site instead");
}
