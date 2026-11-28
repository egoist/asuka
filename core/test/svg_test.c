// SVG documents read at run time (ui/svg.c): what they draw, as a
// Canvas's commands, written out as text to compare; their size and
// viewBox; fitting them into a box, their currentColor and a tint; what
// they refuse (not SVG, too much to draw) and what they survive (bytes
// gone wrong, under ASan).
#include "t.h"
#include "ui/svg.h"

#include <stdarg.h>

static ak_buf doc, out;
static char err[160];

static void num(ak_buf *o, float f) {
	double r = round((double)f * 1000) / 1000;
	if (r == 0) r = 0;
	ak_buf_printf(o, " %g", r);
}

static float rf(const uint8_t *p) {
	float f;
	memcpy(&f, p, 4);
	return f;
}

static uint32_t ru(const uint8_t *p) {
	uint32_t v;
	memcpy(&v, p, 4);
	return v;
}

static void color(ak_buf *o, const uint8_t *p) {
	uint64_t c;
	memcpy(&c, p, 8);
	uint8_t r, g, b, a;
	uint16_t sem;
	if (ak_color_channels(c, &r, &g, &b, &a)) ak_buf_printf(o, " #%02x%02x%02x%02x", r, g, b, a);
	else if (ak_color_semantic(c, &sem)) ak_buf_printf(o, " sem%u/%u", sem, ak_color_alpha(c));
	else ak_buf_printf(o, " %llu", (unsigned long long)c);
}

// path writes a path at *p (u32 length, then ops) as SVG's letters.
static size_t path(ak_buf *o, const uint8_t *p) {
	uint32_t n = ru(p);
	const uint8_t *q = p + 4, *e = q + n;
	ak_buf_printf(o, " [");
	bool first = true;
	while (q < e) {
		uint8_t op = *q++;
		static const char *const names[] = {"?", "M", "L", "Q", "C", "Z", "Rect", "RRect", "Ellipse", "Arc"};
		static const int args[] = {0, 2, 2, 4, 6, 0, 4, 5, 4, 6};
		if (op >= AK_LEN(names)) break;
		ak_buf_printf(o, first ? "%s" : " %s", names[op]);
		first = false;
		for (int k = 0; k < args[op]; k++) num(o, rf(q + 4 * k));
		q += 4 * args[op];
	}
	ak_buf_printf(o, "]");
	return 4 + n;
}

static size_t stops(ak_buf *o, const uint8_t *p) {
	uint32_t n = ru(p);
	size_t i = 4;
	ak_buf_printf(o, " {");
	for (uint32_t k = 0; k < n; k++) {
		color(o, p + i);
		num(o, rf(p + i + 8));
		i += 12;
	}
	ak_buf_printf(o, " }");
	return i;
}

static size_t stroke_style(ak_buf *o, const uint8_t *p) {
	num(o, rf(p));
	ak_buf_printf(o, " cap%u join%u", p[4], p[5]);
	uint32_t nd = ru(p + 6);
	size_t i = 10;
	if (nd) {
		ak_buf_printf(o, " dash");
		num(o, rf(p + i));
		for (uint32_t k = 0; k < nd; k++) num(o, rf(p + i + 4 + 4 * k));
	}
	return i + 4 + 4 * nd;
}

// cmds writes commands as text: "Fill #ff0000ff [M 0 0 L ...]; ...".
static const char *cmds(const uint8_t *c, size_t n) {
	static ak_buf o;
	o.len = 0;
	size_t i = 0;
	while (i < n) {
		if (o.len) ak_buf_printf(&o, "; ");
		uint8_t op = c[i++];
		switch (op) {
		case AK_CANVAS_OP_SAVE: ak_buf_printf(&o, "Save"); break;
		case AK_CANVAS_OP_RESTORE: ak_buf_printf(&o, "Restore"); break;
		case AK_CANVAS_OP_ALPHA: ak_buf_printf(&o, "Alpha"), num(&o, rf(c + i)), i += 4; break;
		case AK_CANVAS_OP_CLIP: ak_buf_printf(&o, "Clip"), i += path(&o, c + i); break;
		case AK_CANVAS_OP_FILL: ak_buf_printf(&o, "Fill"), color(&o, c + i), i += 8, i += path(&o, c + i); break;
		case AK_CANVAS_OP_STROKE:
			ak_buf_printf(&o, "Stroke"), color(&o, c + i), i += 8;
			i += stroke_style(&o, c + i);
			i += path(&o, c + i);
			break;
		case AK_CANVAS_OP_GRADIENT:
			ak_buf_printf(&o, "Gradient"), i += stops(&o, c + i);
			for (int k = 0; k < 4; k++) num(&o, rf(c + i + 4 * k));
			i += 16, i += path(&o, c + i);
			break;
		case AK_CANVAS_OP_RADIAL_GRADIENT:
			ak_buf_printf(&o, "Radial"), i += stops(&o, c + i);
			for (int k = 0; k < 5; k++) num(&o, rf(c + i + 4 * k));
			i += 20, i += path(&o, c + i);
			break;
		case AK_CANVAS_OP_STROKE_GRADIENT: {
			ak_buf_printf(&o, "StrokeGradient");
			i += stroke_style(&o, c + i);
			uint8_t radial = c[i++];
			ak_buf_printf(&o, radial ? " radial" : " linear");
			i += stops(&o, c + i);
			for (int k = 0; k < (radial ? 5 : 4); k++) num(&o, rf(c + i + 4 * k));
			i += radial ? 20 : 16, i += path(&o, c + i);
			break;
		}
		case AK_CANVAS_OP_TEXT: {
			ak_buf_printf(&o, "Text"), color(&o, c + i), i += 8;
			num(&o, rf(c + i));
			ak_buf_printf(&o, " w%u a%u", c[i + 4], c[i + 5]);
			num(&o, rf(c + i + 6)), num(&o, rf(c + i + 10));
			uint32_t sl = ru(c + i + 14);
			ak_buf_printf(&o, " \"%.*s\"", (int)sl, (const char *)c + i + 18);
			i += 18 + sl;
			break;
		}
		case AK_CANVAS_OP_TRANSFORM:
			ak_buf_printf(&o, "Transform");
			for (int k = 0; k < 6; k++) num(&o, rf(c + i + 4 * k));
			i += 24;
			break;
		case AK_CANVAS_OP_FILL_RULE: ak_buf_printf(&o, "EvenOdd %u", c[i]), i++; break;
		default: ak_buf_printf(&o, "?%u", op), i = n; break;
		}
	}
	ak_buf_u8(&o, 0);
	return (const char *)o.data;
}

