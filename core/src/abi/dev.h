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
//   player → CLI:  3 log (u8 fd: 1 out, 2 err; then the text)
//                  4 hello (JSON: the platform, the device)
//                  5 reloaded (u32 state cells kept, u32 microseconds,
//                    u8 flags: AK_DEV_RESTARTED for a restart's)
//                  6 error (text: a module that didn't load, an uncaught error)
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
};

// A reloaded frame's flags.
enum { AK_DEV_RESTARTED = 1 };

// ak_dev_set_reload sets what a module that arrives is given to, on the
// main thread (the ABI's reload: keep state, swap the module, mount);
// fresh for a restart's, which keeps no state.
void ak_dev_set_reload(void (*reload)(const uint8_t *module, size_t len, bool fresh));

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

// ak_dev_send sends a frame to the CLI (any thread); a no-op when not
// connected.
void ak_dev_send(uint8_t kind, const void *p, size_t len);
bool ak_dev_connected(void);

// ak_dev_player is whether this run is asuka dev's player: it connected
// to the CLI, listens for it, or loads the modules a dev reload brings
// (the web's page).
bool ak_dev_player(void);

#endif
