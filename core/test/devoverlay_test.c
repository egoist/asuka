// asuka dev's error overlay (ui/devoverlay.c): what the DevError command
// to the root shows, on the fake host, as errors, a build that fails, the
// host's Dismiss and Reload and roots that come and go change it.
#include "uih.h"

static ak_val plain_app(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_view *col = ak_ui_new(AK_EL_COLUMN);
	add(col, text("Hello"));
	return (ak_val){.p = col};
}

static bool shows(ak_fake *f, const char *title, const char *message, const char *source, const char *details, uint8_t flags) {
	ak_dev_error e;
	if (!ak_fake_dev_error(f, &e)) {
		T_FAIL("no error overlay; want %s", title);
		return false;
	}
	bool ok = ak_span_eqc(e.title, title) && ak_span_eqc(e.message, message) && ak_span_eqc(e.source, source) &&
	          ak_span_eqc(e.details, details) && e.flags == flags;
	if (!ok)
		T_FAIL("the overlay shows %u %.*s | %.*s | %.*s | %.*s; want %u %s | %s | %s | %s", e.flags, (int)e.title.len, e.title.p,
		       (int)e.message.len, e.message.p, (int)e.source.len, e.source.p, (int)e.details.len, e.details.p, flags, title, message,
		       source, details);
	return ok;
}

static bool none(ak_fake *f) {
	ak_dev_error e;
	return !ak_fake_dev_error(f, &e);
}

static void no_misuse(ak_fake *f) {
	if (f->error.len) T_FAIL("the fake host: %.*s", (int)f->error.len, (const char *)f->error.data);
}

static int reloads;

// The app's Reload: it starts again, as the ABI's does (a new root).
static void reload(void) {
	reloads++;
	ak_ui_mount(NULL);
}

// An error of the app's shows with its stack, and Reload; the ones after
// it are counted; asuka dev's source for it fills it in (not another's);
// Dismiss clears it all.
TEST(overlay_app_errors) {
	ak_uitest *a = mount(plain_app, NULL);
	ak_fake *f = ak_uitest_host(a);
	ak_dev_overlay_reset();
	CHECK(none(f));
	uint32_t seq = ak_dev_overlay_error(AK_STR("Uncaught error"), AK_STR("boom"), AK_STR("press (app.asuka:4)"), true);
	ak_uitest_settle(a);  // the flush it posted
	CHECK(shows(f, "Uncaught error", "boom", "", "press (app.asuka:4)", AK_DEV_ERROR_RELOAD));
	uint32_t later = ak_dev_overlay_error(AK_STR("Uncaught error"), AK_STR("again"), AK_STR(""), true);
	CHECK_EQ_INT(later, seq);  // still the first's
	ak_uitest_settle(a);
	CHECK(shows(f, "Uncaught error (and 1 more)", "boom", "", "press (app.asuka:4)", AK_DEV_ERROR_RELOAD));
	ak_dev_overlay_source(seq + 1, AK_STR("not this one's"), AK_STR(""));
	ak_dev_overlay_source(seq, AK_STR("4 | on press { boom() }"), AK_STR("press (app.asuka:4)\n  at app.asuka"));
	ak_uitest_settle(a);
	CHECK(shows(f, "Uncaught error (and 1 more)", "boom", "4 | on press { boom() }", "press (app.asuka:4)\n  at app.asuka", AK_DEV_ERROR_RELOAD));
	CHECK(ak_fake_dev_error_press(f, AK_DEV_ERROR_ACTION_DISMISS));
	ak_uitest_settle(a);
	CHECK(none(f));
	CHECK(!ak_dev_overlay_showing());
	// Dismissed, it's forgotten: the next error is the first again.
	ak_dev_overlay_error(AK_STR("Uncaught error"), AK_STR("third"), AK_STR(""), true);
	ak_uitest_settle(a);
	CHECK(shows(f, "Uncaught error", "third", "", "", AK_DEV_ERROR_RELOAD));
	// A new module: the old one's errors go.
	ak_dev_overlay_clear_app();
	ak_uitest_settle(a);
	CHECK(none(f));
	no_misuse(f);
	ak_dev_overlay_reset();
	done(a);
}