// parse reads markup, true if it's a document (err says why not).
static bool parse(const char *svg) { return ak_svg_parse(svg, strlen(svg), &doc, err); }

// The document's own commands (its viewBox's units), after its header.
static const char *drawn(const char *svg) {
	if (!parse(svg)) return err;
	size_t i = 4 + 1 + 24 + 2;
	uint32_t ns = ru(doc.data + i);
	i += 4 + 8 * ns;
	uint32_t nc = ru(doc.data + i);
	return cmds(doc.data + i + 4, nc);
}

// fitted is what the document draws in a w by h box.
static const char *fitted(float w, float h, int mode, ak_color current, ak_color tint) {
	out.len = 0;
	if (!ak_svg_draw(doc.data, doc.len, &out, w, h, mode, current, tint)) return "not drawn";
	return cmds(out.data, out.len);
}

#define SVG(body) "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 10 10'>" body "</svg>"

TEST(Shapes) {
	CHECK_EQ_STR(drawn(SVG("<rect x='1' y='2' width='3' height='4' fill='red'/>")), "Fill #ff0000ff [M 1 2 L 4 2 L 4 6 L 1 6 Z]");
	// Black by default; a circle as four curves.
	CHECK_EQ_STR(drawn(SVG("<circle cx='5' cy='5' r='2'/>")),
	             "Fill #000000ff [M 7 5 C 7 6.105 6.105 7 5 7 C 3.895 7 3 6.105 3 5 C 3 3.895 3.895 3 5 3 C 6.105 3 7 3.895 7 5 Z]");
	CHECK_EQ_STR(drawn(SVG("<line x1='0' y1='0' x2='10' y2='10' stroke='#00f' stroke-width='2' stroke-linecap='round'/>")),
	             "Fill #000000ff [M 0 0 L 10 10]; Stroke #0000ffff 2 cap1 join0 [M 0 0 L 10 10]");
	CHECK_EQ_STR(drawn(SVG("<polygon points='0,0 10,0 5,8' fill='none' stroke='black' stroke-linejoin='bevel'/>")),
	             "Stroke #000000ff 1 cap0 join2 [M 0 0 L 10 0 L 5 8 Z]");
	// A rect's corners: ry from rx, clamped to half its side.
	CHECK_EQ_STR(drawn(SVG("<rect width='4' height='2' rx='3'/>")),
	             "Fill #000000ff [M 2 0 L 2 0 C 3.105 0 4 0.448 4 1 L 4 1 C 4 1.552 3.105 2 2 2 L 2 2 C 0.895 2 0 1.552 0 1 L 0 1 C 0 0.448 0.895 0 2 0 Z]");
	// Nothing to draw: a rect without a size, a circle without a radius.
	CHECK_EQ_STR(drawn(SVG("<rect width='0' height='4'/><circle r='0'/><ellipse rx='-1' ry='2'/>")), "");
}

