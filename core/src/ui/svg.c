// svg.c: SVG documents read at run time (svg.h), for std/svg's Document
// and Image's .svg sources, drawn as a Canvas's commands.
//
// The platforms' own SVG support differs too much to share a result:
// UIKit, Android and Win32 draw no arbitrary SVG, and browsers, AppKit and
// librsvg each read it their own way. So the core reads it, the same
// everywhere, into the display list every host already replays with its
// own vector drawing (Core Graphics, Android's Canvas, GDI+, Cairo, the 2D
// canvas): crisp at any size and scale, nothing rasterized here.
//
// It reads: svg (nested ones too, as viewports), g, a, switch, path, rect,
// circle, ellipse, line, polyline, polygon, use (of elements, symbols and
// svgs), defs, symbol, linearGradient and radialGradient (units,
// gradientTransform, spreadMethod, href inheritance; stops' colors and
// opacities), clipPath (its units, its children's transforms, clip-rule),
// text and tspan (positions, font size and weight, text-anchor; drawn with
// the platform's font), and style: presentation attributes, style
// attributes and style elements (CSS: type, class, id and universal
// selectors, compounds of them, descendant and child combinators, by
// specificity), inherited as SVG does. Fill and stroke: colors (hex,
// rgb(), hsl(), names, currentColor), gradients, opacities, fill-rule,
// stroke width, caps, joins, dashes and their offset; opacity; transforms;
// viewBox and preserveAspectRatio; display and visibility; lengths in
// px, pt, pc, mm, cm, in, em, ex and percentages.
//
// It doesn't: filters, masks, patterns, markers, images, foreignObject,
// animation, scripts, fonts of the document's (text is the platform's
// font, one style per run that a position starts), textPath, stroke
// miter limits (the platform's), a gradient's paint on text (its first
// stop's color), group opacity as one layer (each thing it draws is drawn
// at the opacity). Nothing outside the document is read: no external
// references, entities, fonts or images; scripts are never run.
//
// Parsing is bounded: the markup's nesting (256), its elements (a
// million), and what it draws (uses nest 16 deep and can't recurse; a
// document drawing more than a million elements, or 64 MB of commands,
// is refused), so a hostile document can't take the app down.
#include "ui/svg.h"

#include <stdio.h>

#define MAX_DEPTH 256
#define MAX_NODES (1u << 20)
#define MAX_VISITS 1000000u
#define MAX_CMDS ((size_t)64 << 20)
#define MAX_USES 16
#define MAX_DASH 16
#define MAX_STOPS 1024
#define MAX_REPEAT 32
#define SVG_PI 3.14159265358979323846

// ---- An arena: the document's strings, freed at once ----

typedef struct arena_blk {
	struct arena_blk *next;
	size_t len, cap;
	char data[];
} arena_blk;

// ---- The document's tree ----

enum { N_ELEM, N_TEXT };

enum {
	T_UNKNOWN,
	T_SVG,
	T_G,
	T_PATH,
	T_RECT,
	T_CIRCLE,
	T_ELLIPSE,
	T_LINE,
	T_POLYLINE,
	T_POLYGON,
	T_TEXT,
	T_TSPAN,
	T_USE,
	T_DEFS,
	T_SYMBOL,
	T_LINEAR,
	T_RADIAL,
	T_STOP,
	T_CLIP,
	T_STYLE,
	T_A,
	T_SWITCH,
};

static const char *const tag_names[] = {
	"", "svg", "g", "path", "rect", "circle", "ellipse", "line", "polyline", "polygon", "text", "tspan", "use",
	"defs", "symbol", "linearGradient", "radialGradient", "stop", "clipPath", "style", "a", "switch",
};

typedef struct {
	uint8_t kind, tag;
	bool busy;  // being drawn: a use of it, inside it, is a cycle
	int32_t parent, first, next;
	uint32_t a0, na;  // its attributes
	uint32_t d0, nd;  // its declarations, cascaded (the last of a property wins)
	const char *name;  // an element's local name; a text node's text
} node;

typedef struct {
	const char *name, *value;
} attr;

// The properties it reads from presentation attributes and CSS.
enum {
	PR_FILL,
	PR_FILL_OPACITY,
	PR_FILL_RULE,
	PR_STROKE,
	PR_STROKE_WIDTH,
	PR_STROKE_OPACITY,
	PR_STROKE_LINECAP,
	PR_STROKE_LINEJOIN,
	PR_STROKE_DASHARRAY,
	PR_STROKE_DASHOFFSET,
	PR_OPACITY,
	PR_DISPLAY,
	PR_VISIBILITY,
	PR_COLOR,
	PR_FONT_SIZE,
	PR_FONT_WEIGHT,
	PR_TEXT_ANCHOR,
	PR_CLIP_PATH,
	PR_CLIP_RULE,
	PR_STOP_COLOR,
	PR_STOP_OPACITY,
	PR_TRANSFORM,
	PR_COUNT,
};

static const char *const prop_names[PR_COUNT] = {
	"fill", "fill-opacity", "fill-rule", "stroke", "stroke-width", "stroke-opacity", "stroke-linecap",
	"stroke-linejoin", "stroke-dasharray", "stroke-dashoffset", "opacity", "display", "visibility", "color",
	"font-size", "font-weight", "text-anchor", "clip-path", "clip-rule", "stop-color", "stop-opacity", "transform",
};

typedef struct {
	uint8_t prop;
	const char *value;
} decl;

// A CSS selector's compound (rect.a#b), and how it's joined to the one
// before it: ' ' a descendant of it, '>' a child.
typedef struct {
	const char *tag;  // NULL: any
	const char *id;
	const char *cls[4];
	uint8_t ncls;
	char comb;
} part;

typedef struct {
	uint32_t p0, np;  // its compounds, left to right
	uint32_t spec, order;
	uint32_t d0, nd;  // its declarations
} rule;

typedef struct {
	const char *src;
	size_t n;
	AK_VEC(node) nodes;
	AK_VEC(attr) attrs;
	AK_VEC(decl) decls;
	AK_VEC(rule) rules;
	AK_VEC(part) parts;
	AK_VEC(decl) rdecls;
	int32_t *ids;
	uint32_t idmask;
	arena_blk *arena;
	char *err;
	bool failed;
} P;

static void fail(P *p, size_t off, const char *msg) {
	if (p->failed) return;
	p->failed = true;
	int line = 1;
	for (size_t i = 0; i < off && i < p->n; i++)
		if (p->src[i] == '\n') line++;
	snprintf(p->err, 160, "line %d: %s", line, msg);
}

static char *arena_alloc(P *p, size_t n) {
	arena_blk *b = p->arena;
	if (!b || b->cap - b->len < n) {
		size_t cap = n > 65536 ? n : 65536;
		arena_blk *nb = ak_alloc_raw(sizeof(arena_blk) + cap);
		nb->next = b, nb->len = 0, nb->cap = cap;
		p->arena = b = nb;
	}
	char *r = b->data + b->len;
	b->len += n;
	return r;
}

static char *arena_str(P *p, const char *s, size_t n) {
	char *r = arena_alloc(p, n + 1);
	if (n) memcpy(r, s, n);
	r[n] = 0;
	return r;
}

static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

static bool streq(const char *a, const char *b) { return a && b && strcmp(a, b) == 0; }

static void put_utf8(char **o, uint32_t c) {
	char *q = *o;
	if (c < 0x80) *q++ = (char)c;
	else if (c < 0x800) *q++ = (char)(0xc0 | c >> 6), *q++ = (char)(0x80 | (c & 0x3f));
	else if (c < 0x10000) *q++ = (char)(0xe0 | c >> 12), *q++ = (char)(0x80 | (c >> 6 & 0x3f)), *q++ = (char)(0x80 | (c & 0x3f));
	else *q++ = (char)(0xf0 | c >> 18), *q++ = (char)(0x80 | (c >> 12 & 0x3f)), *q++ = (char)(0x80 | (c >> 6 & 0x3f)), *q++ = (char)(0x80 | (c & 0x3f));
	*o = q;
}

// decoded is s[0:n] with XML's entities read (the five and character
// references; others are left as written: no DTD's are expanded).
static char *decoded(P *p, const char *s, size_t n) {
	char *r = arena_alloc(p, n + 1), *o = r;
	for (size_t i = 0; i < n;) {
		if (s[i] != '&') {
			*o++ = s[i++];
			continue;
		}
		size_t j = i + 1;
		while (j < n && j - i < 12 && s[j] != ';') j++;
		if (j >= n || s[j] != ';') {
			*o++ = s[i++];
			continue;
		}
		const char *e = s + i + 1;
		size_t en = j - i - 1;
		int ch = -1;
		if (en == 2 && !memcmp(e, "lt", 2)) ch = '<';
		else if (en == 2 && !memcmp(e, "gt", 2)) ch = '>';
		else if (en == 3 && !memcmp(e, "amp", 3)) ch = '&';
		else if (en == 4 && !memcmp(e, "quot", 4)) ch = '"';
		else if (en == 4 && !memcmp(e, "apos", 4)) ch = '\'';
		else if (en > 1 && e[0] == '#') {
			uint32_t v = 0;
			bool hex = e[1] == 'x' || e[1] == 'X', ok = true;
			for (size_t k = hex ? 2 : 1; k < en; k++) {
				char c = e[k];
				int d = c >= '0' && c <= '9' ? c - '0' : hex && c >= 'a' && c <= 'f' ? c - 'a' + 10 : hex && c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
				if (d < 0) ok = false;
				else if (v < 0x110000) v = v * (hex ? 16 : 10) + (uint32_t)d;
			}
			if (ok && en > (hex ? 2u : 1u)) {
				if (v == 0 || v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff)) v = 0xfffd;
				put_utf8(&o, v);
				i = j + 1;
				continue;
			}
		}
		if (ch < 0) {
			*o++ = s[i++];
			continue;
		}
		*o++ = (char)ch;
		i = j + 1;
	}
	*o = 0;
	return r;
}

// ---- XML ----

static int tag_of(const char *name) {
	for (int i = 1; i < (int)AK_LEN(tag_names); i++)
		if (strcmp(name, tag_names[i]) == 0) return i;
	return T_UNKNOWN;
}

static int32_t add_node(P *p, int32_t parent, int32_t *last) {
	if (p->nodes.len >= MAX_NODES) return -1;
	node nd = {.parent = parent, .first = -1, .next = -1};
	ak_vec_push(&p->nodes, nd);
	int32_t i = (int32_t)p->nodes.len - 1;
	if (parent >= 0) {
		if (*last >= 0) p->nodes.data[*last].next = i;
		else p->nodes.data[parent].first = i;
		*last = i;
	}
	return i;
}

static bool keeps_text(P *p, int32_t parent) {
	if (parent < 0) return false;
	int t = p->nodes.data[parent].tag;
	return t == T_TEXT || t == T_TSPAN || t == T_STYLE || t == T_A;
}

static bool name_char(char c) { return c && !is_space(c) && c != '>' && c != '/' && c != '=' && c != '<'; }

static void xml(P *p) {
	const char *s = p->src, *end = p->src + p->n;
	int32_t stack[MAX_DEPTH + 1], last[MAX_DEPTH + 1];
	int depth = 0;
	bool rooted = false;
	if (end - s >= 3 && (uint8_t)s[0] == 0xef && (uint8_t)s[1] == 0xbb && (uint8_t)s[2] == 0xbf) s += 3;
	while (s < end && !p->failed) {
		if (*s != '<') {
			const char *t = s;
			while (s < end && *s != '<') s++;
			if (depth > 0 && keeps_text(p, stack[depth - 1])) {
				int32_t i = add_node(p, stack[depth - 1], &last[depth]);
				if (i < 0) return fail(p, (size_t)(t - p->src), "too many elements");
				p->nodes.data[i].kind = N_TEXT;
				p->nodes.data[i].name = decoded(p, t, (size_t)(s - t));
			}
			continue;
		}
		const char *at = s;
		if (!strncmp(s, "<!--", 4)) {
			const char *e = strstr(s + 4, "-->");
			s = e ? e + 3 : end;
			continue;
		}
		if (!strncmp(s, "<![CDATA[", 9)) {
			const char *t = s + 9, *e = strstr(t, "]]>");
			if (!e) e = end;
			if (depth > 0 && keeps_text(p, stack[depth - 1])) {
				int32_t i = add_node(p, stack[depth - 1], &last[depth]);
				if (i < 0) return fail(p, (size_t)(t - p->src), "too many elements");
				p->nodes.data[i].kind = N_TEXT;
				p->nodes.data[i].name = arena_str(p, t, (size_t)(e - t));
			}
			s = e < end ? e + 3 : end;
			continue;
		}
		if (s[1] == '?') {
			const char *e = strstr(s + 2, "?>");
			s = e ? e + 2 : end;
			continue;
		}
		if (s[1] == '!') {
			// A DOCTYPE, its internal subset skipped (its entities aren't
			// expanded).
			int br = 0;
			for (s += 2; s < end; s++) {
				if (*s == '[') br++;
				else if (*s == ']') br--;
				else if (*s == '>' && br <= 0) break;
			}
			if (s < end) s++;
			continue;
		}
		if (s[1] == '/') {
			s += 2;
			const char *nm = s;
			while (s < end && name_char(*s)) s++;
			size_t nn = (size_t)(s - nm);
			while (s < end && *s != '>') s++;
			if (s < end) s++;
			const char *colon = memchr(nm, ':', nn);
			if (colon && colon - nm == 3 && !memcmp(nm, "svg", 3)) nn -= 4, nm += 4;
			for (int d = depth - 1; d >= 0; d--) {
				const char *on = p->nodes.data[stack[d]].name;
				if (strlen(on) == nn && !memcmp(on, nm, nn)) {
					depth = d;
					break;
				}
			}
			if (rooted && depth == 0) return;
			continue;
		}
		// An element.
		s++;
		const char *nm = s;
		while (s < end && name_char(*s)) s++;
		if (s == nm) return fail(p, (size_t)(at - p->src), "a '<' that starts no element");
		size_t nn = (size_t)(s - nm);
		if (depth >= MAX_DEPTH) return fail(p, (size_t)(at - p->src), "elements nested too deep");
		int32_t parent = depth > 0 ? stack[depth - 1] : -1;
		if (!rooted && depth == 0) rooted = true;
		int32_t i = add_node(p, parent, depth > 0 ? &last[depth] : &(int32_t){-1});
		if (i < 0) return fail(p, (size_t)(at - p->src), "too many elements");
		const char *colon = memchr(nm, ':', nn);
		bool foreign = false;
		if (colon) {
			if (colon - nm == 3 && !memcmp(nm, "svg", 3)) nm += 4, nn -= 4;
			else foreign = true;
		}
		node *nd = &p->nodes.data[i];
		nd->kind = N_ELEM;
		nd->name = arena_str(p, nm, nn);
		nd->tag = foreign ? T_UNKNOWN : (uint8_t)tag_of(nd->name);
		nd->a0 = p->attrs.len;
		bool closed = false;
		for (;;) {
			while (s < end && is_space(*s)) s++;
			if (s >= end) return fail(p, (size_t)(at - p->src), "an element that doesn't end");
			if (*s == '>') {
				s++;
				break;
			}
			if (*s == '/' && s + 1 < end && s[1] == '>') {
				s += 2;
				closed = true;
				break;
			}
			const char *an = s;
			while (s < end && name_char(*s)) s++;
			if (s == an) {
				s++;
				continue;
			}
			size_t anl = (size_t)(s - an);
			while (s < end && is_space(*s)) s++;
			const char *v = "";
			if (s < end && *s == '=') {
				s++;
				while (s < end && is_space(*s)) s++;
				if (s < end && (*s == '"' || *s == '\'')) {
					char q = *s++;
					const char *vs = s;
					while (s < end && *s != q) s++;
					if (s >= end) return fail(p, (size_t)(an - p->src), "an attribute's value that doesn't end");
					v = decoded(p, vs, (size_t)(s - vs));
					s++;
				} else {
					const char *vs = s;
					while (s < end && !is_space(*s) && *s != '>') s++;
					v = decoded(p, vs, (size_t)(s - vs));
				}
			}
			const char *ac = memchr(an, ':', anl);
			attr a = {NULL, v};
			if (ac && ac - an == 5 && !memcmp(an, "xlink", 5)) a.name = arena_str(p, ac + 1, anl - 6);
			else a.name = arena_str(p, an, anl);
			ak_vec_push(&p->attrs, a);
		}
		nd = &p->nodes.data[i];
		nd->na = p->attrs.len - nd->a0;
		if (depth == 0 && nd->tag != T_SVG) return fail(p, (size_t)(at - p->src), "not an SVG document (its root isn't <svg>)");
		if (!closed) {
			stack[depth] = i;
			depth++;
			last[depth] = -1;
		} else if (depth == 0) {
			return;
		}
	}
	if (!rooted && !p->failed) fail(p, p->n, "not an SVG document (no <svg>)");
}

