// The app's service worker: what shows and answers the app's
// notifications while no page of it is open, or none is in front.
//
// The CLI ships it with every web app (sw.js beside index.html), as one
// file: globalThis.asukaApp (the app's name, title, assets and icon, and
// for a build its files: precache), notify-core.js, offline.js, then
// this. The page registers it (asuka.js), scoped to the app's directory.
//
//   - install, activate, fetch: a build's files offline (offline.js says
//     how): each build a worker of its own (its files' list changes it),
//     installed in the background, taking over as the app's only page
//     reloads, once none of the old one is open, or as the app asks
//     ("skipWaiting": platform/update). A build with offline off
//     (asuka.toml's web.offline = false) and asuka dev's have no files
//     listed: no fetch handler, what was kept for offline deleted.
//
//   - push: a Web Push message (the payload docs/web.md describes) shows
//     as a notification. A page of the app that's open is asked first
//     (the one in front, else any): it gets it (push.OnReceive), may
//     rewrite it (push.rewrite) and says how it shows (notify.Present);
//     the other pages get it too. A message without a title or body is a
//     data message: the open pages get it, nothing shows.
//   - notificationclick: the tap, or an action (its reply's text, where
//     the browser has inline replies), goes to the app as
//     notify.OnAction's Response, a push's to push.OnOpen too, a tap's
//     URL to ui.OnOpenURL: to the page in front (focused for a tap or an
//     Open action), else any page of the app, else to a page it opens
//     (a browser runs the app's code only in a page), which gets it as
//     it starts (it says "hello").
//   - notificationclose: the pages forget it.
//   - pushsubscriptionchange: subscribed again with the app's key.
//   - periodicsync ("asuka-notify", where the browser runs it: Chrome, an
//     installed app), and any other time the worker runs with no page
//     open: the scheduled notifications that are due show.