TEST(PathData) {
	CHECK_EQ_STR(drawn(SVG("<path d='M1 1h3v3H1z m1 1 l1 0'/>")), "Fill #000000ff [M 1 1 L 4 1 L 4 4 L 1 4 Z M 2 2 L 3 2]");
	// Implicit lines after a move, smooth curves reflecting their control
	// points, and an arc's compact flags ("a1 1 0 00.5.5"... here 0 01 2).
	CHECK_EQ_STR(drawn(SVG("<path d='M0 0 1 1 2 0S4 2 5 0Q6 1 7 0T9 0'/>")),
	             "Fill #000000ff [M 0 0 L 1 1 L 2 0 C 2 0 4 2 5 0 Q 6 1 7 0 Q 8 -1 9 0]");
	CHECK_EQ_STR(drawn(SVG("<path d='M0 5a5 5 0 0110 0'/>")), "Fill #000000ff [M 0 5 C 0 2.239 2.239 0 5 0 C 7.761 0 10 2.239 10 5]");
	// What comes before an error is drawn.
	CHECK_EQ_STR(drawn(SVG("<path d='M0 0 L5 5 L oops 9'/>")), "Fill #000000ff [M 0 0 L 5 5]");
	// After a close, a segment starts where the subpath began.
	CHECK_EQ_STR(drawn(SVG("<path d='M1 1 L2 1 Z L 3 3'/>")), "Fill #000000ff [M 1 1 L 2 1 Z M 1 1 L 3 3]");
	CHECK_EQ_STR(drawn(SVG("<path d='1 2 3'/><path d=''/>")), "");
}

TEST(Size) {
	REQUIRE(parse("<svg viewBox='0 0 24 12'/>"));
	float w, h;
	REQUIRE(ak_svg_size(doc.data, doc.len, &w, &h));
	CHECK_EQ_F(w, 24);
	CHECK_EQ_F(h, 12);
	REQUIRE(parse("<svg width='1in' viewBox='0 0 24 12'/>"));
	ak_svg_size(doc.data, doc.len, &w, &h);
	CHECK_EQ_F(w, 96);
	CHECK_EQ_F(h, 48);
	REQUIRE(parse("<svg width='30pt' height='20'/>"));
	ak_svg_size(doc.data, doc.len, &w, &h);
	CHECK_EQ_F(w, 40);
	CHECK_EQ_F(h, 20);
	// Percentages say nothing of its size: the viewBox's, else 300 by 150.
	REQUIRE(parse("<svg width='100%' height='100%'/>"));
	ak_svg_size(doc.data, doc.len, &w, &h);
	CHECK_EQ_F(w, 300);
	CHECK_EQ_F(h, 150);
	CHECK(!ak_svg_size((const uint8_t *)"ASVG", 4, &w, &h));
}

TEST(NotSvg) {
	CHECK(!parse("<html><body/></html>"));
	CHECK_EQ_STR(err, "line 1: not an SVG document (its root isn't <svg>)");
	CHECK(!parse("\x89PNG\r\n"));
	CHECK(!parse(""));
	CHECK(!parse("<svg width='10'"));
	CHECK_EQ_STR(err, "line 1: an element that doesn't end");
	CHECK(!parse("<?xml version='1.0'?>\n\n<svg><path d='M0 0' fill=\"red/></svg>"));
	CHECK_EQ_STR(err, "line 3: an attribute's value that doesn't end");
	// A prolog, a DOCTYPE with entities (not expanded), comments, a BOM.
	CHECK(parse("\xef\xbb\xbf<?xml version='1.0'?><!DOCTYPE svg [<!ENTITY a 'b'>]><!-- hi --><svg/>"));
	// Its namespace's prefix.
	CHECK_EQ_STR(drawn("<svg:svg xmlns:svg='http://www.w3.org/2000/svg' viewBox='0 0 1 1'><svg:rect width='1' height='1'/></svg:svg>"),
	             "Fill #000000ff [M 0 0 L 1 0 L 1 1 L 0 1 Z]");
}

TEST(Colors) {
	CHECK_EQ_STR(drawn(SVG("<rect width='1' height='1' fill='#abc'/><rect width='1' height='1' fill='#11223380'/>")),
	             "Fill #aabbccff [M 0 0 L 1 0 L 1 1 L 0 1 Z]; Fill #11223380 [M 0 0 L 1 0 L 1 1 L 0 1 Z]");
	CHECK_EQ_STR(drawn(SVG("<rect width='1' height='1' fill='rgb(255, 0, 128)' fill-opacity='0.5'/>")),
	             "Fill #ff008080 [M 0 0 L 1 0 L 1 1 L 0 1 Z]");
	CHECK_EQ_STR(drawn(SVG("<rect width='1' height='1' fill='rgba(100%,0%,0%,.25)'/><rect width='1' height='1' fill='hsl(120 100% 50%)'/>")),
	             "Fill #ff000040 [M 0 0 L 1 0 L 1 1 L 0 1 Z]; Fill #00ff00ff [M 0 0 L 1 0 L 1 1 L 0 1 Z]");
	CHECK_EQ_STR(drawn(SVG("<rect width='1' height='1' fill='RebeccaPurple'/><rect width='1' height='1' fill='transparent'/>")),
	             "Fill #663399ff [M 0 0 L 1 0 L 1 1 L 0 1 Z]; Fill #00000000 [M 0 0 L 1 0 L 1 1 L 0 1 Z]");
	// A color it can't read is as if unset: inherited.
	CHECK_EQ_STR(drawn(SVG("<g fill='blue'><rect width='1' height='1' fill='bogus'/></g>")), "Fill #0000ffff [M 0 0 L 1 0 L 1 1 L 0 1 Z]");
	// currentColor: the element's color, else the document's.
	CHECK_EQ_STR(drawn(SVG("<g color='green'><rect width='1' height='1' fill='currentColor'/></g>")),
	             "Fill #008000ff [M 0 0 L 1 0 L 1 1 L 0 1 Z]");
	CHECK_EQ_STR(drawn(SVG("<rect width='1' height='1' fill='currentColor' fill-opacity='.5'/>")), "Fill 0 [M 0 0 L 1 0 L 1 1 L 0 1 Z]");
}

