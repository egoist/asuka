// dev.h: asuka dev's connection to a player. The player (a host built
// for development: the iOS simulator's or a phone's, an emulator's, a
// desktop's) connects to the CLI; the CLI sends it the app's bytecode,
// and again on every save; the player sends back its logs.
//
// Frames both ways: u32 length (of what follows, little-endian), u8
// kind, payload.
//   CLI → player:  1 module (bytecode)    2 ping
//                  7 restart (bytecode: loaded keeping none of the
//                    app's state, which all starts again)
//                  9 source (u32 an uncaught error's number; then
//                    u32-length strings: the lines of the app's source
//                    where it went wrong, numbered, and its stack, the
//                    files relative to the app's)
//                 10 failed (a build that failed, u32-length strings:
//                    its first error, the lines of the source at it, its
//                    other errors; again on each build that fails)
//                 11 fixed (the app builds again)
//                 12 inspect (asuka dev's inspector asks for the views:
//                    answered with a tree)
//                 14 highlight (the inspector selected a view: its path,
//                    its indexes from the tree's roots joined by dots,
//                    "0.2.1"; empty: none, the highlight goes)
//                 15 automate (u32 its number, then commands, a line
//                    each: asuka test --gui's; answered with automated)
//                 17 debug-breakpoints (a u32-length string, the file as
//                    the module's functions name it, then u32 n and n
//                    breakpoints, which replace those it had (n 0:
//                    none): u32 line, u32 key, u8 flags, u8 hit op, u32
//                    hit n (vm/debug.h's ak_dbg_bp), u32 m and m codes:
//                    u64 the module's hash, u32 its function, u32 the
//                    condition's function, u32 the log message's
//                    (0xFFFFFFFF: none; the extension's), u32 k and k
//                    u32 locals they take, a u32-length string, why it
//                    didn't compile ("": it did))
//                 18 debug-command (u8: 1 continue, 2 step over, 3 step
//                    in, 4 step out, 5 pause: vm/debug.h's AK_DBG_*)
//                 19 debug-variables (u32 its number, u32 a frame of the
//                    stop's, u32 start, u32 count (0: all, to 1000), u32
//                    n, then n u32s: a path to a value, the index of a
//                    local in the frame's locals, then of a child at each
//                    level; answered with debug-values)
//                 23 debug-reset (the debugger detached: no breakpoints,
//                    no step, and a paused app goes on)
//                 24 perf (u8 flags: 1 the performance overlay shows;
//                    ui.h's ak_perf_set_overlay)
//                 25 profile-start (u8 flags: 1 sample the app's
//                    functions; u32 the interval, microseconds (0: 1
//                    ms): a recording starts, ak_perf_record_start)
//                 26 profile-stop (the recording ends: answered with
//                    profile)
//                 30 debug-extend (an extension of a module, lang/
//                    bytecode's Extension: the functions the
//                    breakpoints' code and evaluations name)
//                 31 debug-exceptions (u8: the errors that stop the app,
//                    1 uncaught, 2 all: AK_DBG_EXC_*)
//                 32 debug-evaluate (u32 its number, u32 a frame of the
//                    stop's, u64 the module's hash, u32 the extension's
//                    function, u32 n and n u32 locals it takes, a
//                    u32-length string: the value's descriptor;
//                    answered with debug-result)
//                  2 ping, answered with a ping by the connection's own
//                    thread, while the app runs or is paused
//   player → CLI: 16 automated (u32 the automate's number, then each
//                    command's answer: a line, ok or error: why, or the
//                    texts a dump says)
//                  3 log (u8 fd: 1 out, 2 err; then the text)
//                  4 hello (JSON: the platform, the device)
//                  5 reloaded (u32 state cells kept, u32 microseconds,
//                    u8 flags: AK_DEV_RESTARTED for a restart's)
//                  6 error (text: a module that didn't load)
//                  8 uncaught (an error nothing in the app caught: u32
//                    its number, u8 flags (AK_DEV_DIDNT_START: the
//                    module's init threw), then u32-length strings: its
//                    message and its stack, a line per frame, innermost
//                    first: "name (file:line)"); the CLI prints it and
//                    answers with its source
//
//                 13 tree (JSON: the app's views, ui.h's ak_ui_inspect)
//                 20 debug-stopped (JSON: the app paused at a breakpoint,
//                    a step or a pause: the stack, innermost first, and
//                    each frame's locals; vm/debug.h's AK_DBG_STOPPED)
//                 21 debug-continued (the app runs again)
//                 22 debug-values (u32 a debug-variables' number, then
//                    JSON: the children of the value it named)
//                 27 profile (JSON: a recording, ak_perf_record_stop's)
//                 33 debug-result (u32 a debug-evaluate's number, then
//                    JSON: the value, or why not)
//                 34 debug-log (JSON: a logpoint's message, or why a
//                    breakpoint's code failed)
//                  2 ping (a ping's answer)
//
// The debugger's frames (17 to 23, 30 to 34) are read and answered on the
// connection's own thread, not posted to the main thread: while the app
// is paused at a breakpoint, its main thread waits for them (vm/debug.h).
// A module or a restart that arrives while it's paused lets it go on
// (it stops nowhere until the new module is loaded), so the reload runs.
// The overlay (ui.h's ak_dev_overlay_*) shows what failed, uncaught and
// source say, over the app.
#ifndef AK_DEV_H
#define AK_DEV_H

