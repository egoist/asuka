// gesture.c: pinches and rotations (OnPinch, OnRotate) on Windows.
//
//   - A touch screen's two fingers: WM_GESTURE's zoom (the distance
//     between them, against where it began) and rotate (the angle since
//     it began), which the view asks for as it listens.
//   - A precision touchpad's pinch, which Windows sends as the wheel with
//     Ctrl held, as it does for apps that zoom with Ctrl and the wheel (a
//     browser): so is a mouse's Ctrl and wheel, 10% a notch. A pinch
//     begins with the first, and ends once none has come for a moment.
//   - A zoomable Scroll (MinZoom, MaxZoom) that doesn't listen for
//     pinches zooms its content with a touch screen's two fingers
//     (scroll.c zooms it with Ctrl and the wheel).
#include "nav.h"

#define PINCH_REST 250  // ms without the wheel that end a wheel's pinch

// two_finger is a pinch or rotate under way on a view (node.two_finger).
typedef struct {
	bool wheel;         // from the wheel (Ctrl)
	float scale;        // a wheel pinch's scale so far
	double start_dist;  // a zoom's distance between the fingers as it began
	POINT at;           // where, in the view (pixels)
} two_finger;

static void emit_pinch(node *n, int state, POINT at, float scale) {
	ak_event e = {.kind = AK_EVENT_PINCH};
	e.g.state = (uint8_t)state;
	e.g.x = wh_pt(n, at.x), e.g.y = wh_pt(n, at.y), e.g.scale = scale;
	wh_emit(n, &e);
}

// wh_allow_gestures has the view's window get zoom and rotate gestures
// (rotate is off by default), as it listens for pinches and rotations,
// and keeps the other gestures as they are.
void wh_allow_gestures(node *n) {
	if (!n->hwnd) return;
	GESTURECONFIG c[2] = {{GID_ZOOM, GC_ZOOM, 0}, {GID_ROTATE, 0, 0}};
	if (node_listening(n, AK_EVENT_ROTATE)) c[1].dwWant = GC_ROTATE;
	else c[1].dwBlock = GC_ROTATE;
	SetGestureConfig(n->hwnd, 0, 2, c, sizeof c[0]);
}

// wh_wheel_pinch takes Ctrl and the wheel over a view that listens for
// pinches: false for one that doesn't (the wheel goes on to its parent).
bool wh_wheel_pinch(node *n, HWND hwnd, int delta, LPARAM lp) {
	two_finger *f = n->two_finger;
	if (!node_listening(n, AK_EVENT_PINCH) || (f && !f->wheel)) return false;
	POINT at = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
	ScreenToClient(hwnd, &at);
	if (!f) {
		f = n->two_finger = wh_alloc(sizeof *f);
		f->wheel = true, f->scale = 1;
		emit_pinch(n, AK_GESTURE_STATE_BEGAN, at, 1);
	}
	f->at = at;
	f->scale *= (float)pow(1.1, (double)delta / WHEEL_DELTA);
	emit_pinch(n, AK_GESTURE_STATE_CHANGED, at, f->scale);
	SetTimer(hwnd, TIMER_PINCH, PINCH_REST, NULL);
	return true;
}

// wh_end_wheel_pinch ends a wheel's pinch once the wheel has rested.
void wh_end_wheel_pinch(node *n, HWND hwnd) {
	KillTimer(hwnd, TIMER_PINCH);
	two_finger *f = n->two_finger;
	if (!f || !f->wheel) return;
	n->two_finger = NULL;
	emit_pinch(n, AK_GESTURE_STATE_ENDED, f->at, f->scale);
	free(f);
}

// wh_touch_gesture handles a WM_GESTURE on a view: a zoom it listens for
// as a pinch, a rotate as a rotate. The others (and gestures on views
// that don't listen) go on to DefWindowProc, which hands them to the
// parent.
bool wh_touch_gesture(node *n, HWND hwnd, LPARAM lp) {
	GESTUREINFO gi = {.cbSize = sizeof gi};
	if (!GetGestureInfo((HGESTUREINFO)lp, &gi)) return false;
	POINT at = {gi.ptsLocation.x, gi.ptsLocation.y};
	ScreenToClient(hwnd, &at);
	int state = AK_GESTURE_STATE_CHANGED;
	if (gi.dwFlags & GF_BEGIN) state = AK_GESTURE_STATE_BEGAN;
	else if (gi.dwFlags & GF_END) state = AK_GESTURE_STATE_ENDED;
	ak_event e = {0};
	e.g.state = (uint8_t)state;
	e.g.x = wh_pt(n, at.x), e.g.y = wh_pt(n, at.y), e.g.scale = 1;
	switch (gi.dwID) {
	case GID_ZOOM: {
		bool zooms = !node_listening(n, AK_EVENT_PINCH) && wh_zoomable(n);
		if (!node_listening(n, AK_EVENT_PINCH) && !zooms) return false;
		double dist = (double)(uint32_t)gi.ullArguments;
		two_finger *f = n->two_finger;
		if (zooms) {
			// The content's zoom, from where it was as the fingers came down,
			// around the point between them.
			if (state == AK_GESTURE_STATE_BEGAN || !f) {
				if (f && f->wheel) KillTimer(hwnd, TIMER_PINCH);
				free(f);
				f = n->two_finger = wh_alloc(sizeof *f);
				f->start_dist = dist > 1 ? dist : 1;
				f->scale = wh_scroll_zoom(n);
				KillTimer(hwnd, TIMER_ZOOM);
			}
			wh_zoom_around(n, f->scale * (float)(dist / f->start_dist), wh_pt(n, at.x), wh_pt(n, at.y), true);
			if (state == AK_GESTURE_STATE_ENDED && (f = n->two_finger)) {
				free(f);
				n->two_finger = NULL;
			}
			break;
		}
		if (state == AK_GESTURE_STATE_BEGAN || !f) {
			if (f && f->wheel) KillTimer(hwnd, TIMER_PINCH);
			free(f);
			f = n->two_finger = wh_alloc(sizeof *f);
			f->start_dist = dist > 1 ? dist : 1;
			e.g.state = AK_GESTURE_STATE_BEGAN;
		}
		e.g.scale = (float)(dist / f->start_dist);
		if (state == AK_GESTURE_STATE_ENDED) {
			free(f);
			n->two_finger = NULL;
		}
		e.kind = AK_EVENT_PINCH;
		wh_emit(n, &e);
		break;
	}
	case GID_ROTATE:
		if (!node_listening(n, AK_EVENT_ROTATE)) return false;
		// The angle since it began (GID_ROTATE_ANGLE_FROM_ARGUMENT);
		// Windows's is counterclockwise, Asuka's clockwise.
		if (state != AK_GESTURE_STATE_BEGAN) e.g.rotation = -(float)((double)(uint32_t)gi.ullArguments / 65535 * 4 * M_PI - 2 * M_PI);
		e.kind = AK_EVENT_ROTATE;
		wh_emit(n, &e);
		break;
	default:
		return false;
	}
	CloseGestureInfoHandle((HGESTUREINFO)lp);
	return true;
}

// wh_from_touch reports whether the mouse message being handled came from
// a touch screen or a pen (Windows's signature in its extra info).
bool wh_from_touch(void) { return ((uint32_t)GetMessageExtraInfo() & 0xFFFFFF00u) == 0xFF515700u; }
