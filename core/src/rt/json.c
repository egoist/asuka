#include "rt/json.h"

#include <stdio.h>
#include <stdlib.h>

// ---- Descriptors ----

static ak_kind desc_kind(const char *p) {
	switch (*p) {
	case 'i':
	case 'e':
		return AK_K_INT;
	case 'f':
		return AK_K_FLOAT;
	case 'b':
		return AK_K_BOOL;
	default:
		return AK_K_REF;
	}
}

static size_t name_len(const char *p) {
	const char *q = strchr(p + 1, ';');
	return q ? (size_t)(q - p - 1) : strlen(p + 1);
}

// ---- Parsing ----

typedef struct {
	const char *s, *end;
	const ak_json_types *t;
	bool failed;
	char msg[200];
} parser;

static void pos_of(parser *ps, const char *at, int *line, int *col) {
	*line = 1;
	*col = 1;
	for (const char *c = ps->s; c < at && c < ps->end; c++) {
		if (*c == '\n') {
			(*line)++;
			*col = 1;
		} else {
			(*col)++;
		}
	}
}

static void fail(parser *ps, const char *fmt, const char *what) {
	if (ps->failed) return;
	ps->failed = true;
	int line, col;
	pos_of(ps, ps->s, &line, &col);
	AK_UNUSED(line);
	AK_UNUSED(col);
	snprintf(ps->msg, sizeof ps->msg, fmt, what);
}

static void ws(parser *ps) {
	while (ps->s < ps->end && (*ps->s == ' ' || *ps->s == '\t' || *ps->s == '\n' || *ps->s == '\r')) ps->s++;
}

static bool lit(parser *ps, const char *w) {
	size_t n = strlen(w);
	if ((size_t)(ps->end - ps->s) >= n && memcmp(ps->s, w, n) == 0) {
		ps->s += n;
		return true;
	}
	return false;
}

static void put_utf8(ak_strb *b, uint32_t c) {
	char t[4];
	size_t n;
	if (c < 0x80) {
		t[0] = (char)c;
		n = 1;
	} else if (c < 0x800) {
		t[0] = (char)(0xc0 | c >> 6);
		t[1] = (char)(0x80 | (c & 0x3f));
		n = 2;
	} else if (c < 0x10000) {
		t[0] = (char)(0xe0 | c >> 12);
		t[1] = (char)(0x80 | (c >> 6 & 0x3f));
		t[2] = (char)(0x80 | (c & 0x3f));
		n = 3;
	} else {
		t[0] = (char)(0xf0 | c >> 18);
		t[1] = (char)(0x80 | (c >> 12 & 0x3f));
		t[2] = (char)(0x80 | (c >> 6 & 0x3f));
		t[3] = (char)(0x80 | (c & 0x3f));
		n = 4;
	}
	ak_strb_cstr(b, t, n);
}

