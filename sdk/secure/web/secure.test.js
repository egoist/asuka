// The keychain against an in-memory backend and Node's WebCrypto: a value
// written comes back (encrypted where it's kept, bound to its key's name),
// a protected write is SecureError.notAvailable and keeps nothing, and
// remove forgets.
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
    app: { name: "Test" },
  },
};

// An IndexedDB of Maps: each store a Map, values kept as they are (as
// IndexedDB keeps a CryptoKey).
const stores = { keys: new Map(), values: new Map() };
globalThis.indexedDB = {
  open() {
    const r = {};
    queueMicrotask(() => {
      r.result = {
        transaction(name) {
          const m = stores[name];
          const tx = {};
          tx.objectStore = () => ({
            get: (k) => ({ result: m.get(k) }),
            put: (v, k) => { m.set(k, v); return {}; },
            delete: (k) => { m.delete(k); return {}; },
          });
          queueMicrotask(() => tx.oncomplete());
          return tx;
        },
      };
      r.onsuccess();
    });
    return r;
  },
};

const { makeKeychain } = await import("./secure.js");
const { SecureError } = await import("./secure.asuka.js");
const k = makeKeychain();
const signal = new AbortController().signal;

test("a value written reads back, encrypted where it's kept", async () => {
  assert.equal(await k.get(signal, "token", ""), null);
  await k.set(signal, "token", "s3cret café", false);
  assert.equal(await k.get(signal, "token", "Unlock"), "s3cret café");
  const rec = stores.values.get("token");
  assert.ok(rec.data instanceof Uint8Array);
  assert.ok(!new TextDecoder().decode(rec.data).includes("s3cret"));
});

test("a protected write is notAvailable and keeps nothing", async () => {
  await k.set(signal, "vault", "before", false);
  await assert.rejects(k.set(signal, "vault", "after", true), (e) => {
    assert.ok(e instanceof SecureError);
    assert.equal(e.index, 1); // notAvailable
    return true;
  });
  assert.equal(await k.get(signal, "vault", ""), "before");
  await assert.rejects(k.set(signal, "fresh", "x", true), SecureError);
  assert.equal(await k.get(signal, "fresh", ""), null);
});

test("a value moved to another name doesn't decrypt", async () => {
  await k.set(signal, "a", "one", false);
  stores.values.set("b", stores.values.get("a"));
  assert.equal(await k.get(signal, "b", ""), null);
  assert.equal(stores.values.has("b"), false);
});

test("remove forgets; nothing there is no error", async () => {
  await k.set(signal, "gone", "x", false);
  await k.remove(signal, "gone");
  assert.equal(await k.get(signal, "gone", ""), null);
  await k.remove(signal, "gone");
});
