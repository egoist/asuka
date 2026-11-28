// The text and look keys a style sets beyond a view's box: letter
// spacing, line height, font variants, truncation, shrinking to fit,
// text shadows, per-corner radii, per-side borders and their style, and
// radial and conic gradients (style.c): what reaches the host, and how
// a text's measurement follows them.
#include "uih.h"

// The prelude's structs, in types.asuka's field order.
static const uint8_t corners_kinds[] = {AK_K_FLOAT, AK_K_FLOAT, AK_K_FLOAT, AK_K_FLOAT, AK_K_FLOAT, AK_K_FLOAT, AK_K_FLOAT, AK_K_FLOAT};
static const ak_shape corners_shape = {"Corners", 8, corners_kinds, no_shapes, NULL, 0, NULL, false, NULL};
static const uint8_t shadow_kinds[] = {AK_K_INT, AK_K_FLOAT, AK_K_FLOAT, AK_K_FLOAT};
static const ak_shape shadow_shape = {"Shadow", 4, shadow_kinds, no_shapes, NULL, 0, NULL, false, NULL};
static const uint8_t stop_kinds[] = {AK_K_INT, AK_K_FLOAT};
static const ak_shape stop_shape = {"Stop", 2, stop_kinds, no_shapes, NULL, 0, NULL, false, NULL};
static const uint8_t gradient_kinds[] = {AK_K_REF, AK_K_FLOAT, AK_K_FLOAT, AK_K_FLOAT, AK_K_FLOAT, AK_K_INT, AK_K_FLOAT, AK_K_FLOAT, AK_K_FLOAT, AK_K_FLOAT};
static const ak_shape gradient_shape = {"Gradient", 10, gradient_kinds, no_shapes, NULL, 0, NULL, false, NULL};

static ak_val corners(float tl, float tr, float br, float bl) {
	ak_struct *s = ak_struct_new(&corners_shape);
	s->f[0].f = tl, s->f[1].f = tr, s->f[2].f = br, s->f[3].f = bl;
	return (ak_val){.p = s};
}

// font is a Font of size, its variants italic, monospaced digits and
// small capitals.
static ak_val font(float size, bool italic, bool digits, bool caps) {
	ak_struct *f = ak_struct_new(&ak_ui_shape_font);
	f->f[1].f = size;
	f->f[3].p = ak_str_c("");
	f->f[4].i = italic, f->f[5].i = digits, f->f[6].i = caps;
	return (ak_val){.p = f};
}

static ak_fake *host(ak_uitest *a) { return ak_uitest_host(a); }

static void edges_are(const ak_fake_view *v, ak_prop p, float t, float r, float b, float l) {
	const ak_value *x = ak_fake_prop_of(v, p);
	if (!x) {
		T_FAIL("no %u", p);
		return;
	}
	if (x->edges[0] != t || x->edges[1] != r || x->edges[2] != b || x->edges[3] != l)
		T_FAIL("edges %g %g %g %g, want %g %g %g %g", x->edges[0], x->edges[1], x->edges[2], x->edges[3], t, r, b, l);
}

// ---- Letter spacing and line height ----

static void *spacing;  // a cell: the first text's letter spacing

static ak_val read_spacing(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	return ak_vm_cell_get(spacing, true);
}

static ak_val spacing_app(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_view *col = ak_ui_new(AK_EL_COLUMN);
	set_i(col, AK_KEY_ALIGN, 1);  // start: each text its own width
	ak_view *t = text("Hello");
	bind(t, AK_KEY_LETTER_SPACING, read_spacing, NULL);
	set_f(t, AK_KEY_LINE_HEIGHT, 30);
	add(col, t);
	add(col, text("Hello"));
	// The first's spacing and line height again: measured once for both.
	t = text("Hello");
	set_f(t, AK_KEY_LETTER_SPACING, 2);
	set_f(t, AK_KEY_LINE_HEIGHT, 30);
	add(col, t);
	return (ak_val){.p = col};
}

