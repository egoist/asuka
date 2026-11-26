// ota.h: an over-the-air update in place of the build's code (ota.c).
#ifndef AK_ABI_OTA_H
#define AK_ABI_OTA_H

#include <stdbool.h>

typedef struct ak_module ak_module;

// ak_link_fingerprint is the build's fingerprint (core/link/link.c): what
// its native side gives code, which an update must have been made
// against; "" for a build that takes no updates.
extern const char *const ak_link_fingerprint;
// ak_link_update_feed and ak_link_update_key are the build's updates' feed
// and public key (base64), with its fingerprint.
extern const char *const ak_link_update_feed, *const ak_link_update_key;
// ak_link_update_version and ak_link_update_built are the app's version
// and when the build was made (seconds since 1970).
extern const char *const ak_link_update_version;
extern const long long ak_link_update_built;

// ak_ota_take is the update to run in place of the build's code, loaded
// (owned), or NULL: none, not this build's, or set aside (its last launch
// ended early, its module doesn't load).
ak_module *ak_ota_take(void);
// ak_ota_take_for is ak_ota_take for a build of fingerprint fp (tests).
ak_module *ak_ota_take_for(const char *fp);
// ak_ota_mounted says the update taken mounted: once it has run a while,
// its launch counts as good.
void ak_ota_mounted(void);
// ak_ota_failed says the update taken failed to start (its init threw):
// it's set aside, and the build's code runs.
void ak_ota_failed(void);
// ak_ota_running is the version of the update running ("" : the build's
// code).
const char *ak_ota_running(void);
// ak_ota_relaunch runs the app again, on the next turn: the update in
// place (one put there since it started), else the build's code.
void ak_ota_relaunch(void);
// ak_abi_relaunch (abi.c) runs m (owned; NULL: the build's code) in place
// of the app running.
int ak_abi_relaunch(ak_module *m);

#endif
