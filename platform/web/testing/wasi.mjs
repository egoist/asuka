// Runs asuka-run built for WebAssembly (cbuild's RunnerOptions.Wasm32: a
// WASI command, the VM and the fake host, an app compiled to C in it or
// not) under Node, so the tests run programs on 32-bit pointers as a web
// build does: wasm32's ak_val is 8 bytes, a pointer 4. Its arguments are
// asuka-run's; what it prints is asuka-run's, and its exit code.
//
// The imports the page gives the core (module "asuka", core.js's) that a
// test can reach have their page's behavior: the engine's stack
// (stack.js), the time zone (UTC), std/log's lines (to stdout, warnings
// and errors to stderr). The rest (views, timers, workers, native
// modules: the runner's own fake host and clock run those) do nothing.
//
//	node wasi.mjs asuka-run.wasm [asuka-run's arguments...]
import { mkdtempSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { WASI } from "node:wasi";
import { stackRoom } from "../src/stack.js";

const [wasm, ...args] = process.argv.slice(2);
// The web's files: /data, the app's (the page's IndexedDB), a folder of
// the run's own here, removed after it.
const data = mkdtempSync(join(tmpdir(), "asuka-wasi-"));
process.on("exit", () => rmSync(data, { recursive: true, force: true }));
const env = { TMPDIR: process.env.TMPDIR || tmpdir() };
const wasi = new WASI({ version: "preview1", args: ["asuka-run", ...args], env, preopens: { "/": "/", "/data": data }, returnOnExit: true });
let instance;
const bytes = (p, n) => Buffer.from(new Uint8Array(instance.exports.memory.buffer, p, n));
const page = {
  stack_room: stackRoom(),
  zone_offset: () => 0,
  zone_name: () => 0,
  log_write: (level, p, n) => (level >= 2 ? process.stderr : process.stdout).write(bytes(p, n).toString() + "\n"),
};
const module = new WebAssembly.Module(readFileSync(wasm));
const asuka = {};
for (const i of WebAssembly.Module.imports(module)) {
  if (i.module === "asuka") asuka[i.name] = page[i.name] ?? (() => 0);
}
instance = new WebAssembly.Instance(module, { wasi_snapshot_preview1: wasi.wasiImport, asuka });
process.exitCode = wasi.start(instance);
