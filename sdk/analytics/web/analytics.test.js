// gtag.js as the package drives it, against a page stubbed: what goes in
// its dataLayer (consent's defaults first, the config, the events), the
// script it loads, collection off as gtag's ga-disable flag, kept.
import { test } from "node:test";
import assert from "node:assert/strict";

class DeclaredError extends Error {}
class UnsupportedError extends Error {}
const app = { name: "notes", dev: false, settings: {} };
globalThis.asuka = {
  native: {
    app, DeclaredError, UnsupportedError, NativeWriter: class {}, NativeReader: class {},
    registerModule() {}, retain() {}, object() {},
  },
};

const scripts = [];
const kept = new Map();
globalThis.window = globalThis;
globalThis.location = { href: "https://example.com/settings", hostname: "example.com" };
globalThis.document = {
  cookie: "",
  head: { appendChild(s) { scripts.push(s); } },
  createElement() { return {}; },
};
globalThis.localStorage = {
  getItem: (k) => kept.get(k) ?? null,
  setItem: (k, v) => kept.set(k, String(v)),
};

const { makeCollector } = await import("./analytics.js");

// pushed is the dataLayer's entries, as arrays.
const pushed = () => window.dataLayer.map((a) => Array.from(a));

test("no measurement ID, no collector", () => {
  assert.throws(() => makeCollector(), UnsupportedError);
});

test("gtag.js for the stream, its defaults first, the app's events", async () => {
  app.settings = {
    "analytics.measurementId": "G-TEST123",
    "analytics.consent.adStorage": "denied",
    "analytics.consent.adUserData": "denied",
  };
  const c = makeCollector();
  assert.equal(scripts[0].src, "https://www.googletagmanager.com/gtag/js?id=G-TEST123");
  assert.equal(scripts[0].async, true);
  const [consent, js, config] = pushed();
  assert.deepEqual(consent, ["consent", "default", { wait_for_update: 500, ad_storage: "denied", ad_user_data: "denied" }]);
  assert.equal(js[0], "js");
  assert.deepEqual(config, ["config", "G-TEST123", { send_page_view: false }]);
  assert.equal(window["ga-disable-G-TEST123"], false);

  window.dataLayer.length = 0;
  c.log("level_end", [{ name: "level", kind: 1, integer: 3 }, { name: "result", kind: 0, text: "won" }, { name: "score", kind: 2, number: 2.5 }], []);
  c.log("purchase", [{ name: "currency", kind: 0, text: "USD" }], [{ params: [{ name: "item_id", kind: 0, text: "SKU_1" }, { name: "price", kind: 2, number: 9.99 }] }]);
  c.log("screen_view", [{ name: "screen_name", kind: 0, text: "Settings" }], []);
  c.setUserId("u_1");
  c.setUserProperty("plan", "pro");
  c.setUserProperty("plan", null);
  c.setDefaults([{ name: "app_variant", kind: 0, text: "beta" }]);
  c.setDefaults([{ name: "level", kind: 1, integer: 2 }]);
  c.setConsent({ analyticsStorage: null, adStorage: true, adUserData: null, adPersonalization: false });
  assert.deepEqual(pushed(), [
    ["event", "level_end", { level: 3, result: "won", score: 2.5, send_to: "G-TEST123" }],
    ["event", "purchase", { currency: "USD", items: [{ item_id: "SKU_1", price: 9.99 }], send_to: "G-TEST123" }],
    ["event", "page_view", { page_title: "Settings", page_location: "https://example.com/settings", send_to: "G-TEST123" }],
    ["set", { user_id: "u_1" }],
    ["set", "user_properties", { plan: "pro" }],
    ["set", "user_properties", { plan: null }],
    ["set", { app_variant: "beta" }],
    ["set", { level: 2, app_variant: null }],
    ["consent", "update", { ad_storage: "granted", ad_personalization: "denied" }],
  ]);

  // The client ID, as gtag answers it.
  window.dataLayer.length = 0;
  const id = c.appInstanceId(new AbortController().signal);
  const [get] = pushed();
  assert.deepEqual(get.slice(0, 3), ["get", "G-TEST123", "client_id"]);
  get[3]("123.456");
  assert.equal(await id, "123.456");

  // Off: gtag's flag, kept for the next visit; no client ID.
  c.setEnabled(false);
  assert.equal(window["ga-disable-G-TEST123"], true);
  assert.equal(kept.get("asuka.notes.analytics.enabled"), "false");
  assert.equal(await c.appInstanceId(null), null);
});

test("collection off from asuka.toml, then as the app left it", () => {
  kept.clear();
  window.dataLayer = [];
  app.settings = { "analytics.measurementId": "G-TEST123", "analytics.enabled": "false" };
  makeCollector();
  assert.equal(window["ga-disable-G-TEST123"], true);
  kept.set("asuka.notes.analytics.enabled", "true");
  makeCollector();
  assert.equal(window["ga-disable-G-TEST123"], false);
  // asuka dev's: DebugView.
  app.dev = true;
  window.dataLayer = [];
  makeCollector();
  assert.deepEqual(pushed().find((a) => a[0] === "config"), ["config", "G-TEST123", { send_page_view: false, debug_mode: true }]);
});
