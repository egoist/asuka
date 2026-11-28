// The modules the platform SDK's packages over platform/host reach by
// name, as a browser can have them (each package's own JavaScript is in
// std/<name>/web where it has some):
//
//   - background (platform/background): a page runs while it's open
//     (throttled while hidden): tasks start and end, with no notification.
//   - library.save (platform/library): a download. contacts.pick
//     (platform/contacts): the Contact Picker API (Chrome on Android).
//     contacts.add and calendar.add (platform/calendar): a vCard and an
//     iCalendar file, which the system offers to add.
//   - The rest fail, saying why: icloud, periodic, widget, liveActivity,
//     the photo library (platform/photos), contacts' and calendars' reading.
//     (notify, push, compose and share are system.js's.)

import * as fs from "../fs.js";

const dec = new TextDecoder();
const notOnWeb = (what, why) => new Error(`asuka: ${what} isn't on the web: ${why}`);
const fail = (what, why) => () => { throw notOnWeb(what, why); };

// --- icloud ---

/** icloud is platform/icloud's module (sdk/icloud's methods): none here. */
function icloud() {
  const m = { stop: () => new Uint8Array(0) };
  for (const k of ["account", "kvGet", "kvSet", "kvDelete", "kvAll", "kvSync", "container", "files", "watch", "unwatch",
    "download", "evict", "read", "write", "delete", "move", "moveIn", "moveOut", "versions", "resolve", "link", "next"]) {
    m[k] = () => { throw new Error("not available: iCloud is Apple's"); };
  }
  return m;
}

// --- background ---

function background() {
  const tasks = new Set();
  const waiting = [];
  return {
    start: (b) => { tasks.add(JSON.parse(dec.decode(b)).id); },
    update: () => {},
    end: (b) => {
      tasks.delete(JSON.parse(dec.decode(b)).id);
      if (!tasks.size) for (const w of waiting.splice(0)) w(new Error("ended"));
    },
    // A page has no notification to tap: no action comes.
    next: () => new Promise((_, reject) => { if (!tasks.size) reject(new Error("ended")); else waiting.push(reject); }),
  };
}

// --- files the system adds: vCard, iCalendar ---

function download(bytes, name, type) {
  const a = document.createElement("a");
  a.href = URL.createObjectURL(new Blob([bytes], { type }));
  a.download = name;
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 10000);
}

const vEscape = (s) => String(s ?? "").replace(/\\/g, "\\\\").replace(/\n/g, "\\n").replace(/([,;])/g, "\\$1");

/** vCard is a contact (platform/contacts' JSON) as a vCard 3.0. */
export function vCard(c) {
  const lines = ["BEGIN:VCARD", "VERSION:3.0"];
  const fn = c.name || [c.givenName, c.familyName].filter(Boolean).join(" ") || c.organization || "";
  lines.push(`N:${vEscape(c.familyName)};${vEscape(c.givenName)};;;`, `FN:${vEscape(fn)}`);
  if (c.organization) lines.push(`ORG:${vEscape(c.organization)}`);
  if (c.jobTitle) lines.push(`TITLE:${vEscape(c.jobTitle)}`);
  for (const p of c.phones || []) lines.push(`TEL${p.label ? `;TYPE=${vEscape(p.label).toUpperCase()}` : ""}:${vEscape(p.value)}`);
  for (const e of c.emails || []) lines.push(`EMAIL${e.label ? `;TYPE=${vEscape(e.label).toUpperCase()}` : ""}:${vEscape(e.value)}`);
  if (c.birthday) lines.push(`BDAY:${c.birthday}`);
  if (c.note) lines.push(`NOTE:${vEscape(c.note)}`);
  lines.push("END:VCARD");
  return lines.join("\r\n") + "\r\n";
}

const icsDate = (ms, allDay) => {
  const d = new Date(ms);
  const p = (n) => String(n).padStart(2, "0");
  if (allDay) return `${d.getFullYear()}${p(d.getMonth() + 1)}${p(d.getDate())}`;
  return `${d.getUTCFullYear()}${p(d.getUTCMonth() + 1)}${p(d.getUTCDate())}T${p(d.getUTCHours())}${p(d.getUTCMinutes())}${p(d.getUTCSeconds())}Z`;
};

