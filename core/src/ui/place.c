// place.c: where an Overlay goes next to its anchor (Floating UI's
// computePosition with its offset, flip, shift and size middleware, in
// one pure function): what the core sends as the overlay's frame, the
// same on every host.
#include "ui/ui_ext.h"

static float maxf(float a, float b) { return a > b ? a : b; }
static float minf(float a, float b) { return a < b ? a : b; }

// The physical sides (a Side's start and end resolved by direction).
enum { P_BOTTOM, P_TOP, P_LEFT, P_RIGHT, P_CENTER };

static int physical(int side, bool rtl) {
	switch (side) {
	case AK_SIDE_TOP: return P_TOP;
	case AK_SIDE_START: return rtl ? P_RIGHT : P_LEFT;
	case AK_SIDE_END: return rtl ? P_LEFT : P_RIGHT;
	case AK_SIDE_CENTER: return P_CENTER;
	}
	return P_BOTTOM;
}

static int logical(int p, bool rtl) {
	switch (p) {
	case P_TOP: return AK_SIDE_TOP;
	case P_LEFT: return rtl ? AK_SIDE_END : AK_SIDE_START;
	case P_RIGHT: return rtl ? AK_SIDE_START : AK_SIDE_END;
	case P_CENTER: return AK_SIDE_CENTER;
	}
	return AK_SIDE_BOTTOM;
}

static int opposite(int p) {
	switch (p) {
	case P_BOTTOM: return P_TOP;
	case P_TOP: return P_BOTTOM;
	case P_LEFT: return P_RIGHT;
	case P_RIGHT: return P_LEFT;
	}
	return p;
}

static bool vertical(int p) { return p == P_BOTTOM || p == P_TOP; }

// room is the space on side p of the anchor a within the bounds b (each
// already inset by the gap), past the offset.
static float room(int p, ak_place_rect a, ak_place_rect b, float offset) {
	switch (p) {
	case P_BOTTOM: return (b.y + b.h) - (a.y + a.h + offset);
	case P_TOP: return (a.y - offset) - b.y;
	case P_LEFT: return (a.x - offset) - b.x;
	case P_RIGHT: return (b.x + b.w) - (a.x + a.w + offset);
	}
	return vertical(p) ? b.h : b.w;
}

// cross places a length on the cross axis of an anchor's span [s, s+n]:
// at its start, center or end (start the reading direction's on a
// horizontal axis), then moved by off toward the end.
static float cross(float s, float n, float len, int align, float off, bool horizontal, bool rtl) {
	bool flipx = horizontal && rtl;
	float v;
	switch (align) {
	case AK_PLACE_ALIGN_START: v = flipx ? s + n - len : s; break;
	case AK_PLACE_ALIGN_END: v = flipx ? s : s + n - len; break;
	default: v = s + (n - len) / 2; break;  // center, auto, stretch, baseline
	}
	return v + (flipx ? -off : off);
}

ak_place_result ak_place(ak_place_rect anchor, float w, float h, ak_place_rect bounds, const ak_place_opts *o) {
	ak_place_result r = {0};
	float gap = maxf(o->gap, 0);
	ak_place_rect b = {bounds.x + gap, bounds.y + gap, maxf(bounds.w - 2 * gap, 0), maxf(bounds.h - 2 * gap, 0)};
	int p = physical(o->side, o->rtl);
	if (!o->has_anchor) {
		// In the bounds: against the side's edge (offset in from it), or
		// in the middle; aligned along it.
		r.side = o->side;
		r.avail_w = b.w, r.avail_h = b.h;
		switch (p) {
		case P_BOTTOM: r.y = b.y + b.h - h - o->offset; break;
		case P_TOP: r.y = b.y + o->offset; break;
		case P_LEFT: r.x = b.x + o->offset; break;
		case P_RIGHT: r.x = b.x + b.w - w - o->offset; break;
		default: r.x = b.x + (b.w - w) / 2, r.y = b.y + (b.h - h) / 2; break;
		}
		if (vertical(p)) r.x = cross(b.x, b.w, w, o->align, o->align_offset, true, o->rtl);
		else if (p != P_CENTER) r.y = cross(b.y, b.h, h, o->align, o->align_offset, false, o->rtl);
		r.x = minf(maxf(r.x, b.x), maxf(b.x + b.w - w, b.x));
		r.y = minf(maxf(r.y, b.y), maxf(b.y + b.h - h, b.y));
		r.arrow = vertical(p) ? w / 2 : h / 2;
		return r;
	}
	// Flip: to the opposite side when it doesn't fit on its own and there's
	// more room there.
	if (o->flip && p != P_CENTER) {
		float need = vertical(p) ? h : w;
		float here = room(p, anchor, b, o->offset), there = room(opposite(p), anchor, b, o->offset);
		if (need > here && there > here) p = opposite(p);
	}
	r.side = logical(p, o->rtl);
	switch (p) {
	case P_BOTTOM: r.y = anchor.y + anchor.h + o->offset; break;
	case P_TOP: r.y = anchor.y - h - o->offset; break;
	case P_LEFT: r.x = anchor.x - w - o->offset; break;
	case P_RIGHT: r.x = anchor.x + anchor.w + o->offset; break;
	default:
		r.x = anchor.x + (anchor.w - w) / 2;
		r.y = anchor.y + (anchor.h - h) / 2;
		break;
	}
	if (vertical(p)) r.x = cross(anchor.x, anchor.w, w, o->align, o->align_offset, true, o->rtl);
	else if (p != P_CENTER) r.y = cross(anchor.y, anchor.h, h, o->align, o->align_offset, false, o->rtl);
	// Shift: along the side, kept in the bounds (its start in them when it's
	// bigger than they are); a centered one on both axes.
	if (vertical(p) || p == P_CENTER) r.x = minf(maxf(r.x, b.x), maxf(b.x + b.w - w, b.x));
	if (!vertical(p) || p == P_CENTER) r.y = minf(maxf(r.y, b.y), maxf(b.y + b.h - h, b.y));
	// Size: the room on its side, and the bounds across.
	if (vertical(p)) {
		r.avail_w = b.w;
		r.avail_h = maxf(room(p, anchor, b, o->offset), 0);
		r.arrow = minf(maxf(anchor.x + anchor.w / 2 - r.x, 0), w);
	} else if (p == P_CENTER) {
		r.avail_w = b.w, r.avail_h = b.h;
		r.arrow = w / 2;
	} else {
		r.avail_w = maxf(room(p, anchor, b, o->offset), 0);
		r.avail_h = b.h;
		r.arrow = minf(maxf(anchor.y + anchor.h / 2 - r.y, 0), h);
	}
	return r;
}