static int hexv(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static uint32_t hex4(parser *ps) {
	if (ps->end - ps->s < 4) {
		fail(ps, "a short \\u escape%s", "");
		return 0;
	}
	uint32_t v = 0;
	for (int i = 0; i < 4; i++) {
		int h = hexv(ps->s[i]);
		if (h < 0) {
			fail(ps, "a bad \\u escape%s", "");
			return 0;
		}
		v = v << 4 | (uint32_t)h;
	}
	ps->s += 4;
	return v;
}

// str reads a JSON string (after its opening quote is checked).
static ak_str *str(parser *ps) {
	if (ps->s >= ps->end || *ps->s != '"') {
		fail(ps, "expected a string%s", "");
		return NULL;
	}
	ps->s++;
	const char *start = ps->s;
	// Fast path: no escapes.
	while (ps->s < ps->end && *ps->s != '"' && *ps->s != '\\') ps->s++;
	if (ps->s < ps->end && *ps->s == '"') {
		ak_str *r = ak_str_new(start, (size_t)(ps->s - start));
		ps->s++;
		return r;
	}
	ak_strb b = {0};
	ak_strb_cstr(&b, start, (size_t)(ps->s - start));
	while (ps->s < ps->end && *ps->s != '"') {
		char c = *ps->s++;
		if (c != '\\') {
			ak_strb_cstr(&b, &c, 1);
			continue;
		}
		if (ps->s >= ps->end) break;
		c = *ps->s++;
		switch (c) {
		case '"': ak_strb_cstr(&b, "\"", 1); break;
		case '\\': ak_strb_cstr(&b, "\\", 1); break;
		case '/': ak_strb_cstr(&b, "/", 1); break;
		case 'b': ak_strb_cstr(&b, "\b", 1); break;
		case 'f': ak_strb_cstr(&b, "\f", 1); break;
		case 'n': ak_strb_cstr(&b, "\n", 1); break;
		case 'r': ak_strb_cstr(&b, "\r", 1); break;
		case 't': ak_strb_cstr(&b, "\t", 1); break;
		case 'u': {
			uint32_t u = hex4(ps);
			if (u >= 0xd800 && u < 0xdc00 && ps->end - ps->s >= 6 && ps->s[0] == '\\' && ps->s[1] == 'u') {
				ps->s += 2;
				uint32_t lo = hex4(ps);
				if (lo >= 0xdc00 && lo < 0xe000) u = 0x10000 + ((u - 0xd800) << 10) + (lo - 0xdc00);
			}
			put_utf8(&b, u);
			break;
		}
		default:
			fail(ps, "a bad escape%s", "");
		}
	}
	if (ps->s >= ps->end) fail(ps, "a string that doesn't end%s", "");
	else ps->s++;
	return ak_strb_finish(&b);
}

static bool skip_value(parser *ps);

static bool skip_value(parser *ps) {
	ws(ps);
	if (ps->s >= ps->end) {
		fail(ps, "expected a value%s", "");
		return false;
	}
	char c = *ps->s;
	if (c == '"') {
		ak_str *s = str(ps);
		ak_release(s);
		return !ps->failed;
	}
	if (c == '{' || c == '[') {
		char close = c == '{' ? '}' : ']';
		ps->s++;
		ws(ps);
		if (ps->s < ps->end && *ps->s == close) {
			ps->s++;
			return true;
		}
		for (;;) {
			if (c == '{') {
				ws(ps);
				ak_str *k = str(ps);
				ak_release(k);
				ws(ps);
				if (!lit(ps, ":")) {
					fail(ps, "expected ':'%s", "");
					return false;
				}
			}
			if (!skip_value(ps)) return false;
			ws(ps);
			if (lit(ps, ",")) continue;
			if (ps->s < ps->end && *ps->s == close) {
				ps->s++;
				return true;
			}
			fail(ps, "expected ',' or a closing bracket%s", "");
			return false;
		}
	}
	if (lit(ps, "true") || lit(ps, "false") || lit(ps, "null")) return true;
	const char *st = ps->s;
	while (ps->s < ps->end && strchr("+-0123456789.eE", *ps->s)) ps->s++;
	if (ps->s == st) {
		fail(ps, "unexpected '%.1s'", st);
		return false;
	}
	return true;
}

static ak_val value(parser *ps, const char *d);

static bool number(parser *ps, double *out, bool *integral, int64_t *iv) {
	ws(ps);
	const char *st = ps->s;
	if (ps->s < ps->end && *ps->s == '-') ps->s++;
	while (ps->s < ps->end && ((*ps->s >= '0' && *ps->s <= '9') || *ps->s == '.' || *ps->s == 'e' || *ps->s == 'E' || *ps->s == '+' || *ps->s == '-')) ps->s++;
	if (ps->s == st) {
		fail(ps, "expected a number%s", "");
		return false;
	}
	char buf[64];
	size_t n = (size_t)(ps->s - st);
	if (n >= sizeof buf) n = sizeof buf - 1;
	memcpy(buf, st, n);
	buf[n] = 0;
	*integral = !strpbrk(buf, ".eE");
	if (*integral) *iv = strtoll(buf, NULL, 10);
	*out = strtod(buf, NULL);
	return true;
}

static ak_val object_struct(parser *ps, const ak_shape *s, const char *d) {
	AK_UNUSED(d);
	ak_struct *o = ak_struct_new(s);
	bool *set = s->nfields ? ak_alloc(s->nfields) : NULL;
	ws(ps);
	if (!lit(ps, "{")) {
		fail(ps, "expected an object for %s", s->name);
		goto done;
	}
	ws(ps);
	if (lit(ps, "}")) goto check;
	for (;;) {
		ws(ps);
		ak_str *k = str(ps);
		if (ps->failed) {
			ak_release(k);
			goto done;
		}
		ws(ps);
		if (!lit(ps, ":")) {
			ak_release(k);
			fail(ps, "expected ':'%s", "");
			goto done;
		}
		int idx = -1;
		for (uint16_t i = 0; i < s->nfields; i++)
			if (s->names && strcmp(s->names[i], k->data) == 0) idx = i;
		ak_release(k);
		if (idx < 0) {
			if (!skip_value(ps)) goto done;  // a field it doesn't have
		} else {
			// The field's own descriptor is in the shape's field
			// descriptors, which the compiler keeps in the shape's names
			// as "name:desc" ... see field_desc.
			extern const char *ak_json_field_desc(const ak_shape *s, int i);
			const char *fd = ak_json_field_desc(s, idx);
			ak_val v = value(ps, fd);
			if (ps->failed) goto done;
			ak_release_val(o->f[idx], (ak_kind)s->kinds[idx]);
			o->f[idx] = v;
			set[idx] = true;
		}
		ws(ps);
		if (lit(ps, ",")) continue;
		if (lit(ps, "}")) break;
		fail(ps, "expected ',' or '}'%s", "");
		goto done;
	}
check:
	for (uint16_t i = 0; i < s->nfields; i++) {
		if (set[i]) continue;
		extern const char *ak_json_field_desc(const ak_shape *s, int i);
		const char *fd = ak_json_field_desc(s, i);
		if (fd[0] == '?' || desc_kind(fd) != AK_K_REF) continue;  // none, or a zero scalar
		// A string or a collection starts empty; a struct is required.
		if (fd[0] == 's') o->f[i].p = ak_str_empty();
		else if (fd[0] == '[') o->f[i].p = ak_arr_new(desc_kind(fd + 1), NULL, 0);
		else if (fd[0] == '{') o->f[i].p = ak_map_new(AK_K_REF, true, desc_kind(fd + 1), NULL);
		else if (fd[0] == '<') o->f[i].p = ak_set_new(desc_kind(fd + 1), fd[1] == 's');
		else {
			fail(ps, "a missing field %s", s->names[i]);
			goto done;
		}
	}
done:
	ak_free(set);
	if (ps->failed) {
		ak_release(o);
		return (ak_val){0};
	}
	return (ak_val){.p = o};
}

static ak_val value(parser *ps, const char *d) {
	ws(ps);
	if (ps->s >= ps->end) {
		fail(ps, "expected a value%s", "");
		return (ak_val){0};
	}
	switch (*d) {
	case '?':
		if (lit(ps, "null")) return (ak_val){0};
		{
			ak_kind k = desc_kind(d + 1);
			ak_val v = value(ps, d + 1);
			if (ps->failed || k == AK_K_REF) return v;
			return (ak_val){.p = ak_box_new(k, v)};
		}
	case 'i': {
		double f;
		bool integral;
		int64_t iv = 0;
		if (!number(ps, &f, &integral, &iv)) return (ak_val){0};
		if (!integral) {
			if (f != floor(f)) fail(ps, "expected an integer%s", "");
			iv = (int64_t)f;
		}
		return (ak_val){.i = iv};
	}
	case 'f': {
		double f;
		bool integral;
		int64_t iv;
		if (!number(ps, &f, &integral, &iv)) return (ak_val){0};
		return (ak_val){.f = f};
	}
	case 'b':
		if (lit(ps, "true")) return (ak_val){.i = 1};
		if (lit(ps, "false")) return (ak_val){.i = 0};
		fail(ps, "expected true or false%s", "");
		return (ak_val){0};
	case 's':
		return (ak_val){.p = str(ps)};
	case 'y': {
		ak_str *s = str(ps);
		if (!s) return (ak_val){0};
		// base64
		static const char *tab = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		uint8_t *out = ak_alloc(s->len);
		size_t n = 0;
		uint32_t acc = 0;
		int bits = 0;
		for (uint32_t i = 0; i < s->len; i++) {
			char c = s->data[i];
			if (c == '=') break;
			const char *p = strchr(tab, c);
			if (!p || !c) {
				if (c == '-') p = tab + 62;
				else if (c == '_') p = tab + 63;
				else continue;
			}
			acc = acc << 6 | (uint32_t)(p - tab);
			bits += 6;
			if (bits >= 8) {
				bits -= 8;
				out[n++] = (uint8_t)(acc >> bits);
			}
		}
		ak_release(s);
		ak_bytes *b = ak_bytes_new(out, n);
		ak_free(out);
		return (ak_val){.p = b};
	}
	case '[': {
		const char *ed = d + 1;
		ak_kind ek = desc_kind(ed);
		// null is an empty array, as Go's encoding/json writes a nil slice.
		bool null = lit(ps, "null");
		if (!null && !lit(ps, "[")) {
			fail(ps, "expected an array%s", "");
			return (ak_val){0};
		}
		const ak_shape *es = NULL;
		if ((*ed == 'S' || *ed == 'E') && ps->t && ps->t->shape) es = ps->t->shape(ps->t->ctx, ed + 1, name_len(ed));
		ak_arr *a = ak_arr_new(ek, es, 0);
		if (*ed == 's') a->h.flags = 1;
		if (null) return (ak_val){.p = a};
		ws(ps);
		if (lit(ps, "]")) return (ak_val){.p = a};
		for (;;) {
			ak_val v = value(ps, ed);
			if (ps->failed) {
				ak_release(a);
				return (ak_val){0};
			}
			ak_arr_push(&a, v);
			ws(ps);
			if (lit(ps, ",")) continue;
			if (lit(ps, "]")) break;
			fail(ps, "expected ',' or ']'%s", "");
			ak_release(a);
			return (ak_val){0};
		}
		return (ak_val){.p = a};
	}
	case '<': {
		// A set: an array of its elements (one written twice is one).
		const char *ed = d + 1;
		ak_kind ek = desc_kind(ed);
		bool null = lit(ps, "null");  // an empty set
		if (!null && !lit(ps, "[")) {
			fail(ps, "expected an array%s", "");
			return (ak_val){0};
		}
		ak_map *s = ak_set_new(ek, *ed == 's');
		if (null) return (ak_val){.p = s};
		ws(ps);
		if (lit(ps, "]")) return (ak_val){.p = s};
		for (;;) {
			ak_val v = value(ps, ed);
			if (ps->failed) {
				ak_release(s);
				return (ak_val){0};
			}
			ak_set_insert(&s, v);
			ws(ps);
			if (lit(ps, ",")) continue;
			if (lit(ps, "]")) break;
			fail(ps, "expected ',' or ']'%s", "");
			ak_release(s);
			return (ak_val){0};
		}
		return (ak_val){.p = s};
	}
	case '{': {
		const char *vd = d + 1;
		ak_kind vk = desc_kind(vd);
		bool null = lit(ps, "null");  // an empty map, as Go writes a nil one
		if (!null && !lit(ps, "{")) {
			fail(ps, "expected an object%s", "");
			return (ak_val){0};
		}
		ak_map *m = ak_map_new(AK_K_REF, true, vk, NULL);
		if (null) return (ak_val){.p = m};
		ws(ps);
		if (lit(ps, "}")) return (ak_val){.p = m};
		for (;;) {
			ws(ps);
			ak_str *k = str(ps);
			ws(ps);
			if (ps->failed || !lit(ps, ":")) {
				ak_release(k);
				fail(ps, "expected ':'%s", "");
				ak_release(m);
				return (ak_val){0};
			}
			ak_val v = value(ps, vd);
			if (ps->failed) {
				ak_release(k);
				ak_release(m);
				return (ak_val){0};
			}
			ak_map_set(&m, (ak_val){.p = k}, v);
			ws(ps);
			if (lit(ps, ",")) continue;
			if (lit(ps, "}")) break;
			fail(ps, "expected ',' or '}'%s", "");
			ak_release(m);
			return (ak_val){0};
		}
		return (ak_val){.p = m};
	}
	case 'S': {
		const ak_shape *s = ps->t && ps->t->shape ? ps->t->shape(ps->t->ctx, d + 1, name_len(d)) : NULL;
		if (!s) {
			fail(ps, "no type %s", d + 1);
			return (ak_val){0};
		}
		return object_struct(ps, s, d);
	}
	case 'e': {
		const char *const *cases;
		uint32_t n;
		if (!ps->t || !ps->t->cases || !ps->t->cases(ps->t->ctx, d + 1, name_len(d), &cases, &n)) {
			fail(ps, "no enum %s", d + 1);
			return (ak_val){0};
		}
		ak_str *s = str(ps);
		if (!s) return (ak_val){0};
		for (uint32_t i = 0; i < n; i++) {
			if (strcmp(cases[i], s->data) == 0) {
				ak_release(s);
				return (ak_val){.i = i};
			}
		}
		fail(ps, "not a case: %s", s->data);
		ak_release(s);
		return (ak_val){0};
	}
	case 'E': {
		const ak_shape *s = ps->t && ps->t->shape ? ps->t->shape(ps->t->ctx, d + 1, name_len(d)) : NULL;
		if (!s) {
			fail(ps, "no type %s", d + 1);
			return (ak_val){0};
		}
		if (ps->s < ps->end && *ps->s == '"') {
			ak_str *cn = str(ps);
			for (uint16_t c = 0; c < s->ncases; c++) {
				if (strcmp(s->cases[c].name, cn->data) == 0 && s->cases[c].nfields == 0) {
					ak_release(cn);
					return (ak_val){.p = ak_enum_new(s, c)};
				}
			}
			fail(ps, "not a case without a payload: %s", cn->data);
			ak_release(cn);
			return (ak_val){0};
		}
		if (!lit(ps, "{")) {
			fail(ps, "expected a case of %s", s->name);
			return (ak_val){0};
		}
		ws(ps);
		ak_str *cn = str(ps);
		ws(ps);
		if (ps->failed || !lit(ps, ":")) {
			ak_release(cn);
			fail(ps, "expected ':'%s", "");
			return (ak_val){0};
		}
		int ci = -1;
		for (uint16_t c = 0; c < s->ncases; c++)
			if (strcmp(s->cases[c].name, cn->data) == 0) ci = c;
		if (ci < 0) {
			fail(ps, "not a case: %s", cn->data);
			ak_release(cn);
			return (ak_val){0};
		}
		ak_release(cn);
		const ak_case *cs = &s->cases[ci];
		ak_enum *e = ak_enum_new(s, (uint16_t)ci);
		extern const char *ak_json_case_desc(const ak_shape *s, int c, int i);
		ws(ps);
		if (lit(ps, "[")) {
			for (uint16_t i = 0; i < cs->nfields; i++) {
				if (i) {
					ws(ps);
					if (!lit(ps, ",")) fail(ps, "expected ','%s", "");
				}
				e->f[i] = value(ps, ak_json_case_desc(s, ci, i));
				if (ps->failed) break;
			}
			ws(ps);
			if (!ps->failed && !lit(ps, "]")) fail(ps, "expected ']'%s", "");
		} else if (lit(ps, "{")) {
			for (;;) {
				ws(ps);
				if (lit(ps, "}")) break;
				ak_str *k = str(ps);
				ws(ps);
				if (ps->failed || !lit(ps, ":")) {
					ak_release(k);
					fail(ps, "expected ':'%s", "");
					break;
				}
				int fi = -1;
				for (uint16_t i = 0; i < cs->nfields; i++)
					if (cs->names && cs->names[i] && strcmp(cs->names[i], k->data) == 0) fi = i;
				ak_release(k);
				if (fi < 0) skip_value(ps);
				else e->f[fi] = value(ps, ak_json_case_desc(s, ci, fi));
				if (ps->failed) break;
				ws(ps);
				lit(ps, ",");
			}
		} else if (cs->nfields == 1) {
			e->f[0] = value(ps, ak_json_case_desc(s, ci, 0));
		} else {
			fail(ps, "expected the case's payload%s", "");
		}
		ws(ps);
		if (!ps->failed && !lit(ps, "}")) fail(ps, "expected '}'%s", "");
		if (ps->failed) {
			if (cs->nfields) ak_release(e);
			return (ak_val){0};
		}
		return (ak_val){.p = e};
	}
	}
	fail(ps, "a bad type descriptor %s", d);
	return (ak_val){0};
}

ak_val ak_json_decode(const char *text, size_t len, const char *d, const ak_json_types *t) {
	parser ps = {.s = text, .end = text + len, .t = t};
	ak_val v = value(&ps, d);
	ws(&ps);
	if (!ps.failed && ps.s != ps.end) fail(&ps, "more after the value%s", "");
	if (ps.failed) {
		int line, col;
		pos_of(&(parser){.s = text, .end = text + len}, ps.s, &line, &col);
		ak_release_val(v, desc_kind(d));
		ak_throw_msg("json: %d:%d: %s", line, col, ps.msg);
		return (ak_val){0};
	}
	return v;
}

// ---- Field descriptors ----
//
// A shape's field descriptors are what the compiler writes after each
// name, "name\0desc" (the name ends at its NUL, the descriptor follows),
// so the same names table carries both.

const char *ak_json_field_desc(const ak_shape *s, int i) {
	const char *n = s->names ? s->names[i] : NULL;
	if (!n) return "";
	const char *d = n + strlen(n) + 1;
	return *d ? d : (s->kinds[i] == AK_K_INT ? "i" : s->kinds[i] == AK_K_FLOAT ? "f" : s->kinds[i] == AK_K_BOOL ? "b" : "s");
}

const char *ak_json_case_desc(const ak_shape *s, int c, int i) {
	const ak_case *cs = &s->cases[c];
	const char *n = cs->names ? cs->names[i] : NULL;
	if (!n) return cs->kinds[i] == AK_K_INT ? "i" : cs->kinds[i] == AK_K_FLOAT ? "f" : cs->kinds[i] == AK_K_BOOL ? "b" : "s";
	const char *d = n + strlen(n) + 1;
	return *d ? d : "s";
}

// ---- Encoding ----

static void enc_str(ak_strb *b, const char *s, size_t n) {
	ak_strb_cstr(b, "\"", 1);
	size_t start = 0;
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)s[i];
		const char *esc = NULL;
		char u[8];
		switch (c) {
		case '"': esc = "\\\""; break;
		case '\\': esc = "\\\\"; break;
		case '\n': esc = "\\n"; break;
		case '\r': esc = "\\r"; break;
		case '\t': esc = "\\t"; break;
		default:
			if (c < 0x20) {
				snprintf(u, sizeof u, "\\u%04x", c);
				esc = u;
			}
		}
		if (esc) {
			ak_strb_cstr(b, s + start, i - start);
			ak_strb_cstr(b, esc, strlen(esc));
			start = i + 1;
		}
	}
	ak_strb_cstr(b, s + start, n - start);
	ak_strb_cstr(b, "\"", 1);
}

