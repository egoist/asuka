#include "rt/json.h"
#include "rt/unicode.h"
#include "std/std.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

// ---- Descriptors ----

static ak_kind desc_kind(const char *p) {
	switch (*p) {
	case 'i':
	case 'n':
	case 'e':
	case 'r':
		return AK_K_INT;
	case 'f':
		return AK_K_FLOAT;
	case 'b':
		return AK_K_BOOL;
	default:
		return AK_K_REF;
	}
}

// desc_end is the end of the descriptor at p.
static const char *desc_end(const char *p) {
	for (;;) {
		switch (*p) {
		case '?':
		case '[':
		case '{':
		case '<':
			p++;
			continue;
		case 'S':
		case 'E':
		case 'J':
		case 'e': {
			const char *q = strchr(p, ';');
			return q ? q + 1 : p + strlen(p);
		}
		case 'm':
			return desc_end(desc_end(p + 1));
		case 'n':
			return p[1] ? p + 2 : p + 1;  // n and a sized integer type's code
		case '\0':
			return p;
		default:
			return p + 1;
		}
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
	int depth;  // arrays and objects open, up to AK_JSON_MAX_DEPTH
	const char *jdesc;       // the json.Value descriptor last looked up,
	const ak_shape *jshape;  // and its shape
	bool failed;
	char msg[200];
	void *err;  // what a type's own fromJSON threw: the decode's error
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

// null_next reports whether the value at ps is null, without reading it.
static bool null_next(const parser *ps) {
	const char *p = ps->s;
	return ps->end - p >= 4 && memcmp(p, "null", 4) == 0 && (ps->end - p == 4 || !((p[4] >= 'a' && p[4] <= 'z') || (p[4] >= '0' && p[4] <= '9')));
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
			if (u >= 0xd800 && u < 0xe000) {
				// A surrogate: a high one and the low one escaped after
				// it are one character; one without its pair is U+FFFD,
				// as JavaScript's TextDecoder and Go read it (UTF-8
				// can't hold it), and the escape after it is its own.
				uint32_t lo = 0;
				if (u < 0xdc00 && ps->end - ps->s >= 6 && ps->s[0] == '\\' && ps->s[1] == 'u') {
					ps->s += 2;
					lo = hex4(ps);
					if (!ps->failed && (lo < 0xdc00 || lo >= 0xe000)) ps->s -= 6;  // not its pair: read again
				}
				u = lo >= 0xdc00 && lo < 0xe000 ? 0x10000 + ((u - 0xd800) << 10) + (lo - 0xdc00) : 0xfffd;
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
	if (ps->failed) {
		ak_buf_free(&b.b);
		return NULL;
	}
	return ak_strb_finish(&b);
}

// enter opens an array or an object, one level deeper; false, failing,
// past AK_JSON_MAX_DEPTH (the parser recurses: a stack's worth of
// nesting would overflow it).
static bool enter(parser *ps) {
	if (ps->depth >= AK_JSON_MAX_DEPTH) {
		fail(ps, "nested too deeply%s", "");
		return false;
	}
	ps->depth++;
	return true;
}

static bool skip_value(parser *ps);
static bool skip_nested(parser *ps, char c);

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
		if (!enter(ps)) return false;
		bool ok = skip_nested(ps, c);
		ps->depth--;
		return ok;
	}
	if (lit(ps, "true") || lit(ps, "false") || lit(ps, "null")) return true;
	const char *st = ps->s;
	while (ps->s < ps->end && *ps->s && strchr("+-0123456789.eE", *ps->s)) ps->s++;
	if (ps->s == st) {
		fail(ps, "unexpected '%.1s'", st);
		return false;
	}
	return true;
}

// skip_nested skips an array's or an object's items, at its bracket c.
static bool skip_nested(parser *ps, char c) {
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

static ak_val value(parser *ps, const char *d);

// own_json reads a value of a type with its own JSON (shape s): what its
// fromJSON takes (s->jdesc), made one by it. What fromJSON throws is the
// decode's error.
static ak_val own_json(parser *ps, const ak_shape *s) {
	ak_val j = value(ps, s->jdesc);
	if (ps->failed) return (ak_val){0};
	ak_val v = ak_shape_call(s, AK_HOOK_FROMJSON, &j, 1);
	ak_release_val(j, desc_kind(s->jdesc));
	if (ak_error_pending()) {
		ps->err = ak_error_take();
		fail(ps, "%s", s->name);
		return (ak_val){0};
	}
	return v;
}

// json_bigint reads std/bigint's BigInt (of shape s): an integer of any
// size, or a string of one (as APIs send what a double can't hold).
static ak_val json_bigint(parser *ps, const ak_shape *s) {
	ws(ps);
	ak_str *q = NULL;
	const char *st = ps->s, *end;
	if (ps->s < ps->end && *ps->s == '"') {
		q = str(ps);
		if (!q) return (ak_val){0};
		st = q->data;
		end = q->data + q->len;
	} else {
		if (ps->s < ps->end && *ps->s == '-') ps->s++;
		while (ps->s < ps->end && *ps->s >= '0' && *ps->s <= '9') ps->s++;
		end = ps->s;
	}
	const char *p = st < end && *st == '-' ? st + 1 : st;
	bool ok = p < end && (p[0] != '0' || end - p == 1 || q);
	for (const char *c = p; ok && c < end; c++) ok = *c >= '0' && *c <= '9';
	if (!q && ok && ps->s < ps->end && (*ps->s == '.' || *ps->s == 'e' || *ps->s == 'E')) ok = false;
	ak_bytes *b = ok ? ak_link_bigint->read(st, (size_t)(end - st)) : NULL;
	ak_release(q);
	if (!b) {
		if (ak_error_pending()) {
			void *err = ak_error_take();
			fail(ps, "%s", ak_error_message(err)->data);
			ak_release(err);
		} else {
			fail(ps, "expected an integer%s", "");
		}
		return (ak_val){0};
	}
	ak_struct *v = ak_struct_new(s);
	v->f[0].p = b;
	return (ak_val){.p = v};
}

// number reads a number, whole; with iv (an int's), an integral one is
// read as an integer too, which must be in int's range.
static bool number(parser *ps, double *out, bool *integral, int64_t *iv) {
	ws(ps);
	if (ps->t && ps->t->nonfinite && !iv) {
		if (lit(ps, "NaN")) return *out = NAN, *integral = false, true;
		if (lit(ps, "Infinity")) return *out = INFINITY, *integral = false, true;
		if (lit(ps, "-Infinity")) return *out = -INFINITY, *integral = false, true;
	}
	const char *st = ps->s;
	if (ps->s < ps->end && *ps->s == '-') ps->s++;
	while (ps->s < ps->end && ((*ps->s >= '0' && *ps->s <= '9') || *ps->s == '.' || *ps->s == 'e' || *ps->s == 'E' || *ps->s == '+' || *ps->s == '-')) ps->s++;
	if (ps->s == st) {
		fail(ps, "expected a number%s", "");
		return false;
	}
	// The whole number, NUL-terminated for strtod: on the stack, or (one
	// longer than any a program writes) on the heap.
	char small[64];
	size_t n = (size_t)(ps->s - st);
	char *buf = n < sizeof small ? small : ak_alloc(n + 1);
	memcpy(buf, st, n);
	buf[n] = 0;
	*integral = !strpbrk(buf, ".eE");
	char *e;
	bool ok = true;
	if (*integral && iv) {
		errno = 0;
		*iv = strtoll(buf, &e, 10);
		if (e != buf + n) ok = false, fail(ps, "a bad number%s", "");
		else if (errno == ERANGE) ok = false, fail(ps, "an integer out of range%s", "");
	}
	*out = strtod(buf, &e);
	if (ok && e != buf + n) ok = false, fail(ps, "a bad number%s", "");
	else if (ok && isinf(*out)) ok = false, fail(ps, "a number out of range%s", "");
	if (buf != small) ak_free(buf);
	return ok;
}

// sized reads an integer of a sized type (its code t): an integer's
// digits, or a number with a fraction or an exponent that's a whole
// number, in the type's range.
static ak_val sized(parser *ps, int t) {
	ws(ps);
	const char *st = ps->s;
	if (ps->s < ps->end && *ps->s == '-') ps->s++;
	while (ps->s < ps->end && ((*ps->s >= '0' && *ps->s <= '9') || *ps->s == '.' || *ps->s == 'e' || *ps->s == 'E' || *ps->s == '+' || *ps->s == '-')) ps->s++;
	size_t n = (size_t)(ps->s - st);
	if (!n) {
		fail(ps, "expected a number%s", "");
		return (ak_val){0};
	}
	int64_t v = 0;
	if (!memchr(st, '.', n) && !memchr(st, 'e', n) && !memchr(st, 'E', n)) {
		if (!ak_int_parse(st, n, 10, t, &v)) fail(ps, "an integer out of %s's range", ak_int_names[t & 7]);
		return (ak_val){.i = v};
	}
	ps->s = st;
	double f;
	bool integral;
	if (!number(ps, &f, &integral, NULL)) return (ak_val){0};
	if (f != floor(f)) {
		fail(ps, "expected an integer%s", "");
	} else if (t == AK_U64) {
		if (f >= 0 && f < 0x1p64) v = (int64_t)(uint64_t)f;
		else fail(ps, "an integer out of %s's range", ak_int_names[t & 7]);
	} else if (!(f >= -0x1p63 && f < 0x1p63) || !ak_int_fits((int64_t)f, AK_I64, t)) {
		fail(ps, "an integer out of %s's range", ak_int_names[t & 7]);
	} else {
		v = (int64_t)f;
	}
	return (ak_val){.i = v};
}

// zero_field starts a field the JSON leaves out, of descriptor fd: none,
// a scalar's zero, an empty string or collection; false for one it can't
// leave out (a struct, an enum, bytes).
static ak_enum *json_null(parser *ps, const char *d);

static bool zero_field(parser *ps, const char *fd, ak_val *out) {
	if (fd[0] == '?' || desc_kind(fd) != AK_K_REF) return true;  // none, or a zero scalar
	if (fd[0] == 'J') {
		// A json.Value left out: null.
		ak_enum *n = json_null(ps, fd);
		out->p = n;
		return n != NULL;
	}
	if (fd[0] == 's') out->p = ak_str_empty();
	else if (fd[0] == '[') out->p = ak_arr_new(ak_desc_arr_kind(fd + 1, desc_kind(fd + 1)), NULL, 0);
	else if (fd[0] == '{') out->p = ak_map_new(AK_K_REF, true, desc_kind(fd + 1), NULL);
	else if (fd[0] == '<') out->p = ak_set_new(desc_kind(fd + 1), fd[1] == 's');
	else return false;
	return true;
}

// snake_eq is whether the n bytes at k, a key written in snake_case
// (or kebab-case) as most web APIs write them, name the field name:
// each word of the key, its first letter in either case, is the name's
// next (html_url reads into htmlUrl and htmlURL, created_at_i into
// createdAtI). The key has an underscore or a dash, so a key that's
// the name in another case alone (ID, Title) names no field.
static bool snake_eq(const char *name, const char *k, size_t n) {
	bool sep = false;
	size_t i = 0, j = 0;
	for (; j < n; j++)
		if (k[j] == '_' || k[j] == '-') sep = true;
	if (!sep) return false;
	for (j = 0; j < n; j++) {
		char c = k[j];
		if (c == '_' || c == '-') continue;
		char p = name[i];
		if (p == 0) return false;
		if (p >= 'A' && p <= 'Z') p = (char)(p - 'A' + 'a');
		if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
		if (p != c) return false;
		i++;
	}
	return name[i] == 0;
}

// field_named is the field of s named by the n bytes at k, -1 when it
// has none: the one after the last (hint) first, as an object's keys
// usually come in its fields' order, then each. A key that's no
// field's name may be one in snake_case (snake_eq): *loose says so.
static int field_named(const ak_shape *s, const char *k, size_t n, int hint, bool *loose) {
	*loose = false;
	if (!s->names) return -1;
	for (int j = 0; j < s->nfields; j++) {
		int i = (hint + j) % s->nfields;
		const char *name = s->names[i];
		if (name && strncmp(name, k, n) == 0 && name[n] == 0) return i;
	}
	for (int i = 0; i < s->nfields; i++) {
		if (s->names[i] && snake_eq(s->names[i], k, n)) {
			*loose = true;
			return i;
		}
	}
	return -1;
}

// key_field reads an object's key, a string, and is the field of s it
// names (-1: none): without making the string when it has no escapes.
static int key_field(parser *ps, const ak_shape *s, int hint, bool *loose) {
	*loose = false;
	if (ps->s < ps->end && *ps->s == '"') {
		const char *k = ps->s + 1, *e = k;
		while (e < ps->end && *e != '"' && *e != '\\') e++;
		if (e < ps->end && *e == '"') {
			ps->s = e + 1;
			return field_named(s, k, (size_t)(e - k), hint, loose);
		}
	}
	ak_str *k = str(ps);
	if (ps->failed) {
		ak_release(k);
		return -1;
	}
	int i = field_named(s, k->data, k->len, hint, loose);
	ak_release(k);
	return i;
}

static ak_val object_struct(parser *ps, const ak_shape *s, const char *d) {
	AK_UNUSED(d);
	ak_struct *o = ak_struct_new(s);
	// Which fields the object set (1; 2 by their own name, which a
	// snake_case key doesn't replace): on the stack for most structs.
	uint8_t few[64];
	uint8_t *set = s->nfields <= 64 ? few : ak_alloc(s->nfields);
	memset(set, 0, s->nfields);
	int next = 0;
	ws(ps);
	if (!lit(ps, "{")) {
		fail(ps, "expected an object for %s", s->name);
		goto done;
	}
	ws(ps);
	if (lit(ps, "}")) goto check;
	for (;;) {
		ws(ps);
		bool loose;
		int idx = key_field(ps, s, next, &loose);
		if (ps->failed) goto done;
		ws(ps);
		if (!lit(ps, ":")) {
			fail(ps, "expected ':'%s", "");
			goto done;
		}
		if (idx >= 0 && !loose) next = idx + 1 < s->nfields ? idx + 1 : 0;
		ws(ps);
		extern const char *ak_json_field_desc(const ak_shape *s, int i);
		if (idx >= 0 && ak_json_field_desc(s, idx)[0] != '?' && null_next(ps)) {
			// null for a field that isn't optional: as if the object didn't
			// have it (an API's "text": null), empty unless a key sets it.
			idx = -1;
		}
		if (idx < 0 || (loose && set[idx] == 2)) {
			if (!skip_value(ps)) goto done;  // a field it doesn't have
		} else {
			// The field's own descriptor is in the shape's field
			// descriptors, which the compiler keeps in the shape's names
			// as "name:desc" ... see field_desc.
			const char *fd = ak_json_field_desc(s, idx);
			ak_val v = value(ps, fd);
			if (ps->failed) goto done;
			ak_release_val(o->f[idx], (ak_kind)s->kinds[idx]);
			o->f[idx] = v;
			set[idx] = loose ? 1 : 2;
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
		if (!zero_field(ps, ak_json_field_desc(s, i), &o->f[i])) {
			fail(ps, "a missing field %s", s->names[i]);
			goto done;
		}
	}
done:
	if (set != few) ak_free(set);
	if (ps->failed) {
		ak_release(o);
		return (ak_val){0};
	}
	return (ak_val){.p = o};
}

// ---- json.Value: any JSON ----
//
// J<name>; is std/json's Value, an enum whose cases are null, bool, int,
// float, string, array and object: the document as it is, whatever its
// shape. Its cases are found by their names' first letters, which differ.

static int json_case(const ak_shape *s, char first) {
	for (uint16_t c = 0; c < s->ncases; c++)
		if (s->cases[c].name[0] == first) return c;
	return -1;
}

static const ak_shape *json_shape(parser *ps, const char *d) {
	if (ps->jdesc == d) return ps->jshape;
	const ak_shape *s = ps->t && ps->t->shape ? ps->t->shape(ps->t->ctx, d + 1, name_len(d)) : NULL;
	if (!s || json_case(s, 'n') < 0) {
		fail(ps, "no type %s", d + 1);
		return NULL;
	}
	ps->jdesc = d, ps->jshape = s;
	return s;
}

static ak_enum *json_null(parser *ps, const char *d) {
	const ak_shape *s = json_shape(ps, d);
	return s ? ak_enum_new(s, (uint16_t)json_case(s, 'n')) : NULL;
}

static ak_enum *json_case_new(const ak_shape *s, char first, ak_val v) {
	ak_enum *e = ak_enum_new(s, (uint16_t)json_case(s, first));
	e->f[0] = v;
	return e;
}

// json_int reads the digits from st to end as an int, when they fit.
static bool json_int(const char *st, const char *end, int64_t *out) {
	bool neg = st < end && *st == '-';
	if (neg) st++;
	if (st == end) return false;
	uint64_t v = 0, limit = neg ? (uint64_t)INT64_MAX + 1 : (uint64_t)INT64_MAX;
	for (; st < end; st++) {
		if (*st < '0' || *st > '9') return false;
		uint64_t dgt = (uint64_t)(*st - '0');
		if (v > (limit - dgt) / 10) return false;
		v = v * 10 + dgt;
	}
	*out = neg ? (int64_t)(0 - v) : (int64_t)v;
	return true;
}

static ak_val value(parser *ps, const char *d);

static ak_val json_any(parser *ps, const char *d) {
	const ak_shape *s = json_shape(ps, d);
	if (!s) return (ak_val){0};
	char c = *ps->s;
	switch (c) {
	case 'n':
		if (lit(ps, "null")) return (ak_val){.p = ak_enum_new(s, (uint16_t)json_case(s, 'n'))};
		break;
	case 't':
	case 'f':
		if (lit(ps, "true")) return (ak_val){.p = json_case_new(s, 'b', (ak_val){.i = 1})};
		if (lit(ps, "false")) return (ak_val){.p = json_case_new(s, 'b', (ak_val){.i = 0})};
		break;
	case '"': {
		ak_str *str_ = str(ps);
		if (!str_) return (ak_val){0};
		return (ak_val){.p = json_case_new(s, 's', (ak_val){.p = str_})};
	}
	case '[': {
		lit(ps, "[");
		ak_arr *a = ak_arr_new(AK_K_REF, s, 0);
		ws(ps);
		if (!lit(ps, "]")) {
			for (;;) {
				ak_val v = value(ps, d);
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
		}
		return (ak_val){.p = json_case_new(s, 'a', (ak_val){.p = a})};
	}
	case '{': {
		lit(ps, "{");
		ak_map *m = ak_map_new(AK_K_REF, true, AK_K_REF, s);
		ws(ps);
		if (!lit(ps, "}")) {
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
				ak_val v = value(ps, d);
				if (ps->failed) {
					ak_release(k);
					ak_release(m);
					return (ak_val){0};
				}
				// A member written twice: the last, as JavaScript's.
				ak_map_set(&m, (ak_val){.p = k}, v);
				ws(ps);
				if (lit(ps, ",")) continue;
				if (lit(ps, "}")) break;
				fail(ps, "expected ',' or '}'%s", "");
				ak_release(m);
				return (ak_val){0};
			}
		}
		return (ak_val){.p = json_case_new(s, 'o', (ak_val){.p = m})};
	}
	default: {
		// A number: an int when it's written as one and fits, else a
		// float (1.5, 1e3, 18446744073709551616).
		if (c != '-' && (c < '0' || c > '9')) break;
		const char *st = ps->s;
		double f;
		bool integral;
		if (!number(ps, &f, &integral, NULL)) return (ak_val){0};
		int64_t iv;
		if (integral && json_int(st, ps->s, &iv)) return (ak_val){.p = json_case_new(s, 'i', (ak_val){.i = iv})};
		return (ak_val){.p = json_case_new(s, 'f', (ak_val){.f = f})};
	}
	}
	fail(ps, "expected a value%s", "");
	return (ak_val){0};
}

static ak_val value_of(parser *ps, const char *d);

// value reads a value of descriptor d's type; an array or an object is a
// level deeper (an option's value, read by its own call, is at its own).
static ak_val value(parser *ps, const char *d) {
	ws(ps);
	if (ps->s >= ps->end) {
		fail(ps, "expected a value%s", "");
		return (ak_val){0};
	}
	if (*d == '?' || (*ps->s != '[' && *ps->s != '{')) return value_of(ps, d);
	if (!enter(ps)) return (ak_val){0};
	ak_val v = value_of(ps, d);
	ps->depth--;
	return v;
}

static ak_val value_of(parser *ps, const char *d) {
	switch (*d) {
	case 'J':
		return json_any(ps, d);
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
			// 2^63 is a double; int's range is [-2^63, 2^63).
			if (f != floor(f)) fail(ps, "expected an integer%s", "");
			else if (!(f >= -0x1p63 && f < 0x1p63)) fail(ps, "an integer out of range%s", "");
			else iv = (int64_t)f;
		}
		return (ak_val){.i = iv};
	}
	case 'n':
		return sized(ps, d[1] - '0');
	case 'f': {
		double f;
		bool integral;
		if (!number(ps, &f, &integral, NULL)) return (ak_val){0};
		return (ak_val){.f = f};
	}
	case 'b':
		if (lit(ps, "true")) return (ak_val){.i = 1};
		if (lit(ps, "false")) return (ak_val){.i = 0};
		fail(ps, "expected true or false%s", "");
		return (ak_val){0};
	case 's':
		return (ak_val){.p = str(ps)};
	case 'r': {
		// A char: a string of one code point.
		ak_str *s = str(ps);
		if (!s) return (ak_val){0};
		size_t i = 0;
		uint32_t c = s->len ? ak_utf8_decode(s->data, s->len, &i) : 0;
		bool one = s->len && i == s->len;
		ak_release(s);
		if (!one) fail(ps, "expected a char: a string of one code point%s", "");
		return (ak_val){.i = one ? (int64_t)c : 0};
	}
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
		ak_arr *a = ak_arr_new(ak_desc_arr_kind(ed, ek), es, 0);
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
		if (ak_shape_has(s, AK_HOOK_FROMJSON)) return own_json(ps, s);
		if (ak_link_bigint && ak_link_bigint->is(d + 1, name_len(d))) return json_bigint(ps, s);
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
		if (ak_shape_has(s, AK_HOOK_FROMJSON)) return own_json(ps, s);
		if (ps->s < ps->end && *ps->s == '"') {
			ak_str *cn = str(ps);
			if (!cn) return (ak_val){0};
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
		// Its payload: an array of its fields, an object of them (as a
		// struct's), or its one field's value. The bracket is a level
		// deeper, as value's are.
		bool nested = ps->s < ps->end && (*ps->s == '[' || *ps->s == '{');
		if (nested && !enter(ps)) {
			if (cs->nfields) ak_release(e);
			return (ak_val){0};
		}
		if (lit(ps, "[")) {
			for (uint16_t i = 0; i < cs->nfields; i++) {
				if (i) {
					ws(ps);
					if (!lit(ps, ",")) {
						fail(ps, "expected ','%s", "");
						break;
					}
				}
				e->f[i] = value(ps, ak_json_case_desc(s, ci, i));
				if (ps->failed) break;
			}
			ws(ps);
			if (!ps->failed && !lit(ps, "]")) fail(ps, "expected ']'%s", "");
		} else if (lit(ps, "{")) {
			uint8_t *set = cs->nfields ? ak_alloc(cs->nfields) : NULL;
			ws(ps);
			if (!lit(ps, "}")) {
				for (;;) {
					ws(ps);
					ak_str *k = str(ps);
					if (!k) break;
					ws(ps);
					if (!lit(ps, ":")) {
						ak_release(k);
						fail(ps, "expected ':'%s", "");
						break;
					}
					int fi = -1;
					bool loose = false;
					for (uint16_t i = 0; i < cs->nfields; i++)
						if (cs->names && cs->names[i] && strcmp(cs->names[i], k->data) == 0) fi = i;
					for (uint16_t i = 0; fi < 0 && i < cs->nfields; i++)
						if (cs->names && cs->names[i] && snake_eq(cs->names[i], k->data, k->len)) fi = i, loose = true;
					ak_release(k);
					ws(ps);
					if (fi >= 0 && ak_json_case_desc(s, ci, fi)[0] != '?' && null_next(ps)) fi = -1;  // null: as if it weren't there
					if (fi < 0 || (loose && set[fi] == 2)) {
						if (!skip_value(ps)) break;  // a field it doesn't have
					} else {
						ak_val v = value(ps, ak_json_case_desc(s, ci, fi));
						if (ps->failed) break;
						ak_release_val(e->f[fi], (ak_kind)cs->kinds[fi]);  // written twice: the last
						e->f[fi] = v;
						set[fi] = loose ? 1 : 2;
					}
					ws(ps);
					if (lit(ps, ",")) continue;
					if (lit(ps, "}")) break;
					fail(ps, "expected ',' or '}'%s", "");
					break;
				}
			}
			for (uint16_t i = 0; i < cs->nfields && !ps->failed; i++)
				if (!set[i] && !zero_field(ps, ak_json_case_desc(s, ci, i), &e->f[i])) fail(ps, "a missing field %s", cs->names ? cs->names[i] : "");
			ak_free(set);
		} else if (cs->nfields == 1) {
			e->f[0] = value(ps, ak_json_case_desc(s, ci, 0));
		} else {
			fail(ps, "expected the case's payload%s", "");
		}
		if (nested) ps->depth--;
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
		ak_release_val(v, desc_kind(d));
		if (ps.err) {
			ak_throw(ps.err);  // as the type's fromJSON threw it
			return (ak_val){0};
		}
		int line, col;
		pos_of(&(parser){.s = text, .end = text + len}, ps.s, &line, &col);
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

static void nl(ak_strb *b, int indent, uint32_t depth) {
	if (indent <= 0) return;
	ak_strb_cstr(b, "\n", 1);
	for (uint64_t i = 0; i < (uint64_t)indent * depth; i++) ak_strb_cstr(b, " ", 1);
}

// The encoder keeps the arrays and objects it's in on a stack of its own,
// not the C stack: a value nests as deep as the app made it (a long
// linked list of a recursive enum), and writing it takes no C stack per
// level.
typedef struct {
	const char *d;  // the container's descriptor: [T, <T, {T, S, E (a case with fields), (, mKV
	void *o;        // the container
	uint32_t i;     // its next item
	bool any;       // an item written
	bool val;       // mKV: item i's key is written, its value next
} enc_open;

typedef struct {
	ak_strb *b;
	const ak_json_types *t;
	int indent;
	AK_VEC(enc_open) open;
	AK_VEC(void *) held;  // what types' own toJSON gave, written as their frames go, let go of at the end
} encoder;

static void open_(encoder *e, const char *d, void *o, const char *text, size_t n) {
	ak_strb_cstr(e->b, text, n);
	ak_vec_push(&e->open, ((enc_open){d, o, 0, false, false}));
}

// enc_value writes v whole if it's a scalar, else opens it: its items are
// written as enc_step reaches them.
static void enc_value(encoder *e, ak_val v, const char *d) {
	ak_strb *b = e->b;
	while (*d == '?') {
		if (!v.p) {
			ak_strb_cstr(b, "null", 4);
			return;
		}
		if (desc_kind(d + 1) != AK_K_REF) v = ((ak_box *)v.p)->v;
		d++;
	}
	if ((*d == 'S' || *d == 'E') && v.p) {
		// A type with its own JSON: what its toJSON gives, written as that
		// is (kept until it's written).
		const ak_obj *o = v.p;
		const ak_shape *s = o->tag == AK_TAG_STRUCT ? ((const ak_struct *)o)->shape : o->tag == AK_TAG_ENUM ? ((const ak_enum *)o)->shape : NULL;
		if (s && ak_shape_has(s, AK_HOOK_TOJSON)) {
			ak_val j = ak_shape_call(s, AK_HOOK_TOJSON, &v, 1);
			if (desc_kind(s->jdesc) == AK_K_REF && j.p) ak_vec_push(&e->held, j.p);
			enc_value(e, j, s->jdesc);
			return;
		}
	}
	switch (*d) {
	case 'i':
		ak_strb_int(b, v.i);
		return;
	case 'n':
		if (d[1] - '0' == AK_U64) ak_strb_uint(b, (uint64_t)v.i);
		else ak_strb_int(b, v.i);
		return;
	case 'f':
		if (v.f != v.f || v.f == INFINITY || v.f == -INFINITY) {
			if (e->t && e->t->nonfinite) {
				const char *w = v.f != v.f ? "NaN" : v.f > 0 ? "Infinity" : "-Infinity";
				ak_strb_cstr(b, w, strlen(w));
			} else {
				ak_strb_cstr(b, "null", 4);
			}
		} else {
			size_t at = ak_strb_buf(b)->len;
			ak_strb_float(b, v.f);
			if (e->t && e->t->points) {
				// A whole one written as digits alone: its point, to read
				// back as a float.
				const ak_buf *w = ak_strb_buf(b);
				bool whole = true;
				for (size_t i = at; i < w->len; i++) {
					char c = (char)w->data[i];
					if (c == '.' || c == 'e' || c == 'E' || c == 'n' || c == 'N' || c == 'I') whole = false;
				}
				if (whole) ak_strb_cstr(b, ".0", 2);
			}
		}
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
	case 'r': {
		char u[4];
		enc_str(b, u, ak_utf8_encode((uint32_t)v.i, u));
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
	case '[':
	case '<':
		open_(e, d, v.p, "[", 1);
		return;
	case '{':
	case 'S':
	case 'm':
		if (*d == 'S' && ak_link_bigint && ak_link_bigint->is(d + 1, name_len(d))) {
			// std/bigint's BigInt: a number of all its digits.
			ak_link_bigint->write(b, ((ak_struct *)v.p)->f[0].p);
			return;
		}
		open_(e, d, v.p, "{", 1);
		return;
	case '(':
		open_(e, d, v.p, "(", 1);
		return;
	case 'e': {
		const ak_json_types *t = e->t;
		const char *const *cases;
		uint32_t n;
		if (t && t->cases && t->cases(t->ctx, d + 1, name_len(d), &cases, &n) && v.i >= 0 && (uint64_t)v.i < n) enc_str(b, cases[v.i], strlen(cases[v.i]));
		else ak_strb_int(b, v.i);
		return;
	}
	case 'J': {
		// A json.Value: as it is. An array's or an object's items are
		// the frame's, its descriptor this one.
		ak_enum *en = v.p;
		const ak_case *c = en ? &en->shape->cases[ak_enum_case(en)] : NULL;
		switch (c ? c->name[0] : 'n') {
		case 'b': ak_strb_bool(b, en->f[0].i != 0); return;
		case 'i': ak_strb_int(b, en->f[0].i); return;
		case 'f': enc_value(e, en->f[0], "f"); return;
		case 's': enc_value(e, en->f[0], "s"); return;
		case 'a': open_(e, d, en->f[0].p, "[", 1); return;
		case 'o': open_(e, d, en->f[0].p, "{", 1); return;
		}
		ak_strb_cstr(b, "null", 4);
		return;
	}
	case 'E': {
		ak_enum *en = v.p;
		const ak_case *c = &en->shape->cases[ak_enum_case(en)];
		if (c->nfields == 0) {
			enc_str(b, c->name, strlen(c->name));
			return;
		}
		ak_strb_cstr(b, "{", 1);
		enc_str(b, c->name, strlen(c->name));
		open_(e, d, en, e->indent > 0 ? ": [" : ":[", e->indent > 0 ? 3 : 2);
		return;
	}
	}
	ak_strb_cstr(b, "null", 4);
}

// enc_step writes the innermost open container's next item, or closes it.
static void enc_step(encoder *e) {
	ak_strb *b = e->b;
	uint32_t depth = e->open.len - 1;
	enc_open *o = &e->open.data[depth];
	ak_val item = {0};
	const char *id = NULL;
	const char *key = NULL;
	size_t keylen = 0;
	bool done = false;
	char kind = *o->d;
	if (kind == 'J') kind = o->o && ((ak_obj *)o->o)->tag == AK_TAG_ARR ? 'a' : 'o';
	switch (kind) {
	case 'a':
	case 'o':
		// A json.Value's array or object: its items are Values too.
		if (kind == 'a') {
			ak_arr *a = o->o;
			if (!a || o->i >= a->len) done = true;
			else item = ak_arr_get(a, o->i++), id = o->d;
		} else {
			ak_map *m = o->o;
			while (m && o->i < m->len && m->dead[o->i]) o->i++;
			if (!m || o->i >= m->len) {
				done = true;
			} else {
				ak_str *k = m->keys[o->i].p;
				key = k->data, keylen = k->len;
				item = m->vals[o->i++], id = o->d;
			}
		}
		break;
	case '[': {
		ak_arr *a = o->o;
		if (!a || o->i >= a->len) done = true;
		else item = ak_arr_get(a, o->i++), id = o->d + 1;
		break;
	}
	case '<':
	case '{': {
		ak_map *m = o->o;
		while (m && o->i < m->len && m->dead[o->i]) o->i++;
		if (!m || o->i >= m->len) {
			done = true;
		} else if (*o->d == '<') {
			item = m->keys[o->i++], id = o->d + 1;
		} else {
			ak_str *k = m->keys[o->i].p;
			key = k->data, keylen = k->len;
			item = m->vals[o->i++], id = o->d + 1;
		}
		break;
	}
	case 'S':
	case '(': {
		ak_struct *s = o->o;
		if (!s || o->i >= s->shape->nfields) {
			done = true;
		} else {
			if (*o->d == 'S') {
				key = s->shape->names ? s->shape->names[o->i] : "";
				keylen = strlen(key);
			}
			id = ak_json_field_desc(s->shape, (int)o->i);
			item = s->f[o->i++];
		}
		break;
	}
	case 'm': {
		// A key, then (once it's written, however deep) its value.
		ak_map *m = o->o;
		const char *kd = o->d + 1;
		if (o->val) {
			o->val = false;
			ak_strb_cstr(b, e->indent > 0 ? ": " : ":", e->indent > 0 ? 2 : 1);
			item = m->vals[o->i++];
			enc_value(e, item, desc_end(kd));
			return;
		}
		while (m && o->i < m->len && m->dead[o->i]) o->i++;
		if (!m || o->i >= m->len) {
			done = true;
			break;
		}
		if (o->any) ak_strb_cstr(b, ",", 1);
		o->any = true;
		o->val = true;
		nl(b, e->indent, depth + 1);
		enc_value(e, m->keys[o->i], kd);  // last: it may open the key
		return;
	}
	case 'E': {
		ak_enum *en = o->o;
		const ak_case *c = &en->shape->cases[ak_enum_case(en)];
		if (o->i >= c->nfields) {
			ak_strb_cstr(b, "]}", 2);
			e->open.len--;
			return;
		}
		if (o->any) ak_strb_cstr(b, ",", 1);
		o->any = true;
		id = ak_json_case_desc(en->shape, ak_enum_case(en), (int)o->i);
		item = en->f[o->i++];
		enc_value(e, item, id);
		return;
	}
	}
	if (done) {
		if (o->any) nl(b, e->indent, depth);
		ak_strb_cstr(b, kind == '[' || kind == '<' || kind == 'a' ? "]" : kind == '(' ? ")" : "}", 1);
		e->open.len--;
		return;
	}
	if (o->any) ak_strb_cstr(b, ",", 1);
	o->any = true;
	nl(b, e->indent, depth + 1);
	if (key) {
		enc_str(b, key, keylen);
		ak_strb_cstr(b, e->indent > 0 ? ": " : ":", e->indent > 0 ? 2 : 1);
	}
	enc_value(e, item, id);  // last: opening item grows the stack under o
}

ak_str *ak_json_encode(ak_val v, const char *d, const ak_json_types *t, int indent) {
	ak_strb b = {0};
	encoder e = {&b, t, indent, {0}, {0}};
	enc_value(&e, v, d);
	while (e.open.len) enc_step(&e);
	ak_vec_free(&e.open);
	for (uint32_t i = 0; i < e.held.len; i++) ak_release(e.held.data[i]);
	ak_vec_free(&e.held);
	return ak_strb_finish(&b);
}

// ---- A value as a json.Value, directly ----
//
// ak_json_value builds what ak_json_encode would write of v, read back
// as a json.Value (ak_json_decode by J<name>;, with points: a float
// stays one), without the text: each case as that round trip makes it.
// It walks v by its descriptor as the encoder does, its open containers
// on a stack of its own (a value however deep takes no C stack per
// level), and builds the Value's arrays and objects bottom up. What it
// can share it does: a string is the same string, a json.Value inside
// v the same Value (unless it holds a float the round trip changes:
// NaN and the infinities, which it makes null, or -0, 0). A struct's
// field names are made once a conversion, each name's string shared by
// every object of that struct's.

typedef struct {
	const char *d;   // the source container's descriptor ([T, <T, {T, S, E, J)
	void *o;         // the source container (a J's: its array or map)
	uint32_t i;      // its next item
	bool obj;        // what's built: an object (map), else an array
	ak_arr *arr;     // the array being built
	ak_map *map;     // the object being built
	ak_str *wrap;    // E: the case's name (the array built goes in {name: [...]})
	ak_str *key;     // the key it goes in, in the object open below it
} vframe;

// A struct's field names as strings, made once a shape a conversion.
typedef struct {
	const ak_shape *s;
	ak_str **names;
} vnames;

#define VNAMES 16

typedef struct {
	const ak_json_types *t;
	const ak_shape *js;  // json.Value's shape
	uint16_t cn, cb, ci, cf, cs, ca, co;  // its cases
	AK_VEC(vframe) open;
	AK_VEC(void *) held;  // what types' own toJSON gave, let go of at the end
	vnames names[VNAMES];
	ak_val result;
	bool failed;
} vconv;

static ak_val vcase(vconv *c, uint16_t k, ak_val v) {
	ak_enum *e = ak_enum_new(c->js, k);
	e->f[0] = v;
	return (ak_val){.p = e};
}

static ak_val vnull(vconv *c) { return (ak_val){.p = ak_enum_new(c->js, c->cn)}; }
static ak_val vstr(vconv *c, ak_str *s) { return vcase(c, c->cs, (ak_val){.p = s}); }

static ak_str *field_name(vconv *c, const ak_shape *s, uint32_t i) {
	vnames *n = &c->names[((uintptr_t)s >> 4) % VNAMES];
	if (n->s != s) {
		if (n->names) {
			for (uint32_t k = 0; k < n->s->nfields; k++) ak_release(n->names[k]);
			ak_free(n->names);
		}
		n->s = s;
		n->names = ak_alloc(sizeof(ak_str *) * (s->nfields ? s->nfields : 1));
		for (uint32_t k = 0; k < s->nfields; k++) {
			const char *name = s->names ? s->names[k] : "";
			n->names[k] = ak_str_new(name, strlen(name));
		}
	}
	return ak_retain(n->names[i]);
}

// jclean: whether the json.Value v holds no float the round trip
// changes (NaN and the infinities, written null; -0, written 0): then
// it's itself.
static bool jclean(vconv *c, ak_enum *v) {
	AK_VEC(ak_enum *) todo = {0};
	ak_vec_push(&todo, v);
	bool clean = true;
	while (clean && todo.len) {
		ak_enum *e = todo.data[--todo.len];
		uint16_t k = ak_enum_case(e);
		if (k == c->cf) {
			double f = e->f[0].f;
			clean = f == f && f != INFINITY && f != -INFINITY && !(f == 0 && signbit(f));
		} else if (k == c->ca) {
			ak_arr *a = e->f[0].p;
			for (uint32_t i = 0; a && i < a->len; i++) ak_vec_push(&todo, (ak_enum *)ak_arr_get(a, i).p);
		} else if (k == c->co) {
			ak_map *m = e->f[0].p;
			for (uint32_t i = 0; m && i < m->len; i++)
				if (!m->dead[i]) ak_vec_push(&todo, (ak_enum *)m->vals[i].p);
		}
	}
	ak_vec_free(&todo);
	return clean;
}

static void vopen(vconv *c, const char *d, void *o, bool obj, ak_str *wrap) {
	vframe f = {d, o, 0, obj, NULL, NULL, wrap, NULL};
	if (obj) f.map = ak_map_new(AK_K_REF, true, AK_K_REF, c->js);
	else f.arr = ak_arr_new(AK_K_REF, c->js, 0);
	ak_vec_push(&c->open, f);
}

// vvalue converts v (of descriptor d, borrowed): true with *out (owned)
// when it's done, false when it opened a container whose items come
// next.
static bool vvalue(vconv *c, ak_val v, const char *d, ak_val *out) {
	while (*d == '?') {
		if (!v.p) {
			*out = vnull(c);
			return true;
		}
		if (desc_kind(d + 1) != AK_K_REF) v = ((ak_box *)v.p)->v;
		d++;
	}
	if ((*d == 'S' || *d == 'E') && v.p) {
		// A type with its own JSON: what its toJSON gives, as that is.
		const ak_obj *o = v.p;
		const ak_shape *s = o->tag == AK_TAG_STRUCT ? ((const ak_struct *)o)->shape : o->tag == AK_TAG_ENUM ? ((const ak_enum *)o)->shape : NULL;
		if (s && ak_shape_has(s, AK_HOOK_TOJSON)) {
			ak_val j = ak_shape_call(s, AK_HOOK_TOJSON, &v, 1);
			if (desc_kind(s->jdesc) == AK_K_REF && j.p) ak_vec_push(&c->held, j.p);
			return vvalue(c, j, s->jdesc, out);
		}
	}
	switch (*d) {
	case 'i':
		*out = vcase(c, c->ci, v);
		return true;
	case 'n':
		// A u64 past int's range is written as its digits, which read
		// back as a float.
		if (d[1] - '0' == AK_U64 && v.i < 0) *out = vcase(c, c->cf, (ak_val){.f = (double)(uint64_t)v.i});
		else *out = vcase(c, c->ci, v);
		return true;
	case 'f':
		// NaN and the infinities are written null, -0 as 0; any other
		// float as the shortest digits that read back as it.
		if (v.f != v.f || v.f == INFINITY || v.f == -INFINITY) *out = vnull(c);
		else *out = vcase(c, c->cf, (ak_val){.f = v.f == 0 ? 0.0 : v.f});
		return true;
	case 'b':
		*out = vcase(c, c->cb, (ak_val){.i = v.i != 0});
		return true;
	case 's':
		*out = vstr(c, v.p ? ak_retain(v.p) : ak_str_empty());
		return true;
	case 'r': {
		char u[4];
		*out = vstr(c, ak_str_new(u, ak_utf8_encode((uint32_t)v.i, u)));
		return true;
	}
	case 'y': {
		static const char *tab = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		ak_bytes *by = v.p;
		uint32_t n = by ? by->len : 0;
		ak_str *s = ak_str_new(NULL, (size_t)(n + 2) / 3 * 4);
		char *w = s->data;
		for (uint32_t i = 0; i < n; i += 3) {
			uint32_t x = (uint32_t)by->data[i] << 16 | (i + 1 < n ? (uint32_t)by->data[i + 1] << 8 : 0) | (i + 2 < n ? by->data[i + 2] : 0);
			*w++ = tab[x >> 18 & 63];
			*w++ = tab[x >> 12 & 63];
			*w++ = i + 1 < n ? tab[x >> 6 & 63] : '=';
			*w++ = i + 2 < n ? tab[x & 63] : '=';
		}
		*out = vstr(c, s);
		return true;
	}
	case '[':
	case '<':
		vopen(c, d, v.p, false, NULL);
		return false;
	case '{':
		vopen(c, d, v.p, true, NULL);
		return false;
	case 'S':
		if (ak_link_bigint && ak_link_bigint->is(d + 1, name_len(d))) {
			// std/bigint's BigInt is written as all its digits: an int
			// when they fit, else the nearest float.
			ak_strb b = {0};
			ak_link_bigint->write(&b, ((ak_struct *)v.p)->f[0].p);
			ak_str *digits = ak_strb_finish(&b);
			int64_t iv;
			if (json_int(digits->data, digits->data + digits->len, &iv)) {
				*out = vcase(c, c->ci, (ak_val){.i = iv});
			} else {
				double f = strtod(digits->data, NULL);
				if (isinf(f)) {
					ak_throw_msg("json: a number out of range");
					c->failed = true;
					*out = vnull(c);
				} else {
					*out = vcase(c, c->cf, (ak_val){.f = f});
				}
			}
			ak_release(digits);
			return true;
		}
		vopen(c, d, v.p, true, NULL);
		return false;
	case 'e': {
		const ak_json_types *t = c->t;
		const char *const *cases;
		uint32_t n;
		if (t && t->cases && t->cases(t->ctx, d + 1, name_len(d), &cases, &n) && v.i >= 0 && (uint64_t)v.i < n) *out = vstr(c, ak_str_new(cases[v.i], strlen(cases[v.i])));
		else *out = vcase(c, c->ci, v);
		return true;
	}
	case 'J': {
		ak_enum *en = v.p;
		if (!en) {
			*out = vnull(c);
			return true;
		}
		if (jclean(c, en)) {
			*out = (ak_val){.p = ak_retain(en)};
			return true;
		}
		uint16_t k = ak_enum_case(en);
		if (k == c->ca || k == c->co) {
			vopen(c, d, en->f[0].p, k == c->co, NULL);
			return false;
		}
		return vvalue(c, en->f[0], "f", out);  // a float the round trip changes
	}
	case 'E': {
		ak_enum *en = v.p;
		const ak_case *cs = &en->shape->cases[ak_enum_case(en)];
		ak_str *name = ak_str_new(cs->name, strlen(cs->name));
		if (cs->nfields == 0) {
			*out = vstr(c, name);
			return true;
		}
		// {"case": [its fields]}
		vopen(c, d, en, false, name);
		return false;
	}
	}
	// What JSON writes as null (and what only print writes: a tuple, a
	// map from other than strings, which JSON can't read back).
	*out = vnull(c);
	return true;
}

// vput puts a finished value (owned) in the innermost open container,
// under key (owned) in an object; with none open, it's the result.
static void vput(vconv *c, ak_str *key, ak_val v) {
	if (!c->open.len) {
		c->result = v;
		return;
	}
	vframe *f = &c->open.data[c->open.len - 1];
	if (f->obj) ak_map_set(&f->map, (ak_val){.p = key}, v);
	else ak_arr_push(&f->arr, v);
}

// vstep converts the innermost open container's next item, or closes it.
static void vstep(vconv *c) {
	vframe *f = &c->open.data[c->open.len - 1];
	ak_val item = {0};
	const char *id = NULL;
	ak_str *key = NULL;
	bool done = false;
	char kind = *f->d;
	if (kind == 'J') kind = f->obj ? 'o' : 'a';
	switch (kind) {
	case 'a': {
		ak_arr *a = f->o;
		if (!a || f->i >= a->len) done = true;
		else item = ak_arr_get(a, f->i++), id = f->d;
		break;
	}
	case 'o': {
		ak_map *m = f->o;
		while (m && f->i < m->len && m->dead[f->i]) f->i++;
		if (!m || f->i >= m->len) done = true;
		else key = ak_retain(m->keys[f->i].p), item = m->vals[f->i++], id = f->d;
		break;
	}
	case '[': {
		ak_arr *a = f->o;
		if (!a || f->i >= a->len) done = true;
		else item = ak_arr_get(a, f->i++), id = f->d + 1;
		break;
	}
	case '<':
	case '{': {
		ak_map *m = f->o;
		while (m && f->i < m->len && m->dead[f->i]) f->i++;
		if (!m || f->i >= m->len) done = true;
		else if (kind == '<') item = m->keys[f->i++], id = f->d + 1;
		else key = ak_retain(m->keys[f->i].p), item = m->vals[f->i++], id = f->d + 1;
		break;
	}
	case 'S': {
		ak_struct *s = f->o;
		if (!s || f->i >= s->shape->nfields) {
			done = true;
		} else {
			key = field_name(c, s->shape, f->i);
			id = ak_json_field_desc(s->shape, (int)f->i);
			item = s->f[f->i++];
		}
		break;
	}
	case 'E': {
		ak_enum *en = f->o;
		const ak_case *cs = &en->shape->cases[ak_enum_case(en)];
		if (f->i >= cs->nfields) done = true;
		else id = ak_json_case_desc(en->shape, ak_enum_case(en), (int)f->i), item = en->f[f->i++];
		break;
	}
	default:
		done = true;
	}
	if (done) {
		vframe g = c->open.data[--c->open.len];
		ak_val v = g.obj ? vcase(c, c->co, (ak_val){.p = g.map}) : vcase(c, c->ca, (ak_val){.p = g.arr});
		if (g.wrap) {
			ak_map *m = ak_map_new(AK_K_REF, true, AK_K_REF, c->js);
			ak_map_set(&m, (ak_val){.p = g.wrap}, v);
			v = vcase(c, c->co, (ak_val){.p = m});
		}
		vput(c, g.key, v);
		return;
	}
	ak_val v;
	if (vvalue(c, item, id, &v)) vput(c, key, v);
	else c->open.data[c->open.len - 1].key = key;  // it opened: its key waits on it
}

ak_val ak_json_value(ak_val v, const char *d, const char *jd, const ak_json_types *t) {
	const ak_shape *js = t && t->shape ? t->shape(t->ctx, jd + 1, name_len(jd)) : NULL;
	if (!js || json_case(js, 'n') < 0) {
		ak_throw_msg("json: no type %s", jd + 1);
		return (ak_val){0};
	}
	vconv c = {.t = t, .js = js};
	c.cn = (uint16_t)json_case(js, 'n'), c.cb = (uint16_t)json_case(js, 'b'), c.ci = (uint16_t)json_case(js, 'i');
	c.cf = (uint16_t)json_case(js, 'f'), c.cs = (uint16_t)json_case(js, 's'), c.ca = (uint16_t)json_case(js, 'a');
	c.co = (uint16_t)json_case(js, 'o');
	ak_val out;
	if (vvalue(&c, v, d, &out)) c.result = out;
	while (c.open.len) vstep(&c);
	ak_vec_free(&c.open);
	for (uint32_t i = 0; i < c.held.len; i++) ak_release(c.held.data[i]);
	ak_vec_free(&c.held);
	for (int i = 0; i < VNAMES; i++) {
		vnames *n = &c.names[i];
		if (!n->names) continue;
		for (uint32_t k = 0; k < n->s->nfields; k++) ak_release(n->names[k]);
		ak_free(n->names);
	}
	if (c.failed) {
		ak_release(c.result.p);
		return (ak_val){0};
	}
	return c.result;
}