static const char *attr_of(const P *p, int32_t i, const char *name) {
	const node *nd = &p->nodes.data[i];
	for (uint32_t k = 0; k < nd->na; k++)
		if (strcmp(p->attrs.data[nd->a0 + k].name, name) == 0) return p->attrs.data[nd->a0 + k].value;
	return NULL;
}

// ---- Ids ----

static uint32_t hash_str(const char *s, size_t n) {
	uint32_t h = 2166136261u;
	for (size_t i = 0; i < n; i++) h = (h ^ (uint8_t)s[i]) * 16777619u;
	return h;
}

static void index_ids(P *p) {
	uint32_t n = 0;
	for (uint32_t i = 0; i < p->nodes.len; i++)
		if (p->nodes.data[i].kind == N_ELEM && attr_of(p, (int32_t)i, "id")) n++;
	uint32_t cap = 16;
	while (cap < n * 2) cap <<= 1;
	p->ids = ak_alloc_raw(cap * sizeof(int32_t));
	for (uint32_t i = 0; i < cap; i++) p->ids[i] = -1;
	p->idmask = cap - 1;
	for (uint32_t i = 0; i < p->nodes.len; i++) {
		if (p->nodes.data[i].kind != N_ELEM) continue;
		const char *id = attr_of(p, (int32_t)i, "id");
		if (!id || !*id) continue;
		uint32_t h = hash_str(id, strlen(id)) & p->idmask;
		for (;; h = (h + 1) & p->idmask) {
			int32_t k = p->ids[h];
			if (k < 0) {
				p->ids[h] = (int32_t)i;
				break;
			}
			if (strcmp(attr_of(p, k, "id"), id) == 0) break;  // the first keeps it
		}
	}
}

static int32_t find_id(const P *p, const char *id, size_t n) {
	if (!p->ids || n == 0) return -1;
	for (uint32_t h = hash_str(id, n) & p->idmask;; h = (h + 1) & p->idmask) {
		int32_t k = p->ids[h];
		if (k < 0) return -1;
		const char *kid = attr_of(p, k, "id");
		if (strlen(kid) == n && !memcmp(kid, id, n)) return k;
	}
}

// href is what an href ("#id") names in the document; -1 for anything
// else (outside it: never fetched).
static int32_t href_of(const P *p, int32_t i) {
	const char *h = attr_of(p, i, "href");
	if (!h) return -1;
	while (is_space(*h)) h++;
	if (*h != '#') return -1;
	size_t n = strlen(h + 1);
	while (n > 0 && is_space(h[n])) n--;
	return find_id(p, h + 1, n);
}

// use_ref is what use element i draws: -1 for nothing (none, itself or
// what holds it, or what's being drawn: a cycle).
static int32_t use_ref(const P *p, int32_t i) {
	int32_t ref = href_of(p, i);
	if (ref < 0 || p->nodes.data[ref].busy) return -1;
	for (int32_t a = i; a >= 0; a = p->nodes.data[a].parent)
		if (a == ref) return -1;
	return ref;
}

// url_ref is what a url(#id) at *s names (-1: none, or outside the
// document), s moved past it.
static int32_t url_ref(const P *p, const char **s) {
	const char *q = *s;
	while (is_space(*q)) q++;
	if (strncmp(q, "url(", 4) != 0) return -1;
	q += 4;
	while (is_space(*q)) q++;
	char quote = 0;
	if (*q == '"' || *q == '\'') quote = *q++;
	const char *id = q;
	while (*q && *q != ')' && (!quote || *q != quote)) q++;
	size_t n = (size_t)(q - id);
	if (quote && *q == quote) q++;
	while (*q && *q != ')') q++;
	if (*q == ')') q++;
	*s = q;
	while (n > 0 && is_space(id[n - 1])) n--;
	if (n < 2 || id[0] != '#') return -1;
	return find_id(p, id + 1, n - 1);
}

// ---- Declarations (style attributes, CSS blocks) ----

static int prop_of(const char *name, size_t n) {
	for (int i = 0; i < PR_COUNT; i++)
		if (strlen(prop_names[i]) == n && !memcmp(prop_names[i], name, n)) return i;
	return -1;
}

static char *trim(char *s) {
	while (is_space(*s)) s++;
	size_t n = strlen(s);
	while (n > 0 && is_space(s[n - 1])) s[--n] = 0;
	return s;
}

// decls reads "name: value; ..." (s, changed in place) onto v.
static void parse_decls(char *s, void *vec) {
	AK_VEC(decl) *v = vec;
	while (*s) {
		char *semi = s;
		int paren = 0;
		while (*semi && (*semi != ';' || paren > 0)) {
			if (*semi == '(') paren++;
			else if (*semi == ')' && paren > 0) paren--;
			semi++;
		}
		char save = *semi;
		*semi = 0;
		char *colon = strchr(s, ':');
		if (colon) {
			*colon = 0;
			char *name = trim(s), *val = trim(colon + 1);
			char *imp = strstr(val, "!important");
			if (imp) *imp = 0, val = trim(val);
			int pr = prop_of(name, strlen(name));
			if (pr >= 0 && *val) ak_vec_push(v, ((decl){(uint8_t)pr, val}));
		}
		if (!save) break;
		s = semi + 1;
	}
}

// ---- CSS ----

static bool ident_char(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || (uint8_t)c >= 0x80; }

// parse_selector reads one selector (sel, NUL-terminated) as compounds;
// false for what it doesn't read (pseudo-classes, attributes, siblings).
static bool parse_selector(P *p, const char *sel, uint32_t *p0, uint32_t *np, uint32_t *spec) {
	uint32_t start = p->parts.len, ids = 0, cls = 0, tags = 0;
	const char *s = sel;
	char comb = 0;
	for (;;) {
		bool sp = false;
		while (is_space(*s)) s++, sp = true;
		if (!*s) break;
		if (*s == '>') {
			comb = '>';
			s++;
			continue;
		}
		if (sp && p->parts.len > start && !comb) comb = ' ';
		if (p->parts.len > start && !comb) goto bad;
		part pt = {.comb = p->parts.len > start ? comb : 0};
		comb = 0;
		bool any = false;
		if (*s == '*') s++, any = true;
		else if (ident_char(*s)) {
			const char *t = s;
			while (ident_char(*s)) s++;
			pt.tag = arena_str(p, t, (size_t)(s - t));
			tags++;
			any = true;
		}
		while (*s == '.' || *s == '#') {
			char k = *s++;
			const char *t = s;
			while (ident_char(*s)) s++;
			if (s == t) goto bad;
			const char *id = arena_str(p, t, (size_t)(s - t));
			if (k == '#') pt.id = id, ids++;
			else if (pt.ncls < 4) pt.cls[pt.ncls++] = id, cls++;
			else goto bad;
			any = true;
		}
		if (!any) goto bad;
		if (*s && !is_space(*s) && *s != '>') goto bad;
		if (p->parts.len - start >= 8) goto bad;
		ak_vec_push(&p->parts, pt);
	}
	if (p->parts.len == start || comb) goto bad;
	*p0 = start, *np = p->parts.len - start;
	*spec = ids * 10000 + cls * 100 + tags;
	return true;
bad:
	p->parts.len = start;
	return false;
}

static void css(P *p, char *s) {
	// Comments out.
	for (char *c = strstr(s, "/*"); c; c = strstr(c, "/*")) {
		char *e = strstr(c + 2, "*/");
		char *z = e ? e + 2 : c + strlen(c);
		memset(c, ' ', (size_t)(z - c));
		c = z;
	}
	while (*s) {
		while (is_space(*s)) s++;
		if (!*s) break;
		if (*s == '@') {
			// An at-rule: its block or its statement skipped.
			while (*s && *s != ';' && *s != '{') s++;
			if (*s == '{') {
				int d = 0;
				for (; *s; s++) {
					if (*s == '{') d++;
					else if (*s == '}' && --d == 0) {
						s++;
						break;
					}
				}
			} else if (*s) s++;
			continue;
		}
		char *open = strchr(s, '{');
		if (!open) break;
		char *close = strchr(open, '}');
		*open = 0;
		if (close) *close = 0;
		uint32_t d0 = p->rdecls.len;
		parse_decls(open + 1, &p->rdecls);
		uint32_t nd = p->rdecls.len - d0;
		// Each selector of its list a rule.
		for (char *sel = s; sel;) {
			char *comma = strchr(sel, ',');
			if (comma) *comma = 0;
			rule r = {.order = p->rules.len, .d0 = d0, .nd = nd};
			if (nd && parse_selector(p, sel, &r.p0, &r.np, &r.spec)) ak_vec_push(&p->rules, r);
			sel = comma ? comma + 1 : NULL;
		}
		if (!close) break;
		s = close + 1;
	}
}

static bool has_class(const char *list, const char *c) {
	size_t n = strlen(c);
	for (const char *s = list; *s;) {
		while (is_space(*s)) s++;
		const char *t = s;
		while (*s && !is_space(*s)) s++;
		if ((size_t)(s - t) == n && !memcmp(t, c, n)) return true;
	}
	return false;
}

static bool part_match(const P *p, int32_t i, const part *pt) {
	const node *nd = &p->nodes.data[i];
	if (nd->kind != N_ELEM) return false;
	if (pt->tag && strcmp(pt->tag, nd->name) != 0) return false;
	if (pt->id && !streq(attr_of(p, i, "id"), pt->id)) return false;
	if (pt->ncls) {
		const char *cl = attr_of(p, i, "class");
		if (!cl) return false;
		for (int k = 0; k < pt->ncls; k++)
			if (!has_class(cl, pt->cls[k])) return false;
	}
	return true;
}

static bool sel_match(const P *p, int32_t i, const part *parts, int k) {
	if (i < 0 || !part_match(p, i, &parts[k])) return false;
	if (k == 0) return true;
	int32_t up = p->nodes.data[i].parent;
	if (parts[k].comb == '>') return sel_match(p, up, parts, k - 1);
	for (; up >= 0; up = p->nodes.data[up].parent)
		if (sel_match(p, up, parts, k - 1)) return true;
	return false;
}

// cascade gives each element its declarations, in the order they win
// in: presentation attributes, then the style sheets' rules by
// specificity and order, then its style attribute.
static void cascade(P *p) {
	AK_VEC(uint32_t) hits = {0};
	for (uint32_t i = 0; i < p->nodes.len; i++) {
		node *nd = &p->nodes.data[i];
		if (nd->kind != N_ELEM) continue;
		uint32_t d0 = p->decls.len;
		for (uint32_t k = 0; k < nd->na; k++) {
			const attr *a = &p->attrs.data[nd->a0 + k];
			int pr = prop_of(a->name, strlen(a->name));
			if (pr >= 0 && *a->value) ak_vec_push(&p->decls, ((decl){(uint8_t)pr, a->value}));
		}
		hits.len = 0;
		for (uint32_t r = 0; r < p->rules.len; r++) {
			const rule *ru = &p->rules.data[r];
			if (!sel_match(p, (int32_t)i, &p->parts.data[ru->p0], (int)ru->np - 1)) continue;
			// Inserted in order of specificity, then of the sheet.
			ak_vec_push(&hits, r);
			for (uint32_t j = hits.len - 1; j > 0; j--) {
				const rule *a = &p->rules.data[hits.data[j - 1]];
				if (a->spec <= ru->spec) break;
				uint32_t t = hits.data[j];
				hits.data[j] = hits.data[j - 1], hits.data[j - 1] = t;
			}
		}
		for (uint32_t h = 0; h < hits.len; h++) {
			const rule *ru = &p->rules.data[hits.data[h]];
			for (uint32_t k = 0; k < ru->nd; k++) ak_vec_push(&p->decls, p->rdecls.data[ru->d0 + k]);
		}
		const char *st = attr_of(p, (int32_t)i, "style");
		if (st && *st) parse_decls(arena_str(p, st, strlen(st)), &p->decls);
		nd = &p->nodes.data[i];
		nd->d0 = d0, nd->nd = p->decls.len - d0;
	}
	ak_vec_free(&hits);
}

static const char *prop(const P *p, int32_t i, int pr) {
	const node *nd = &p->nodes.data[i];
	for (uint32_t k = nd->nd; k > 0; k--) {
		const decl *d = &p->decls.data[nd->d0 + k - 1];
		if (d->prop == pr) return strcmp(d->value, "inherit") == 0 ? NULL : d->value;
	}
	return NULL;
}

// ---- Numbers, lengths, colors ----

static void skip_sep(const char **s) {
	while (is_space(**s) || **s == ',') (*s)++;
}

// num reads a number ("1.5.5" is 1.5 then .5, "1e-3" one, "2-1" two),
// after spaces and a comma.
static bool num(const char **s, double *out) {
	const char *q = *s;
	skip_sep(&q);
	bool neg = false;
	if (*q == '+' || *q == '-') neg = *q++ == '-';
	double v = 0;
	int digits = 0, e10 = 0;
	for (; *q >= '0' && *q <= '9'; q++, digits++) {
		if (digits < 18) v = v * 10 + (*q - '0');
		else e10++;
	}
	if (*q == '.') {
		for (q++; *q >= '0' && *q <= '9'; q++, digits++)
			if (digits < 18) v = v * 10 + (*q - '0'), e10--;
	}
	if (!digits) return false;
	if (*q == 'e' || *q == 'E') {
		const char *e = q + 1;
		bool en = false;
		if (*e == '+' || *e == '-') en = *e++ == '-';
		if (*e >= '0' && *e <= '9') {
			int x = 0;
			for (; *e >= '0' && *e <= '9'; e++)
				if (x < 1000) x = x * 10 + (*e - '0');
			e10 += en ? -x : x;
			q = e;
		}
	}
	if (e10 > 60) e10 = 60;
	if (e10 < -60) e10 = -60;
	v *= pow(10, e10);
	if (v > 1e30) v = 1e30;
	*out = neg ? -v : v;
	*s = q;
	return true;
}

