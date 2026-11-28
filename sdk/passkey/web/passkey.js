// platform/passkey on the web: WebAuthn, navigator.credentials.create and
// get, the browser's sheet (its passkeys, a phone's by QR code, a security
// key). The options are the server's JSON, read by the browser's
// parseCreationOptionsFromJSON where it has it, else from the fields
// Asuka's side read; the credential is the browser's toJSON, else made
// here. The relying party is the page's site: rp.id must be its host, or
// a domain it's under.
import { PasskeyError } from "./passkey.asuka.js";

export const b64url = {
  encode(buf) {
    const b = buf instanceof Uint8Array ? buf : new Uint8Array(buf);
    let s = "";
    for (let i = 0; i < b.length; i++) s += String.fromCharCode(b[i]);
    return btoa(s).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
  },
};

const PKC = () => globalThis.PublicKeyCredential;

async function available() {
  if (!PKC() || !globalThis.navigator?.credentials) return false;
  return true; // a passkey of this device's, a phone's or a security key's
}

/** The extensions the server asked for, as its JSON says them. */
function extensions(json) {
  try {
    return JSON.parse(json).extensions;
  } catch {
    return undefined;
  }
}

/** register's options for navigator.credentials.create. */
export function creationOptions(c) {
  if (typeof PKC()?.parseCreationOptionsFromJSON === "function") {
    return PKC().parseCreationOptionsFromJSON(JSON.parse(c.json));
  }
  const o = {
    rp: { id: c.rpId, name: c.rpName },
    user: { id: c.userId, name: c.userName, displayName: c.userDisplayName },
    challenge: c.challenge,
    pubKeyCredParams: c.algorithms.map((alg) => ({ type: "public-key", alg: Number(alg) })),
    excludeCredentials: c.exclude.map((id) => ({ type: "public-key", id })),
    authenticatorSelection: {
      residentKey: c.residentKey,
      requireResidentKey: c.residentKey === "required",
      userVerification: c.userVerification,
    },
    attestation: c.attestation,
  };
  if (c.attachment) o.authenticatorSelection.authenticatorAttachment = c.attachment;
  if (Number(c.timeoutMs) > 0) o.timeout = Number(c.timeoutMs);
  const ext = extensions(c.json);
  if (ext) o.extensions = ext;
  return o;
}

/** signIn's options for navigator.credentials.get. */
export function requestOptions(r) {
  if (typeof PKC()?.parseRequestOptionsFromJSON === "function") {
    return PKC().parseRequestOptionsFromJSON(JSON.parse(r.json));
  }
  const o = {
    challenge: r.challenge,
    rpId: r.rpId,
    allowCredentials: r.allow.map((id) => ({ type: "public-key", id })),
    userVerification: r.userVerification,
  };
  if (Number(r.timeoutMs) > 0) o.timeout = Number(r.timeoutMs);
  const ext = extensions(r.json);
  if (ext) o.extensions = ext;
  return o;
}

/** A credential as WebAuthn's JSON: the browser's toJSON, or made of its parts. */
export function credentialJSON(cred) {
  if (typeof cred.toJSON === "function") return JSON.stringify(cred.toJSON());
  const r = cred.response;
  const response = { clientDataJSON: b64url.encode(r.clientDataJSON) };
  if (r.attestationObject) {
    response.attestationObject = b64url.encode(r.attestationObject);
    if (typeof r.getAuthenticatorData === "function") response.authenticatorData = b64url.encode(r.getAuthenticatorData());
    const key = typeof r.getPublicKey === "function" ? r.getPublicKey() : null;
    if (key) response.publicKey = b64url.encode(key);
    if (typeof r.getPublicKeyAlgorithm === "function") response.publicKeyAlgorithm = r.getPublicKeyAlgorithm();
    response.transports = typeof r.getTransports === "function" ? r.getTransports() : [];
  } else {
    response.authenticatorData = b64url.encode(r.authenticatorData);
    response.signature = b64url.encode(r.signature);
    if (r.userHandle && r.userHandle.byteLength > 0) response.userHandle = b64url.encode(r.userHandle);
  }
  const out = { id: cred.id, rawId: b64url.encode(cred.rawId), type: cred.type || "public-key", response };
  if (cred.authenticatorAttachment) out.authenticatorAttachment = cred.authenticatorAttachment;
  out.clientExtensionResults = typeof cred.getClientExtensionResults === "function" ? cred.getClientExtensionResults() : {};
  return JSON.stringify(out);
}

/** A DOMException of WebAuthn's as the package's error, where it's one of them. */
export function errorOf(e) {
  switch (e?.name) {
    case "NotAllowedError":
      return new PasskeyError("canceled"); // the user's no, or the time ran out
    case "InvalidStateError":
      return new PasskeyError("exists");
    case "SecurityError":
      return new PasskeyError("notAssociated");
    case "NotSupportedError":
      return new PasskeyError("notAvailable");
  }
  return e;
}

export function makeAuthenticator() {
  return {
    available: (_signal) => available(),
    async register(signal, c) {
      if (!(await available())) throw new PasskeyError("notAvailable");
      let cred;
      try {
        cred = await navigator.credentials.create({ publicKey: creationOptions(c), signal });
      } catch (e) {
        if (signal.aborted) throw e;
        throw errorOf(e);
      }
      return { json: credentialJSON(cred) };
    },
    async signIn(signal, r) {
      if (!(await available())) throw new PasskeyError("notAvailable");
      let cred;
      try {
        cred = await navigator.credentials.get({ publicKey: requestOptions(r), signal });
      } catch (e) {
        if (signal.aborted) throw e;
        throw errorOf(e);
      }
      if (!cred) throw new PasskeyError("noCredentials");
      return { json: credentialJSON(cred) };
    },
  };
}
