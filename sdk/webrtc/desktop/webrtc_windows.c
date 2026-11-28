// platform/webrtc's VideoView on Windows: an empty child window, as
// there are no tracks to show here (webrtc.c: data channels only).
#include "asuka_windows.h"
#include "webrtc.asuka.h"

#include <stdlib.h>

static bool handle(void *ctx, HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT *result) {
	(void)ctx, (void)hwnd, (void)msg, (void)wp, (void)lp, (void)result;
	return false;
}

static void set_track(webrtc_VideoView *self, const webrtc_Track *value) { (void)self, (void)value; }
static void set_mirror(webrtc_VideoView *self, bool value) { (void)self, (void)value; }
static void set_fit(webrtc_VideoView *self, webrtc_Fit value) { (void)self, (void)value; }

static const webrtc_VideoView_vtable vtable = {.set_track = set_track, .set_mirror = set_mirror, .set_fit = set_fit};

webrtc_VideoView *webrtc_make_VideoView(webrtc_VideoView_events events) {
	webrtc_VideoView *v = calloc(1, sizeof *v);
	if (!v) return NULL;
	v->vt = &vtable;
	v->widget = asuka_win_window(0, L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, (HWND)events.parent, handle, NULL);
	if (!v->widget) {
		free(v);
		return NULL;
	}
	return v;
}