enum { U_NUM, U_PX, U_PT, U_PC, U_MM, U_CM, U_IN, U_EM, U_EX, U_PCT };

typedef struct {
	double v;
	uint8_t unit;
} len;

static bool parse_len_at(const char **s, len *l) {
	if (!num(s, &l->v)) return false;
	const char *q = *s;
	l->unit = U_NUM;
	if (*q == '%') l->unit = U_PCT, q++;
	else if (q[0] && q[1]) {
		static const char units[][3] = {"px", "pt", "pc", "mm", "cm", "in", "em", "ex"};
		for (int k = 0; k < 8; k++)
			if (q[0] == units[k][0] && q[1] == units[k][1]) {
				l->unit = (uint8_t)(U_PX + k);
				q += 2;
				break;
			}
	}
	*s = q;
	return true;
}

static bool parse_len(const char *s, len *l) {
	if (!s) return false;
	const char *q = s;
	if (!parse_len_at(&q, l)) return false;
	while (is_space(*q)) q++;
	return *q == 0;
}

// px is a length in user units; a percentage is of of, an em of font.
static double px(len l, double of, double font) {
	switch (l.unit) {
	case U_PT: return l.v * 4 / 3;
	case U_PC: return l.v * 16;
	case U_MM: return l.v * 96 / 25.4;
	case U_CM: return l.v * 96 / 2.54;
	case U_IN: return l.v * 96;
	case U_EM: return l.v * font;
	case U_EX: return l.v * font / 2;
	case U_PCT: return l.v * of / 100;
	}
	return l.v;
}

static const struct {
	const char *name;
	uint32_t rgb;
} named[] = {
	{"aliceblue", 0xf0f8ff}, {"antiquewhite", 0xfaebd7}, {"aqua", 0x00ffff}, {"aquamarine", 0x7fffd4},
	{"azure", 0xf0ffff}, {"beige", 0xf5f5dc}, {"bisque", 0xffe4c4}, {"black", 0x000000},
	{"blanchedalmond", 0xffebcd}, {"blue", 0x0000ff}, {"blueviolet", 0x8a2be2}, {"brown", 0xa52a2a},
	{"burlywood", 0xdeb887}, {"cadetblue", 0x5f9ea0}, {"chartreuse", 0x7fff00}, {"chocolate", 0xd2691e},
	{"coral", 0xff7f50}, {"cornflowerblue", 0x6495ed}, {"cornsilk", 0xfff8dc}, {"crimson", 0xdc143c},
	{"cyan", 0x00ffff}, {"darkblue", 0x00008b}, {"darkcyan", 0x008b8b}, {"darkgoldenrod", 0xb8860b},
	{"darkgray", 0xa9a9a9}, {"darkgreen", 0x006400}, {"darkgrey", 0xa9a9a9}, {"darkkhaki", 0xbdb76b},
	{"darkmagenta", 0x8b008b}, {"darkolivegreen", 0x556b2f}, {"darkorange", 0xff8c00}, {"darkorchid", 0x9932cc},
	{"darkred", 0x8b0000}, {"darksalmon", 0xe9967a}, {"darkseagreen", 0x8fbc8f}, {"darkslateblue", 0x483d8b},
	{"darkslategray", 0x2f4f4f}, {"darkslategrey", 0x2f4f4f}, {"darkturquoise", 0x00ced1}, {"darkviolet", 0x9400d3},
	{"deeppink", 0xff1493}, {"deepskyblue", 0x00bfff}, {"dimgray", 0x696969}, {"dimgrey", 0x696969},
	{"dodgerblue", 0x1e90ff}, {"firebrick", 0xb22222}, {"floralwhite", 0xfffaf0}, {"forestgreen", 0x228b22},
	{"fuchsia", 0xff00ff}, {"gainsboro", 0xdcdcdc}, {"ghostwhite", 0xf8f8ff}, {"gold", 0xffd700},
	{"goldenrod", 0xdaa520}, {"gray", 0x808080}, {"green", 0x008000}, {"greenyellow", 0xadff2f},
	{"grey", 0x808080}, {"honeydew", 0xf0fff0}, {"hotpink", 0xff69b4}, {"indianred", 0xcd5c5c},
	{"indigo", 0x4b0082}, {"ivory", 0xfffff0}, {"khaki", 0xf0e68c}, {"lavender", 0xe6e6fa},
	{"lavenderblush", 0xfff0f5}, {"lawngreen", 0x7cfc00}, {"lemonchiffon", 0xfffacd}, {"lightblue", 0xadd8e6},
	{"lightcoral", 0xf08080}, {"lightcyan", 0xe0ffff}, {"lightgoldenrodyellow", 0xfafad2}, {"lightgray", 0xd3d3d3},
	{"lightgreen", 0x90ee90}, {"lightgrey", 0xd3d3d3}, {"lightpink", 0xffb6c1}, {"lightsalmon", 0xffa07a},
	{"lightseagreen", 0x20b2aa}, {"lightskyblue", 0x87cefa}, {"lightslategray", 0x778899}, {"lightslategrey", 0x778899},
	{"lightsteelblue", 0xb0c4de}, {"lightyellow", 0xffffe0}, {"lime", 0x00ff00}, {"limegreen", 0x32cd32},
	{"linen", 0xfaf0e6}, {"magenta", 0xff00ff}, {"maroon", 0x800000}, {"mediumaquamarine", 0x66cdaa},
	{"mediumblue", 0x0000cd}, {"mediumorchid", 0xba55d3}, {"mediumpurple", 0x9370db}, {"mediumseagreen", 0x3cb371},
	{"mediumslateblue", 0x7b68ee}, {"mediumspringgreen", 0x00fa9a}, {"mediumturquoise", 0x48d1cc}, {"mediumvioletred", 0xc71585},
	{"midnightblue", 0x191970}, {"mintcream", 0xf5fffa}, {"mistyrose", 0xffe4e1}, {"moccasin", 0xffe4b5},
	{"navajowhite", 0xffdead}, {"navy", 0x000080}, {"oldlace", 0xfdf5e6}, {"olive", 0x808000},
	{"olivedrab", 0x6b8e23}, {"orange", 0xffa500}, {"orangered", 0xff4500}, {"orchid", 0xda70d6},
	{"palegoldenrod", 0xeee8aa}, {"palegreen", 0x98fb98}, {"paleturquoise", 0xafeeee}, {"palevioletred", 0xdb7093},
	{"papayawhip", 0xffefd5}, {"peachpuff", 0xffdab9}, {"peru", 0xcd853f}, {"pink", 0xffc0cb},
	{"plum", 0xdda0dd}, {"powderblue", 0xb0e0e6}, {"purple", 0x800080}, {"rebeccapurple", 0x663399},
	{"red", 0xff0000}, {"rosybrown", 0xbc8f8f}, {"royalblue", 0x4169e1}, {"saddlebrown", 0x8b4513},
	{"salmon", 0xfa8072}, {"sandybrown", 0xf4a460}, {"seagreen", 0x2e8b57}, {"seashell", 0xfff5ee},
	{"sienna", 0xa0522d}, {"silver", 0xc0c0c0}, {"skyblue", 0x87ceeb}, {"slateblue", 0x6a5acd},
	{"slategray", 0x708090}, {"slategrey", 0x708090}, {"snow", 0xfffafa}, {"springgreen", 0x00ff7f},
	{"steelblue", 0x4682b4}, {"tan", 0xd2b48c}, {"teal", 0x008080}, {"thistle", 0xd8bfd8},
	{"tomato", 0xff6347}, {"turquoise", 0x40e0d0}, {"violet", 0xee82ee}, {"wheat", 0xf5deb3},
	{"white", 0xffffff}, {"whitesmoke", 0xf5f5f5}, {"yellow", 0xffff00}, {"yellowgreen", 0x9acd32},
};

enum { C_INVALID, C_NONE, C_RGBA, C_CURRENT };

