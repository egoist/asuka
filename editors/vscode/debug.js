// The debugger: VS Code's side of `asuka dev`'s debug adapter, which
// speaks the Debug Adapter Protocol on a port of this computer for as
// long as `asuka dev` runs (it prints the address; 8791 unless another
// `asuka dev` has it, or `--debug-port` says).
//
// - attach: connects to that port (`port`, `host`).
// - launch: runs `asuka dev --debug-port <a free port>` as a task (the
//   app's directory, `platform`, `device`, `args`), in a terminal of its
//   own that stays with what it printed once it ends; attaches once the
//   port answers, and stops `asuka dev` (Ctrl-C in its terminal) when
//   the session ends.

"use strict";

const net = require("net");
const path = require("path");
const vscode = require("vscode");

const DEFAULT_PORT = 8791;
const DEFAULT_HOST = "127.0.0.1";

/**
 * An `asuka dev` a launch started: the app's key, its task's execution,
 * its process's id once it runs, and its end (its exit code; undefined:
 * unknown).
 * @typedef {{ key: string, execution: vscode.TaskExecution, pid: Promise<number | undefined>, ended: Promise<number | undefined>, done: boolean }} Dev
 */

/** The `asuka dev`s launches started, by their debug sessions' ids. */
const launched = new Map();
/** The `asuka dev`s being stopped: a launch waits for them to end. */
const stopping = new Set();

/** Whether something accepts connections on host:port. */
function reachable(port, host) {
  return new Promise((resolve) => {
    const s = net.connect({ port, host });
    s.once("connect", () => {
      s.destroy();
      resolve(true);
    });
    s.once("error", () => resolve(false));
  });
}