// A text's letter spacing and line height reach its view, and its size
// follows them: they're measured with it, and a text with other ones is
// another measurement.
TEST(letter_spacing_and_line_height) {
	spacing = ak_vm_cell_new((ak_val){.f = 2}, AK_K_FLOAT, NULL).p;
	ak_uitest *a = mount(spacing_app, NULL);
	ak_fake_view *spaced = nth(a, AK_KIND_TEXT, 0), *plain = nth(a, AK_KIND_TEXT, 1), *again = nth(a, AK_KIND_TEXT, 2);
	REQUIRE(spaced && plain && again);
	CHECK_EQ_F(ak_fake_float(spaced, AK_PROP_LETTER_SPACING), 2);
	CHECK_EQ_F(ak_fake_float(spaced, AK_PROP_LINE_HEIGHT), 30);
	CHECK(!has_prop(plain, AK_PROP_LETTER_SPACING) && !has_prop(plain, AK_PROP_LINE_HEIGHT));
	// Five characters of 8 and 2 after each, on a line of 30.
	CHECK_EQ_F(spaced->frame[2], 50);
	CHECK_EQ_F(spaced->frame[3], 30);
	CHECK_EQ_F(plain->frame[2], 40);
	CHECK_EQ_F(plain->frame[3], 20);
	CHECK_EQ_F(again->frame[2], 50);
	// Two of each (layout asks under two constraints): the third is the
	// first's measurements.
	CHECK_EQ_INT(host(a)->measures, 4);
	// The spacing changes: the text is measured again, at its new size.
	ak_vm_cell_set(spacing, (ak_val){.f = 4});
	ak_ui_flush();
	CHECK_EQ_F(ak_fake_float(spaced, AK_PROP_LETTER_SPACING), 4);
	CHECK_EQ_F(spaced->frame[2], 60);
	CHECK_EQ_INT(host(a)->measures, 6);
	done(a);
	ak_release(spacing);
}

// ---- Shrinking to fit ----

static ak_val shrink_app(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_view *col = ak_ui_new(AK_EL_COLUMN);
	set_i(col, AK_KEY_ALIGN, 1);
	set_i(col, AK_KEY_WIDTH, ak_length_pt(80));
	ak_view *t = text("ABCDEFGHIJKLMNOPQRST");  // 160 points at its size
	set_f(t, AK_KEY_MINIMUM_SCALE, 0.25);
	add(col, t);
	t = text("ABCDEFGHIJKLMNOPQRST");
	set_f(t, AK_KEY_MINIMUM_SCALE, 0.75);  // it can't shrink enough
	add(col, t);
	t = text("ABCDEFGHIJKLMNOPQRST");
	set_f(t, AK_KEY_MINIMUM_SCALE, 0.5);
	set_i(t, AK_KEY_LINES, 2);  // two lines hold it at its size
	add(col, t);
	add(col, text("ABCDEFGHIJKLMNOPQRST"));
	return (ak_val){.p = col};
}

// A text with a minimum scale is measured at the scale its lines (one,
// when it has no limit) hold it in its width at, never under the
// minimum; one that fits doesn't shrink.
TEST(minimum_scale) {
	ak_uitest *a = mount(shrink_app, NULL);
	ak_fake_view *half = nth(a, AK_KIND_TEXT, 0), *least = nth(a, AK_KIND_TEXT, 1), *two = nth(a, AK_KIND_TEXT, 2), *wraps = nth(a, AK_KIND_TEXT, 3);
	REQUIRE(half && least && two && wraps);
	CHECK_EQ_F(ak_fake_float(half, AK_PROP_MINIMUM_SCALE), 0.25);
	// Half its size: 4-point characters on a 10-point line.
	CHECK_EQ_F(half->frame[2], 80);
	CHECK_EQ_F(half->frame[3], 10);
	// At its minimum, 0.75: 6-point characters, still one line.
	CHECK_EQ_F(least->frame[2], 78);
	CHECK_EQ_F(least->frame[3], 15);
	// Two lines of 80 hold its 160 points: its own size.
	CHECK_EQ_F(two->frame[2], 80);
	CHECK_EQ_F(two->frame[3], 40);
	// Without one, it wraps.
	CHECK_EQ_F(wraps->frame[3], 40);
	done(a);
}