static int hexd(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

static uint8_t clamp8(double v) { return v <= 0 ? 0 : v >= 255 ? 255 : (uint8_t)(v + 0.5); }

static double hue(double p, double q, double t) {
	if (t < 0) t += 1;
	if (t > 1) t -= 1;
	if (t < 1.0 / 6) return p + (q - p) * 6 * t;
	if (t < 0.5) return q;
	if (t < 2.0 / 3) return p + (q - p) * (2.0 / 3 - t) * 6;
	return p;
}

// color reads a color (s, its end at *end or a NUL): C_RGBA and *rgba
// (0xRRGGBBAA), C_CURRENT, C_NONE, or C_INVALID.
static int color_at(const char **sp, uint32_t *rgba) {
	const char *s = *sp;
	while (is_space(*s)) s++;
	if (*s == '#') {
		const char *h = ++s;
		while (hexd(*s) >= 0) s++;
		size_t n = (size_t)(s - h);
		uint32_t v = 0;
		for (size_t i = 0; i < n; i++) v = v << 4 | (uint32_t)hexd(h[i]);
		*sp = s;
		switch (n) {
		case 3: *rgba = ((v >> 8 & 15) * 17) << 24 | ((v >> 4 & 15) * 17) << 16 | ((v & 15) * 17) << 8 | 255; return C_RGBA;
		case 4: *rgba = ((v >> 12 & 15) * 17) << 24 | ((v >> 8 & 15) * 17) << 16 | ((v >> 4 & 15) * 17) << 8 | (v & 15) * 17; return C_RGBA;
		case 6: *rgba = v << 8 | 255; return C_RGBA;
		case 8: *rgba = v; return C_RGBA;
		}
		return C_INVALID;
	}
	const char *w = s;
	while (ident_char(*s)) s++;
	size_t n = (size_t)(s - w);
	char name[32];
	if (n == 0 || n >= sizeof name) return C_INVALID;
	for (size_t i = 0; i < n; i++) name[i] = w[i] >= 'A' && w[i] <= 'Z' ? (char)(w[i] + 32) : w[i];
	name[n] = 0;
	*sp = s;
	if (*s == '(') {
		bool rgb = !strcmp(name, "rgb") || !strcmp(name, "rgba"), hsl = !strcmp(name, "hsl") || !strcmp(name, "hsla");
		if (!rgb && !hsl) return C_INVALID;
		const char *q = s + 1;
		double c[4] = {0, 0, 0, 1};
		int k = 0;
		for (; k < 4; k++) {
			while (is_space(*q) || *q == ',' || *q == '/') q++;
			if (*q == ')') break;
			if (!num(&q, &c[k])) return C_INVALID;
			bool pct = *q == '%';
			if (pct) q++;
			else while (ident_char(*q)) q++;  // hsl's deg
			if (k < 3 && rgb) c[k] = pct ? c[k] * 2.55 : c[k];
			else if (k < 3 && k > 0) c[k] = c[k] / 100;
			else if (k == 3 && pct) c[k] /= 100;
		}
		while (*q && *q != ')') q++;
		if (*q != ')' || k < 3) return C_INVALID;
		*sp = q + 1;
		uint8_t a = clamp8(c[3] * 255);
		if (rgb) {
			*rgba = (uint32_t)clamp8(c[0]) << 24 | (uint32_t)clamp8(c[1]) << 16 | (uint32_t)clamp8(c[2]) << 8 | a;
		} else {
			double h = fmod(c[0], 360) / 360, sat = c[1] < 0 ? 0 : c[1] > 1 ? 1 : c[1], l = c[2] < 0 ? 0 : c[2] > 1 ? 1 : c[2];
			if (h < 0) h += 1;
			double qv = l < 0.5 ? l * (1 + sat) : l + sat - l * sat, pv = 2 * l - qv;
			*rgba = (uint32_t)clamp8(hue(pv, qv, h + 1.0 / 3) * 255) << 24 | (uint32_t)clamp8(hue(pv, qv, h) * 255) << 16 |
			        (uint32_t)clamp8(hue(pv, qv, h - 1.0 / 3) * 255) << 8 | a;
		}
		return C_RGBA;
	}
	if (!strcmp(name, "none")) return C_NONE;
	if (!strcmp(name, "currentcolor")) return C_CURRENT;
	if (!strcmp(name, "transparent")) {
		*rgba = 0;
		return C_RGBA;
	}
	size_t lo = 0, hi = AK_LEN(named);
	while (lo < hi) {
		size_t mid = (lo + hi) / 2;
		int c = strcmp(name, named[mid].name);
		if (c == 0) {
			*rgba = named[mid].rgb << 8 | 255;
			return C_RGBA;
		}
		if (c < 0) hi = mid;
		else lo = mid + 1;
	}
	return C_INVALID;
}

static int color(const char *s, uint32_t *rgba) {
	if (!s) return C_INVALID;
	int k = color_at(&s, rgba);
	while (is_space(*s)) s++;
	// "#fff icc-color(...)": the sRGB color.
	return k;
}

// ---- Matrices: x' = a x + c y + e, y' = b x + d y + f ----

typedef double mat[6];

static const mat identity = {1, 0, 0, 1, 0, 0};

// mul sets r to m n (n applied first).
static void mul(mat r, const mat m, const mat n) {
	mat t = {
		m[0] * n[0] + m[2] * n[1], m[1] * n[0] + m[3] * n[1], m[0] * n[2] + m[2] * n[3],
		m[1] * n[2] + m[3] * n[3], m[0] * n[4] + m[2] * n[5] + m[4], m[1] * n[4] + m[3] * n[5] + m[5],
	};
	memcpy(r, t, sizeof t);
}

static bool invert(mat r, const mat m) {
	double det = m[0] * m[3] - m[1] * m[2];
	if (!(fabs(det) > 1e-12)) return false;
	mat t = {m[3] / det, -m[1] / det, -m[2] / det, m[0] / det, (m[2] * m[5] - m[3] * m[4]) / det, (m[1] * m[4] - m[0] * m[5]) / det};
	memcpy(r, t, sizeof t);
	return true;
}

static bool is_identity(const mat m) { return m[0] == 1 && m[1] == 0 && m[2] == 0 && m[3] == 1 && m[4] == 0 && m[5] == 0; }

static void apply(const mat m, double x, double y, double *ox, double *oy) {
	*ox = m[0] * x + m[2] * y + m[4];
	*oy = m[1] * x + m[3] * y + m[5];
}

// angle reads an angle's unit after its number: degrees by default.
static double angle_unit(const char **s, double v) {
	const char *q = *s;
	if (!strncmp(q, "deg", 3)) q += 3;
	else if (!strncmp(q, "grad", 4)) q += 4, v = v * 0.9;
	else if (!strncmp(q, "rad", 3)) q += 3, v = v * 180 / SVG_PI;
	else if (!strncmp(q, "turn", 4)) q += 4, v = v * 360;
	*s = q;
	return v;
}

// parse_transform reads a transform list (an attribute's, or CSS's
// transform with its units); false, m the identity, for one it can't.
static bool parse_transform(const char *s, mat m) {
	memcpy(m, identity, sizeof(mat));
	if (!s) return false;
	bool any = false;
	for (;;) {
		skip_sep(&s);
		if (!*s) return any;
		const char *f = s;
		while ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z')) s++;
		size_t fn = (size_t)(s - f);
		while (is_space(*s)) s++;
		if (*s != '(' || fn == 0) goto bad;
		s++;
		double a[6];
		int n = 0;
		while (n < 6) {
			double v;
			const char *q = s;
			if (!num(&q, &v)) break;
			if (fn == 6 && !strncmp(f, "rotate", 6) && n == 0) v = angle_unit(&q, v);
			else if (fn == 5 && !strncmp(f, "skew", 4)) v = angle_unit(&q, v);
			else if (q[0] == 'p' && q[1] == 'x') q += 2;
			a[n++] = v;
			s = q;
		}
		skip_sep(&s);
		if (*s != ')') goto bad;
		s++;
		mat t;
		memcpy(t, identity, sizeof t);
		if (fn == 6 && !strncmp(f, "matrix", 6) && n == 6) memcpy(t, a, sizeof t);
		else if (fn == 9 && !strncmp(f, "translate", 9) && (n == 1 || n == 2)) t[4] = a[0], t[5] = n == 2 ? a[1] : 0;
		else if (fn == 5 && !strncmp(f, "scale", 5) && (n == 1 || n == 2)) t[0] = a[0], t[3] = n == 2 ? a[1] : a[0];
		else if (fn == 6 && !strncmp(f, "rotate", 6) && (n == 1 || n == 3)) {
			double r = a[0] * SVG_PI / 180, c = cos(r), sn = sin(r);
			t[0] = c, t[1] = sn, t[2] = -sn, t[3] = c;
			if (n == 3) {
				t[4] = a[1] - c * a[1] + sn * a[2];
				t[5] = a[2] - sn * a[1] - c * a[2];
			}
		} else if (fn == 5 && !strncmp(f, "skewX", 5) && n == 1) t[2] = tan(a[0] * SVG_PI / 180);
		else if (fn == 5 && !strncmp(f, "skewY", 5) && n == 1) t[1] = tan(a[0] * SVG_PI / 180);
		else if (fn == 10 && !strncmp(f, "translateX", 10) && n == 1) t[4] = a[0];
		else if (fn == 10 && !strncmp(f, "translateY", 10) && n == 1) t[5] = a[0];
		else goto bad;
		mul(m, m, t);
		any = true;
	}
bad:
	memcpy(m, identity, sizeof(mat));
	return false;
}

// ---- Paths: Shape's Path format, in user units ----

static void pb_op(ak_buf *b, uint8_t op, const double *a, int n) {
	ak_buf_u8(b, op);
	for (int i = 0; i < n; i++) ak_buf_f32(b, (float)a[i]);
}

static void pb_move(ak_buf *b, double x, double y) { pb_op(b, AK_PATH_OP_MOVE, (double[]){x, y}, 2); }
static void pb_line(ak_buf *b, double x, double y) { pb_op(b, AK_PATH_OP_LINE, (double[]){x, y}, 2); }
static void pb_quad(ak_buf *b, double a, double c, double x, double y) { pb_op(b, AK_PATH_OP_QUAD, (double[]){a, c, x, y}, 4); }
static void pb_cubic(ak_buf *b, double a, double c, double d, double e, double x, double y) {
	pb_op(b, AK_PATH_OP_CUBIC, (double[]){a, c, d, e, x, y}, 6);
}
static void pb_close(ak_buf *b) { ak_buf_u8(b, AK_PATH_OP_CLOSE); }

static int path_args(uint8_t op) {
	switch (op) {
	case AK_PATH_OP_MOVE:
	case AK_PATH_OP_LINE: return 2;
	case AK_PATH_OP_QUAD:
	case AK_PATH_OP_RECT:
	case AK_PATH_OP_ELLIPSE: return 4;
	case AK_PATH_OP_ROUNDED_RECT: return 5;
	case AK_PATH_OP_CUBIC:
	case AK_PATH_OP_ARC: return 6;
	case AK_PATH_OP_CLOSE: return 0;
	}
	return -1;
}

static float rdf(const uint8_t *p) {
	float f;
	memcpy(&f, p, 4);
	return f;
}


// xform_path appends path (its moves, lines and curves) through m.
static void xform_path(const uint8_t *p, size_t n, const mat m, ak_buf *out) {
	for (size_t i = 0; i < n;) {
		uint8_t op = p[i++];
		int na = path_args(op);
		if (na < 0 || i + (size_t)na * 4 > n) return;
		ak_buf_u8(out, op);
		for (int k = 0; k + 1 < na; k += 2) {
			double x, y;
			apply(m, rdf(p + i + 4 * k), rdf(p + i + 4 * k + 4), &x, &y);
			ak_buf_f32(out, (float)x);
			ak_buf_f32(out, (float)y);
		}
		i += (size_t)na * 4;
	}
}

typedef struct {
	double x0, y0, x1, y1;
	bool seen;
} box;

static void box_add(box *b, double x, double y) {
	if (!b->seen) {
		b->x0 = b->x1 = x, b->y0 = b->y1 = y, b->seen = true;
		return;
	}
	if (x < b->x0) b->x0 = x;
	if (x > b->x1) b->x1 = x;
	if (y < b->y0) b->y0 = y;
	if (y > b->y1) b->y1 = y;
}

// extrema adds a curve's points where it turns (t in (0, 1)) on one axis:
// a, b, c are its derivative's coefficients.
static void roots(double a, double b, double c, double *t, int *n) {
	*n = 0;
	if (fabs(a) < 1e-12) {
		if (fabs(b) > 1e-12) t[(*n)++] = -c / b;
		return;
	}
	double d = b * b - 4 * a * c;
	if (d < 0) return;
	double sq = sqrt(d);
	t[(*n)++] = (-b + sq) / (2 * a);
	t[(*n)++] = (-b - sq) / (2 * a);
}

// path_box grows b by the path's outline (curves' extremes, not their
// control points).
static void path_box(const uint8_t *p, size_t n, box *b) {
	double cx = 0, cy = 0;
	for (size_t i = 0; i < n;) {
		uint8_t op = p[i++];
		int na = path_args(op);
		if (na < 0 || i + (size_t)na * 4 > n) return;
		double a[6] = {0};
		for (int k = 0; k < na; k++) a[k] = rdf(p + i + 4 * k);
		i += (size_t)na * 4;
		switch (op) {
		case AK_PATH_OP_MOVE:
		case AK_PATH_OP_LINE:
			box_add(b, a[0], a[1]);
			cx = a[0], cy = a[1];
			break;
		case AK_PATH_OP_QUAD:
			for (int ax = 0; ax < 2; ax++) {
				double p0 = ax ? cy : cx, p1 = a[ax], p2 = a[2 + ax], den = p0 - 2 * p1 + p2;
				if (fabs(den) > 1e-12) {
					double t = (p0 - p1) / den;
					if (t > 0 && t < 1) {
						double u = 1 - t;
						double x = u * u * cx + 2 * u * t * a[0] + t * t * a[2], y = u * u * cy + 2 * u * t * a[1] + t * t * a[3];
						box_add(b, x, y);
					}
				}
			}
			box_add(b, a[2], a[3]);
			cx = a[2], cy = a[3];
			break;
		case AK_PATH_OP_CUBIC:
			for (int ax = 0; ax < 2; ax++) {
				double p0 = ax ? cy : cx, p1 = a[ax], p2 = a[2 + ax], p3 = a[4 + ax];
				double t[2];
				int nt;
				roots(3 * (-p0 + 3 * p1 - 3 * p2 + p3), 6 * (p0 - 2 * p1 + p2), 3 * (p1 - p0), t, &nt);
				for (int k = 0; k < nt; k++) {
					if (!(t[k] > 0 && t[k] < 1)) continue;
					double u = 1 - t[k], w0 = u * u * u, w1 = 3 * u * u * t[k], w2 = 3 * u * t[k] * t[k], w3 = t[k] * t[k] * t[k];
					box_add(b, w0 * cx + w1 * a[0] + w2 * a[2] + w3 * a[4], w0 * cy + w1 * a[1] + w2 * a[3] + w3 * a[5]);
				}
			}
			box_add(b, a[4], a[5]);
			cx = a[4], cy = a[5];
			break;
		default: break;
		}
	}
}

#define KAPPA 0.5522847498307936

static void path_rect(ak_buf *b, double x, double y, double w, double h, double rx, double ry) {
	if (rx > w / 2) rx = w / 2;
	if (ry > h / 2) ry = h / 2;
	if (rx <= 0 || ry <= 0) {
		pb_move(b, x, y), pb_line(b, x + w, y), pb_line(b, x + w, y + h), pb_line(b, x, y + h), pb_close(b);
		return;
	}
	double kx = rx * KAPPA, ky = ry * KAPPA;
	pb_move(b, x + rx, y);
	pb_line(b, x + w - rx, y);
	pb_cubic(b, x + w - rx + kx, y, x + w, y + ry - ky, x + w, y + ry);
	pb_line(b, x + w, y + h - ry);
	pb_cubic(b, x + w, y + h - ry + ky, x + w - rx + kx, y + h, x + w - rx, y + h);
	pb_line(b, x + rx, y + h);
	pb_cubic(b, x + rx - kx, y + h, x, y + h - ry + ky, x, y + h - ry);
	pb_line(b, x, y + ry);
	pb_cubic(b, x, y + ry - ky, x + rx - kx, y, x + rx, y);
	pb_close(b);
}

static void path_ellipse(ak_buf *b, double cx, double cy, double rx, double ry) {
	double kx = rx * KAPPA, ky = ry * KAPPA;
	pb_move(b, cx + rx, cy);
	pb_cubic(b, cx + rx, cy + ky, cx + kx, cy + ry, cx, cy + ry);
	pb_cubic(b, cx - kx, cy + ry, cx - rx, cy + ky, cx - rx, cy);
	pb_cubic(b, cx - rx, cy - ky, cx - kx, cy - ry, cx, cy - ry);
	pb_cubic(b, cx + kx, cy - ry, cx + rx, cy - ky, cx + rx, cy);
	pb_close(b);
}

// path_arc adds SVG's elliptical arc from x1, y1 to x2, y2 as cubics:
// SVG 1.1's endpoint-to-center conversion (F.6.5, F.6.6), a curve per
// quarter turn or less.
static void path_arc(ak_buf *b, double x1, double y1, double rx, double ry, double phi, bool large, bool sweep, double x2, double y2) {
	if (x1 == x2 && y1 == y2) return;
	rx = fabs(rx), ry = fabs(ry);
	if (rx == 0 || ry == 0) {
		pb_line(b, x2, y2);
		return;
	}
	double sn = sin(phi * SVG_PI / 180), cs = cos(phi * SVG_PI / 180);
	double dx = (x1 - x2) / 2, dy = (y1 - y2) / 2;
	double x1p = cs * dx + sn * dy, y1p = -sn * dx + cs * dy;
	double l = x1p * x1p / (rx * rx) + y1p * y1p / (ry * ry);
	if (l > 1) rx *= sqrt(l), ry *= sqrt(l);
	double nm = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p;
	double den = rx * rx * y1p * y1p + ry * ry * x1p * x1p;
	double co = den > 0 ? sqrt(fmax(0, nm / den)) : 0;
	if (large == sweep) co = -co;
	double cxp = co * rx * y1p / ry, cyp = -co * ry * x1p / rx;
	double cx = cs * cxp - sn * cyp + (x1 + x2) / 2, cy = sn * cxp + cs * cyp + (y1 + y2) / 2;
	double ux = (x1p - cxp) / rx, uy = (y1p - cyp) / ry, vx = (-x1p - cxp) / rx, vy = (-y1p - cyp) / ry;
	double t1 = atan2(uy, ux), dt = atan2(ux * vy - uy * vx, ux * vx + uy * vy);
	if (!sweep && dt > 0) dt -= 2 * SVG_PI;
	else if (sweep && dt < 0) dt += 2 * SVG_PI;
	int n = (int)ceil(fabs(dt) / (SVG_PI / 2) - 1e-9);
	if (n < 1) n = 1;
	double d = dt / n, k = 4.0 / 3 * tan(d / 4);
	for (int i = 0; i < n; i++) {
		double a = t1 + i * d, z = t1 + (i + 1) * d;
		double sa = sin(a), ca = cos(a), sz = sin(z), cz = cos(z);
		double u1 = ca - k * sa, v1 = sa + k * ca, u2 = cz + k * sz, v2 = sz - k * cz;
		double ex = cx + rx * cs * cz - ry * sn * sz, ey = cy + rx * sn * cz + ry * cs * sz;
		if (i == n - 1) ex = x2, ey = y2;
		pb_cubic(b, cx + rx * cs * u1 - ry * sn * v1, cy + rx * sn * u1 + ry * cs * v1, cx + rx * cs * u2 - ry * sn * v2,
		         cy + rx * sn * u2 + ry * cs * v2, ex, ey);
	}
}

static bool flag(const char **s, bool *out) {
	skip_sep(s);
	if (**s == '0' || **s == '1') {
		*out = **s == '1';
		(*s)++;
		return true;
	}
	return false;
}

static bool nums(const char **s, double *a, int n) {
	for (int i = 0; i < n; i++)
		if (!num(s, &a[i])) return false;
	return true;
}

// path_data adds SVG path data: as SVG says, what comes before an error
// is drawn, the rest isn't.
static void path_data(ak_buf *b, const char *s) {
	double cx = 0, cy = 0, sx = 0, sy = 0, lcx = 0, lcy = 0, a[7];
	char cmd = 0, prev = 0;
	bool started = false, closed = false;
	for (;;) {
		skip_sep(&s);
		if (!*s) return;
		char c = *s;
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) cmd = c, s++;
		else if (!cmd) return;
		bool rel = cmd >= 'a';
		char up = (char)(cmd & ~0x20);
		if (up == 'Z') {
			if (started) pb_close(b);
			cx = sx, cy = sy;
			prev = 'Z', cmd = 0, closed = true;
			continue;
		}
		if (!started && up != 'M') return;
		// A segment after a close starts its subpath where the last began.
		if (closed && up != 'M') pb_move(b, sx, sy);
		closed = false;
		double ox = rel ? cx : 0, oy = rel ? cy : 0;
		switch (up) {
		case 'M':
			if (!nums(&s, a, 2)) return;
			cx = ox + a[0], cy = oy + a[1], sx = cx, sy = cy;
			pb_move(b, cx, cy);
			started = true;
			cmd = rel ? 'l' : 'L';
			break;
		case 'L':
			if (!nums(&s, a, 2)) return;
			cx = ox + a[0], cy = oy + a[1];
			pb_line(b, cx, cy);
			break;
		case 'H':
			if (!nums(&s, a, 1)) return;
			cx = ox + a[0];
			pb_line(b, cx, cy);
			break;
		case 'V':
			if (!nums(&s, a, 1)) return;
			cy = oy + a[0];
			pb_line(b, cx, cy);
			break;
		case 'C':
			if (!nums(&s, a, 6)) return;
			pb_cubic(b, ox + a[0], oy + a[1], ox + a[2], oy + a[3], ox + a[4], oy + a[5]);
			lcx = ox + a[2], lcy = oy + a[3], cx = ox + a[4], cy = oy + a[5];
			break;
		case 'S': {
			if (!nums(&s, a, 4)) return;
			double c1x = cx, c1y = cy;
			if (prev == 'C' || prev == 'S') c1x = 2 * cx - lcx, c1y = 2 * cy - lcy;
			pb_cubic(b, c1x, c1y, ox + a[0], oy + a[1], ox + a[2], oy + a[3]);
			lcx = ox + a[0], lcy = oy + a[1], cx = ox + a[2], cy = oy + a[3];
			break;
		}
		case 'Q':
			if (!nums(&s, a, 4)) return;
			pb_quad(b, ox + a[0], oy + a[1], ox + a[2], oy + a[3]);
			lcx = ox + a[0], lcy = oy + a[1], cx = ox + a[2], cy = oy + a[3];
			break;
		case 'T': {
			if (!nums(&s, a, 2)) return;
			double qx = cx, qy = cy;
			if (prev == 'Q' || prev == 'T') qx = 2 * cx - lcx, qy = 2 * cy - lcy;
			pb_quad(b, qx, qy, ox + a[0], oy + a[1]);
			lcx = qx, lcy = qy, cx = ox + a[0], cy = oy + a[1];
			break;
		}
		case 'A': {
			bool large, sweep;
			if (!nums(&s, a, 3) || !flag(&s, &large) || !flag(&s, &sweep) || !nums(&s, a + 3, 2)) return;
			path_arc(b, cx, cy, a[0], a[1], a[2], large, sweep, ox + a[3], oy + a[4]);
			cx = ox + a[3], cy = oy + a[4];
			break;
		}
		default: return;
		}
		prev = up;
	}
}

