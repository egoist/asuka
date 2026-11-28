// The modules the platform SDK's packages over platform/host reach by name
// (modules/services.js): node --test platform/web.
import { test } from "node:test";
import assert from "node:assert/strict";
import { vCard, iCalendar, modules } from "../src/modules/services.js";

test("contacts.add's vCard and calendar.add's iCalendar", () => {
  const v = vCard({ name: "Ada Lovelace", givenName: "Ada", familyName: "Lovelace", organization: "Analytical; Engines", phones: [{ label: "mobile", value: "+44 1" }], emails: [{ label: "", value: "ada@example.com" }] });
  assert.match(v, /^BEGIN:VCARD\r\nVERSION:3.0\r\nN:Lovelace;Ada;;;\r\nFN:Ada Lovelace\r\nORG:Analytical\\; Engines\r\nTEL;TYPE=MOBILE:\+44 1\r\nEMAIL:ada@example.com\r\nEND:VCARD\r\n$/);
  const ics = iCalendar({ title: "Launch, day", startMs: Date.UTC(2026, 0, 2, 9, 30), endMs: Date.UTC(2026, 0, 2, 10), location: "HQ" }, "x@asuka", Date.UTC(2026, 0, 1));
  assert.match(ics, /DTSTART:20260102T093000Z\r\nDTEND:20260102T100000Z\r\nSUMMARY:Launch\\, day\r\nLOCATION:HQ\r\n/);
  assert.match(ics, /UID:x@asuka\r\nDTSTAMP:20260101T000000Z/);
});

test("what the web hasn't, and background's tasks", async () => {
  const m = modules();
  assert.equal(m.photoLibrary.current(), "denied");
  for (const k of ["manage", "list", "count", "thumbnail", "export", "albums", "createAlbum", "addToAlbum", "delete", "nextChange"]) {
    assert.throws(() => m.photoLibrary[k](), { message: new RegExp(`^asuka: photoLibrary.${k} isn't on the web: a page can't read the photo library`) });
  }
  assert.deepEqual([...m.contacts.request()], [1]);
  assert.throws(() => m.contacts.list(), { message: "not allowed" });
  assert.throws(() => m.icloud.kvGet(), { message: /^not available/ });
  assert.equal(m.icloud.stop().length, 0);
  assert.throws(() => m.widget.set(), /widget.set isn't on the web/);
  assert.deepEqual([...m.liveActivity.enabled()], [0]);
  // background: next waits while a task lives, and fails "ended" when the last ends.
  const enc = (o) => new TextEncoder().encode(JSON.stringify(o));
  m.background.start(enc({ id: 1, title: "Upload" }));
  const next = m.background.next();
  m.background.end(enc({ id: 1 }));
  await assert.rejects(next, { message: "ended" });
  await assert.rejects(m.background.next(), { message: "ended" });
});

test("every module the file has: platform/host's, as the registry names them", async () => {
  const { files } = await import("../src/modules.js");
  const all = modules();
  for (const name of Object.keys(files).filter((n) => files[n] === "services")) assert.ok(all[name], name);
  assert.deepEqual(Object.keys(all).sort(), Object.keys(files).filter((n) => files[n] === "services").sort());
});
