// platform/analytics on the web: Google Analytics' gtag.js, as Firebase's
// JavaScript SDK runs it, for the web stream whose measurement ID is
// asuka.toml's analytics.measurementId (the page's settings). Without one
// there's no Collector: the package sends nothing.
//
// gtag.js loads from Google after the page; what the app says before
// waits in its dataLayer. Consent Mode's defaults (asuka.toml's
// analytics.consent) are said before anything else, collection off
// (analytics.enabled = false) is gtag's ga-disable flag, kept in
// localStorage once the app changes it, and asuka dev's builds send to
// DebugView (debug_mode).

const { app, UnsupportedError } = globalThis.asuka.native;

const setting = (k) => app.settings?.["analytics." + k] ?? "";
const KEPT = () => "asuka." + app.name + ".analytics.enabled";

const consentNames = {
  analyticsStorage: "analytics_storage",
  adStorage: "ad_storage",
  adUserData: "ad_user_data",
  adPersonalization: "ad_personalization",
};

/** The parameters as gtag takes them: text a string, the rest numbers. */
function params(list) {
  const p = {};
  for (const v of list) p[v.name] = v.kind === 0 ? v.text : v.kind === 1 ? v.integer : v.number;
  return p;
}

export function makeCollector() {
  const id = setting("measurementId");
  if (!id || typeof document === "undefined") {
    throw new UnsupportedError("analytics: no measurement ID (asuka.toml's analytics.measurementId)");
  }
  window.dataLayer = window.dataLayer || [];
  // gtag pushes its arguments object, as Google's snippet does.
  const gtag = function () { window.dataLayer.push(arguments); };
  window.gtag = window.gtag || gtag;

  const defaults = { wait_for_update: 500 };
  for (const [k, name] of Object.entries(consentNames)) {
    const v = setting("consent." + k);
    if (v) defaults[name] = v === "denied" ? "denied" : "granted";
  }
  if (Object.keys(defaults).length > 1) gtag("consent", "default", defaults);

  let defaultNames = []; // the names setDefaults set last
  let enabled = setting("enabled") !== "false";
  try {
    const kept = localStorage.getItem(KEPT());
    if (kept != null) enabled = kept === "true";
  } catch {}
  window["ga-disable-" + id] = !enabled;

  gtag("js", new Date());
  // The app logs its screens: no page view of its own as gtag loads.
  const config = { send_page_view: false };
  if (app.dev || setting("debug") === "true") config.debug_mode = true;
  gtag("config", id, config);
  const s = document.createElement("script");
  s.async = true;
  s.src = "https://www.googletagmanager.com/gtag/js?id=" + encodeURIComponent(id);
  document.head.appendChild(s);

  return {
    log(name, list, items) {
      const p = params(list);
      if (items.length) p.items = items.map((it) => params(it.params));
      if (name === "screen_view") {
        // A web stream's screens are pages: titled with the screen's name.
        const page = { page_title: p.screen_name, page_location: location.href, send_to: id };
        if (p.screen_class) page.screen_class = p.screen_class;
        gtag("event", "page_view", page);
        return;
      }
      p.send_to = id;
      gtag("event", name, p);
    },
    setUserId(v) { gtag("set", { user_id: v ?? null }); },
    setUserProperty(name, value) { gtag("set", "user_properties", { [name]: value ?? null }); },
    setDefaults(list) {
      // gtag adds to what's set: these replace the app's defaults before.
      const p = params(list);
      for (const k of defaultNames) if (!(k in p)) p[k] = null;
      defaultNames = Object.keys(params(list));
      gtag("set", p);
    },
    setEnabled(on) {
      enabled = on;
      window["ga-disable-" + id] = !on;
      try { localStorage.setItem(KEPT(), String(on)); } catch {}
    },
    setConsent(c) {
      const u = {};
      for (const [k, name] of Object.entries(consentNames)) {
        if (c[k] != null) u[name] = c[k] ? "granted" : "denied";
      }
      if (Object.keys(u).length) gtag("consent", "update", u);
    },
    appInstanceId(signal) {
      if (!enabled) return null;
      // gtag answers once it has loaded: none if it never does (blocked).
      return new Promise((resolve) => {
        const t = setTimeout(() => resolve(null), 5000);
        signal?.addEventListener("abort", () => { clearTimeout(t); resolve(null); });
        gtag("get", id, "client_id", (v) => { clearTimeout(t); resolve(v || null); });
      });
    },
    reset() {
      // The browser's client ID is the _ga cookies': forgotten, the next
      // page load is a new instance.
      gtag("set", { user_id: null });
      const host = location.hostname.split(".");
      for (const c of document.cookie.split(";")) {
        const name = c.split("=")[0].trim();
        if (name !== "_ga" && !name.startsWith("_ga_")) continue;
        for (let i = 0; i < host.length; i++) {
          const domain = host.slice(i).join(".");
          document.cookie = `${name}=; expires=Thu, 01 Jan 1970 00:00:00 GMT; path=/; domain=${domain}`;
        }
        document.cookie = `${name}=; expires=Thu, 01 Jan 1970 00:00:00 GMT; path=/`;
      }
    },
  };
}
