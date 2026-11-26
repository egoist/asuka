// pdf_view_test.js: platform/pdf's web view (web/pdf.js). An asset is
// shown by the browser's own viewer in a frame whose URL is the asset's at
// #page=1; setting page loads it again at that page, the old frame taken
// away once the new one has loaded; loaded says 0 pages once; an asset
// that isn't there is said with error; a file of the app's is shown from
// a Blob typed application/pdf; and a browser with no viewer shows a link
// that opens it instead, and says so. e2e's TestStdWebViews runs it in
// headless Chrome: it ends by writing "PASS ..." or "FAIL ..." into the
// page's #result.
import { makeView } from "../web/pdf.js";

const failures = [];
const check = (ok, what) => {
  if (!ok) failures.push(what);
};

// What the host gives a package's JavaScript: an asset's URL (this
// directory stands for the app's assets), and a file of the app's as a
// Blob (here, /docs/three.pdf is the fixture; any other path isn't there).
const dir = new URL(".", import.meta.url).pathname;
globalThis.asuka = {
  native: {
    assetURL: (src) => (/^https?:/.test(src) ? src : dir + src),
    files: {
      async blob(src, type) {
        if (src !== "/docs/three.pdf") throw new Error("no file at " + src);
        return new Blob([await (await fetch(dir + "three.pdf")).arrayBuffer()], { type });
      },
    },
  },
};

/** until waits for cond, at most ms. */
async function until(cond, ms = 15000) {
  for (const end = Date.now() + ms; Date.now() < end; await new Promise((r) => setTimeout(r, 50))) {
    if (cond()) return true;
  }
  return cond();
}

/** view makes a view 300 by 400 in the page, and what it says. */
function view() {
  const said = { loaded: [], pageChange: [], error: [] };
  const v = makeView({
    loaded: (n) => said.loaded.push(n),
    pageChange: (p) => said.pageChange.push(p),
    error: (m) => said.error.push(m),
  });
  const box = document.createElement("div");
  box.style.cssText = "width: 300px; height: 400px";
  box.appendChild(v.el);
  document.body.appendChild(box);
  return { v, said, frames: () => [...v.el.querySelectorAll("iframe")] };
}

async function run() {
  check(navigator.pdfViewerEnabled === true, `navigator.pdfViewerEnabled is ${navigator.pdfViewerEnabled}: this Chrome has no viewer to test`);

  // An asset, at its first page.
  const a = view();
  a.v.setSource("three.pdf");
  let f = a.frames();
  check(f.length === 1, `${f.length} frames, want 1`);
  check(f[0]?.src.endsWith(dir + "three.pdf#page=1"), `the frame's src is ${f[0]?.src}, want the asset's at #page=1`);
  check(f[0]?.title === "three.pdf", `the frame's title is ${JSON.stringify(f[0]?.title)}, want the file's name`);
  check(await until(() => a.said.loaded.length > 0), "no loaded");
  check(JSON.stringify(a.said.loaded) === "[0]", `loaded ${JSON.stringify(a.said.loaded)}, want [0] (the web doesn't say)`);

  // Page 2 (the third): the document loads again there, and the old frame goes.
  a.v.setPage(2);
  check(a.frames().some((x) => x.src.endsWith("three.pdf#page=3")), "setting page 2 made no frame at #page=3");
  check(await until(() => a.frames().length === 1 && a.frames()[0].src.endsWith("#page=3")), `after page 2 the frames are ${a.frames().map((x) => x.src).join(", ")}, want one at #page=3`);
  check(JSON.stringify(a.said.loaded) === "[0]", `loaded ${JSON.stringify(a.said.loaded)} after a page change, want it said once`);
  check(a.said.error.length === 0, `errors: ${a.said.error.join("; ")}`);
  check(a.said.pageChange.length === 0, "pageChange from a viewer that doesn't report scrolling");
  a.v.destroy();
  check(a.frames().length === 0, "destroy left a frame");

  // An asset that isn't there.
  const m = view();
  m.v.setSource("missing.pdf");
  check(await until(() => m.said.error.length > 0), "no error for an asset that isn't there");
  check(/^platform\/pdf: missing\.pdf: 404/.test(m.said.error[0] ?? ""), `the error is ${JSON.stringify(m.said.error[0])}, want missing.pdf's 404`);
  check(m.frames().length === 0, "a frame stays for an asset that isn't there");
  check(m.said.loaded.length === 0, "loaded for an asset that isn't there");
  m.v.destroy();

  // A file of the app's, at page 1 (the second) set before its source.
  const p = view();
  p.v.setPage(1);
  p.v.setSource("/docs/three.pdf");
  check(await until(() => p.frames().length === 1), "no frame for a file of the app's");
  f = p.frames();
  check(f[0]?.src.startsWith("blob:") && f[0]?.src.endsWith("#page=2"), `the file's frame's src is ${f[0]?.src}, want a Blob at #page=2`);
  check(await until(() => p.said.loaded.length > 0), "no loaded for a file of the app's");
  p.v.setSource("/docs/gone.pdf");
  check(await until(() => p.said.error.length > 0), "no error for a file that isn't there");
  check(p.said.error[0] === "platform/pdf: /docs/gone.pdf: there's no such file", `the error is ${JSON.stringify(p.said.error[0])}`);
  check(p.frames().length === 0, "the old file's frame stays after its source changed");
  p.v.destroy();

  // A browser with no viewer: a link that opens the PDF in a new tab.
  Object.defineProperty(navigator, "pdfViewerEnabled", { value: false, configurable: true });
  const n = view();
  n.v.setSource("three.pdf");
  const link = n.v.el.querySelector("a");
  check(n.frames().length === 0, "a frame where the browser has no viewer");
  check(link?.textContent === "Open PDF", `the fallback's link says ${JSON.stringify(link?.textContent)}`);
  check(link?.href.endsWith(dir + "three.pdf") && link?.target === "_blank" && link?.rel === "noopener", `the link is ${link?.href} (target ${link?.target}, rel ${link?.rel}), want the asset's in a new tab`);
  check(JSON.stringify(n.said.error) === JSON.stringify(["platform/pdf: this browser has no PDF viewer: the link opens it"]), `errors ${JSON.stringify(n.said.error)}`);
  check(n.said.loaded.length === 0, "loaded where the browser has no viewer");
  n.v.destroy();
  delete navigator.pdfViewerEnabled;
}

const result = document.getElementById("result");
run().then(
  () => { result.textContent = failures.length ? "FAIL\n" + failures.join("\n") : "PASS platform/pdf's web view"; },
  (e) => { result.textContent = "FAIL " + (e.stack || e); },
);