TEST(Style) {
	const char *svg = SVG("<style><![CDATA["
	                      ".a { fill: red } #b { fill: blue } rect.a.c { fill: lime }"
	                      " g > .d { stroke: black; stroke-width: 2px } g .e { fill: yellow !important }"
	                      " /* a comment */ @media print { rect { fill: pink } } circle:hover { fill: red }"
	                      "]]></style>"
	                      "<rect class='a' width='1' height='1'/>"
	                      "<rect class='a' id='b' width='1' height='1'/>"
	                      "<rect class='c a' width='1' height='1' fill='gray'/>"
	                      "<g><rect class='d' width='1' height='1' fill='none'/><g><rect class='e' width='1' height='1' style='fill:#123'/></g></g>"
	                      "<circle r='1'/>");
	CHECK_EQ_STR(drawn(svg), "Fill #ff0000ff [M 0 0 L 1 0 L 1 1 L 0 1 Z]; "
	                         "Fill #0000ffff [M 0 0 L 1 0 L 1 1 L 0 1 Z]; "
	                         "Fill #00ff00ff [M 0 0 L 1 0 L 1 1 L 0 1 Z]; "
	                         "Stroke #000000ff 2 cap0 join0 [M 0 0 L 1 0 L 1 1 L 0 1 Z]; "
	                         "Fill #112233ff [M 0 0 L 1 0 L 1 1 L 0 1 Z]; "
	                         "Fill #000000ff [M 1 0 C 1 0.552 0.552 1 0 1 C -0.552 1 -1 0.552 -1 0 C -1 -0.552 -0.552 -1 0 -1 C 0.552 -1 1 -0.552 1 0 Z]");
	// Inherited down groups; display none hides all of it, visibility only
	// what doesn't say visible.
	CHECK_EQ_STR(drawn(SVG("<g stroke='red' stroke-dasharray='1 2 3' stroke-dashoffset='1' fill='none'><path d='M0 0H1'/></g>"
	                       "<g display='none'><rect width='1' height='1'/></g>"
	                       "<g visibility='hidden'><rect width='1' height='1'/><rect visibility='visible' width='2' height='2'/></g>")),
	             "Stroke #ff0000ff 1 cap0 join0 dash 1 1 2 3 1 2 3 [M 0 0 L 1 0]; Fill #000000ff [M 0 0 L 2 0 L 2 2 L 0 2 Z]");
}

TEST(Transforms) {
	CHECK_EQ_STR(drawn(SVG("<g transform='translate(1 2) scale(2)'><rect width='1' height='1'/></g>")),
	             "Save; Transform 2 0 0 2 1 2; Fill #000000ff [M 0 0 L 1 0 L 1 1 L 0 1 Z]; Restore");
	CHECK_EQ_STR(drawn(SVG("<rect transform='rotate(90 5 5)' width='1' height='1'/>")),
	             "Save; Transform 0 1 -1 0 10 0; Fill #000000ff [M 0 0 L 1 0 L 1 1 L 0 1 Z]; Restore");
	CHECK_EQ_STR(drawn(SVG("<rect style='transform: rotate(0.25turn)' width='1' height='1'/>")),
	             "Save; Transform 0 1 -1 0 0 0; Fill #000000ff [M 0 0 L 1 0 L 1 1 L 0 1 Z]; Restore");
	// One it can't read: none.
	CHECK_EQ_STR(drawn(SVG("<rect transform='spin(3)' width='1' height='1'/>")), "Fill #000000ff [M 0 0 L 1 0 L 1 1 L 0 1 Z]");
	CHECK_EQ_STR(drawn(SVG("<g opacity='0.5'><rect width='1' height='1'/></g><g opacity='0'><rect width='1' height='1'/></g>")),
	             "Save; Alpha 0.5; Fill #000000ff [M 0 0 L 1 0 L 1 1 L 0 1 Z]; Restore");
}

TEST(FillRule) {
	CHECK_EQ_STR(drawn(SVG("<path fill-rule='evenodd' d='M0 0H4V4H0z M1 1H3V3H1z'/><rect width='1' height='1'/>")),
	             "EvenOdd 1; Fill #000000ff [M 0 0 L 4 0 L 4 4 L 0 4 Z M 1 1 L 3 1 L 3 3 L 1 3 Z]; EvenOdd 0; Fill #000000ff [M 0 0 L 1 0 L 1 1 L 0 1 Z]");
}

