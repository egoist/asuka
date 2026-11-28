// platform/webrtc's VideoView on Linux: an empty box, as there are no
// tracks to show here (webrtc.c: data channels only).
#include "asuka_linux.h"
#include "webrtc.asuka.h"

#include <stdlib.h>

static void set_track(webrtc_VideoView *self, const webrtc_Track *value) { (void)self, (void)value; }
static void set_mirror(webrtc_VideoView *self, bool value) { (void)self, (void)value; }
static void set_fit(webrtc_VideoView *self, webrtc_Fit value) { (void)self, (void)value; }

static const webrtc_VideoView_vtable vtable = {.set_track = set_track, .set_mirror = set_mirror, .set_fit = set_fit};

webrtc_VideoView *webrtc_make_VideoView(webrtc_VideoView_events events) {
	(void)events;
	if (!asuka_linux()) return NULL;
	webrtc_VideoView *v = calloc(1, sizeof *v);
	if (!v) return NULL;
	v->vt = &vtable;
	v->widget = G(gtk_box_new)(GTK_ORIENTATION_VERTICAL, 0);
	return v;
}
