// place_test.c: ak_place, where an overlay goes next to its anchor
// (Floating UI's computePosition with offset, flip, shift and size).
#include "uih.h"

// A 400x800 window, the anchor a 100x40 button.
static const ak_place_rect window = {0, 0, 400, 800};

static ak_place_opts opts(int side, int align) {
	return (ak_place_opts){.side = side, .align = align, .offset = 4, .gap = 8, .flip = true, .has_anchor = true};
}

TEST(below_and_aligned) {
	ak_place_rect a = {100, 100, 100, 40};
	ak_place_opts o = opts(AK_SIDE_BOTTOM, AK_PLACE_ALIGN_START);
	ak_place_result r = ak_place(a, 150, 200, window, &o);
	CHECK_EQ_INT(r.side, AK_SIDE_BOTTOM);
	CHECK_EQ_F(r.x, 100);
	CHECK_EQ_F(r.y, 144);
	o.align = AK_PLACE_ALIGN_CENTER;
	r = ak_place(a, 150, 200, window, &o);
	CHECK_EQ_F(r.x, 75);
	o.align = AK_PLACE_ALIGN_END;
	r = ak_place(a, 150, 200, window, &o);
	CHECK_EQ_F(r.x, 50);
	// The room below: the window's bottom less the gap, from under the offset.
	CHECK_EQ_F(r.avail_h, 800 - 8 - 144);
	CHECK_EQ_F(r.avail_w, 400 - 16);
	// The anchor's middle along its top edge: an arrow's place.
	CHECK_EQ_F(r.arrow, 150 - 50);
}

TEST(flips_when_there_is_no_room) {
	ak_place_rect a = {100, 700, 100, 40};
	ak_place_opts o = opts(AK_SIDE_BOTTOM, AK_PLACE_ALIGN_START);
	ak_place_result r = ak_place(a, 150, 200, window, &o);
	CHECK_EQ_INT(r.side, AK_SIDE_TOP);
	CHECK_EQ_F(r.y, 700 - 4 - 200);
	// Without flip it stays, its room what's left below.
	o.flip = false;
	r = ak_place(a, 150, 200, window, &o);
	CHECK_EQ_INT(r.side, AK_SIDE_BOTTOM);
	CHECK_EQ_F(r.avail_h, 800 - 8 - 744);
	// Where neither side fits, it keeps the side with more room.
	o.flip = true;
	ak_place_rect mid = {100, 380, 100, 40};
	r = ak_place(mid, 150, 600, window, &o);
	CHECK_EQ_INT(r.side, AK_SIDE_BOTTOM);
}

TEST(shifts_into_the_bounds) {
	ak_place_rect a = {350, 100, 40, 40};
	ak_place_opts o = opts(AK_SIDE_BOTTOM, AK_PLACE_ALIGN_START);
	ak_place_result r = ak_place(a, 150, 100, window, &o);
	CHECK_EQ_F(r.x, 400 - 8 - 150);
	// The arrow still points at the anchor's middle.
	CHECK_EQ_F(r.arrow, 370 - r.x);
	// Wider than the bounds: its start in them.
	r = ak_place(a, 500, 100, window, &o);
	CHECK_EQ_F(r.x, 8);
}

TEST(sides_mirror_right_to_left) {
	ak_place_rect a = {100, 100, 100, 40};
	ak_place_opts o = opts(AK_SIDE_END, AK_PLACE_ALIGN_START);
	ak_place_result r = ak_place(a, 80, 40, window, &o);
	CHECK_EQ_F(r.x, 204);  // after it, on the right
	CHECK_EQ_INT(r.side, AK_SIDE_END);
	o.rtl = true;
	r = ak_place(a, 80, 40, window, &o);
	CHECK_EQ_F(r.x, 100 - 4 - 80);  // on the left
	CHECK_EQ_INT(r.side, AK_SIDE_END);
	// Aligned to its start: its right edge, right to left.
	o.side = AK_SIDE_BOTTOM;
	r = ak_place(a, 80, 40, window, &o);
	CHECK_EQ_F(r.x, 200 - 80);
	// An end side that flips is reported as the start.
	o.rtl = false;
	o.side = AK_SIDE_END;
	ak_place_rect edge = {300, 100, 90, 40};
	r = ak_place(edge, 80, 40, window, &o);
	CHECK_EQ_INT(r.side, AK_SIDE_START);
}

TEST(without_an_anchor) {
	ak_place_opts o = {.side = AK_SIDE_CENTER, .align = AK_PLACE_ALIGN_CENTER, .gap = 0};
	ak_place_result r = ak_place((ak_place_rect){0}, 200, 100, window, &o);
	CHECK_EQ_F(r.x, 100);
	CHECK_EQ_F(r.y, 350);
	o.side = AK_SIDE_BOTTOM;
	o.offset = 16;
	r = ak_place((ak_place_rect){0}, 200, 100, window, &o);
	CHECK_EQ_F(r.y, 800 - 100 - 16);
	CHECK_EQ_F(r.x, 100);
	// The safe area's bounds.
	ak_place_rect safe = {0, 47, 400, 800 - 47 - 34};
	o.side = AK_SIDE_TOP;
	o.offset = 0;
	r = ak_place((ak_place_rect){0}, 200, 100, safe, &o);
	CHECK_EQ_F(r.y, 47);
}

TEST(placing_allocates_nothing) {
	ak_place_rect a = {100, 700, 100, 40};
	ak_place_opts o = opts(AK_SIDE_BOTTOM, AK_PLACE_ALIGN_CENTER);
	int64_t before = ak_alloc_count;
	for (int i = 0; i < 100; i++) (void)ak_place(a, 150 + (float)i, 200, window, &o);
	CHECK_EQ_INT(ak_alloc_count - before, 0);
}

T_MAIN()