TEST(Gradients) {
	// In the shape's bounds (objectBoundingBox): its left to its right.
	CHECK_EQ_STR(drawn(SVG("<linearGradient id='g'><stop offset='0' stop-color='red'/><stop offset='100%' stop-color='blue' stop-opacity='.5'/></linearGradient>"
	                       "<rect x='2' y='2' width='4' height='2' fill='url(#g)'/>")),
	             "Gradient { #ff0000ff 0 #0000ff80 1 } 2 0 6 0 [M 2 2 L 6 2 L 6 4 L 2 4 Z]");
	// Down its bounds: a gradient in a box not square stays a linear one,
	// worked out in user space.
	CHECK_EQ_STR(drawn(SVG("<linearGradient id='g' x2='1' y2='1'><stop stop-color='red'/><stop offset='1' stop-color='blue'/></linearGradient>"
	                       "<rect width='4' height='2' fill='url(#g)'/>")),
	             "Gradient { #ff0000ff 0 #0000ffff 1 } 0 0 1.6 3.2 [M 0 0 L 4 0 L 4 2 L 0 2 Z]");
	// userSpaceOnUse, a gradientTransform, stops inherited by href, offsets
	// kept in order.
	CHECK_EQ_STR(drawn(SVG("<defs><linearGradient id='base'><stop offset='.5' stop-color='red'/><stop offset='.2' stop-color='lime'/></linearGradient>"
	                       "<linearGradient id='g' href='#base' gradientUnits='userSpaceOnUse' x1='0' x2='5' gradientTransform='translate(1 0)'/></defs>"
	                       "<rect width='10' height='10' fill='url(#g)'/>")),
	             "Gradient { #ff0000ff 0.5 #00ff00ff 0.5 } 1 0 6 0 [M 0 0 L 10 0 L 10 10 L 0 10 Z]");
	// repeat: the stops laid out across the shape.
	CHECK_EQ_STR(drawn(SVG("<linearGradient id='g' gradientUnits='userSpaceOnUse' x2='5' spreadMethod='repeat'><stop stop-color='red'/><stop offset='1' stop-color='blue'/></linearGradient>"
	                       "<rect width='10' height='1' fill='url(#g)'/>")),
	             "Gradient { #ff0000ff 0 #0000ffff 0.5 #ff0000ff 0.5 #0000ffff 1 } 0 0 10 0 [M 0 0 L 10 0 L 10 1 L 0 1 Z]");
	// One stop: its color.
	CHECK_EQ_STR(drawn(SVG("<linearGradient id='g'><stop stop-color='red'/></linearGradient><rect width='1' height='1' fill='url(#g)'/>")),
	             "Fill #ff0000ff [M 0 0 L 1 0 L 1 1 L 0 1 Z]");
	// A paint server missing: its fallback.
	CHECK_EQ_STR(drawn(SVG("<rect width='1' height='1' fill='url(#nope) green'/><rect width='1' height='1' fill='url(#nope)'/>")),
	             "Fill #008000ff [M 0 0 L 1 0 L 1 1 L 0 1 Z]");
	// Radial, in a square: in user space; in a box not square, drawn
	// through its transform.
	CHECK_EQ_STR(drawn(SVG("<radialGradient id='r'><stop stop-color='white'/><stop offset='1' stop-color='black'/></radialGradient>"
	                       "<rect width='4' height='4' fill='url(#r)'/>")),
	             "Radial { #ffffffff 0 #000000ff 1 } 2 2 2 2 2 [M 0 0 L 4 0 L 4 4 L 0 4 Z]");
	CHECK_EQ_STR(drawn(SVG("<radialGradient id='r' fx='.25'><stop stop-color='white'/><stop offset='1' stop-color='black'/></radialGradient>"
	                       "<rect width='4' height='2' fill='url(#r)'/>")),
	             "Save; Transform 4 0 0 2 0 0; Radial { #ffffffff 0 #000000ff 1 } 0.5 0.5 0.5 0.25 0.5 [M 0 0 L 1 0 L 1 1 L 0 1 Z]; Restore");
	// A stroke's gradient.
	CHECK_EQ_STR(drawn(SVG("<linearGradient id='g'><stop stop-color='red'/><stop offset='1' stop-color='blue'/></linearGradient>"
	                       "<path d='M0 0H4V2' fill='none' stroke='url(#g)' stroke-width='2'/>")),
	             "StrokeGradient 2 cap0 join0 linear { #ff0000ff 0 #0000ffff 1 } 0 0 4 0 [M 0 0 L 4 0 L 4 2]");
	// Bounds with no width paint nothing with a bounding-box gradient.
	CHECK_EQ_STR(drawn(SVG("<linearGradient id='g'><stop stop-color='red'/><stop offset='1' stop-color='blue'/></linearGradient>"
	                       "<path d='M0 0V4' fill='none' stroke='url(#g)'/>")),
	             "");
}

