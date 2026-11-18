// devoverlay.c: asuka dev's error overlay (the DevError command, to the
// root: ops/deverror.go). Under asuka dev, an error nothing in the app
// caught, a module that didn't start and a build that failed show over
// the app, in the host's own views (React Native's LogBox): the app's
// tree may be the thing that's broken, so the overlay isn't built from it.
//
// The core decides what shows: a build that failed (asuka dev's latest)
// before an error of the app's, which the build that failed would have
// replaced; of the app's errors, the first, with how many came after it
// (the first is the one the others follow from). Dismiss hides what
// shows; Reload, offered for an app's error, starts the app again with
// none of its state (ak_dev_overlay_set_reload's). A build that compiles
// again clears its failure, and a module that loads the app's errors.
//
// What shows is sent to each root as it mounts and cleared before one
// goes, so a host's overlay never outlives the root it was sent to.
#include "ui/ui_int.h"
#include "rt/sched.h"

#include <stdio.h>

typedef struct {
	bool on;
	uint8_t flags;
	ak_buf title, message, source, details;
} report;

static struct {
	report build, app;
	uint32_t more;  // the app's errors after the first
	uint32_t seq;   // the app's error's number, which its source names
	bool sent;      // DevError went to the root, and DevErrorClear hasn't
	bool posted;    // a flush is posted
	void (*reload)(void);
} D;

static void put(ak_buf *b, ak_span s) {
	ak_buf_reset(b);
	ak_buf_put(b, s.p, s.len);
}

static ak_span span(const ak_buf *b) { return (ak_span){(const char *)b->data, (uint32_t)b->len}; }

static void report_set(report *r, uint8_t flags, ak_span title, ak_span message, ak_span source, ak_span details) {
	r->on = true;
	r->flags = flags;
	put(&r->title, title);
	put(&r->message, message);
	put(&r->source, source);
	put(&r->details, details);
}

static void report_free(report *r) {
	ak_buf_free(&r->title);
	ak_buf_free(&r->message);
	ak_buf_free(&r->source);
	ak_buf_free(&r->details);
	*r = (report){0};
}

// shown is what the overlay shows now (NULL: nothing).
static const report *shown(void) {
	if (D.build.on) return &D.build;
	if (D.app.on) return &D.app;
	return NULL;
}

static void flush_posted(void *ctx) {
	AK_UNUSED(ctx);
	D.posted = false;
	ak_ui_flush();
}

// send sends r what shows (or that nothing does) in its batch.
static void send(ak_ui_root *r) {
	if (!r || !r->el || r->disposed) return;
	const report *s = shown();
	if (!s) {
		if (D.sent) ak_batch_command(r->batch, r->el->id, AK_CMD_DEV_ERROR_CLEAR, NULL, 0);
		D.sent = false;
		return;
	}
	char title[160];
	ak_span t = span(&s->title);
	if (s == &D.app && D.more) {
		int n = snprintf(title, sizeof title, "%.*s (and %u more)", (int)t.len, t.p, D.more);
		t = (ak_span){title, (uint32_t)(n < (int)sizeof title ? n : (int)sizeof title - 1)};
	}
	ak_dev_error e = {s->flags, t, span(&s->message), span(&s->source), span(&s->details)};
	ak_buf *b = ak_batch_command_begin(r->batch, r->el->id, AK_CMD_DEV_ERROR);
	ak_append_dev_error(b, &e);
	ak_batch_command_end(r->batch);
	D.sent = true;
}

// changed sends the root what shows now, and has it flushed soon: an
// error may come from anywhere (a handler, an effect, a task), and the
// flush of what it came from may have been and gone.
static void changed(void) {
	if (!ak_ui.root) return;
	send(ak_ui.root);
	if (!D.posted) {
		D.posted = true;
		ak_post(flush_posted, NULL);
	}
}

uint32_t ak_dev_overlay_error(ak_span title, ak_span message, ak_span details, bool reload) {
	if (D.app.on) {
		D.more++;
	} else {
		report_set(&D.app, reload ? AK_DEV_ERROR_RELOAD : 0, title, message, AK_STR(""), details);
		D.seq++;
	}
	changed();
	return D.seq;
}

void ak_dev_overlay_source(uint32_t seq, ak_span source, ak_span details) {
	if (!D.app.on || seq != D.seq) return;  // dismissed, or another's
	put(&D.app.source, source);
	if (details.len) put(&D.app.details, details);
	changed();
}

void ak_dev_overlay_build(ak_span message, ak_span source, ak_span details) {
	report_set(&D.build, AK_DEV_ERROR_BUILD, AK_STR("Build failed"), message, source, details);
	changed();
}

void ak_dev_overlay_fixed(void) {
	if (!D.build.on) return;
	D.build.on = false;
	changed();
}

void ak_dev_overlay_clear_app(void) {
	D.more = 0;
	if (!D.app.on) return;
	D.app.on = false;
	changed();
}

void ak_dev_overlay_set_reload(void (*reload)(void)) { D.reload = reload; }

bool ak_dev_overlay_showing(void) { return shown() != NULL; }

void ak_dev_overlay_action(int64_t action) {
	const report *s = shown();
	if (!s) return;
	if (action == AK_DEV_ERROR_ACTION_RELOAD) {
		if (!(s->flags & AK_DEV_ERROR_RELOAD) || !D.reload) return;  // not offered
		// The app starts again: its errors go with the app that made
		// them (a module that fails again shows its own).
		D.more = 0;
		D.app.on = false;
		D.reload();
		changed();
		return;
	}
	if (action != AK_DEV_ERROR_ACTION_DISMISS) return;
	if (s == &D.build) {
		D.build.on = false;
	} else {
		D.app.on = false;
		D.more = 0;
	}
	changed();
}

void ak_dev_overlay_mounted(ak_ui_root *r) {
	D.sent = false;  // a new root: the old one's overlay went with it
	if (shown()) send(r);
}

void ak_dev_overlay_unmount(ak_ui_root *r) {
	if (D.sent && r->el) ak_batch_command(r->batch, r->el->id, AK_CMD_DEV_ERROR_CLEAR, NULL, 0);
	D.sent = false;
}

void ak_dev_overlay_reset(void) {
	report_free(&D.build);
	report_free(&D.app);
	D.more = D.seq = 0;
	D.sent = false;
	D.reload = NULL;
}