// ---- Font variants, truncation and text shadows ----

static void *variant;  // a cell: 0 italic and monospaced digits, 1 small capitals, 2 none

static ak_val read_font(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	switch (ak_vm_cell_get(variant, true).i) {
	case 0: return font(17, true, true, false);
	case 1: return font(17, false, false, true);
	}
	return font(17, false, false, false);
}

static ak_val shadow(void) {
	ak_struct *s = ak_struct_new(&shadow_shape);
	s->f[0].i = (int64_t)ak_rgba(0, 0, 0, 0x80);
	s->f[1].f = 4, s->f[2].f = 1, s->f[3].f = 2;
	return (ak_val){.p = s};
}

static ak_val traits_app(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_view *col = ak_ui_new(AK_EL_COLUMN);
	ak_view *t = text("12:30");
	bind(t, AK_KEY_FONT, read_font, NULL);
	set_i(t, AK_KEY_TRUNCATION, AK_TRUNCATION_MIDDLE);
	set(t, AK_KEY_TEXT_SHADOW, shadow());
	add(col, t);
	// A view isn't a text: what's a text's isn't sent to it.
	ak_view *v = ak_ui_new(AK_EL_VIEW);
	set_i(v, AK_KEY_BACKGROUND, (int64_t)ak_rgba(255, 0, 0, 255));
	set_i(v, AK_KEY_TRUNCATION, AK_TRUNCATION_HEAD);
	set(v, AK_KEY_TEXT_SHADOW, shadow());
	set_f(v, AK_KEY_LETTER_SPACING, 3);
	add(col, v);
	return (ak_val){.p = col};
}

// A font's variants cross as FontTraits' bits, sent as they change; a
// text's truncation and shadow reach it, and a view that isn't a text
// gets none of them.
TEST(font_traits_truncation_and_text_shadow) {
	variant = ak_vm_cell_new((ak_val){.i = 0}, AK_K_INT, NULL).p;
	ak_uitest *a = mount(traits_app, NULL);
	ak_fake_view *t = nth(a, AK_KIND_TEXT, 0), *v = nth(a, AK_KIND_VIEW, 1);
	REQUIRE(t && v);
	CHECK_EQ_INT(ak_fake_int(t, AK_PROP_FONT_TRAITS), 1 | 2);
	CHECK_EQ_INT(ak_fake_int(t, AK_PROP_TRUNCATION), AK_TRUNCATION_MIDDLE);
	ak_span sh = bytes_of(t, AK_PROP_TEXT_SHADOW);
	REQUIRE(sh.len == 20);
	uint64_t color;
	float f[3];
	memcpy(&color, sh.p, 8);
	memcpy(f, sh.p + 8, 12);
	CHECK(color == (uint64_t)ak_rgba(0, 0, 0, 0x80));
	CHECK(f[0] == 4 && f[1] == 1 && f[2] == 2);
	CHECK(!has_prop(v, AK_PROP_TRUNCATION) && !has_prop(v, AK_PROP_TEXT_SHADOW) && !has_prop(v, AK_PROP_LETTER_SPACING));
	// Small capitals alone; then none, which is sent as 0. The size
	// stays: it isn't sent again.
	reset(a);
	write_i(variant, 1);
	ak_ui_flush();
	CHECK_EQ_INT(ak_fake_int(t, AK_PROP_FONT_TRAITS), 4);
	CHECK_EQ_INT(sets(a), 1);
	write_i(variant, 2);
	ak_ui_flush();
	REQUIRE(has_prop(t, AK_PROP_FONT_TRAITS));
	CHECK_EQ_INT(ak_fake_int(t, AK_PROP_FONT_TRAITS), 0);
	done(a);
	ak_release(variant);
}

// ---- Corners ----

static void *corner_mode;  // a cell: 0 the same radius, 1 a top's, 2 another same radius

static ak_val read_corners(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	switch (ak_vm_cell_get(corner_mode, true).i) {
	case 0: return corners(8, 8, 8, 8);
	case 1: return corners(16, 16, 0, 0);
	}
	return corners(4, 4, 4, 4);
}