TEST(Clipping) {
	CHECK_EQ_STR(drawn(SVG("<clipPath id='c'><rect width='5' height='5' transform='translate(1 1)'/><circle r='1' clip-rule='evenodd'/></clipPath>"
	                       "<rect width='10' height='10' clip-path='url(#c)'/>")),
	             "Save; Clip [M 1 1 L 6 1 L 6 6 L 1 6 Z M 1 0 C 1 0.552 0.552 1 0 1 C -0.552 1 -1 0.552 -1 0 C -1 -0.552 -0.552 -1 0 -1 C 0.552 -1 1 -0.552 1 0 Z]; "
	             "Fill #000000ff [M 0 0 L 10 0 L 10 10 L 0 10 Z]; Restore");
	// In the element's bounds; a clip-rule.
	CHECK_EQ_STR(drawn(SVG("<clipPath id='c' clipPathUnits='objectBoundingBox'><rect width='.5' height='1' clip-rule='evenodd'/></clipPath>"
	                       "<g clip-path='url(#c)'><rect x='2' width='4' height='2'/></g>")),
	             "Save; EvenOdd 1; Clip [M 2 0 L 4 0 L 4 2 L 2 2 Z]; EvenOdd 0; Fill #000000ff [M 2 0 L 6 0 L 6 2 L 2 2 Z]; Restore");
	// Nothing in the clip: nothing drawn.
	CHECK_EQ_STR(drawn(SVG("<clipPath id='c'/><rect width='10' height='10' clip-path='url(#c)'/>")), "");
	// A group's bounds from a long path through a transform (its outline
	// read as it's moved, never from a buffer that grows under it).
	ak_buf long_path = {0};
	ak_buf_printf(&long_path, "<svg viewBox='0 0 10 10'><clipPath id='c' clipPathUnits='objectBoundingBox'><rect width='1' height='1'/></clipPath>"
	                          "<g clip-path='url(#c)'><path transform='scale(2)' d='M0 0");
	for (int i = 1; i <= 400; i++) ak_buf_printf(&long_path, " L%d %d", i % 7, i % 5);
	ak_buf_printf(&long_path, "'/></g></svg>");
	ak_buf_u8(&long_path, 0);
	const char *s = drawn((const char *)long_path.data);
	CHECK(strncmp(s, "Save; Clip [M 0 0 L 12 0 L 12 8 L 0 8 Z]; Save; Transform 2 0 0 2 0 0; Fill", 72) == 0);
	ak_buf_free(&long_path);
}

TEST(Use) {
	CHECK_EQ_STR(drawn(SVG("<defs><rect id='r' width='1' height='1'/></defs><use href='#r' x='2' fill='red'/><use xlink:href='#r' y='3'/>")),
	             "Save; Transform 1 0 0 1 2 0; Fill #ff0000ff [M 0 0 L 1 0 L 1 1 L 0 1 Z]; Restore; "
	             "Save; Transform 1 0 0 1 0 3; Fill #000000ff [M 0 0 L 1 0 L 1 1 L 0 1 Z]; Restore");
	// A symbol: its viewBox fitted into the use's box, clipped to it.
	CHECK_EQ_STR(drawn(SVG("<symbol id='s' viewBox='0 0 2 2'><rect width='2' height='2'/></symbol><use href='#s' width='4' height='4'/>")),
	             "Save; Save; Clip [Rect 0 0 4 4]; Transform 2 0 0 2 0 0; Fill #000000ff [M 0 0 L 2 0 L 2 2 L 0 2 Z]; Restore; Restore");
	// A use of what holds it, or of itself, draws nothing more; another
	// document's element isn't fetched.
	CHECK_EQ_STR(drawn(SVG("<g id='g'><rect width='1' height='1'/><use href='#g'/></g><use id='u' href='#u'/><use href='other.svg#r'/>")),
	             "Fill #000000ff [M 0 0 L 1 0 L 1 1 L 0 1 Z]");
}

TEST(NestedSvg) {
	CHECK_EQ_STR(drawn(SVG("<svg x='1' y='1' width='4' height='2' viewBox='0 0 1 1'><rect width='1' height='1'/></svg>")),
	             "Save; Clip [Rect 1 1 4 2]; Transform 2 0 0 2 2 1; Fill #000000ff [M 0 0 L 1 0 L 1 1 L 0 1 Z]; Restore");
	CHECK_EQ_STR(drawn(SVG("<svg width='4' height='2' viewBox='0 0 1 1' preserveAspectRatio='none'><rect width='1' height='1'/></svg>")),
	             "Save; Clip [Rect 0 0 4 2]; Transform 4 0 0 2 0 0; Fill #000000ff [M 0 0 L 1 0 L 1 1 L 0 1 Z]; Restore");
	// A switch draws its first child it can: not one that asks for an
	// extension.
	CHECK_EQ_STR(drawn(SVG("<switch><foreignObject requiredExtensions='http://ns.adobe.com/AdobeIllustrator/10.0/'/><g><rect width='1' height='1'/></g><rect width='2' height='2'/></switch>")),
	             "Fill #000000ff [M 0 0 L 1 0 L 1 1 L 0 1 Z]");
}

