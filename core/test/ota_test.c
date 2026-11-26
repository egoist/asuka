// Over-the-air updates at launch (core/src/abi/ota.c): which update runs
// in place of the build's code, and which is set aside. A build with no
// fingerprint takes none; an update of another build's fingerprint is
// left as it is; one whose module doesn't load, or whose last two
// launches ended early (their launching marks left behind), is set aside,
// its files renamed .bad.
#define _POSIX_C_SOURCE 200809L
#include "t.h"
#include "abi/ota.h"
#include "std/std.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static char updates[1100];

static void put(const char *name, const char *text) {
	char p[1200];
	snprintf(p, sizeof p, "%s/%s", updates, name);
	FILE *f = fopen(p, "wb");
	fputs(text, f);
	fclose(f);
}

static bool there(const char *name) {
	char p[1200];
	struct stat st;
	snprintf(p, sizeof p, "%s/%s", updates, name);
	return stat(p, &st) == 0;
}

static void fresh_dir(void) {
	char *app = ak_fs_temp_dir();
	ak_fs_set_app_dir(app);
	snprintf(updates, sizeof updates, "%s/Updates", app);
	mkdir(updates, 0755);
	free(app);
}

TEST(ota_none_or_not_this_builds) {
	fresh_dir();
	CHECK(ak_ota_take_for("abc") == NULL);  // none in place
	put("current", "1.0.0+1\nother\n1.0.0+1.asukab\n");
	put("1.0.0+1.asukab", "not a module");
	CHECK(ak_ota_take_for("") == NULL);     // a build without updates
	CHECK(ak_ota_take_for("abc") == NULL);  // another build's: left alone
	CHECK(there("current") && there("1.0.0+1.asukab") && !there("launching"));
}

TEST(ota_set_aside_when_it_doesnt_load) {
	fresh_dir();
	put("current", "1.0.0+2\nabc\n1.0.0+2.asukab\n");
	put("1.0.0+2.asukab", "not a module");
	CHECK(ak_ota_take_for("abc") == NULL);
	CHECK(!there("current") && !there("1.0.0+2.asukab") && !there("launching"));
	CHECK(there("1.0.0+2.asukab.bad") && there("1.0.0+2.asukab.current.bad"));
}

TEST(ota_set_aside_after_a_launch_that_ended_early) {
	fresh_dir();
	put("current", "1.0.0+3\nabc\n1.0.0+3.asukab\n");
	put("1.0.0+3.asukab", "not a module");
	// One launch that ended early: the module is tried again (it doesn't
	// load here, so it's set aside for that instead; the mark says one).
	put("launching", "1.0.0+3\n1\n");
	CHECK(ak_ota_take_for("abc") == NULL);
	CHECK(!there("current") && !there("launching") && there("1.0.0+3.asukab.bad"));
	// A file name that leaves the directory isn't read.
	put("current", "1.0.0+4\nabc\n../x\n");
	CHECK(ak_ota_take_for("abc") == NULL);
	CHECK(there("current"));
}

T_MAIN()