// A build that failed shows before the app's error, offers no Reload, and
// goes when the app builds again, the app's error showing again; a
// failure dismissed stays hidden until the next one.
TEST(overlay_build_failures) {
	ak_uitest *a = mount(plain_app, NULL);
	ak_fake *f = ak_uitest_host(a);
	ak_dev_overlay_reset();
	ak_dev_overlay_error(AK_STR("Uncaught error"), AK_STR("boom"), AK_STR(""), true);
	ak_dev_overlay_build(AK_STR("app.asuka:3:5: count isn't declared"), AK_STR("3 | count += 1"), AK_STR(""));
	ak_uitest_settle(a);
	CHECK(shows(f, "Build failed", "app.asuka:3:5: count isn't declared", "3 | count += 1", "", AK_DEV_ERROR_BUILD));
	CHECK(!ak_fake_dev_error_press(f, AK_DEV_ERROR_ACTION_RELOAD));  // not offered
	ak_dev_overlay_fixed();
	ak_uitest_settle(a);
	CHECK(shows(f, "Uncaught error", "boom", "", "", AK_DEV_ERROR_RELOAD));
	ak_dev_overlay_build(AK_STR("again"), AK_STR(""), AK_STR("2 more errors"));
	ak_uitest_settle(a);
	CHECK(shows(f, "Build failed", "again", "", "2 more errors", AK_DEV_ERROR_BUILD));
	CHECK(ak_fake_dev_error_press(f, AK_DEV_ERROR_ACTION_DISMISS));
	ak_uitest_settle(a);
	CHECK(shows(f, "Uncaught error", "boom", "", "", AK_DEV_ERROR_RELOAD));  // what was under it
	ak_dev_overlay_fixed();  // dismissed already: nothing changes
	ak_uitest_settle(a);
	CHECK(shows(f, "Uncaught error", "boom", "", "", AK_DEV_ERROR_RELOAD));
	no_misuse(f);
	ak_dev_overlay_reset();
	done(a);
}

// A root that goes clears the overlay first, and the next root gets it;
// Reload starts the app again and clears the app's error.
TEST(overlay_roots_and_reload) {
	ak_uitest *a = mount(plain_app, NULL);
	ak_fake *f = ak_uitest_host(a);
	ak_dev_overlay_reset();
	ak_dev_overlay_set_reload(reload);
	reloads = 0;
	ak_dev_overlay_error(AK_STR("Uncaught error"), AK_STR("boom"), AK_STR(""), true);
	ak_uitest_settle(a);
	ak_id first = root(a)->id;
	CHECK_EQ_INT(f->dev_error_on, first);
	// A reload's root: the old one's overlay cleared before it went (the
	// fake host fails a root destroyed under it), the new one's sent.
	ak_ui_unmount();
	ak_ui_mount(NULL);
	ak_uitest_settle(a);
	no_misuse(f);
	CHECK(f->dev_error_on && f->dev_error_on != first);
	CHECK(shows(f, "Uncaught error", "boom", "", "", AK_DEV_ERROR_RELOAD));
	ak_fake_reset(f);
	CHECK(ak_fake_dev_error_press(f, AK_DEV_ERROR_ACTION_RELOAD));
	ak_uitest_settle(a);
	CHECK_EQ_INT(reloads, 1);
	CHECK(none(f));
	// Cleared before its root went, and not sent again to the next.
	ak_fake_index_vec sent = {0};
	ak_fake_commands(f, AK_CMD_DEV_ERROR, &sent);
	CHECK_EQ_INT(sent.len, 0);
	ak_vec_free(&sent);
	no_misuse(f);
	ak_dev_overlay_reset();
	done(a);
}

// A module that didn't load, or whose init threw, leaves no app: the
// overlay shows over an empty root. Without Reload, the host's Reload does
// nothing.
TEST(overlay_without_reload) {
	ak_uitest *a = mount(plain_app, NULL);
	ak_fake *f = ak_uitest_host(a);
	ak_dev_overlay_reset();
	ak_dev_overlay_set_reload(reload);
	reloads = 0;
	ak_ui_unmount();
	ak_dev_overlay_error(AK_STR("The app didn't load"), AK_STR("bytecode: version 3, want 4"), AK_STR(""), false);
	ak_ui_mount(NULL);
	ak_uitest_settle(a);
	CHECK(shows(f, "The app didn't load", "bytecode: version 3, want 4", "", "", 0));
	ak_fake_dispatch(f, &(ak_event){.id = f->dev_error_on, .kind = AK_EVENT_DEV_ERROR_ACTION, .i = AK_DEV_ERROR_ACTION_RELOAD});
	ak_uitest_settle(a);
	CHECK_EQ_INT(reloads, 0);
	CHECK(shows(f, "The app didn't load", "bytecode: version 3, want 4", "", "", 0));
	no_misuse(f);
	ak_dev_overlay_reset();
	done(a);
}

T_MAIN()