TEST(Text) {
	CHECK_EQ_STR(drawn(SVG("<text x='5' y='8' font-size='4' font-weight='bold' text-anchor='middle' fill='#333'>  Hi &amp;\n  there </text>")),
	             "Text #333333ff 4 w7 a1 5 8 \"Hi & there\"");
	// A tspan with a position starts a run, in its style; one without goes
	// on in the run's.
	CHECK_EQ_STR(drawn(SVG("<text x='1' y='2' font-family='serif'>A <tspan font-weight='600'>b</tspan><tspan x='1' dy='3' font-size='50%'>C</tspan></text>")),
	             "Text #000000ff 16 w4 a0 1 2 \"A b\"; Text #000000ff 8 w4 a0 1 5 \"C\"");
	// A gradient's text: its first stop's color; none: nothing.
	CHECK_EQ_STR(drawn(SVG("<linearGradient id='g'><stop stop-color='red'/><stop offset='1' stop-color='blue'/></linearGradient>"
	                       "<text fill='url(#g)'>A</text><text fill='none'>B</text>")),
	             "Text #ff0000ff 16 w4 a0 0 0 \"A\"");
	CHECK_EQ_STR(drawn(SVG("<text xml:space='preserve'> a  b </text>")), "Text #000000ff 16 w4 a0 0 0 \" a  b \"");
	CHECK_EQ_STR(drawn(SVG("<text>&#x41;&#66;&lt;&unknown;</text>")), "Text #000000ff 16 w4 a0 0 0 \"AB<&unknown;\"");
}

TEST(Fit) {
	REQUIRE(parse("<svg viewBox='0 0 10 5'><rect width='10' height='5' fill='currentColor'/></svg>"));
	ak_color label = ak_semantic_color(AK_SEMANTIC_COLOR_LABEL);
	// Fitted into 20 by 20: twice its size, centered down the box.
	CHECK_EQ_STR(fitted(20, 20, AK_IMAGE_MODE_FIT, label, 0),
	             "Save; Clip [Rect 0 0 20 20]; Transform 2 0 0 2 0 5; Fill sem1/255 [M 0 0 L 10 0 L 10 5 L 0 5 Z]; Restore");
	CHECK_EQ_STR(fitted(20, 20, AK_IMAGE_MODE_FILL, label, 0),
	             "Save; Clip [Rect 0 0 20 20]; Transform 4 0 0 4 -10 0; Fill sem1/255 [M 0 0 L 10 0 L 10 5 L 0 5 Z]; Restore");
	CHECK_EQ_STR(fitted(20, 20, AK_IMAGE_MODE_STRETCH, label, 0),
	             "Save; Clip [Rect 0 0 20 20]; Transform 2 0 0 4 0 0; Fill sem1/255 [M 0 0 L 10 0 L 10 5 L 0 5 Z]; Restore");
	CHECK_EQ_STR(fitted(20, 20, AK_IMAGE_MODE_CENTER, ak_rgba(1, 2, 3, 255), 0),
	             "Save; Clip [Rect 0 0 20 20]; Transform 1 0 0 1 5 7.5; Fill #010203ff [M 0 0 L 10 0 L 10 5 L 0 5 Z]; Restore");
	// A tint: every color its, at the color's opacity.
	REQUIRE(parse(SVG("<rect width='1' height='1' fill='red' fill-opacity='.5'/><linearGradient id='g'><stop stop-color='red'/><stop offset='1' stop-color='blue' stop-opacity='0'/></linearGradient><rect width='1' height='1' fill='url(#g)'/>")));
	CHECK_EQ_STR(fitted(10, 10, AK_IMAGE_MODE_FIT, label, ak_rgba(0, 0, 255, 255)),
	             "Save; Clip [Rect 0 0 10 10]; Transform 1 0 0 1 0 0; Fill #0000ff80 [M 0 0 L 1 0 L 1 1 L 0 1 Z]; "
	             "Gradient { #0000ffff 0 #0000ff00 1 } 0 0 1 0 [M 0 0 L 1 0 L 1 1 L 0 1 Z]; Restore");
	// The document's own preserveAspectRatio, in its box.
	REQUIRE(parse("<svg width='20' height='10' viewBox='0 0 1 1' preserveAspectRatio='xMinYMid slice'><rect width='1' height='1'/></svg>"));
	CHECK_EQ_STR(fitted(20, 10, AK_IMAGE_MODE_FIT, label, 0),
	             "Save; Clip [Rect 0 0 20 10]; Transform 20 0 0 20 0 -5; Fill #000000ff [M 0 0 L 1 0 L 1 1 L 0 1 Z]; Restore");
	// Nothing in a box with no area.
	CHECK_EQ_STR(fitted(0, 10, AK_IMAGE_MODE_FIT, label, 0), "");
}