/** iCalendar is an event (platform/calendar's add JSON) as an iCalendar file. */
export function iCalendar(e, uid, now = Date.now()) {
  const all = !!e.allDay;
  const end = all && e.endMs <= e.startMs ? e.startMs + 86400000 : e.endMs;
  const lines = [
    "BEGIN:VCALENDAR", "VERSION:2.0", "PRODID:-//Asuka//web//EN", "BEGIN:VEVENT",
    `UID:${uid}`, `DTSTAMP:${icsDate(now, false)}`,
    all ? `DTSTART;VALUE=DATE:${icsDate(e.startMs, true)}` : `DTSTART:${icsDate(e.startMs, false)}`,
    all ? `DTEND;VALUE=DATE:${icsDate(end, true)}` : `DTEND:${icsDate(end, false)}`,
    `SUMMARY:${vEscape(e.title)}`,
  ];
  if (e.location) lines.push(`LOCATION:${vEscape(e.location)}`);
  if (e.notes) lines.push(`DESCRIPTION:${vEscape(e.notes)}`);
  if (e.url) lines.push(`URL:${e.url}`);
  lines.push("END:VEVENT", "END:VCALENDAR");
  return lines.join("\r\n") + "\r\n";
}

const fileName = (s, ext) => (String(s || "").replace(/[\\/:*?"<>|]+/g, " ").trim() || "untitled") + ext;

function contacts() {
  return {
    request: () => new Uint8Array([1]), // Denied: a page can't read the address book
    list: () => { throw new Error("not allowed"); },
    pick: async () => {
      if (!navigator.contacts?.select) throw notOnWeb("contacts.pick", "this browser has no contact picker (Chrome on Android has)");
      const props = await navigator.contacts.getProperties?.() ?? ["name", "email", "tel"];
      const [c] = await navigator.contacts.select(props.filter((p) => ["name", "email", "tel"].includes(p)), { multiple: false });
      if (!c) return null;
      const name = c.name?.[0] || "";
      const parts = name.split(/\s+/);
      return JSON.stringify({
        id: "", name, givenName: parts.length > 1 ? parts.slice(0, -1).join(" ") : name, familyName: parts.length > 1 ? parts.at(-1) : "",
        organization: "", jobTitle: "", birthday: "", note: "",
        phones: (c.tel || []).map((value) => ({ label: "", value })),
        emails: (c.email || []).map((value) => ({ label: "", value })),
      });
    },
    // The system offers to add the vCard; the page isn't told if it did.
    add: (b) => {
      const c = JSON.parse(dec.decode(b));
      download(vCard(c), fileName(c.name || [c.givenName, c.familyName].filter(Boolean).join(" "), ".vcf"), "text/vcard");
      return new Uint8Array([0]);
    },
  };
}

function calendar() {
  return {
    request: () => new Uint8Array([1]), // Denied: a page can't read calendars
    calendars: () => { throw new Error("not allowed"); },
    events: () => { throw new Error("not allowed"); },
    add: (b) => {
      const e = JSON.parse(dec.decode(b));
      const uid = (globalThis.crypto?.randomUUID?.() ?? String(Date.now())) + "@asuka";
      download(iCalendar(e, uid), fileName(e.title, ".ics"), "text/calendar");
      return new Uint8Array([0]);
    },
  };
}

function library() {
  return {
    save: (b) => {
      const o = JSON.parse(dec.decode(b));
      const bytes = fs.readFile(o.path);
      if (!bytes) throw new Error(`no file ${o.path}`);
      download(bytes, o.name, o.mime || "application/octet-stream");
      if (o.move) fs.removeFile(o.path);
      return o.name; // in the browser's downloads
    },
  };
}

export function modules() {
  const noLibrary = "a page can't read the photo library; device.pickMedia picks photos and videos";
  return {
    // platform/icloud: iCloud is Apple's (CloudKit JS has neither the
    // key-value store nor the app's containers).
    icloud: icloud(),
    periodic: {
      schedule: fail("periodic.schedule", "a browser runs no page's tasks while it's closed"),
      cancel: () => {},
      next: fail("periodic.next", "a browser runs no page's tasks while it's closed"),
      done: () => {},
    },
    background: background(),
    widget: { set: fail("widget.set", "a page has no home screen widgets"), entities: fail("widget.setEntities", "a page has no home screen widgets") },
    liveActivity: {
      enabled: () => new Uint8Array([0]),
      start: () => { throw new Error("not available: a page has no live activities"); },
      update: () => {},
      end: () => {},
      running: () => "[]",
      next: () => { throw new Error("not available: a page has no live activities"); },
      stop: () => {},
    },
    photoLibrary: {
      current: () => "denied",
      request: () => "denied",
      manage: fail("photoLibrary.manage", noLibrary),
      list: fail("photoLibrary.list", noLibrary),
      thumbnail: fail("photoLibrary.thumbnail", noLibrary),
      export: fail("photoLibrary.export", noLibrary),
      count: fail("photoLibrary.count", noLibrary),
      albums: fail("photoLibrary.albums", noLibrary),
      createAlbum: fail("photoLibrary.createAlbum", noLibrary),
      addToAlbum: fail("photoLibrary.addToAlbum", noLibrary),
      delete: fail("photoLibrary.delete", noLibrary),
      nextChange: fail("photoLibrary.nextChange", noLibrary),
      stop: () => {},
    },
    library: library(),
    contacts: contacts(),
    calendar: calendar(),
  };
}