#include "base/base.h"

enum {
	AK_DEV_MODULE = 1,
	AK_DEV_PING = 2,
	AK_DEV_LOG = 3,
	AK_DEV_HELLO = 4,
	AK_DEV_RELOADED = 5,
	AK_DEV_ERROR = 6,
	AK_DEV_RESTART = 7,
	AK_DEV_UNCAUGHT = 8,
	AK_DEV_SOURCE = 9,
	AK_DEV_FAILED = 10,
	AK_DEV_FIXED = 11,
	AK_DEV_INSPECT = 12,
	AK_DEV_TREE = 13,
	AK_DEV_HIGHLIGHT = 14,
	AK_DEV_AUTOMATE = 15,
	AK_DEV_AUTOMATED = 16,
	AK_DEV_DEBUG_BREAKPOINTS = 17,
	AK_DEV_DEBUG_COMMAND = 18,
	AK_DEV_DEBUG_VARIABLES = 19,
	AK_DEV_DEBUG_STOPPED = 20,
	AK_DEV_DEBUG_CONTINUED = 21,
	AK_DEV_DEBUG_VALUES = 22,
	AK_DEV_DEBUG_RESET = 23,
	AK_DEV_PERF = 24,
	AK_DEV_PROFILE_START = 25,
	AK_DEV_PROFILE_STOP = 26,
	AK_DEV_PROFILE = 27,
	AK_DEV_DEBUG_EXTEND = 30,
	AK_DEV_DEBUG_EXCEPTIONS = 31,
	AK_DEV_DEBUG_EVALUATE = 32,
	AK_DEV_DEBUG_RESULT = 33,
	AK_DEV_DEBUG_LOG = 34,
};
// A perf frame's flags.
enum { AK_DEV_PERF_OVERLAY = 1 };
// A profile-start frame's flags.
enum { AK_DEV_PROFILE_CPU = 1 };

// A reloaded frame's flags.
enum { AK_DEV_RESTARTED = 1 };
// An uncaught frame's flags.
enum { AK_DEV_DIDNT_START = 1 };

// ak_dev_set_reload sets what a module that arrives is given to, on the
// main thread (the ABI's reload: keep state, swap the module, mount);
// fresh for a restart's, which keeps no state.
void ak_dev_set_reload(void (*reload)(const uint8_t *module, size_t len, bool fresh));
// ak_dev_set_frames sets what the CLI's other frames (source, failed,
// fixed) are given to, on the main thread.
void ak_dev_set_frames(void (*frame)(uint8_t kind, const uint8_t *p, size_t len));

// ak_dev_connect connects to the CLI at one of hosts (comma-separated:
// tried in turn) on port, on a thread of its own that reads its frames,
// and connects again whenever the connection ends (the CLI restarted,
// the network went), so the player comes back by itself; hello is the
// player's JSON, sent on each connection. With capture, what the process
// writes to stdout and stderr is sent too. False only when it can't
// start (an error in err): not reaching the CLI yet isn't one.
bool ak_dev_connect(const char *hosts, int port, const char *hello, bool capture, char *err, size_t errcap);
// ak_dev_listen takes connections from the CLI on the loopback's port
// too: an iPhone's player, which the Mac reaches through the cable
// (usbmuxd) when the phone isn't on its network. A connection the CLI
// makes takes over from the player's own. False when it can't listen.
bool ak_dev_listen(int port, const char *hello, bool capture, char *err, size_t errcap);

// ak_dev_send sends a frame to the CLI (any thread): false when not
// connected (on the web, when the page has no asuka dev to send it to).
bool ak_dev_send(uint8_t kind, const void *p, size_t len);
bool ak_dev_connected(void);

// ak_dev_player is whether this run is asuka dev's player: it connected
// to the CLI, listens for it, or loads the modules a dev reload brings
// (the web's page).
bool ak_dev_player(void);

#if defined(AK_TEST)
// ak_dev_test_sending, when set, is called by ak_dev_send with the socket
// it read, before it writes (tests: a connection ending meanwhile).
extern void (*ak_dev_test_sending)(int sock);
#endif

#endif