TEST(Refused) {
	// Not a document; a document whose commands were cut or changed.
	CHECK(!ak_svg_draw((const uint8_t *)"nope", 4, &out, 10, 10, 0, 0, 0));
	REQUIRE(parse(SVG("<g transform='scale(2)'><rect width='1' height='1'/></g>")));
	doc.data[doc.len - 2] ^= 0xff;
	out.len = 0;
	CHECK(ak_svg_draw(doc.data, doc.len, &out, 10, 10, 0, 0, 0) || out.len == 0);
	REQUIRE(parse(SVG("<g transform='scale(2)'><rect width='1' height='1'/></g>")));
	doc.data[doc.len - 1] = AK_CANVAS_OP_SAVE;  // Restore made Save: balanced again by the drawing
	out.len = 0;
	REQUIRE(ak_svg_draw(doc.data, doc.len, &out, 10, 10, 0, 0, 0));
	const char *s = cmds(out.data, out.len);
	CHECK(strstr(s, "Save; Restore; Restore; Restore") != NULL);
	// Elements nested too deep, too many uses of uses (a billion laughs).
	ak_buf deep = {0};
	for (int i = 0; i < 300; i++) ak_buf_put(&deep, "<g>", 3);
	ak_buf_u8(&deep, 0);
	CHECK(!parse((const char *)deep.data));
	ak_buf_free(&deep);
	ak_buf laughs = {0};
	ak_buf_printf(&laughs, "<svg><rect id='l0' width='1' height='1'/>");
	for (int i = 1; i < 10; i++) {
		ak_buf_printf(&laughs, "<g id='l%d'>", i);
		for (int k = 0; k < 10; k++) ak_buf_printf(&laughs, "<use href='#l%d'/>", i - 1);
		ak_buf_printf(&laughs, "</g>");
	}
	ak_buf_printf(&laughs, "</svg>");
	ak_buf_u8(&laughs, 0);
	CHECK(!parse((const char *)laughs.data));
	CHECK_EQ_STR(err, "too complex to draw (more than a million elements, or 64 MB of drawing)");
	ak_buf_free(&laughs);
}

TEST(DrawAllocatesNothing) {
	REQUIRE(parse(SVG("<rect width='1' height='1' fill='currentColor'/><circle r='3' fill='red'/>")));
	out.len = 0;
	ak_svg_draw(doc.data, doc.len, &out, 50, 50, 0, 0, 0);
	uint64_t before = ak_alloc_count;
	for (int i = 0; i < 100; i++) {
		out.len = 0;
		ak_svg_draw(doc.data, doc.len, &out, 50 + (float)(i % 3), 50, 0, ak_semantic_color(AK_SEMANTIC_COLOR_LABEL), 0);
	}
	CHECK_EQ_INT(ak_alloc_count - before, 0);
}

// Markup gone wrong, and documents' bytes gone wrong: read, refused or
// drawn, never out of bounds.
TEST(Fuzz) {
	static const char *const seeds[] = {
		SVG("<style>.a{fill:red}</style><g transform='rotate(3)' class='a'><path d='M0 0C1 2 3 4 5 6a1 1 0 1 1 2 2z'/><text x='1'>hi<tspan x='2'>t</tspan></text></g>"),
		SVG("<linearGradient id='g' spreadMethod='reflect' x2='.1'><stop stop-color='red'/><stop offset='1' stop-color='blue'/></linearGradient><rect width='5' height='5' fill='url(#g)' stroke='url(#g)'/>"),
		SVG("<clipPath id='c' clipPathUnits='objectBoundingBox'><use href='#r'/></clipPath><rect id='r' width='3' height='4' clip-path='url(#c)'/><symbol id='s' viewBox='0 0 1 1'/><use href='#s'/>"),
	};
	uint64_t x = 0x9e3779b97f4a7c15u;
	ak_buf m = {0};
	for (int round_ = 0; round_ < 3000; round_++) {
		const char *seed = seeds[round_ % AK_LEN(seeds)];
		size_t n = strlen(seed);
		m.len = 0;
		ak_buf_put(&m, seed, n);
		for (int k = 0; k < 1 + round_ % 5; k++) {
			x ^= x << 13, x ^= x >> 7, x ^= x << 17;
			size_t at = (size_t)(x % m.len);
			switch ((x >> 40) % 3) {
			case 0: m.data[at] = (uint8_t)(x >> 16); break;
			case 1: m.len = at; break;
			default: m.data[at] = "<>/='\"&#;"[(x >> 20) % 9]; break;
			}
			if (m.len == 0) break;
		}
		if (ak_svg_parse((const char *)m.data, m.len, &doc, err)) {
			out.len = 0;
			ak_svg_draw(doc.data, doc.len, &out, 30, 20, (int)(x % 4), 0, x % 2 ? ak_rgba(1, 2, 3, 4) : 0);
			// The document's bytes changed: drawn whole or not at all.
			x ^= x << 13, x ^= x >> 7, x ^= x << 17;
			doc.data[x % doc.len] = (uint8_t)(x >> 24);
			out.len = 0;
			ak_svg_draw(doc.data, doc.len, &out, 30, 20, 0, 0, 0);
		}
	}
	ak_buf_free(&m);
}

TEST(Cleanup) {
	ak_buf_free(&doc);
	ak_buf_free(&out);
}

T_MAIN()