(() => {
  const g = globalThis;
  const core = g.asukaNotify;
  const cfg = g.asukaApp || {};
  const name = cfg.name || "app";
  const st = core.store(name);
  const scope = () => g.registration?.scope || g.location?.href || "http://localhost/";

  const windows = async () => {
    try {
      return await g.clients.matchAll({ type: "window", includeUncontrolled: true });
    } catch {
      return [];
    }
  };
  const front = (ws) => ws.find((c) => c.focused) || ws.find((c) => c.visibilityState === "visible") || null;

  // An image's source, as the page's: an http(s), data or blob URL as it
  // is, an asset's path from the app's assets; a file's (the app's own
  // files are the page's, in IndexedDB) can't show from here.
  const imageURL = (src) => {
    if (!src) return undefined;
    if (/^(https?|blob|data):/.test(src)) return src;
    if (src.startsWith("/") || src.startsWith("file:")) return undefined;
    return new URL((cfg.assets || "assets").replace(/\/$/, "") + "/" + src.replace(/^\.?\//, ""), scope()).href;
  };

  /** show shows spec (sdk/notify's Spec), with the app's categories' actions. */
  async function show(spec, o = {}) {
    const categories = (await st.get("categories")) || [];
    const { title, options } = core.notificationOf(spec, {
      ...o, categories, app: name, image: imageURL, icon: cfg.icon ? new URL(cfg.icon, scope()).href : undefined,
    });
    if (spec.badge > 0) g.navigator?.setAppBadge?.(spec.badge)?.catch?.(() => {});
    await g.registration.showNotification(title, options);
  }

  /** ask posts msg to a page with a port for its answer, and waits for it (ms at most: then null). */
  function ask(client, msg, ms) {
    return new Promise((resolve) => {
      const ch = new g.MessageChannel();
      const t = setTimeout(() => { ch.port1.close?.(); resolve(null); }, ms);
      ch.port1.onmessage = (e) => {
        clearTimeout(t);
        ch.port1.close?.();
        resolve(e.data);
      };
      try {
        client.postMessage(msg, [ch.port2]);
      } catch {
        clearTimeout(t);
        resolve(null);
      }
    });
  }

  /** payloadOf is a push's data: its JSON, else its text as the body. */
  function payloadOf(data) {
    if (!data) return {};
    let text = "";
    try { text = data.text(); } catch { return {}; }
    if (!text) return {};
    try { return JSON.parse(text); } catch { return { body: text }; }
  }

  // How long the page in front has to answer (a rewrite runs the app's
  // code: push.rewrite).
  const answerMs = 10000;

  async function onPush(event) {
    const payload = payloadOf(event.data);
    const message = core.messageOf(payload, core.newID());
    const ws = await windows();
    const chosen = front(ws) || ws[0] || null;
    for (const c of ws) if (c !== chosen) c.postMessage({ asuka: "received", app: name, message });
    let spec = core.specOfMessage(message, payload), how = 0;
    if (chosen) {
      const a = await ask(chosen, { asuka: "push", app: name, message }, answerMs);
      if (a?.spec) spec = { ...a.spec, url: a.spec.url || a.spec.data?.url || "", silent: !!(a.spec.silent || spec.silent) };
      how = a?.how || 0;
    } else {
      await fireDue();
    }
    if (!spec.title && !spec.body && !spec.sender) return; // a data message
    // Hidden by the page in front (no browser asks for a notification then).
    if (how === 2 && ws.some((c) => c.visibilityState === "visible")) return;
    const push = { ...message, title: spec.title || spec.sender?.name || "", body: spec.body || "", subtitle: spec.subtitle || "" };
    await show(spec, { remote: true, push, how });
  }

  async function onClick(event) {
    const n = event.notification;
    const d = n?.data;
    if (!d?.asuka) return;
    n.close();
    const action = event.action || "";
    const open = action === "" || (d.opens || []).includes(action);
    const item = {
      asuka: "response", app: d.app || name, response: core.responseOf(d, action, event.reply),
      push: d.push || null, url: action === "" ? d.shown?.url || "" : "",
    };
    const ws = await windows();
    const target = front(ws) || ws[0] || null;
    if (target) {
      if (open && !target.focused) {
        try { await target.focus(); } catch {}
      }
      target.postMessage(item);
      return;
    }
    // No page: one opens, and gets it as it starts.
    await core.addPending(st, name, item);
    try {
      await g.clients.openWindow(scope());
    } catch {}
  }

  async function onClose(event) {
    const d = event.notification?.data;
    if (!d?.asuka) return;
    for (const c of await windows()) c.postMessage({ asuka: "closed", app: d.app || name, id: d.shown?.id || "" });
  }

  async function onMessage(event) {
    const m = event.data;
    if (!m?.asuka) return;
    if (m.asuka === "skipWaiting") {
      await g.skipWaiting?.();
      return;
    }
    if (m.asuka === "hello") {
      // A page started: the answers that waited for one.
      for (const item of await core.takePending(st, m.app || name)) event.source?.postMessage(item);
    }
  }

  /** fireDue shows the scheduled notifications that are due, when no page of the app is open (a page shows its own). */
  async function fireDue() {
    if ((await windows()).length) return;
    for (const s of await core.takeDue(st, name, Date.now())) {
      try { await show(s, { remote: false }); } catch {}
    }
  }

  async function onPeriodicSync(event) {
    if (event.tag === "asuka-notify") await fireDue();
  }

  // The browser ended the subscription (it expired, or its keys changed):
  // a new one with the same key, which the app's next push.Register
  // reports (the server can only learn it from the app).
  async function onSubscriptionChange(event) {
    const pm = g.registration.pushManager;
    if (!pm || (await pm.getSubscription())) return;
    const key = event.oldSubscription?.options?.applicationServerKey || (await st.get("vapid"));
    if (!key) return;
    await pm.subscribe({ userVisibleOnly: true, applicationServerKey: key });
  }

  const handlers = {
    push: onPush, notificationclick: onClick, notificationclose: onClose, message: onMessage,
    periodicsync: onPeriodicSync, pushsubscriptionchange: onSubscriptionChange,
  };
  g.asukaWorker = { ...handlers, fireDue, show };

  // The build's files offline (none listed: asuka dev's, offline off).
  const offline = cfg.precache?.files?.length && g.asukaOffline ? g.asukaOffline.create({ manifest: cfg.precache, scope: scope() }) : null;

  // A new version installs its files; it waits for the old one's pages
  // to go, but for the first version of the app that works offline (an
  // older worker answers no loads), which takes over at once.
  async function onInstall() {
    if (offline) {
      await offline.install();
      if (await offline.installed()) return;
    }
    await g.skipWaiting?.();
  }

  async function onActivate() {
    if (offline) await offline.activate();
    else await g.asukaOffline?.clear(scope());
    await g.clients.claim?.();
    await fireDue();
  }

  g.asukaWorker.install = onInstall;
  g.asukaWorker.activate = onActivate;
  g.asukaWorker.offline = offline;

  if (typeof g.registration === "object" && g.registration && typeof g.addEventListener === "function" && g.clients) {
    g.addEventListener("install", (e) => e.waitUntil(onInstall()));
    g.addEventListener("activate", (e) => e.waitUntil(onActivate()));
    if (offline) {
      g.addEventListener("fetch", (e) => {
        const r = offline.respond(e.request);
        if (r) e.respondWith(r);
      });
    }
    for (const [type, f] of Object.entries(handlers)) {
      g.addEventListener(type, (e) => e.waitUntil(f(e).catch((err) => console.error("asuka: service worker:", type, err))));
    }
  }
})();