static void path_points(ak_buf *b, const char *s, bool close) {
	double a[2];
	int n = 0;
	for (; nums(&s, a, 2); n++) {
		if (n == 0) pb_move(b, a[0], a[1]);
		else pb_line(b, a[0], a[1]);
	}
	if (close && n > 0) pb_close(b);
}

// ---- Drawing ----

enum { P_NONE, P_COLOR, P_CURRENT, P_URL };

typedef struct {
	uint8_t kind;  // P_*
	uint32_t rgba;
	int32_t ref;  // P_URL's paint server
	uint8_t fb;   // P_URL's fallback: P_NONE, P_COLOR, P_CURRENT
	uint32_t fb_rgba;
} paint;

typedef struct {
	paint fill, stroke;
	float fill_op, stroke_op, width, dash_off;
	float dash[MAX_DASH];
	uint8_t ndash, cap, join, fill_rule, clip_rule, weight, anchor;
	bool own_color;  // color is set: currentColor is it, not the document's
	uint32_t color;
	bool hidden, preserve;
	double font, vw, vh;  // the font's size; the viewport, for percentages
} style;

typedef struct {
	uint32_t off;
	float opacity;
} slot;

typedef struct {
	P *p;
	ak_buf *cmds;
	AK_VEC(slot) slots;
	AK_VEC(uint8_t) saved;  // the fill rules Save kept
	ak_buf path, clip, tmp, boxed;
	uint8_t rule;
	uint32_t visits, uses;
	bool over;
} R;

static double diag(const style *s) { return sqrt((s->vw * s->vw + s->vh * s->vh) / 2); }

static bool lattr(const P *p, int32_t i, const char *name, double of, const style *s, double *out) {
	len l;
	if (!parse_len(attr_of(p, i, name), &l)) return false;
	*out = px(l, of, s->font);
	return true;
}

static double lattr_or(const P *p, int32_t i, const char *name, double of, const style *s, double dflt) {
	double v;
	return lattr(p, i, name, of, s, &v) ? v : dflt;
}

static double opacity_of(const char *v) {
	if (!v) return 1;
	const char *q = v;
	double o;
	if (!num(&q, &o)) return 1;
	if (*q == '%') o /= 100;
	return o < 0 ? 0 : o > 1 ? 1 : o;
}

static bool parse_paint(const P *p, const char *v, paint *out) {
	if (!v) return false;
	const char *s = v;
	while (is_space(*s)) s++;
	if (!strncmp(s, "url(", 4)) {
		int32_t ref = url_ref(p, &s);
		paint pt = {.kind = P_URL, .ref = ref};
		uint32_t rgba = 0;
		int k = color(s, &rgba);
		pt.fb = k == C_RGBA ? P_COLOR : k == C_CURRENT ? P_CURRENT : P_NONE;
		pt.fb_rgba = rgba;
		*out = pt;
		return true;
	}
	uint32_t rgba = 0;
	switch (color(s, &rgba)) {
	case C_NONE: *out = (paint){.kind = P_NONE}; return true;
	case C_CURRENT: *out = (paint){.kind = P_CURRENT}; return true;
	case C_RGBA: *out = (paint){.kind = P_COLOR, .rgba = rgba}; return true;
	}
	return false;
}

static uint8_t weight_of(const char *v, uint8_t parent) {
	if (!strcmp(v, "normal")) return AK_FONT_WEIGHT_REGULAR;
	if (!strcmp(v, "bold")) return AK_FONT_WEIGHT_BOLD;
	if (!strcmp(v, "bolder")) return parent < AK_FONT_WEIGHT_BOLD ? AK_FONT_WEIGHT_BOLD : AK_FONT_WEIGHT_BLACK;
	if (!strcmp(v, "lighter")) return parent > AK_FONT_WEIGHT_MEDIUM ? AK_FONT_WEIGHT_REGULAR : AK_FONT_WEIGHT_THIN;
	const char *q = v;
	double w;
	if (!num(&q, &w)) return parent;
	int k = (int)lround(w / 100);
	return (uint8_t)(k < 1 ? 1 : k > 9 ? 9 : k);
}

static double font_of(const char *v, const style *ps) {
	static const struct {
		const char *k;
		double px;
	} kw[] = {{"xx-small", 9}, {"x-small", 10}, {"small", 13}, {"medium", 16}, {"large", 18}, {"x-large", 24}, {"xx-large", 32}};
	for (size_t i = 0; i < AK_LEN(kw); i++)
		if (!strcmp(v, kw[i].k)) return kw[i].px;
	if (!strcmp(v, "larger")) return ps->font * 1.2;
	if (!strcmp(v, "smaller")) return ps->font / 1.2;
	len l;
	if (!parse_len(v, &l)) return ps->font;
	double f = px(l, ps->font, ps->font);
	return f > 0 ? f : ps->font;
}

static void dash_of(const char *v, style *s) {
	if (!strcmp(v, "none")) {
		s->ndash = 0;
		return;
	}
	float d[MAX_DASH];
	int n = 0;
	double sum = 0;
	const char *q = v;
	len l;
	while (n < MAX_DASH / 2) {
		skip_sep(&q);
		if (!*q) break;
		if (!parse_len_at(&q, &l)) return;
		double x = px(l, diag(s), s->font);
		if (x < 0) return;
		d[n++] = (float)x, sum += x;
	}
	if (n == 0 || sum <= 0) {
		s->ndash = 0;
		return;
	}
	if (n % 2) memcpy(d + n, d, (size_t)n * sizeof(float)), n *= 2;
	memcpy(s->dash, d, (size_t)n * sizeof(float));
	s->ndash = (uint8_t)n;
}

// inherit is the style of element i, a child of what has ps: its
// inherited properties as it declares them.
static void inherit(const P *p, int32_t i, const style *ps, style *s) {
	*s = *ps;
	const char *v;
	if ((v = prop(p, i, PR_COLOR))) {
		uint32_t rgba;
		if (color(v, &rgba) == C_RGBA) s->own_color = true, s->color = rgba;
	}
	if ((v = prop(p, i, PR_FONT_SIZE))) s->font = font_of(v, ps);
	paint pt;
	if (parse_paint(p, prop(p, i, PR_FILL), &pt)) s->fill = pt;
	if (parse_paint(p, prop(p, i, PR_STROKE), &pt)) s->stroke = pt;
	if ((v = prop(p, i, PR_FILL_OPACITY))) s->fill_op = (float)opacity_of(v);
	if ((v = prop(p, i, PR_STROKE_OPACITY))) s->stroke_op = (float)opacity_of(v);
	if ((v = prop(p, i, PR_FILL_RULE))) s->fill_rule = !strcmp(v, "evenodd");
	if ((v = prop(p, i, PR_CLIP_RULE))) s->clip_rule = !strcmp(v, "evenodd");
	if ((v = prop(p, i, PR_STROKE_WIDTH))) {
		len l;
		if (parse_len(v, &l)) {
			double w = px(l, diag(s), s->font);
			if (w >= 0) s->width = (float)w;
		}
	}
	if ((v = prop(p, i, PR_STROKE_LINECAP))) {
		if (!strcmp(v, "butt")) s->cap = AK_LINE_CAP_BUTT;
		else if (!strcmp(v, "round")) s->cap = AK_LINE_CAP_ROUND;
		else if (!strcmp(v, "square")) s->cap = AK_LINE_CAP_SQUARE;
	}
	if ((v = prop(p, i, PR_STROKE_LINEJOIN))) {
		if (!strcmp(v, "miter") || !strcmp(v, "miter-clip") || !strcmp(v, "arcs")) s->join = AK_LINE_JOIN_MITER;
		else if (!strcmp(v, "round")) s->join = AK_LINE_JOIN_ROUND;
		else if (!strcmp(v, "bevel")) s->join = AK_LINE_JOIN_BEVEL;
	}
	if ((v = prop(p, i, PR_STROKE_DASHARRAY))) dash_of(v, s);
	if ((v = prop(p, i, PR_STROKE_DASHOFFSET))) {
		len l;
		if (parse_len(v, &l)) s->dash_off = (float)px(l, diag(s), s->font);
	}
	if ((v = prop(p, i, PR_VISIBILITY))) s->hidden = strcmp(v, "visible") != 0;
	if ((v = prop(p, i, PR_FONT_WEIGHT))) s->weight = weight_of(v, ps->weight);
	if ((v = prop(p, i, PR_TEXT_ANCHOR))) {
		if (!strcmp(v, "start")) s->anchor = AK_TEXT_ALIGN_START;
		else if (!strcmp(v, "middle")) s->anchor = AK_TEXT_ALIGN_CENTER;
		else if (!strcmp(v, "end")) s->anchor = AK_TEXT_ALIGN_END;
	}
	const char *sp = attr_of(p, i, "xml:space");
	if (sp) s->preserve = !strcmp(sp, "preserve");
}

// ---- Emitting commands ----

static void e_u8(R *r, uint8_t v) { ak_buf_u8(r->cmds, v); }
static void e_f(R *r, double v) { ak_buf_f32(r->cmds, (float)v); }

static void e_save(R *r) {
	e_u8(r, AK_CANVAS_OP_SAVE);
	ak_vec_push(&r->saved, r->rule);
}

static void e_restore(R *r) {
	e_u8(r, AK_CANVAS_OP_RESTORE);
	r->rule = ak_vec_pop(&r->saved);
}

static void e_rule(R *r, uint8_t rule) {
	if (r->rule == rule) return;
	e_u8(r, AK_CANVAS_OP_FILL_RULE);
	e_u8(r, rule);
	r->rule = rule;
}

static void e_matrix(R *r, const mat m) {
	if (is_identity(m)) return;
	e_u8(r, AK_CANVAS_OP_TRANSFORM);
	for (int i = 0; i < 6; i++) e_f(r, m[i]);
}

static void e_path(R *r, const ak_buf *path) {
	ak_buf_u32(r->cmds, (uint32_t)path->len);
	ak_buf_put(r->cmds, path->data, path->len);
}

static void e_rgba(R *r, uint32_t rgba, double op) {
	ak_buf_u64(r->cmds, ak_rgba((uint8_t)(rgba >> 24), (uint8_t)(rgba >> 16), (uint8_t)(rgba >> 8), clamp8((rgba & 255) * op)));
}

// e_color writes a paint's color: currentColor the element's color, else
// the document's (a slot the drawing fills).
static void e_color(R *r, const style *s, uint8_t kind, uint32_t rgba, double op) {
	if (kind == P_CURRENT && s->own_color) kind = P_COLOR, rgba = s->color;
	if (kind == P_CURRENT) {
		ak_vec_push(&r->slots, ((slot){(uint32_t)r->cmds->len, (float)op}));
		ak_buf_u64(r->cmds, 0);
		return;
	}
	e_rgba(r, rgba, op);
}

// ---- Gradients ----

typedef struct {
	uint8_t kind;  // P_COLOR, P_CURRENT
	uint32_t rgba;
	float op, off;
} stop;

typedef struct {
	bool radial, user;
	mat m;
	uint8_t spread;  // 0 pad, 1 reflect, 2 repeat
	len v[7];        // x1, y1, x2, y2 or cx, cy, r, fx, fy
	uint32_t set;
	int32_t stops;
} grad;

enum { G_UNITS = 1 << 9, G_XFORM = 1 << 10, G_SPREAD = 1 << 11 };

static const char *const lin_attrs[] = {"x1", "y1", "x2", "y2"};
static const char *const rad_attrs[] = {"cx", "cy", "r", "fx", "fy"};

static bool resolve_grad(const P *p, int32_t g, grad *out) {
	int t = p->nodes.data[g].tag;
	if (t != T_LINEAR && t != T_RADIAL) return false;
	grad gr = {.radial = t == T_RADIAL, .stops = -1};
	memcpy(gr.m, identity, sizeof(mat));
	int32_t seen[16];
	int ns = 0;
	for (int32_t k = g; k >= 0 && ns < 16; k = href_of(p, k)) {
		for (int j = 0; j < ns; j++)
			if (seen[j] == k) goto done;
		seen[ns++] = k;
		int kt = p->nodes.data[k].tag;
		if (kt != T_LINEAR && kt != T_RADIAL) break;
		if (kt == t) {
			const char *const *names = gr.radial ? rad_attrs : lin_attrs;
			int nn = gr.radial ? 5 : 4;
			for (int j = 0; j < nn; j++) {
				if (gr.set & (1u << j)) continue;
				if (parse_len(attr_of(p, k, names[j]), &gr.v[j])) gr.set |= 1u << j;
			}
		}
		const char *v;
		if (!(gr.set & G_UNITS) && (v = attr_of(p, k, "gradientUnits"))) gr.user = !strcmp(v, "userSpaceOnUse"), gr.set |= G_UNITS;
		if (!(gr.set & G_XFORM) && (v = attr_of(p, k, "gradientTransform"))) parse_transform(v, gr.m), gr.set |= G_XFORM;
		if (!(gr.set & G_SPREAD) && (v = attr_of(p, k, "spreadMethod")))
			gr.spread = !strcmp(v, "reflect") ? 1 : !strcmp(v, "repeat") ? 2 : 0, gr.set |= G_SPREAD;
		if (gr.stops < 0) {
			for (int32_t c = p->nodes.data[k].first; c >= 0; c = p->nodes.data[c].next)
				if (p->nodes.data[c].kind == N_ELEM && p->nodes.data[c].tag == T_STOP) {
					gr.stops = k;
					break;
				}
		}
	}
done:
	*out = gr;
	return true;
}

static uint32_t gather_stops(const P *p, const grad *g, const style *s, double op, stop *out, uint32_t cap) {
	uint32_t n = 0;
	if (g->stops < 0) return 0;
	float last = 0;
	for (int32_t c = p->nodes.data[g->stops].first; c >= 0 && n < cap; c = p->nodes.data[c].next) {
		const node *nd = &p->nodes.data[c];
		if (nd->kind != N_ELEM || nd->tag != T_STOP) continue;
		double off = 0;
		const char *ov = attr_of(p, c, "offset");
		if (ov) {
			const char *q = ov;
			if (num(&q, &off) && *q == '%') off /= 100;
		}
		if (off < last) off = last;
		if (off > 1) off = 1;
		last = (float)off;
		stop st = {.kind = P_COLOR, .rgba = 255, .off = (float)off};
		uint32_t rgba;
		const char *sc = prop(p, c, PR_STOP_COLOR);
		int k = sc ? color(sc, &rgba) : C_INVALID;
		if (k == C_RGBA) st.rgba = rgba;
		else if (k == C_CURRENT) {
			if (s->own_color) st.rgba = s->color;
			else st.kind = P_CURRENT;
		}
		st.op = (float)(opacity_of(prop(p, c, PR_STOP_OPACITY)) * op);
		out[n++] = st;
	}
	return n;
}

