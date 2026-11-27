// The Asuka extension: the language's grammar and settings, the
// language server, `asuka lsp`, for everything that needs the compiler
// (diagnostics, completion, hover, go to definition, references,
// rename, formatting, symbols, signature help, semantic tokens, the ▶
// Preview lens over each preview block), and the debugger, `asuka
// dev`'s (debug.js).
//
// Plain JavaScript, no build step: VS Code loads this file as it is.

"use strict";

const path = require("path");
const os = require("os");
const vscode = require("vscode");
const { LanguageClient, TransportKind, State } = require("vscode-languageclient/node");
const debug = require("./debug");

/** @type {LanguageClient | undefined} */
let client;
/** @type {vscode.OutputChannel | undefined} */
let output;

/** The asuka command: asuka.server.path, resolved. */
function asukaCommand() {
  const cfg = vscode.workspace.getConfiguration("asuka");
  let cmd = (cfg.get("server.path") || "asuka").trim();
  if (cmd.startsWith("~/")) {
    cmd = path.join(os.homedir(), cmd.slice(2));
  }
  // A relative path (./bin/asuka) is the workspace's.
  if ((cmd.includes("/") || cmd.includes("\\")) && !path.isAbsolute(cmd)) {
    const folder = vscode.workspace.workspaceFolders && vscode.workspace.workspaceFolders[0];
    if (folder) {
      cmd = path.join(folder.uri.fsPath, cmd);
    }
  }
  return cmd;
}

/** The command the server runs as. */
function serverCommand() {
  const cfg = vscode.workspace.getConfiguration("asuka");
  const args = ["lsp", ...(cfg.get("server.args") || [])];
  return { cmd: asukaCommand(), args };
}

async function start() {
  const { cmd, args } = serverCommand();
  // TransportKind.stdio adds --stdio, which asuka lsp takes.
  const run = { command: cmd, args, transport: TransportKind.stdio };
  const serverOptions = { run, debug: { ...run, args: [...args, "--verbose"] } };
  const watcher = vscode.workspace.createFileSystemWatcher("**/{*.asuka,asuka.toml,asuka.lock}");
  const clientOptions = {
    documentSelector: [{ scheme: "file", language: "asuka" }],
    // Files changed outside the editor (a git checkout, asuka add):
    // the server re-checks what read them.
    synchronize: { fileEvents: watcher },
    outputChannel: output,
    traceOutputChannel: output,
  };
  client = new LanguageClient("asuka", "Asuka", serverOptions, clientOptions);
  try {
    await client.start();
  } catch (err) {
    client = undefined;
    watcher.dispose();
    const pick = await vscode.window.showErrorMessage(
      `Asuka: couldn't start the language server (${cmd} lsp): ${err && err.message ? err.message : err}. ` +
        "Install the asuka CLI, or set asuka.server.path to it.",
      "Open Settings",
      "Show Output",
    );
    if (pick === "Open Settings") {
      vscode.commands.executeCommand("workbench.action.openSettings", "asuka.server.path");
    } else if (pick === "Show Output") {
      output.show(true);
    }
    return;
  }
  client.onDidChangeState((e) => {
    if (e.newState === State.Stopped) {
      output.appendLine("The language server stopped.");
    }
  });
}

async function stop() {
  if (!client) {
    return;
  }
  const c = client;
  client = undefined;
  try {
    await c.stop();
  } catch (err) {
    output.appendLine(`Stopping the language server: ${err}`);
  }
}

async function restart() {
  await stop();
  await start();
}

/**
 * Has the asuka dev that runs the active file's app show the preview at
 * the cursor (app: the app again), through the language server's command,
 * as the ▶ Preview lens does.
 * @param {boolean} app
 */
async function showPreview(app) {
  const ed = vscode.window.activeTextEditor;
  if (!client || !ed || ed.document.languageId !== "asuka") {
    vscode.window.showInformationMessage("Asuka: open an .asuka file of the app first.");
    return;
  }
  const file = ed.document.uri.fsPath;
  const line = ed.selection.active.line + 1;
  await client.sendRequest("workspace/executeCommand", {
    command: app ? "asuka.preview.showApp" : "asuka.preview.show",
    arguments: [app ? { file } : { file, line }],
  });
}

/** @param {vscode.ExtensionContext} context */
async function activate(context) {
  output = vscode.window.createOutputChannel("Asuka");
  debug.register(context, asukaCommand);
  context.subscriptions.push(
    output,
    vscode.commands.registerCommand("asuka.restartServer", restart),
    vscode.commands.registerCommand("asuka.showOutput", () => output.show(true)),
    vscode.commands.registerCommand("asuka.showPreview", () => showPreview(false)),
    vscode.commands.registerCommand("asuka.showApp", () => showPreview(true)),
    vscode.workspace.onDidChangeConfiguration((e) => {
      if (e.affectsConfiguration("asuka.server")) {
        restart();
      }
    }),
  );
  await start();
}

function deactivate() {
  return stop();
}

module.exports = { activate, deactivate };