static ak_val corners_app(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_view *col = ak_ui_new(AK_EL_COLUMN);
	ak_view *v = ak_ui_new(AK_EL_VIEW);
	set_i(v, AK_KEY_BACKGROUND, (int64_t)ak_rgba(255, 0, 0, 255));
	bind(v, AK_KEY_RADIUS, read_corners, NULL);
	add(col, v);
	// The leading corners, in a view laid out right to left: the right
	// ones.
	v = ak_ui_new(AK_EL_VIEW);
	set_i(v, AK_KEY_BACKGROUND, (int64_t)ak_rgba(0, 0, 255, 255));
	set_i(v, AK_KEY_LAYOUT_DIRECTION, AK_LAYOUT_DIRECTION_RIGHT_TO_LEFT);
	ak_struct *s = ak_struct_new(&corners_shape);
	s->f[4].f = 10;  // topStart
	s->f[6].f = 6;   // bottomStart
	set(v, AK_KEY_RADIUS, (ak_val){.p = s});
	add(col, v);
	// A radius of 0 everywhere is none: the view needs no view of its own.
	v = ak_ui_new(AK_EL_VIEW);
	set(v, AK_KEY_RADIUS, corners(0, 0, 0, 0));
	add(v, text("flat"));
	add(col, v);
	return (ak_val){.p = col};
}

// The same radius on every corner crosses as CornerRadius; different
// ones as CornerRadii, with CornerRadius 0, and back. Logical corners
// are on their direction's sides.
TEST(corner_radii) {
	corner_mode = ak_vm_cell_new((ak_val){.i = 0}, AK_K_INT, NULL).p;
	ak_uitest *a = mount(corners_app, NULL);
	ak_fake_view *v = nth(a, AK_KIND_VIEW, 1), *rtl = nth(a, AK_KIND_VIEW, 2);
	REQUIRE(v && rtl);
	CHECK_EQ_F(ak_fake_float(v, AK_PROP_CORNER_RADIUS), 8);
	CHECK(!has_prop(v, AK_PROP_CORNER_RADII));
	write_i(corner_mode, 1);
	ak_ui_flush();
	CHECK_EQ_F(ak_fake_float(v, AK_PROP_CORNER_RADIUS), 0);
	edges_are(v, AK_PROP_CORNER_RADII, 16, 16, 0, 0);
	write_i(corner_mode, 2);
	ak_ui_flush();
	CHECK_EQ_F(ak_fake_float(v, AK_PROP_CORNER_RADIUS), 4);
	edges_are(v, AK_PROP_CORNER_RADII, 0, 0, 0, 0);
	// topStart and bottomStart: top right and bottom right.
	edges_are(rtl, AK_PROP_CORNER_RADII, 0, 10, 6, 0);
	CHECK_EQ_INT(count_kind(a, AK_KIND_VIEW), 3);  // the root's, and the two with a background
	done(a);
	ak_release(corner_mode);
}

// ---- Borders ----

static ak_val borders_app(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_view *col = ak_ui_new(AK_EL_COLUMN);
	set_i(col, AK_KEY_ALIGN, 1);
	ak_view *v = ak_ui_new(AK_EL_VIEW);
	set(v, AK_KEY_BORDER, edges4(0, 0, 2, 0));
	set_i(v, AK_KEY_BORDER_COLOR, (int64_t)ak_rgba(0, 0, 0, 255));
	set_i(v, AK_KEY_BORDER_STYLE, AK_BORDER_STYLE_DASHED);
	add(v, text("Hi"));
	add(col, v);
	// A start border, right to left: on the right.
	v = ak_ui_new(AK_EL_VIEW);
	set_i(v, AK_KEY_LAYOUT_DIRECTION, AK_LAYOUT_DIRECTION_RIGHT_TO_LEFT);
	set(v, AK_KEY_BORDER, edges_se(3, 0));
	add(v, text("Hi"));
	add(col, v);
	// The same width on every side: BorderWidth.
	v = ak_ui_new(AK_EL_VIEW);
	set(v, AK_KEY_BORDER, edges4(1, 1, 1, 1));
	add(v, text("Hi"));
	add(col, v);
	return (ak_val){.p = col};
}

