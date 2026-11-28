// platform/passkey's JavaScript against a stand-in navigator.credentials:
// the options from the server's JSON or from their fields, the
// credential as the browser's toJSON or made here, the DOMExceptions as
// the package's errors.
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

const { makeAuthenticator, creationOptions, requestOptions, credentialJSON, b64url } = await import("./passkey.js");

const bytes = (...b) => new Uint8Array(b);
const creation = {
  json: JSON.stringify({ rp: { id: "example.com", name: "Example" }, user: { id: "AQI", name: "ana", displayName: "Ana" }, challenge: "AwQ",
    pubKeyCredParams: [{ type: "public-key", alg: -7 }], extensions: { credProps: true } }),
  rpId: "example.com", rpName: "Example", userId: bytes(1, 2), userName: "ana", userDisplayName: "Ana", challenge: bytes(3, 4),
  algorithms: [-7], exclude: [bytes(9)], residentKey: "required", userVerification: "required", attestation: "none",
  attachment: "", timeoutMs: 60000, clientData: bytes(),
};
const request = { json: JSON.stringify({ challenge: "AwQ", rpId: "example.com" }), rpId: "example.com", challenge: bytes(3, 4),
  allow: [], userVerification: "preferred", timeoutMs: 0, clientData: bytes() };

test("options from their fields, without the browser's JSON parsing", () => {
  globalThis.PublicKeyCredential = class {};
  const o = creationOptions(creation);
  assert.deepEqual(o.user.id, bytes(1, 2));
  assert.deepEqual(o.pubKeyCredParams, [{ type: "public-key", alg: -7 }]);
  assert.equal(o.authenticatorSelection.requireResidentKey, true);
  assert.equal(o.authenticatorSelection.authenticatorAttachment, undefined);
  assert.deepEqual(o.excludeCredentials, [{ type: "public-key", id: bytes(9) }]);
  assert.deepEqual(o.extensions, { credProps: true });
  assert.equal(o.timeout, 60000);
  const r = requestOptions(request);
  assert.equal(r.rpId, "example.com");
  assert.equal(r.timeout, undefined);
});

test("options from the server's JSON, where the browser parses it", () => {
  globalThis.PublicKeyCredential = class {
    static parseCreationOptionsFromJSON(j) { return { parsed: j.rp.id }; }
    static parseRequestOptionsFromJSON(j) { return { parsed: j.rpId }; }
  };
  assert.deepEqual(creationOptions(creation), { parsed: "example.com" });
  assert.deepEqual(requestOptions(request), { parsed: "example.com" });
});

test("a credential made of its parts", () => {
  const reg = {
    id: "CQ", rawId: bytes(9).buffer, type: "public-key", authenticatorAttachment: "platform",
    getClientExtensionResults: () => ({ credProps: { rk: true } }),
    response: {
      clientDataJSON: bytes(1).buffer, attestationObject: bytes(2).buffer,
      getAuthenticatorData: () => bytes(3).buffer, getPublicKey: () => bytes(4).buffer, getPublicKeyAlgorithm: () => -7,
      getTransports: () => ["internal", "hybrid"],
    },
  };
  assert.deepEqual(JSON.parse(credentialJSON(reg)), {
    id: "CQ", rawId: "CQ", type: "public-key", authenticatorAttachment: "platform", clientExtensionResults: { credProps: { rk: true } },
    response: { clientDataJSON: "AQ", attestationObject: "Ag", authenticatorData: "Aw", publicKey: "BA", publicKeyAlgorithm: -7, transports: ["internal", "hybrid"] },
  });
  const signed = { id: "CQ", rawId: bytes(9), response: { clientDataJSON: bytes(1), authenticatorData: bytes(2), signature: bytes(0xfb, 0xff), userHandle: bytes() } };
  const a = JSON.parse(credentialJSON(signed));
  assert.deepEqual(a.response, { clientDataJSON: "AQ", authenticatorData: "Ag", signature: "-_8" });
  assert.equal(a.type, "public-key");
  assert.equal(b64url.encode(bytes(0xfb, 0xff, 0xbf)), "-_-_");
});

test("the browser's errors as the package's", async () => {
  globalThis.PublicKeyCredential = class {};
  const setNavigator = (v) => Object.defineProperty(globalThis, "navigator", { value: v, configurable: true, writable: true });
  const fail = (name) => { setNavigator({ credentials: { create: async () => { throw Object.assign(new Error(name), { name }); }, get: async () => null } }); };
  const a = makeAuthenticator();
  const signal = new AbortController().signal;
  for (const [dom, ours] of [["NotAllowedError", "passkey: canceled"], ["InvalidStateError", "passkey: there's a passkey for this account already"],
    ["SecurityError", "passkey: the app isn't associated with the relying party's domain"], ["NotSupportedError", "passkey: not available"]]) {
    fail(dom);
    await assert.rejects(a.register(signal, creation), { message: ours });
  }
  await assert.rejects(a.signIn(signal, request), { message: "passkey: no passkey for this account" });
  setNavigator({ credentials: { get: async () => ({ toJSON: () => ({ id: "CQ" }) }) } });
  assert.deepEqual(await a.signIn(signal, request), { json: '{"id":"CQ"}' });
  delete globalThis.PublicKeyCredential;
  assert.equal(await a.available(signal), false);
});