/** A port nothing listens on now. */
function freePort() {
  return new Promise((resolve, reject) => {
    const srv = net.createServer();
    srv.once("error", reject);
    srv.listen(0, DEFAULT_HOST, () => {
      const { port } = srv.address();
      srv.close(() => resolve(port));
    });
  });
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/**
 * Runs `asuka dev` as a task: its terminal shows what it prints, and
 * stays once it ends (a build that failed says why there). A launch of
 * the same app (key: its directory, platform and device) is the same
 * task, run again in the same terminal.
 * @returns {Promise<Dev>}
 */
async function startDev(folder, cmd, args, cwd, key) {
  const definition = { type: "asuka", command: "dev", ...key };
  const task = new vscode.Task(
    definition,
    folder || vscode.TaskScope.Workspace,
    "asuka dev",
    "asuka",
    new vscode.ProcessExecution(cmd, args, { cwd }),
  );
  task.isBackground = true;
  task.presentationOptions = {
    reveal: vscode.TaskRevealKind.Always,
    focus: false,
    panel: vscode.TaskPanelKind.Dedicated,
    clear: true,
    echo: false,
    showReuseMessage: false,
  };
  /** @type {Dev} */
  const dev = { key: JSON.stringify(key), execution: undefined, pid: undefined, ended: undefined, done: false };
  let started, ended;
  dev.pid = new Promise((r) => (started = r));
  dev.ended = new Promise((r) => (ended = r));
  // Its events, known by its definition (they may come before
  // executeTask's answer): no other run of it is going.
  const mine = (e) => {
    const d = e.execution.task.definition;
    return d.type === "asuka" && d.command === "dev" && Object.keys(key).every((k) => d[k] === key[k]);
  };
  const subs = [
    vscode.tasks.onDidStartTaskProcess((e) => {
      if (mine(e)) {
        started(e.processId);
      }
    }),
    vscode.tasks.onDidEndTaskProcess((e) => {
      if (mine(e)) {
        dev.done = true;
        started(undefined);
        ended(e.exitCode);
      }
    }),
    // Ended without a process (it couldn't start: no such command).
    vscode.tasks.onDidEndTask((e) => {
      if (mine(e)) {
        dev.done = true;
        started(undefined);
        ended(undefined);
      }
    }),
  ];
  dev.ended.then(() => subs.forEach((s) => s.dispose()));
  dev.execution = await vscode.tasks.executeTask(task);
  return dev;
}

/**
 * Stops an `asuka dev`: Ctrl-C in its terminal, as one would (it stops
 * the player and exits, its terminal staying); ended for good if it
 * hasn't after a while.
 * @param {Dev} dev
 */
async function stopDev(dev) {
  if (dev.done) {
    return;
  }
  stopping.add(dev);
  try {
    const pid = await Promise.race([dev.pid, sleep(5000)]);
    let terminal;
    if (pid !== undefined) {
      for (const t of vscode.window.terminals) {
        if ((await t.processId) === pid) {
          terminal = t;
          break;
        }
      }
    }
    if (terminal) {
      terminal.sendText("\u0003", false);
    } else {
      dev.execution.terminate();
    }
    const gone = await Promise.race([dev.ended.then(() => true), sleep(10000).then(() => false)]);
    if (!gone) {
      dev.execution.terminate();
      await Promise.race([dev.ended, sleep(5000)]);
    }
  } finally {
    stopping.delete(dev);
  }
}

/**
 * Waits until host:port accepts connections: false if `asuka dev` ended
 * first (it said why in its terminal) or the wait was cancelled.
 * @param {Dev} dev
 */
async function waitForPort(port, host, dev, token) {
  while (!dev.done && !token.isCancellationRequested) {
    if (await reachable(port, host)) {
      return true;
    }
    await sleep(300);
  }
  return false;
}

/** @param {() => string} asukaCommand the `asuka` command to run */
function register(context, asukaCommand) {
  const provider = {
    /** F5 with no launch.json in an .asuka file: launch the workspace's app. */
    resolveDebugConfiguration(folder, config) {
      if (!config.type && !config.request && !config.name) {
        const editor = vscode.window.activeTextEditor;
        if (!editor || editor.document.languageId !== "asuka") {
          return config;
        }
        config.type = "asuka";
        config.request = "launch";
        config.name = "Asuka: Run and Debug";
      }
      if (config.request === "attach") {
        config.port = config.port || DEFAULT_PORT;
        config.host = config.host || DEFAULT_HOST;
      } else if (config.request === "launch") {
        config.app = config.app || (folder ? folder.uri.fsPath : undefined);
        if (!config.app) {
          return vscode.window.showErrorMessage("Asuka: open the app's folder, or set the launch configuration's app").then(() => undefined);
        }
      }
      return config;
    },
  };

  const factory = {
    async createDebugAdapterDescriptor(session) {
      const config = session.configuration;
      if (config.request === "attach") {
        const port = config.port || DEFAULT_PORT;
        const host = config.host || DEFAULT_HOST;
        if (!(await reachable(port, host))) {
          throw new Error(
            `Nothing answers on ${host}:${port}. Start the app with asuka dev, which prints the debugger's address, and attach to that port.`,
          );
        }
        return new vscode.DebugAdapterServer(port, host);
      }
      // launch: asuka dev as a task, then attach. One of the same app
      // still running (another session's) stops, and one a session
      // that ended (a restart's) ends, first: its player goes before
      // the new one comes.
      const key = { app: config.app, platform: config.platform || "", device: config.device || "" };
      for (const d of launched.values()) {
        if (d.key === JSON.stringify(key)) {
          stopDev(d);
        }
      }
      await Promise.all([...stopping].map((d) => Promise.race([d.ended, sleep(15000)])));
      const port = config.port || (await freePort());
      const args = ["dev", "--debug-port", String(port)];
      if (config.platform) {
        args.push("--platform", config.platform);
      }
      if (config.device) {
        args.push("--device", config.device);
      }
      args.push(...(config.args || []), config.app);
      const folder = session.workspaceFolder;
      const dev = await startDev(folder, asukaCommand(), args, folder ? folder.uri.fsPath : path.dirname(config.app), key);
      launched.set(session.id, dev);
      const ok = await vscode.window.withProgress(
        { location: vscode.ProgressLocation.Notification, title: "Asuka: building and starting the app", cancellable: true },
        (_, token) => waitForPort(port, DEFAULT_HOST, dev, token),
      );
      if (!ok) {
        launched.delete(session.id);
        if (dev.done) {
          throw new Error("asuka dev stopped before the debugger could attach: its terminal says why.");
        }
        stopDev(dev);
        throw new Error("Cancelled: asuka dev stops.");
      }
      return new vscode.DebugAdapterServer(port, DEFAULT_HOST);
    },
  };

  context.subscriptions.push(
    vscode.debug.registerDebugConfigurationProvider("asuka", provider),
    vscode.debug.registerDebugAdapterDescriptorFactory("asuka", factory),
    vscode.debug.onDidTerminateDebugSession((session) => {
      const dev = launched.get(session.id);
      if (dev) {
        launched.delete(session.id);
        stopDev(dev);
      }
    }),
  );
}

module.exports = { register, DEFAULT_PORT };
