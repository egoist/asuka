// The debugger: VS Code's side of `asuka dev`'s debug adapter, which
// speaks the Debug Adapter Protocol on a port of this computer for as
// long as `asuka dev` runs (it prints the address; 8791 unless another
// `asuka dev` has it, or `--debug-port` says).
//
// - attach: connects to that port (`port`, `host`).
// - launch: runs `asuka dev --debug-port <a free port>` in a terminal
//   (the app's directory, `platform`, `device`, `args`), attaches once
//   the port answers, and stops `asuka dev` (Ctrl-C in its terminal)
//   when the session ends.

"use strict";

const net = require("net");
const path = require("path");
const vscode = require("vscode");

const DEFAULT_PORT = 8791;
const DEFAULT_HOST = "127.0.0.1";

/** Sessions launch started, and the terminal running their `asuka dev`. */
const launched = new Map();

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

/**
 * Waits until host:port accepts connections: false if the terminal
 * closed (asuka dev stopped: a build that failed) or the wait was
 * cancelled first.
 */
async function waitForPort(port, host, terminal, token) {
  let closed = false;
  const sub = vscode.window.onDidCloseTerminal((t) => {
    if (t === terminal) {
      closed = true;
    }
  });
  try {
    while (!closed && !token.isCancellationRequested) {
      if (await reachable(port, host)) {
        return true;
      }
      await new Promise((r) => setTimeout(r, 300));
    }
    return false;
  } finally {
    sub.dispose();
  }
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
      // launch: asuka dev in a terminal, then attach.
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
      const terminal = vscode.window.createTerminal({
        name: "asuka dev",
        shellPath: asukaCommand(),
        shellArgs: args,
        cwd: folder ? folder.uri.fsPath : path.dirname(config.app),
      });
      terminal.show(true);
      launched.set(session.id, terminal);
      const ok = await vscode.window.withProgress(
        { location: vscode.ProgressLocation.Notification, title: "Asuka: building and starting the app", cancellable: true },
        (_, token) => waitForPort(port, DEFAULT_HOST, terminal, token),
      );
      if (!ok) {
        launched.delete(session.id);
        throw new Error("asuka dev stopped before the debugger could attach: its terminal says why.");
      }
      return new vscode.DebugAdapterServer(port, DEFAULT_HOST);
    },
  };

  context.subscriptions.push(
    vscode.debug.registerDebugConfigurationProvider("asuka", provider),
    vscode.debug.registerDebugAdapterDescriptorFactory("asuka", factory),
    vscode.debug.onDidTerminateDebugSession((session) => {
      const terminal = launched.get(session.id);
      if (terminal) {
        launched.delete(session.id);
        // Ctrl-C: asuka dev stops the player and exits; its terminal
        // stays, with what it printed.
        terminal.sendText("\u0003", false);
      }
    }),
  );
}

module.exports = { register, DEFAULT_PORT };
