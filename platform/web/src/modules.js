// Native modules (ui.Call), by name: the app's own (asuka.registerModule),
// and the few of the host's that the core and the platform SDK's
// packages over platform/host still call by name. Every
// other platform API is a package's own JavaScript (std/<name>/web),
// loaded only by the apps that import it.
//
// A method answers once, with Result on the root: u32 call id, u8 ok, then
// the result's bytes (an error's message when not ok). A module the web
// host doesn't have, or a method a browser can't do, answers with an error
// that says so.
//
// The host's modules are in modules/: each file exports modules(h), its
// modules by name, each a map of methods. A file is imported, and its
// modules made, on its first call (files names which has each). A method
// takes the call's bytes and the call ({ root, h }) and returns (or
// resolves to) the result's bytes (a Uint8Array, or a string, sent as
// UTF-8), or throws.

import { Events, Reader, Writer } from "./ops.js";

const enc = new TextEncoder();

/** files is which of modules/ has each of the host's modules. */
export const files = {
  share: "system", compose: "system", notify: "system", push: "system", // the share sheet; platform/compose, notify, push
  web: "web", // WebView's eval and cookies
  view: "capture", // imageedit.capture: a view drawn into an image file
  assets: "assets", // fs.readAsset
  dock: "desktop", recent: "desktop", login: "desktop", // platform/host's
  icloud: "services", periodic: "services", background: "services", widget: "services", liveActivity: "services",
  photoLibrary: "services", library: "services", contacts: "services", calendar: "services", // platform/host's
};

const load = {
  system: () => import("./modules/system.js"),
  web: () => import("./modules/web.js"),
  capture: () => import("./modules/capture.js"),
  assets: () => import("./modules/assets.js"),
  desktop: () => import("./modules/desktop.js"),
  services: () => import("./modules/services.js"),
};

export class Modules {
  constructor(h) {
    this.h = h;
    this.registered = new Map(); // asuka.registerModule's
    this.made = new Map(); // a file of modules/ → a promise of its modules
  }

  call(root, args) {
    const r = new Reader(args);
    const id = r.u32();
    const module = r.string(), method = r.string();
    const body = args.slice(r.p);
    const reply = (result, err) => {
      const w = new Writer().u32(id).u8(err ? 0 : 1);
      if (err) w.raw(enc.encode(String(err?.message ?? err)));
      else if (result != null) w.raw(result instanceof Uint8Array ? result : enc.encode(String(result)));
      this.h.dispatch(Events.result(root.id, w.done()));
    };
    // Answered once the core has returned: never inside the batch.
    this.h.afterCore(() => {
      this.method(module, method)
        .then((m) => {
          if (!m) throw new Error(`asuka: ${module}.${method} isn't on the web`);
          return m(body, { root, h: this.h });
        })
        .then((v) => reply(v, null), (e) => reply(null, e));
    });
  }

  /** method is module's method: the app's, else the host's (null: none). */
  async method(module, method) {
    return this.registered.get(module)?.[method] || (await this.builtin(module))?.[method] || null;
  }

  /** builtin is a promise of the host's module name's methods (null: the host has none). */
  async builtin(name) {
    const file = Object.hasOwn(files, name) ? files[name] : null;
    if (!file) return null;
    let p = this.made.get(file);
    if (!p) {
      p = load[file]().then((f) => f.modules(this.h));
      this.made.set(file, p);
    }
    const all = await p;
    return Object.hasOwn(all, name) ? all[name] : null;
  }
}