// A border's sides are laid out as CSS's (each its own width inside the
// box) and cross as BorderWidths when they differ, BorderWidth when
// they're the same; its style crosses as BorderStyle.
TEST(border_sides_and_style) {
	ak_uitest *a = mount(borders_app, NULL);
	ak_fake_view *bottom = nth(a, AK_KIND_VIEW, 1), *start = nth(a, AK_KIND_VIEW, 2), *all = nth(a, AK_KIND_VIEW, 3);
	REQUIRE(bottom && start && all);
	edges_are(bottom, AK_PROP_BORDER_WIDTHS, 0, 0, 2, 0);
	CHECK(!has_prop(bottom, AK_PROP_BORDER_WIDTH));
	CHECK_EQ_INT(ak_fake_int(bottom, AK_PROP_BORDER_STYLE), AK_BORDER_STYLE_DASHED);
	// Its text, 16 by 20, and 2 under it.
	CHECK_EQ_F(bottom->frame[2], 16);
	CHECK_EQ_F(bottom->frame[3], 22);
	edges_are(start, AK_PROP_BORDER_WIDTHS, 0, 3, 0, 0);
	CHECK_EQ_F(start->frame[2], 19);
	ak_fake_view *hi = nth(a, AK_KIND_TEXT, 1);
	REQUIRE(hi);
	CHECK_EQ_F(hi->frame[0], 0);  // the border is on its right
	CHECK_EQ_F(ak_fake_float(all, AK_PROP_BORDER_WIDTH), 1);
	CHECK(!has_prop(all, AK_PROP_BORDER_WIDTHS));
	CHECK_EQ_F(all->frame[2], 18);
	CHECK_EQ_F(all->frame[3], 22);
	done(a);
}

// ---- Gradients ----

static ak_val gradient(int64_t kind) {
	ak_struct *g = ak_struct_new(&gradient_shape);
	ak_arr *stops = ak_arr_new(AK_K_REF, &stop_shape, 2);
	for (int i = 0; i < 2; i++) {
		ak_struct *st = ak_struct_new(&stop_shape);
		st->f[0].i = (int64_t)(i ? ak_rgba(0, 0, 255, 255) : ak_rgba(255, 0, 0, 255));
		st->f[1].f = i;
		ak_arr_push(&stops, (ak_val){.p = st});
	}
	g->f[0].p = stops;
	g->f[4].f = 1;  // endY
	g->f[5].i = kind;
	g->f[6].f = 0.25, g->f[7].f = 0.75;  // center
	g->f[8].f = 0.6;                     // radius
	g->f[9].f = 90;                      // angle
	return (ak_val){.p = g};
}

static ak_val gradient_app(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_view *col = ak_ui_new(AK_EL_COLUMN);
	ak_view *v = ak_ui_new(AK_EL_VIEW);
	set(v, AK_KEY_GRADIENT, gradient(AK_GRADIENT_KIND_RADIAL));
	add(col, v);
	return (ak_val){.p = col};
}

// A gradient crosses with its kind, its center, radius and angle after
// its stops and its line.
TEST(radial_gradient) {
	ak_uitest *a = mount(gradient_app, NULL);
	ak_fake_view *v = nth(a, AK_KIND_VIEW, 1);
	REQUIRE(v);
	ak_span b = bytes_of(v, AK_PROP_GRADIENT);
	REQUIRE(b.len == 4 + 2 * 12 + 16 + 1 + 16);
	uint32_t count;
	memcpy(&count, b.p, 4);
	CHECK_EQ_INT(count, 2);
	float line[4], rest[4];
	memcpy(line, b.p + 28, 16);
	CHECK(line[0] == 0 && line[1] == 0 && line[2] == 0 && line[3] == 1);
	CHECK_EQ_INT((uint8_t)b.p[44], AK_GRADIENT_KIND_RADIAL);
	memcpy(rest, b.p + 45, 16);
	CHECK(rest[0] == 0.25f && rest[1] == 0.75f && rest[2] == 0.6f && rest[3] == 90);
	done(a);
}

T_MAIN()
