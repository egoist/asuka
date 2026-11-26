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
//   player → CLI:  3 log (u8 fd: 1 out, 2 err; then the text)
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
//
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
};

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