static void e_stops(R *r, const style *s, const stop *st, uint32_t n) {
	ak_buf_u32(r->cmds, n);
	for (uint32_t i = 0; i < n; i++) {
		e_color(r, s, st[i].kind, st[i].rgba, st[i].op);
		e_f(r, st[i].off);
	}
}

// repeat lays the stops out k0 to k1 times over (spreadMethod reflect or
// repeat), into out.
static uint32_t repeat_stops(const stop *st, uint32_t n, int k0, int k1, bool reflect, stop *out, uint32_t cap) {
	uint32_t m = 0;
	for (int k = k0; k < k1; k++) {
		bool rev = reflect && (k % 2 != 0);
		for (uint32_t j = 0; j < n && m < cap; j++) {
			stop s = rev ? st[n - 1 - j] : st[j];
			double o = rev ? 1 - s.off : s.off;
			s.off = (float)((k + o - k0) / (k1 - k0));
			out[m++] = s;
		}
	}
	return m;
}

typedef struct {
	bool solid;  // one color: the last stop's
	stop color;
	bool radial;
	double a[5];        // start x, y, end x, y; or center x, y, radius, focus x, y
	bool xform;         // radial, drawn through m (path through m's inverse)
	mat m, inv;
	stop *stops;
	uint32_t n;
} gpaint;

static double glen(len l, bool user, double of, const style *s) {
	if (user) return px(l, of, s->font);
	if (l.unit == U_PCT) return l.v / 100;
	return l.v;
}

// gradient works out a gradient's paint for a shape of bounds bb: false
// if it paints nothing. Its stops are in st (cap of them).
static bool gradient(R *r, int32_t g, const style *s, const box *bb, double op, gpaint *gp, stop *st, stop *tmp, uint32_t cap) {
	grad gr;
	if (!resolve_grad(r->p, g, &gr)) return false;
	uint32_t n = gather_stops(r->p, &gr, s, op, st, cap);
	if (n == 0) return false;
	*gp = (gpaint){.radial = gr.radial, .stops = st, .n = n};
	mat G;
	memcpy(G, identity, sizeof G);
	double bw = bb->x1 - bb->x0, bh = bb->y1 - bb->y0;
	if (!gr.user) {
		if (!bb->seen || bw <= 0 || bh <= 0) return false;
		G[0] = bw, G[3] = bh, G[4] = bb->x0, G[5] = bb->y0;
	}
	mul(G, G, gr.m);
	mat inv;
	if (!invert(inv, G)) return false;
	if (n == 1) {
		gp->solid = true, gp->color = st[0];
		return true;
	}
	double d = diag(s);
	box corners = {0};
	if (bb->seen) {
		double xs[2] = {bb->x0, bb->x1}, ys[2] = {bb->y0, bb->y1};
		for (int i = 0; i < 4; i++) {
			double x, y;
			apply(inv, xs[i & 1], ys[i >> 1], &x, &y);
			box_add(&corners, x, y);
		}
	}
	if (!gr.radial) {
		double v[4] = {0, 0, 1, 0};
		for (int j = 0; j < 4; j++) {
			if (!(gr.set & (1u << j))) continue;
			v[j] = glen(gr.v[j], gr.user, j % 2 ? s->vh : s->vw, s);
		}
		double dx = v[2] - v[0], dy = v[3] - v[1], l2 = dx * dx + dy * dy;
		if (l2 <= 0) {
			gp->solid = true, gp->color = st[n - 1];
			return true;
		}
		// t(p) = (inv(p) - start) . g, as a gradient in user space: p . w + c.
		double gx = dx / l2, gy = dy / l2;
		double wx = inv[0] * gx + inv[1] * gy, wy = inv[2] * gx + inv[3] * gy;
		double c = (inv[4] - v[0]) * gx + (inv[5] - v[1]) * gy;
		double ww = wx * wx + wy * wy;
		if (ww <= 0) {
			gp->solid = true, gp->color = st[n - 1];
			return true;
		}
		double sx = -c * wx / ww, sy = -c * wy / ww, ex = sx + wx / ww, ey = sy + wy / ww;
		if (gr.spread && bb->seen) {
			double tmin = 1e300, tmax = -1e300, xs[2] = {bb->x0, bb->x1}, ys[2] = {bb->y0, bb->y1};
			for (int i = 0; i < 4; i++) {
				double t = xs[i & 1] * wx + ys[i >> 1] * wy + c;
				tmin = fmin(tmin, t), tmax = fmax(tmax, t);
			}
			int k0 = (int)floor(tmin), k1 = (int)ceil(tmax);
			if (k0 > 0) k0 = 0;
			if (k1 < 1) k1 = 1;
			if ((k0 < 0 || k1 > 1) && k1 - k0 <= MAX_REPEAT) {
				gp->n = repeat_stops(st, n, k0, k1, gr.spread == 1, tmp, cap);
				gp->stops = tmp;
				double ux = ex - sx, uy = ey - sy;
				ex = sx + k1 * ux, ey = sy + k1 * uy;
				sx = sx + k0 * ux, sy = sy + k0 * uy;
			}
		}
		gp->a[0] = sx, gp->a[1] = sy, gp->a[2] = ex, gp->a[3] = ey;
		return true;
	}
	double v[5] = {0.5, 0.5, 0.5, 0, 0};
	for (int j = 0; j < 5; j++) {
		if (!(gr.set & (1u << j))) continue;
		v[j] = glen(gr.v[j], gr.user, j == 2 ? d : j % 2 ? s->vh : s->vw, s);
	}
	if (!(gr.set & 8)) v[3] = v[0];
	if (!(gr.set & 16)) v[4] = v[1];
	if (v[2] <= 0) {
		gp->solid = true, gp->color = st[n - 1];
		return true;
	}
	// A focus outside the circle is moved onto it (SVG 1.1).
	double fx = v[3] - v[0], fy = v[4] - v[1], fd = sqrt(fx * fx + fy * fy);
	if (fd > v[2] * 0.999) v[3] = v[0] + fx * v[2] * 0.999 / fd, v[4] = v[1] + fy * v[2] * 0.999 / fd;
	if (gr.spread && corners.seen && fd < 1e-9) {
		double tmax = 0, xs[2] = {corners.x0, corners.x1}, ys[2] = {corners.y0, corners.y1};
		for (int i = 0; i < 4; i++) tmax = fmax(tmax, hypot(xs[i & 1] - v[0], ys[i >> 1] - v[1]) / v[2]);
		int k1 = (int)ceil(tmax);
		if (k1 > 1 && k1 <= MAX_REPEAT) {
			gp->n = repeat_stops(st, n, 0, k1, gr.spread == 1, tmp, cap);
			gp->stops = tmp;
			v[2] *= k1;
		}
	}
	// A similarity (no skew, the same scale both ways) moves the circle
	// into user space; else it's drawn through the transform.
	bool similar = fabs(G[0] - G[3]) < 1e-9 * (fabs(G[0]) + 1) && fabs(G[1] + G[2]) < 1e-9 * (fabs(G[1]) + 1);
	bool mirrored = fabs(G[0] + G[3]) < 1e-9 * (fabs(G[0]) + 1) && fabs(G[1] - G[2]) < 1e-9 * (fabs(G[1]) + 1);
	if (similar || mirrored) {
		double k = sqrt(fabs(G[0] * G[3] - G[1] * G[2]));
		apply(G, v[0], v[1], &gp->a[0], &gp->a[1]);
		gp->a[2] = v[2] * k;
		apply(G, v[3], v[4], &gp->a[3], &gp->a[4]);
		return true;
	}
	gp->xform = true;
	memcpy(gp->m, G, sizeof G);
	memcpy(gp->inv, inv, sizeof inv);
	memcpy(gp->a, v, sizeof v);
	return true;
}

// first_stop is a gradient's first stop (text, which a gradient doesn't
// paint).
static bool first_stop(R *r, int32_t g, const style *s, double op, stop *out) {
	grad gr;
	stop st[1];
	if (!resolve_grad(r->p, g, &gr) || gather_stops(r->p, &gr, s, op, st, 1) == 0) return false;
	*out = st[0];
	return true;
}

// ---- Shapes ----

static bool is_shape(int t) { return t >= T_PATH && t <= T_POLYGON; }

// shape_path appends element i's outline (a shape's) to out.
static void shape_path(const P *p, int32_t i, const style *s, ak_buf *out) {
	double vw = s->vw, vh = s->vh, d = diag(s);
	switch (p->nodes.data[i].tag) {
	case T_PATH: {
		const char *dv = attr_of(p, i, "d");
		if (dv) path_data(out, dv);
		break;
	}
	case T_RECT: {
		double x = lattr_or(p, i, "x", vw, s, 0), y = lattr_or(p, i, "y", vh, s, 0);
		double w = lattr_or(p, i, "width", vw, s, 0), h = lattr_or(p, i, "height", vh, s, 0);
		double rx, ry;
		bool hx = lattr(p, i, "rx", vw, s, &rx), hy = lattr(p, i, "ry", vh, s, &ry);
		if (hx && !hy) ry = rx;
		if (hy && !hx) rx = ry;
		if (!hx && !hy) rx = ry = 0;
		if (w > 0 && h > 0) path_rect(out, x, y, w, h, rx < 0 ? 0 : rx, ry < 0 ? 0 : ry);
		break;
	}
	case T_CIRCLE: {
		double r = lattr_or(p, i, "r", d, s, 0);
		if (r > 0) path_ellipse(out, lattr_or(p, i, "cx", vw, s, 0), lattr_or(p, i, "cy", vh, s, 0), r, r);
		break;
	}
	case T_ELLIPSE: {
		double rx, ry;
		bool hx = lattr(p, i, "rx", vw, s, &rx), hy = lattr(p, i, "ry", vh, s, &ry);
		if (hx && !hy) ry = rx;
		if (hy && !hx) rx = ry;
		if (hx || hy)
			if (rx > 0 && ry > 0) path_ellipse(out, lattr_or(p, i, "cx", vw, s, 0), lattr_or(p, i, "cy", vh, s, 0), rx, ry);
		break;
	}
	case T_LINE:
		pb_move(out, lattr_or(p, i, "x1", vw, s, 0), lattr_or(p, i, "y1", vh, s, 0));
		pb_line(out, lattr_or(p, i, "x2", vw, s, 0), lattr_or(p, i, "y2", vh, s, 0));
		break;
	case T_POLYLINE:
	case T_POLYGON: {
		const char *pts = attr_of(p, i, "points");
		if (pts) path_points(out, pts, p->nodes.data[i].tag == T_POLYGON);
		break;
	}
	}
}

static bool transform_of(const P *p, int32_t i, mat m) { return parse_transform(prop(p, i, PR_TRANSFORM), m); }

static bool hidden_display(const P *p, int32_t i) {
	const char *v = prop(p, i, PR_DISPLAY);
	return v && !strcmp(v, "none");
}

static void elem_box(R *r, int32_t i, const style *ps, const mat m, box *b, int depth);

static void children_box(R *r, int32_t i, const style *s, const mat m, box *b, int depth) {
	for (int32_t c = r->p->nodes.data[i].first; c >= 0; c = r->p->nodes.data[c].next) elem_box(r, c, s, m, b, depth + 1);
}

// elem_box grows b by element i's bounds through m (a shape's outline;
// a group's, its children's).
static void elem_box(R *r, int32_t i, const style *ps, const mat m, box *b, int depth) {
	P *p = r->p;
	node *nd = &p->nodes.data[i];
	if (nd->kind != N_ELEM || depth > MAX_DEPTH || hidden_display(p, i) || ++r->visits > MAX_VISITS) return;
	style s;
	inherit(p, i, ps, &s);
	mat t, mm;
	transform_of(p, i, t);
	mul(mm, m, t);
	if (is_shape(nd->tag)) {
		r->tmp.len = 0;
		shape_path(p, i, &s, &r->tmp);
		// Through mm into a buffer of its own: appending to tmp could move it.
		r->boxed.len = 0;
		if (r->tmp.len) xform_path(r->tmp.data, r->tmp.len, mm, &r->boxed);
		if (r->boxed.len) path_box(r->boxed.data, r->boxed.len, b);
	} else if (nd->tag == T_G || nd->tag == T_A || nd->tag == T_SWITCH) {
		children_box(r, i, &s, mm, b, depth);
	} else if (nd->tag == T_USE) {
		int32_t ref = use_ref(p, i);
		if (ref < 0 || r->uses >= MAX_USES) return;
		mat tr = {1, 0, 0, 1, lattr_or(p, i, "x", s.vw, &s, 0), lattr_or(p, i, "y", s.vh, &s, 0)};
		mul(mm, mm, tr);
		p->nodes.data[ref].busy = true, r->uses++;
		if (p->nodes.data[ref].tag == T_SYMBOL) {
			style ss;
			inherit(p, ref, &s, &ss);
			children_box(r, ref, &ss, mm, b, depth);
		} else elem_box(r, ref, &s, mm, b, depth + 1);
		p->nodes.data[ref].busy = false, r->uses--;
	}
}

static void draw(R *r, int32_t i, const style *ps, int depth);

static void draw_children(R *r, int32_t i, const style *s, int depth) {
	for (int32_t c = r->p->nodes.data[i].first; c >= 0 && !r->over; c = r->p->nodes.data[c].next) draw(r, c, s, depth + 1);
}

// fill paints path (b its bounds) with s's fill.
static void fill(R *r, const style *s, const ak_buf *path, const box *bb) {
	paint pt = s->fill;
	if (pt.kind == P_URL) {
		int32_t g = pt.ref;
		if (g >= 0 && (r->p->nodes.data[g].tag == T_LINEAR || r->p->nodes.data[g].tag == T_RADIAL)) {
			stop st[64], tmp[MAX_STOPS];
			gpaint gp;
			if (!gradient(r, g, s, bb, s->fill_op, &gp, st, tmp, 64)) return;
			if (gp.solid) {
				e_rule(r, s->fill_rule);
				e_u8(r, AK_CANVAS_OP_FILL);
				e_color(r, s, gp.color.kind, gp.color.rgba, gp.color.op);
				e_path(r, path);
				return;
			}
			if (gp.xform) {
				e_save(r);
				e_matrix(r, gp.m);
			}
			e_rule(r, s->fill_rule);
			e_u8(r, gp.radial ? AK_CANVAS_OP_RADIAL_GRADIENT : AK_CANVAS_OP_GRADIENT);
			e_stops(r, s, gp.stops, gp.n);
			for (int k = 0; k < (gp.radial ? 5 : 4); k++) e_f(r, gp.a[k]);
			if (gp.xform) {
				r->clip.len = 0;
				xform_path(path->data, path->len, gp.inv, &r->clip);
				e_path(r, &r->clip);
				e_restore(r);
			} else e_path(r, path);
			return;
		}
		pt.kind = pt.fb, pt.rgba = pt.fb_rgba;
	}
	if (pt.kind == P_NONE) return;
	e_rule(r, s->fill_rule);
	e_u8(r, AK_CANVAS_OP_FILL);
	e_color(r, s, pt.kind, pt.rgba, s->fill_op);
	e_path(r, path);
}

