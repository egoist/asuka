// What the page's platform/notify and platform/push (modules/system-notify.js,
// modules/system-push.js) and the app's service worker (sw.js) share:
// how a notification (sdk/notify's Spec) and a push's payload become the
// browser's notification, what the user's answer to one is on the wire
// (sdk/notify's WireResponse), and the app's small shared values (the
// categories, the scheduled notifications, the answers waiting for a
// page), kept in Cache Storage, which a page and its worker both reach.
//
// A plain script, no imports or exports, so that it runs as a page's
// module and in a classic service worker alike (the CLI writes it into
// the worker's file): it sets globalThis.asukaNotify.

(() => {
  const g = globalThis;

  // --- The app's shared values ---

  /**
   * store is the app's (by its name) shared values, as JSON: Cache
   * Storage where there is (a secure context: the page and its service
   * worker), else the page's localStorage.
   */
  function store(app, env = {}) {
    const caches = "caches" in env ? env.caches : g.caches;
    const local = "local" in env ? env.local : (() => { try { return g.localStorage; } catch { return null; } })();
    const origin = env.origin || g.location?.origin || "http://localhost";
    const url = (k) => new URL("/__asuka/" + encodeURIComponent(app) + "/" + k, origin).href;
    const open = () => caches.open("asuka");
    return {
      async get(k) {
        if (caches) {
          try {
            const r = await (await open()).match(url(k));
            return r ? await r.json() : null;
          } catch {}
          return null;
        }
        try {
          const v = local?.getItem("asuka." + app + "." + k);
          return v ? JSON.parse(v) : null;
        } catch { return null; }
      },
      async set(k, v) {
        const empty = v == null || (typeof v === "object" && Object.keys(v).length === 0);
        if (caches) {
          try {
            const c = await open();
            if (empty) await c.delete(url(k));
            else await c.put(url(k), new Response(JSON.stringify(v), { headers: { "content-type": "application/json" } }));
          } catch {}
          return;
        }
        try {
          if (empty) local?.removeItem("asuka." + app + "." + k);
          else local?.setItem("asuka." + app + "." + k, JSON.stringify(v));
        } catch {}
      },
    };
  }

  /** locked runs f holding the app's lock (Web Locks: across its pages and its worker), where there are. */
  function locked(app, f, nav = g.navigator) {
    if (nav?.locks?.request) return nav.locks.request("asuka-notify:" + app, f);
    return f();
  }

  // --- Triggers (sdk/notify's WireTrigger) ---

  const repeats = (t) => !!t && !!t.kind && t.kind !== "at" && t.kind !== "after";

  /**
   * nextAt is when trigger t shows next after after (ms since 1970), on
   * the device's clock: "daily", "weekly" (weekday 1: Sunday), "monthly"
   * (a month without the day skipped), "yearly"; "every" seconds (at
   * least 60); "at" and "after" once at their time. 0: once, and past.
   */
  function nextAt(t, after) {
    if (!t || !repeats(t)) return t?.at > after ? t.at : 0;
    if (t.kind === "every") return after + Math.max(60, t.seconds || 60) * 1000;
    const now = new Date(after);
    const y = now.getFullYear(), mo = now.getMonth(), d = now.getDate();
    const h = t.hour || 0, m = t.minute || 0;
    const wall = (yy, mm, dd) => new Date(yy, mm, dd, h, m, 0, 0).getTime();
    const daysIn = (yy, mm) => new Date(yy, mm + 1, 0).getDate();
    if (t.kind === "daily") {
      const c = wall(y, mo, d);
      return c > after ? c : wall(y, mo, d + 1);
    }
    if (t.kind === "weekly") {
      const ahead = (((t.weekday || 1) - 1 - now.getDay()) % 7 + 7) % 7;
      const c = wall(y, mo, d + ahead);
      return c > after ? c : wall(y, mo, d + ahead + 7);
    }
    if (t.kind === "monthly") {
      const day = t.day || 1;
      for (let i = 0; i <= 12; i++) {
        const mm = mo + i, yy = y + Math.floor(mm / 12);
        if (day > daysIn(yy, mm % 12)) continue;
        const c = wall(yy, mm % 12, day);
        if (c > after) return c;
      }
      return 0;
    }
    if (t.kind === "yearly") {
      const mm = (t.month || 1) - 1, day = t.day || 1;
      for (let i = 0; i <= 8; i++) {
        if (day > daysIn(y + i, mm)) continue;
        const c = wall(y + i, mm, day);
        if (c > after) return c;
      }
      return 0;
    }
    return 0;
  }

  /**
   * afterDue is when one first due at due shows next after now: due if
   * it's ahead; a repeating one's next time (an interval's counted on
   * from due, keeping its pace, the times missed skipped); 0 once past.
   */
  function afterDue(t, due, now) {
    if (due > now) return due;
    if (!repeats(t)) return 0;
    if (t.kind === "every") {
      const iv = Math.max(60, t.seconds || 60) * 1000;
      return due + (Math.floor((now - due) / iv) + 1) * iv;
    }
    return nextAt(t, now);
  }

  // --- Scheduled notifications: { id: { s: Spec, at: ms } } ---

  /** schedule keeps s to show at at (ms since 1970), in place of one with its ID. */
  function schedule(st, app, s, at, nav) {
    return locked(app, async () => {
      const m = (await st.get("scheduled")) || {};
      m[s.id] = { s, at };
      await st.set("scheduled", m);
    }, nav);
  }

  /** unschedule forgets the scheduled ones whose IDs ids has. */
  function unschedule(st, app, ids, nav) {
    return locked(app, async () => {
      const m = (await st.get("scheduled")) || {};
      let changed = false;
      for (const id of ids) if (id in m) { delete m[id]; changed = true; }
      if (changed) await st.set("scheduled", m);
    }, nav);
  }

  /** scheduled is what's scheduled. */
  async function scheduled(st) {
    const m = (await st.get("scheduled")) || {};
    return Object.entries(m).filter(([id, e]) => e?.s?.id === id && typeof e.at === "number").map(([, e]) => e);
  }

  /**
   * takeDue takes the scheduled ones due at now out of the store, under
   * the app's lock, and returns them, the oldest first: whichever page,
   * or the worker, takes one shows it, once. A repeating one stays, due
   * at its next time (the times it missed with no page open shown once).
   */
  function takeDue(st, app, now, nav) {
    return locked(app, async () => {
      const m = (await st.get("scheduled")) || {};
      const due = [];
      for (const [id, e] of Object.entries(m)) {
        if (!e?.s || e.s.id !== id) { delete m[id]; continue; }
        if (e.at <= now) {
          due.push({ ...e });
          const next = repeats(e.s.trigger) ? afterDue(e.s.trigger, e.at, now) : 0;
          if (next) e.at = next;
          else delete m[id];
        }
      }
      if (due.length) await st.set("scheduled", m);
      return due.sort((a, b) => a.at - b.at).map((e) => e.s);
    }, nav);
  }

  // --- Answers waiting for a page (the worker's) ---

  function addPending(st, app, item, nav) {
    return locked(app, async () => {
      const l = (await st.get("pending")) || [];
      l.push(item);
      await st.set("pending", l.slice(-50));
    }, nav);
  }

  function takePending(st, app, nav) {
    return locked(app, async () => {
      const l = (await st.get("pending")) || [];
      if (l.length) await st.set("pending", null);
      return l;
    }, nav);
  }

  // --- Notifications ---

  let ids = 0;
  /** newID names a notification that has no ID. */
  const newID = () => Date.now().toString(36) + "-" + (++ids).toString(36) + Math.random().toString(36).slice(2, 6);

  /** shownOf is sdk/notify's WireDelivered: what Showing, Present and OnAction report of s. */
  function shownOf(s, time, remote = false) {
    return {
      id: s.id || "", title: s.title || s.sender?.name || "", subtitle: s.subtitle || "", body: s.body || "", url: s.url || "",
      data: s.data || {}, category: s.category || "", thread: s.thread || "", time, remote: !!remote,
    };
  }

  /**
   * notificationOf is s (sdk/notify's Spec) as showNotification's title
   * and options: its ID the tag (one with the same replaces it, and
   * alerts again unless silent), its subtitle the body's first line, its
   * category's actions as the notification's (a Reply one a text field,
   * where the browser has them), its image the picture, the sender's
   * the icon. data keeps what the click reports (shown, push).
   *
   * o: { categories, time, remote, push (push.Message's JSON), app, how
   * (Present's: 1 quiet), image (a source to its URL), icon (the app's) }
   */
  function notificationOf(s, o = {}) {
    const time = o.time ?? Date.now();
    const image = o.image || ((src) => src || undefined);
    const quiet = !!s.silent || o.how === 1;
    const cat = (o.categories || []).find((c) => c && c.id === s.category);
    const actions = (cat?.actions || []).map((a) => {
      const out = { action: a.id, title: a.title };
      if (a.reply) {
        out.type = "text";
        out.placeholder = a.reply.placeholder || "";
      }
      if (a.icon && /^(https?|data):|\//.test(a.icon)) out.icon = a.icon;
      return out;
    });
    let body = s.body || "";
    if (s.sender && s.conversation?.group && s.sender.name && s.title !== s.sender.name) body = s.sender.name + ": " + body;
    body = [s.subtitle, body].filter(Boolean).join("\n");
    const options = {
      body,
      tag: s.id || undefined,
      renotify: !!s.id && !quiet,
      silent: quiet,
      icon: image(s.sender?.image || s.conversation?.image) || o.icon || undefined,
      image: image(s.image) || undefined,
      timestamp: time,
      actions,
      data: {
        asuka: 1, app: o.app || "", shown: shownOf(s, time, o.remote), push: o.push || null,
        // The actions that bring the app to the front (Action.Open).
        opens: (cat?.actions || []).filter((a) => a.open).map((a) => a.id),
      },
    };
    return { title: s.title || s.sender?.name || "", options };
  }

  // --- Pushes ---

  const str = (v) => (v == null ? "" : typeof v === "string" ? v : String(v));

  /**
   * messageOf is a push's payload as push.Message's JSON. The payload is
   * the JSON the app's server sends (docs/web.md): title, body, subtitle,
   * data (its values strings; others are written as JSON), id, category,
   * thread, summary, image, badge, silent; one that isn't JSON is the
   * body.
   */
  function messageOf(payload, id) {
    const p = payload && typeof payload === "object" && !Array.isArray(payload) ? payload : { body: str(payload) };
    const data = {};
    if (p.data && typeof p.data === "object") {
      for (const [k, v] of Object.entries(p.data)) data[k] = typeof v === "object" && v !== null ? JSON.stringify(v) : str(v);
    }
    const badge = Math.max(0, Math.trunc(Number(p.badge) || 0));
    return {
      id: str(p.id) || id || newID(), title: str(p.title), body: str(p.body), subtitle: str(p.subtitle), data,
      category: str(p.category), thread: str(p.thread), channel: str(p.channel), image: str(p.image), badge, summary: str(p.summary),
    };
  }

  /** specOfMessage is the notification a push shows: its data's "url" opens the app. */
  function specOfMessage(m, payload = {}) {
    return {
      id: m.id, title: m.title, subtitle: m.subtitle, body: m.body, url: m.data?.url || "", data: m.data,
      category: m.category, thread: m.thread, summary: m.summary, badge: m.badge, image: m.image,
      silent: !!(payload && typeof payload === "object" && payload.silent),
    };
  }

  /** responseOf is sdk/notify's WireResponse: the user's answer to the notification whose data is d. */
  function responseOf(d, action, reply) {
    return { token: newID(), action: action || "", reply: reply || "", seconds: 25, notification: d.shown };
  }

  g.asukaNotify = {
    store, locked, schedule, unschedule, scheduled, takeDue, addPending, takePending, nextAt, afterDue, repeats,
    newID, shownOf, notificationOf, messageOf, specOfMessage, responseOf,
  };
})();