static void nl(ak_strb *b, int indent, int depth) {
	if (indent <= 0) return;
	ak_strb_cstr(b, "\n", 1);
	for (int i = 0; i < indent * depth; i++) ak_strb_cstr(b, " ", 1);
}

static void enc(ak_strb *b, ak_val v, const char *d, const ak_json_types *t, int indent, int depth) {
	switch (*d) {
	case 'i':
		ak_strb_int(b, v.i);
		return;
	case 'f':
		if (v.f != v.f || v.f == INFINITY || v.f == -INFINITY) ak_strb_cstr(b, "null", 4);
		else ak_strb_float(b, v.f);
		return;
	case 'b':
		ak_strb_bool(b, v.i != 0);
		return;
	case 's': {
		ak_str *s = v.p;
		if (s) enc_str(b, s->data, s->len);
		else ak_strb_cstr(b, "\"\"", 2);
		return;
	}
	case 'y': {
		static const char *tab = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		ak_bytes *by = v.p;
		ak_strb_cstr(b, "\"", 1);
		for (uint32_t i = 0; by && i < by->len; i += 3) {
			uint32_t x = (uint32_t)by->data[i] << 16 | (i + 1 < by->len ? (uint32_t)by->data[i + 1] << 8 : 0) | (i + 2 < by->len ? by->data[i + 2] : 0);
			char o[4] = {tab[x >> 18 & 63], tab[x >> 12 & 63], i + 1 < by->len ? tab[x >> 6 & 63] : '=', i + 2 < by->len ? tab[x & 63] : '='};
			ak_strb_cstr(b, o, 4);
		}
		ak_strb_cstr(b, "\"", 1);
		return;
	}
	case '?':
		if (!v.p) {
			ak_strb_cstr(b, "null", 4);
			return;
		}
		if (desc_kind(d + 1) != AK_K_REF) v = ((ak_box *)v.p)->v;
		enc(b, v, d + 1, t, indent, depth);
		return;
	case '[': {
		ak_arr *a = v.p;
		ak_strb_cstr(b, "[", 1);
		for (uint32_t i = 0; a && i < a->len; i++) {
			if (i) ak_strb_cstr(b, ",", 1);
			nl(b, indent, depth + 1);
			enc(b, a->data[i], d + 1, t, indent, depth + 1);
		}
		if (a && a->len) nl(b, indent, depth);
		ak_strb_cstr(b, "]", 1);
		return;
	}
	case '<': {
		ak_map *s = v.p;
		ak_strb_cstr(b, "[", 1);
		bool first = true;
		for (uint32_t i = 0; s && i < s->len; i++) {
			if (s->dead[i]) continue;
			if (!first) ak_strb_cstr(b, ",", 1);
			first = false;
			nl(b, indent, depth + 1);
			enc(b, s->keys[i], d + 1, t, indent, depth + 1);
		}
		if (!first) nl(b, indent, depth);
		ak_strb_cstr(b, "]", 1);
		return;
	}
	case '{': {
		ak_map *m = v.p;
		ak_strb_cstr(b, "{", 1);
		bool first = true;
		for (uint32_t i = 0; m && i < m->len; i++) {
			if (m->dead[i]) continue;
			if (!first) ak_strb_cstr(b, ",", 1);
			first = false;
			nl(b, indent, depth + 1);
			ak_str *k = m->keys[i].p;
			enc_str(b, k->data, k->len);
			ak_strb_cstr(b, indent > 0 ? ": " : ":", indent > 0 ? 2 : 1);
			enc(b, m->vals[i], d + 1, t, indent, depth + 1);
		}
		if (!first) nl(b, indent, depth);
		ak_strb_cstr(b, "}", 1);
		return;
	}
	case 'S': {
		ak_struct *s = v.p;
		ak_strb_cstr(b, "{", 1);
		for (uint16_t i = 0; s && i < s->shape->nfields; i++) {
			if (i) ak_strb_cstr(b, ",", 1);
			nl(b, indent, depth + 1);
			const char *n = s->shape->names ? s->shape->names[i] : "";
			enc_str(b, n, strlen(n));
			ak_strb_cstr(b, indent > 0 ? ": " : ":", indent > 0 ? 2 : 1);
			enc(b, s->f[i], ak_json_field_desc(s->shape, i), t, indent, depth + 1);
		}
		if (s && s->shape->nfields) nl(b, indent, depth);
		ak_strb_cstr(b, "}", 1);
		return;
	}
	case 'e': {
		const char *const *cases;
		uint32_t n;
		if (t && t->cases && t->cases(t->ctx, d + 1, name_len(d), &cases, &n) && v.i >= 0 && (uint64_t)v.i < n) enc_str(b, cases[v.i], strlen(cases[v.i]));
		else ak_strb_int(b, v.i);
		return;
	}
	case 'E': {
		ak_enum *e = v.p;
		const ak_case *c = &e->shape->cases[ak_enum_case(e)];
		if (c->nfields == 0) {
			enc_str(b, c->name, strlen(c->name));
			return;
		}
		ak_strb_cstr(b, "{", 1);
		enc_str(b, c->name, strlen(c->name));
		ak_strb_cstr(b, indent > 0 ? ": [" : ":[", indent > 0 ? 3 : 2);
		for (uint16_t i = 0; i < c->nfields; i++) {
			if (i) ak_strb_cstr(b, ",", 1);
			enc(b, e->f[i], ak_json_case_desc(e->shape, ak_enum_case(e), i), t, indent, depth + 1);
		}
		ak_strb_cstr(b, "]}", 2);
		return;
	}
	}
	ak_strb_cstr(b, "null", 4);
}

ak_str *ak_json_encode(ak_val v, const char *d, const ak_json_types *t, int indent) {
	ak_strb b = {0};
	enc(&b, v, d, t, indent, 0);
	return ak_strb_finish(&b);
}