static void stroke_style(R *r, const style *s) {
	e_f(r, s->width);
	e_u8(r, s->cap);
	e_u8(r, s->join);
	ak_buf_u32(r->cmds, s->ndash);
	e_f(r, s->dash_off);
	for (int k = 0; k < s->ndash; k++) e_f(r, s->dash[k]);
}

static void stroke(R *r, const style *s, const ak_buf *path, const box *bb) {
	if (s->width <= 0) return;
	paint pt = s->stroke;
	if (pt.kind == P_URL) {
		int32_t g = pt.ref;
		if (g >= 0 && (r->p->nodes.data[g].tag == T_LINEAR || r->p->nodes.data[g].tag == T_RADIAL)) {
			stop st[64], tmp[MAX_STOPS];
			gpaint gp;
			if (!gradient(r, g, s, bb, s->stroke_op, &gp, st, tmp, 64)) return;
			if (gp.solid) {
				e_u8(r, AK_CANVAS_OP_STROKE);
				e_color(r, s, gp.color.kind, gp.color.rgba, gp.color.op);
				stroke_style(r, s);
				e_path(r, path);
				return;
			}
			if (gp.xform) {
				// No transform for a stroke (it would bend its width): the
				// circle at its average scale.
				double k = sqrt(fabs(gp.m[0] * gp.m[3] - gp.m[1] * gp.m[2]));
				double a[5];
				apply(gp.m, gp.a[0], gp.a[1], &a[0], &a[1]);
				a[2] = gp.a[2] * k;
				apply(gp.m, gp.a[3], gp.a[4], &a[3], &a[4]);
				memcpy(gp.a, a, sizeof a);
			}
			e_u8(r, AK_CANVAS_OP_STROKE_GRADIENT);
			stroke_style(r, s);
			e_u8(r, gp.radial ? 1 : 0);
			e_stops(r, s, gp.stops, gp.n);
			for (int k = 0; k < (gp.radial ? 5 : 4); k++) e_f(r, gp.a[k]);
			e_path(r, path);
			return;
		}
		pt.kind = pt.fb, pt.rgba = pt.fb_rgba;
	}
	if (pt.kind == P_NONE) return;
	e_u8(r, AK_CANVAS_OP_STROKE);
	e_color(r, s, pt.kind, pt.rgba, s->stroke_op);
	stroke_style(r, s);
	e_path(r, path);
}

// ---- Clipping ----

// clip_path makes element cp's (a clipPath's) outline, for an element of
// bounds bb, in out: false if it clips everything away.
static bool clip_path(R *r, int32_t cp, const style *ps, const box *bb, ak_buf *out, uint8_t *rule) {
	P *p = r->p;
	out->len = 0;
	style cs;
	inherit(p, cp, ps, &cs);
	mat base, t;
	transform_of(p, cp, base);
	const char *units = attr_of(p, cp, "clipPathUnits");
	if (units && !strcmp(units, "objectBoundingBox")) {
		if (!bb->seen || bb->x1 <= bb->x0 || bb->y1 <= bb->y0) return false;
		mat u = {bb->x1 - bb->x0, 0, 0, bb->y1 - bb->y0, bb->x0, bb->y0};
		mul(base, base, u);
	}
	bool first = true;
	*rule = cs.clip_rule;
	for (int32_t c = p->nodes.data[cp].first; c >= 0; c = p->nodes.data[c].next) {
		node *nd = &p->nodes.data[c];
		if (nd->kind != N_ELEM || hidden_display(p, c)) continue;
		style s;
		inherit(p, c, &cs, &s);
		int32_t el = c;
		mat m;
		transform_of(p, c, t);
		mul(m, base, t);
		if (nd->tag == T_USE) {
			el = href_of(p, c);
			if (el < 0 || !is_shape(p->nodes.data[el].tag) || hidden_display(p, el)) continue;
			mat tr = {1, 0, 0, 1, lattr_or(p, c, "x", s.vw, &s, 0), lattr_or(p, c, "y", s.vh, &s, 0)};
			mul(m, m, tr);
			transform_of(p, el, t);
			mul(m, m, t);
			style us;
			inherit(p, el, &s, &us);
			s = us;
		} else if (!is_shape(nd->tag)) continue;
		if (s.hidden) continue;
		if (first) *rule = s.clip_rule, first = false;
		r->tmp.len = 0;
		shape_path(p, el, &s, &r->tmp);
		xform_path(r->tmp.data, r->tmp.len, m, out);
	}
	return out->len > 0;
}

static int32_t clip_ref(const P *p, int32_t i) {
	const char *v = prop(p, i, PR_CLIP_PATH);
	if (!v) return -1;
	int32_t k = url_ref(p, &v);
	return k >= 0 && p->nodes.data[k].tag == T_CLIP ? k : -1;
}

// ---- Viewports ----

static void par_of(const char *v, uint8_t *align, bool *slice) {
	*align = 5, *slice = false;
	if (!v) return;
	static const char *const names[] = {"xMinYMin", "xMidYMin", "xMaxYMin", "xMinYMid", "xMidYMid", "xMaxYMid", "xMinYMax", "xMidYMax", "xMaxYMax"};
	const char *s = v;
	while (is_space(*s)) s++;
	if (!strncmp(s, "defer", 5)) s += 5;
	while (is_space(*s)) s++;
	if (!strncmp(s, "none", 4)) {
		*align = 0;
		return;
	}
	for (int k = 0; k < 9; k++)
		if (!strncmp(s, names[k], 8)) {
			*align = (uint8_t)(k + 1);
			s += 8;
			break;
		}
	while (is_space(*s)) s++;
	*slice = !strncmp(s, "slice", 5);
}

static bool viewbox_of(const char *v, double vb[4]) {
	if (!v) return false;
	const char *s = v;
	if (!nums(&s, vb, 4)) return false;
	return vb[2] > 0 && vb[3] > 0;
}

// fit_matrix maps the viewBox vb onto the box x, y, w, h as
// preserveAspectRatio's align and slice say.
static void fit_matrix(const double vb[4], double x, double y, double w, double h, uint8_t align, bool slice, mat m) {
	double sx = w / vb[2], sy = h / vb[3];
	double ax = 0, ay = 0;
	if (align) {
		double s = slice ? fmax(sx, sy) : fmin(sx, sy);
		sx = sy = s;
		ax = ((align - 1) % 3) * 0.5, ay = ((align - 1) / 3) * 0.5;
	}
	m[0] = sx, m[1] = 0, m[2] = 0, m[3] = sy;
	m[4] = x - vb[0] * sx + (w - vb[2] * sx) * ax;
	m[5] = y - vb[1] * sy + (h - vb[3] * sy) * ay;
}

static void e_rect_clip(R *r, double x, double y, double w, double h) {
	e_rule(r, 0);
	e_u8(r, AK_CANVAS_OP_CLIP);
	ak_buf_u32(r->cmds, 17);
	e_u8(r, AK_PATH_OP_RECT);
	e_f(r, x), e_f(r, y), e_f(r, w), e_f(r, h);
}

// viewport draws element vp's (a nested svg's, a symbol's) children in
// the box x, y, w, h, its viewBox fitted to it, clipped to it.
static void viewport(R *r, int32_t vp, const style *s, double x, double y, double w, double h, int depth) {
	if (w <= 0 || h <= 0) return;
	P *p = r->p;
	style vs = *s;
	e_save(r);
	e_rect_clip(r, x, y, w, h);
	double vb[4];
	if (viewbox_of(attr_of(p, vp, "viewBox"), vb)) {
		uint8_t align;
		bool slice;
		par_of(attr_of(p, vp, "preserveAspectRatio"), &align, &slice);
		mat m;
		fit_matrix(vb, x, y, w, h, align, slice, m);
		e_matrix(r, m);
		vs.vw = vb[2], vs.vh = vb[3];
	} else {
		mat m = {1, 0, 0, 1, x, y};
		e_matrix(r, m);
		vs.vw = w, vs.vh = h;
	}
	draw_children(r, vp, &vs, depth);
	e_restore(r);
}

// ---- Text ----

typedef struct {
	size_t off;  // its text, in r->tmp
	double x, y;
	style st;
	bool space;  // the text so far ends in a space (collapsed)
} run;

static void flush_run(R *r, run *ru) {
	size_t n = r->tmp.len - ru->off;
	if (n == 0) return;
	const char *t = (const char *)r->tmp.data + ru->off;
	if (!ru->st.preserve)
		while (n > 0 && t[n - 1] == ' ') n--;
	const style *s = &ru->st;
	if (n > 0 && !s->hidden) {
		paint pt = s->fill;
		stop st = {.kind = pt.kind, .rgba = pt.rgba, .op = s->fill_op};
		bool ok = true;
		if (pt.kind == P_URL) {
			if (pt.ref >= 0 && first_stop(r, pt.ref, s, s->fill_op, &st)) {
			} else if (pt.fb != P_NONE) st.kind = pt.fb, st.rgba = pt.fb_rgba;
			else ok = false;
		} else if (pt.kind == P_NONE) ok = false;
		if (ok) {
			e_u8(r, AK_CANVAS_OP_TEXT);
			e_color(r, s, st.kind, st.rgba, st.op);
			e_f(r, s->font);
			e_u8(r, s->weight);
			e_u8(r, s->anchor);
			e_f(r, ru->x), e_f(r, ru->y);
			ak_buf_str(r->cmds, t, n);
		}
	}
	r->tmp.len = ru->off;
}

static void add_text(R *r, run *ru, const char *t) {
	for (; *t; t++) {
		char c = *t;
		if (c == '\n' || c == '\r') {
			if (ru->st.preserve) c = ' ';
			else continue;
		}
		if (c == '\t') c = ' ';
		if (c == ' ' && !ru->st.preserve) {
			if (ru->space || r->tmp.len == ru->off) continue;
			ru->space = true;
		} else ru->space = false;
		ak_buf_u8(&r->tmp, (uint8_t)c);
	}
}

static double first_num(const char *v, double of, double font, bool *has) {
	len l;
	const char *q = v;
	*has = false;
	if (!v || !parse_len_at(&q, &l)) return 0;
	*has = true;
	return px(l, of, font);
}

static void text_children(R *r, int32_t i, const style *s, run *ru, int depth) {
	P *p = r->p;
	for (int32_t c = p->nodes.data[i].first; c >= 0; c = p->nodes.data[c].next) {
		node *nd = &p->nodes.data[c];
		if (nd->kind == N_TEXT) {
			add_text(r, ru, nd->name);
			continue;
		}
		if ((nd->tag != T_TSPAN && nd->tag != T_A) || hidden_display(p, c) || depth > MAX_DEPTH) continue;
		style cs;
		inherit(p, c, s, &cs);
		bool hx, hy, hdx, hdy;
		double x = first_num(attr_of(p, c, "x"), cs.vw, cs.font, &hx), y = first_num(attr_of(p, c, "y"), cs.vh, cs.font, &hy);
		double dx = first_num(attr_of(p, c, "dx"), cs.vw, cs.font, &hdx), dy = first_num(attr_of(p, c, "dy"), cs.vh, cs.font, &hdy);
		bool empty = r->tmp.len == ru->off;
		if (hx || empty) {
			// A position starts a run of its own, in its style.
			double nx = hx ? x : ru->x, ny = hy ? y : ru->y;
			if (!empty) flush_run(r, ru);
			*ru = (run){.off = r->tmp.len, .x = nx + dx, .y = ny + dy, .st = cs};
		}
		text_children(r, c, &cs, ru, depth + 1);
		AK_UNUSED(hdx), AK_UNUSED(hdy);
	}
}

static void draw_text(R *r, int32_t i, const style *s) {
	P *p = r->p;
	bool hx, hy, hdx, hdy;
	double x = first_num(attr_of(p, i, "x"), s->vw, s->font, &hx), y = first_num(attr_of(p, i, "y"), s->vh, s->font, &hy);
	double dx = first_num(attr_of(p, i, "dx"), s->vw, s->font, &hdx), dy = first_num(attr_of(p, i, "dy"), s->vh, s->font, &hdy);
	r->tmp.len = 0;
	run ru = {.off = 0, .x = x + dx, .y = y + dy, .st = *s};
	text_children(r, i, s, &ru, 0);
	flush_run(r, &ru);
	AK_UNUSED(hx), AK_UNUSED(hy), AK_UNUSED(hdx), AK_UNUSED(hdy);
}

// ---- Elements ----

static bool renders(int t) { return is_shape(t) || t == T_G || t == T_A || t == T_SWITCH || t == T_SVG || t == T_USE || t == T_TEXT; }

static void draw_use(R *r, int32_t i, const style *s, int depth) {
	P *p = r->p;
	int32_t ref = use_ref(p, i);
	if (ref < 0 || r->uses >= MAX_USES) return;
	int t = p->nodes.data[ref].tag;
	double x = lattr_or(p, i, "x", s->vw, s, 0), y = lattr_or(p, i, "y", s->vh, s, 0);
	p->nodes.data[ref].busy = true, r->uses++;
	if (t == T_SYMBOL || t == T_SVG) {
		style vs;
		inherit(p, ref, s, &vs);
		double w, h;
		if (!lattr(p, i, "width", s->vw, s, &w)) w = lattr_or(p, ref, "width", s->vw, s, s->vw);
		if (!lattr(p, i, "height", s->vh, s, &h)) h = lattr_or(p, ref, "height", s->vh, s, s->vh);
		if (t == T_SVG) x += lattr_or(p, ref, "x", s->vw, s, 0), y += lattr_or(p, ref, "y", s->vh, s, 0);
		if (!hidden_display(p, ref)) viewport(r, ref, &vs, x, y, w, h, depth);
	} else {
		mat tr = {1, 0, 0, 1, x, y};
		e_matrix(r, tr);
		draw(r, ref, s, depth + 1);
	}
	p->nodes.data[ref].busy = false, r->uses--;
}

static void draw(R *r, int32_t i, const style *ps, int depth) {
	P *p = r->p;
	node *nd = &p->nodes.data[i];
	if (nd->kind != N_ELEM || !renders(nd->tag) || depth > MAX_DEPTH) return;
	if (++r->visits > MAX_VISITS || r->cmds->len > MAX_CMDS) {
		r->over = true;
		return;
	}
	if (hidden_display(p, i)) return;
	style s;
	inherit(p, i, ps, &s);
	mat m;
	bool has_m = transform_of(p, i, m);
	double op = opacity_of(prop(p, i, PR_OPACITY));
	if (op <= 0) return;
	int32_t cp = clip_ref(p, i);
	int tag = nd->tag;
	bool save = has_m || op < 1 || cp >= 0 || tag == T_USE;
	r->path.len = 0;
	box bb = {0};
	if (is_shape(tag)) {
		shape_path(p, i, &s, &r->path);
		if (r->path.len == 0) return;
		path_box(r->path.data, r->path.len, &bb);
	}
	if (tag == T_USE && use_ref(p, i) < 0) return;
	// The clip is made (in r->clip) before anything's drawn: one that
	// leaves nothing draws nothing.
	uint8_t crule = 0;
	if (cp >= 0) {
		box cb = bb;
		if (!is_shape(tag)) {
			mat id;
			memcpy(id, identity, sizeof id);
			if (tag == T_G || tag == T_A || tag == T_SWITCH) children_box(r, i, &s, id, &cb, depth);
			else elem_box(r, i, ps, id, &cb, depth);
		}
		if (!clip_path(r, cp, &s, &cb, &r->clip, &crule)) return;
	}
	if (save) e_save(r);
	e_matrix(r, m);
	if (cp >= 0) {
		e_rule(r, crule);
		e_u8(r, AK_CANVAS_OP_CLIP);
		e_path(r, &r->clip);
	}
	if (op < 1) {
		e_u8(r, AK_CANVAS_OP_ALPHA);
		e_f(r, op);
	}
	switch (tag) {
	case T_G:
	case T_A: draw_children(r, i, &s, depth); break;
	case T_SWITCH:
		for (int32_t c = nd->first; c >= 0; c = p->nodes.data[c].next) {
			const node *cn = &p->nodes.data[c];
			if (cn->kind != N_ELEM) continue;
			const char *ext = attr_of(p, c, "requiredExtensions");
			if (!renders(cn->tag) || (ext && *ext)) continue;
			draw(r, c, &s, depth + 1);
			break;
		}
		break;
	case T_SVG: {
		double x = lattr_or(p, i, "x", ps->vw, ps, 0), y = lattr_or(p, i, "y", ps->vh, ps, 0);
		double w = lattr_or(p, i, "width", ps->vw, ps, ps->vw), h = lattr_or(p, i, "height", ps->vh, ps, ps->vh);
		viewport(r, i, &s, x, y, w, h, depth);
		break;
	}
	case T_USE: draw_use(r, i, &s, depth); break;
	case T_TEXT: draw_text(r, i, &s); break;
	default:
		if (!s.hidden) {
			// The outline is reused by what fill and stroke draw: copied, as
			// a gradient's clip and the clip path use the scratch buffers.
			ak_buf path = r->path;
			r->path = (ak_buf){0};
			fill(r, &s, &path, &bb);
			stroke(r, &s, &path, &bb);
			ak_buf_free(&r->path);
			r->path = path;
		}
		break;
	}
	if (save) e_restore(r);
}

// ---- The document ----


bool ak_svg_parse(const char *src, size_t n, ak_buf *out, char *err) {
	out->len = 0;
	err[0] = 0;
	if (!src) src = "", n = 0;
	P p = {.src = src, .n = n, .err = err};
	// The markup stops at a NUL: what's after it isn't text.
	const char *z = memchr(src, 0, n);
	if (z) p.n = (size_t)(z - src);
	// Markup starts with a '<' (after a BOM and spaces): bytes that don't
	// (an image's, gzipped svgz) are refused at once.
	size_t lead = p.n >= 3 && (uint8_t)src[0] == 0xef && (uint8_t)src[1] == 0xbb && (uint8_t)src[2] == 0xbf ? 3 : 0;
	while (lead < p.n && is_space(src[lead])) lead++;
	if (lead >= p.n || src[lead] != '<') fail(&p, 0, "not an SVG document");
	else xml(&p);
	bool ok = !p.failed && p.nodes.len > 0;
	if (ok) {
		index_ids(&p);
		for (uint32_t i = 0; i < p.nodes.len; i++) {
			const node *nd = &p.nodes.data[i];
			if (nd->kind != N_ELEM || nd->tag != T_STYLE) continue;
			ak_buf text = {0};
			for (int32_t c = nd->first; c >= 0; c = p.nodes.data[c].next)
				if (p.nodes.data[c].kind == N_TEXT) ak_buf_put(&text, p.nodes.data[c].name, strlen(p.nodes.data[c].name));
			char *s = arena_str(&p, (const char *)text.data, text.len);
			ak_buf_free(&text);
			css(&p, s);
		}
		cascade(&p);
	}
	if (ok) {
		// The root: its size, its viewBox, its viewport's style.
		style s = {
			.fill = {.kind = P_COLOR, .rgba = 0x000000ff},
			.stroke = {.kind = P_NONE},
			.fill_op = 1,
			.stroke_op = 1,
			.width = 1,
			.weight = AK_FONT_WEIGHT_REGULAR,
			.font = 16,
		};
		double vb[4] = {0, 0, 0, 0};
		bool has_vb = viewbox_of(attr_of(&p, 0, "viewBox"), vb);
		len lw, lh;
		bool hw = parse_len(attr_of(&p, 0, "width"), &lw) && lw.unit != U_PCT && lw.v > 0;
		bool hh = parse_len(attr_of(&p, 0, "height"), &lh) && lh.unit != U_PCT && lh.v > 0;
		double w = hw ? px(lw, 0, 16) : 0, h = hh ? px(lh, 0, 16) : 0;
		if (has_vb) {
			if (!hw && !hh) w = vb[2], h = vb[3];
			else if (!hw) w = h * vb[2] / vb[3];
			else if (!hh) h = w * vb[3] / vb[2];
		} else {
			if (!hw) w = 300;
			if (!hh) h = 150;
			vb[2] = w, vb[3] = h;
		}
		uint8_t align;
		bool slice;
		par_of(attr_of(&p, 0, "preserveAspectRatio"), &align, &slice);
		s.vw = vb[2], s.vh = vb[3];
		ak_buf cmds = {0};
		R r = {.p = &p, .cmds = &cmds};
		style rs;
		inherit(&p, 0, &s, &rs);
		mat m;
		bool has_m = transform_of(&p, 0, m);
		double op = opacity_of(prop(&p, 0, PR_OPACITY));
		if (!hidden_display(&p, 0) && op > 0) {
			bool save = has_m || op < 1;
			if (save) e_save(&r);
			e_matrix(&r, m);
			if (op < 1) e_u8(&r, AK_CANVAS_OP_ALPHA), e_f(&r, op);
			draw_children(&r, 0, &rs, 0);
			if (save && !r.over) e_restore(&r);
		}
		if (r.over) {
			snprintf(err, 160, "too complex to draw (more than a million elements, or 64 MB of drawing)");
			ok = false;
		} else {
			ak_buf_put(out, "ASVG", 4);
			ak_buf_u8(out, 1);
			ak_buf_f32(out, (float)w), ak_buf_f32(out, (float)h);
			for (int k = 0; k < 4; k++) ak_buf_f32(out, (float)vb[k]);
			ak_buf_u8(out, align);
			ak_buf_u8(out, slice);
			ak_buf_u32(out, r.slots.len);
			for (uint32_t k = 0; k < r.slots.len; k++) {
				ak_buf_u32(out, r.slots.data[k].off);
				ak_buf_f32(out, r.slots.data[k].opacity);
			}
			ak_buf_u32(out, (uint32_t)cmds.len);
			ak_buf_put(out, cmds.data, cmds.len);
		}
		ak_buf_free(&cmds);
		ak_vec_free(&r.slots);
		ak_vec_free(&r.saved);
		ak_buf_free(&r.path);
		ak_buf_free(&r.clip);
		ak_buf_free(&r.tmp);
		ak_buf_free(&r.boxed);
	}
	if (!ok && !err[0]) snprintf(err, 160, "not an SVG document");
	ak_vec_free(&p.nodes);
	ak_vec_free(&p.attrs);
	ak_vec_free(&p.decls);
	ak_vec_free(&p.rules);
	ak_vec_free(&p.parts);
	ak_vec_free(&p.rdecls);
	ak_free(p.ids);
	for (arena_blk *b = p.arena; b;) {
		arena_blk *nb = b->next;
		ak_free(b);
		b = nb;
	}
	return ok;
}

// ---- Reading a document back ----

typedef struct {
	float w, h, vb[4];
	uint8_t align, slice;
	const uint8_t *slots;
	uint32_t nslots;
	const uint8_t *cmds;
	uint32_t ncmds;
} doc;

static bool read_doc(const uint8_t *d, size_t n, doc *o) {
	if (!d || n < 4 + 1 + 24 + 2 + 4 || memcmp(d, "ASVG", 4) != 0 || d[4] != 1) return false;
	size_t i = 5;
	o->w = rdf(d + i), o->h = rdf(d + i + 4);
	for (int k = 0; k < 4; k++) o->vb[k] = rdf(d + i + 8 + 4 * k);
	i += 24;
	o->align = d[i], o->slice = d[i + 1];
	i += 2;
	uint32_t ns;
	memcpy(&ns, d + i, 4);
	i += 4;
	if (ns > (n - i) / 8) return false;
	o->slots = d + i, o->nslots = ns;
	i += (size_t)ns * 8;
	if (n - i < 4) return false;
	uint32_t nc;
	memcpy(&nc, d + i, 4);
	i += 4;
	if (nc != n - i) return false;
	o->cmds = d + i, o->ncmds = nc;
	if (o->align > 9 || !(o->vb[2] > 0) || !(o->vb[3] > 0) || !(o->w >= 0) || !(o->h >= 0)) return false;
	for (uint32_t k = 0; k < ns; k++) {
		uint32_t off;
		memcpy(&off, o->slots + 8 * k, 4);
		if (off > nc || nc - off < 8) return false;
	}
	return true;
}

bool ak_svg_size(const uint8_t *d, size_t n, float *w, float *h) {
	doc o;
	if (!read_doc(d, n, &o)) return false;
	*w = o.w, *h = o.h;
	return true;
}

// walk checks a run of commands (they're whole, their paths whole, their
// saves and restores balanced) and with a tint, makes every color it.
typedef struct {
	uint8_t *p;
	size_t n, i;
	bool bad;
} wk;

static uint8_t *take(wk *w, size_t k) {
	if (w->bad || w->n - w->i < k) {
		w->bad = true;
		return NULL;
	}
	uint8_t *r = w->p + w->i;
	w->i += k;
	return r;
}

static uint32_t take_u32(wk *w) {
	uint8_t *q = take(w, 4);
	uint32_t v = 0;
	if (q) memcpy(&v, q, 4);
	return v;
}

static void take_color(wk *w, ak_color tint) {
	uint8_t *q = take(w, 8);
	if (!q || !tint) return;
	uint64_t c;
	memcpy(&c, q, 8);
	c = ak_color_opacity(tint, (float)ak_color_alpha(c) / 255);
	memcpy(q, &c, 8);
}

static void take_path(wk *w) {
	uint32_t n = take_u32(w);
	uint8_t *q = take(w, n);
	if (!q) return;
	for (uint32_t i = 0; i < n;) {
		int na = path_args(q[i++]);
		if (na < 0 || n - i < (uint32_t)na * 4) {
			w->bad = true;
			return;
		}
		i += (uint32_t)na * 4;
	}
}

static void take_stops(wk *w, ak_color tint) {
	uint32_t n = take_u32(w);
	if (n > (w->n - w->i) / 12) {
		w->bad = true;
		return;
	}
	for (uint32_t k = 0; k < n; k++) take_color(w, tint), take(w, 4);
}

static void take_stroke(wk *w) {
	take(w, 6);
	uint32_t nd = take_u32(w);
	if (nd > (w->n - w->i) / 4) {
		w->bad = true;
		return;
	}
	take(w, 4 + (size_t)nd * 4);
}

static int walk(uint8_t *p, size_t n, ak_color tint) {
	wk w = {p, n, 0, false};
	int depth = 0;
	while (w.i < w.n && !w.bad) {
		switch (p[w.i++]) {
		case AK_CANVAS_OP_SAVE: depth++; break;
		case AK_CANVAS_OP_RESTORE:
			if (--depth < 0) return -1;
			break;
		case AK_CANVAS_OP_TRANSLATE:
		case AK_CANVAS_OP_SCALE: take(&w, 8); break;
		case AK_CANVAS_OP_ROTATE:
		case AK_CANVAS_OP_ALPHA: take(&w, 4); break;
		case AK_CANVAS_OP_CLIP: take_path(&w); break;
		case AK_CANVAS_OP_FILL: take_color(&w, tint), take_path(&w); break;
		case AK_CANVAS_OP_STROKE: take_color(&w, tint), take_stroke(&w), take_path(&w); break;
		case AK_CANVAS_OP_GRADIENT: take_stops(&w, tint), take(&w, 16), take_path(&w); break;
		case AK_CANVAS_OP_TEXT: take_color(&w, tint), take(&w, 14), take(&w, take_u32(&w)); break;
		case AK_CANVAS_OP_TRANSFORM: take(&w, 24); break;
		case AK_CANVAS_OP_FILL_RULE: take(&w, 1); break;
		case AK_CANVAS_OP_RADIAL_GRADIENT: take_stops(&w, tint), take(&w, 20), take_path(&w); break;
		case AK_CANVAS_OP_STROKE_GRADIENT: {
			take_stroke(&w);
			uint8_t *k = take(&w, 1);
			take_stops(&w, tint);
			take(&w, k && *k ? 20 : 16);
			take_path(&w);
			break;
		}
		default: return -1;
		}
	}
	return w.bad ? -1 : depth;
}

bool ak_svg_draw(const uint8_t *d, size_t n, ak_buf *out, float w, float h, int mode, ak_color current, ak_color tint) {
	doc o;
	if (!read_doc(d, n, &o)) return false;
	size_t start = out->len;
	if (!(w > 0) || !(h > 0) || !(o.w > 0) || !(o.h > 0)) return true;
	// Its box: its size fitted into the view's by mode.
	double bx = 0, by = 0, bw = w, bh = h;
	switch (mode) {
	case AK_IMAGE_MODE_FILL:
	case AK_IMAGE_MODE_FIT: {
		double s = mode == AK_IMAGE_MODE_FIT ? fmin(w / o.w, h / o.h) : fmax(w / o.w, h / o.h);
		bw = o.w * s, bh = o.h * s;
		break;
	}
	case AK_IMAGE_MODE_CENTER: bw = o.w, bh = o.h; break;
	}
	bx = (w - bw) / 2, by = (h - bh) / 2;
	double vb[4] = {o.vb[0], o.vb[1], o.vb[2], o.vb[3]};
	mat m;
	fit_matrix(vb, bx, by, bw, bh, mode == AK_IMAGE_MODE_STRETCH ? 0 : o.align, o.slice, m);
	ak_buf_u8(out, AK_CANVAS_OP_SAVE);
	ak_buf_u8(out, AK_CANVAS_OP_CLIP);
	ak_buf_u32(out, 17);
	ak_buf_u8(out, AK_PATH_OP_RECT);
	ak_buf_f32(out, 0), ak_buf_f32(out, 0), ak_buf_f32(out, w), ak_buf_f32(out, h);
	ak_buf_u8(out, AK_CANVAS_OP_TRANSFORM);
	for (int k = 0; k < 6; k++) ak_buf_f32(out, (float)m[k]);
	size_t at = out->len;
	ak_buf_put(out, o.cmds, o.ncmds);
	for (uint32_t k = 0; k < o.nslots; k++) {
		uint32_t off;
		float op;
		memcpy(&off, o.slots + 8 * k, 4);
		memcpy(&op, o.slots + 8 * k + 4, 4);
		uint64_t c = ak_color_opacity(current, op);
		memcpy(out->data + at + off, &c, 8);
	}
	int depth = walk(out->data + at, o.ncmds, tint);
	if (depth < 0) {
		out->len = start;
		return false;
	}
	while (depth-- > 0) ak_buf_u8(out, AK_CANVAS_OP_RESTORE);
	ak_buf_u8(out, AK_CANVAS_OP_RESTORE);
	return true;
}
