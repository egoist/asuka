// toml.c: std/toml's builtins. TOML 1.0 (toml.io/en/v1.0.0) is read
// into a tree (every table's rules checked as Python's tomllib checks
// them), then into a value of the type the call asks for, by its
// descriptor (rt/json.h's): a struct from a table, by its fields' names,
// [T] from an array, [string: T] from a table, std/json's Value from
// anything. A value is written back by its descriptor too: a table's
// plain keys, then its tables as [sections] and its arrays of tables as
// [[sections]].
//
// A date-time is read as its text, in RFC 3339's form (T and Z upper
// case: what time.parse reads); TOML has no null, so a field that's none
// isn't written and an optional the document leaves out is none.
#include "std/std.h"
#include "rt/json.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>

// json.c's: a shape's field's descriptor.
extern const char *ak_json_field_desc(const ak_shape *s, int i);

// ---- The tree ----

enum {
	T_STR,
	T_INT,
	T_FLOAT,
	T_BOOL,
	T_ODT,  // an offset date-time
	T_LDT,  // a local date-time
	T_LD,   // a local date
	T_LT,   // a local time
	T_ARR,
	T_TAB,
};

enum {
	F_EXPLICIT = 1,  // a table a [header] made, or dotted keys of an earlier section: no more keys by dotted keys or a header
	F_FROZEN = 2,    // an inline table or an array literal: nothing added to it
	F_AOT = 4,       // an array of tables ([[header]])
};

typedef struct tnode tnode;

typedef struct {
	const char *k;
	uint32_t kn;
	tnode *v;
} tent;

struct tnode {
	uint8_t kind, flags;
	uint16_t depth;  // tables and arrays above it
	uint32_t at;     // where it starts in the text (a table: its header or its first key)
	union {
		int64_t i;
		double f;
		bool b;
		struct {
			const char *p;
			uint32_t n;
		} s;
		struct {
			tnode **items;
			uint32_t n, cap;
		} a;
		struct {
			tent *e;
			uint32_t n, cap;
			uint32_t *idx;  // an index of the keys, once there are many
			uint32_t idxcap;
		} t;
	};
};

// Nodes and their text live in an arena, freed at once.
typedef struct ablock {
	struct ablock *next;
	size_t used, cap;
	_Alignas(8) char data[];
} ablock;

typedef struct {
	ablock *head;
} arena;

static void *aalloc(arena *a, size_t n) {
	n = (n + 7) & ~(size_t)7;
	if (!a->head || a->head->used + n > a->head->cap) {
		size_t cap = n > 32768 ? n : 32768;
		ablock *b = ak_alloc(sizeof *b + cap);
		b->cap = cap;
		b->next = a->head;
		a->head = b;
	}
	void *p = a->head->data + a->head->used;
	a->head->used += n;
	return p;
}

static void afree(arena *a) {
	for (ablock *b = a->head, *nx; b; b = nx) {
		nx = b->next;
		ak_free(b);
	}
	a->head = NULL;
}

static const char *acopy(arena *a, const void *p, size_t n) {
	char *d = aalloc(a, n + 1);
	if (n) memcpy(d, p, n);
	return d;
}

#define MAX_DEPTH 256  // tables and arrays nested in each other, as dotted keys or headers or inline

static uint32_t key_hash(const char *k, uint32_t n) {
	uint32_t h = 2166136261u;
	for (uint32_t i = 0; i < n; i++) h = (h ^ (uint8_t)k[i]) * 16777619u;
	return h;
}

static tent *tab_find(tnode *t, const char *k, uint32_t n) {
	if (t->t.idx) {
		uint32_t m = t->t.idxcap - 1;
		for (uint32_t h = key_hash(k, n) & m;; h = (h + 1) & m) {
			uint32_t e = t->t.idx[h];
			if (!e) return NULL;
			tent *x = &t->t.e[e - 1];
			if (x->kn == n && memcmp(x->k, k, n) == 0) return x;
		}
	}
	for (uint32_t i = 0; i < t->t.n; i++)
		if (t->t.e[i].kn == n && memcmp(t->t.e[i].k, k, n) == 0) return &t->t.e[i];
	return NULL;
}

static void idx_put(tnode *t, uint32_t i) {
	uint32_t m = t->t.idxcap - 1;
	uint32_t h = key_hash(t->t.e[i].k, t->t.e[i].kn) & m;
	while (t->t.idx[h]) h = (h + 1) & m;
	t->t.idx[h] = i + 1;
}

static void tab_add(arena *a, tnode *t, const char *k, uint32_t n, tnode *v) {
	if (t->t.n == t->t.cap) {
		uint32_t cap = t->t.cap ? t->t.cap * 2 : 4;
		tent *e = aalloc(a, sizeof(tent) * cap);
		if (t->t.n) memcpy(e, t->t.e, sizeof(tent) * t->t.n);
		t->t.e = e;
		t->t.cap = cap;
	}
	t->t.e[t->t.n++] = (tent){k, n, v};
	if (t->t.n < 16) return;
	if (!t->t.idx || t->t.n * 2 > t->t.idxcap) {
		uint32_t cap = 64;
		while (cap < t->t.n * 4) cap *= 2;
		t->t.idx = aalloc(a, sizeof(uint32_t) * cap);
		t->t.idxcap = cap;
		for (uint32_t i = 0; i < t->t.n; i++) idx_put(t, i);
	} else {
		idx_put(t, t->t.n - 1);
	}
}

static void arr_add(arena *a, tnode *arr, tnode *v) {
	if (arr->a.n == arr->a.cap) {
		uint32_t cap = arr->a.cap ? arr->a.cap * 2 : 4;
		tnode **items = aalloc(a, sizeof(tnode *) * cap);
		if (arr->a.n) memcpy(items, arr->a.items, sizeof(tnode *) * arr->a.n);
		arr->a.items = items;
		arr->a.cap = cap;
	}
	arr->a.items[arr->a.n++] = v;
}

// ---- Positions ----

// line_col is where offset at is: its line and its column, in
// characters, both from 1.
static void line_col(const char *src, size_t n, size_t at, int *line, int *col) {
	*line = 1;
	*col = 1;
	for (size_t i = 0; i < at && i < n;) {
		if (src[i] == '\n') {
			(*line)++;
			*col = 1;
			i++;
		} else {
			ak_utf8_decode(src, n, &i);
			(*col)++;
		}
	}
}

// ---- Parsing ----

typedef struct {
	const char *s;
	uint32_t n;
	const char *at;
} tkey;

typedef struct {
	const char *src, *p, *end;
	arena ar;
	tnode *root, *cur;
	AK_VEC(tnode *) pending;  // tables dotted keys made in this section
	ak_buf sb;                // a string as it's decoded
	bool failed;
	const char *errp;
	char msg[200];
} tp;

AK_PRINTF(3, 4) static void tfail(tp *ps, const char *at, const char *fmt, ...) {
	if (ps->failed) return;
	ps->failed = true;
	ps->errp = at;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(ps->msg, sizeof ps->msg, fmt, ap);
	va_end(ap);
}

static tnode *mk(tp *ps, int kind, const char *at) {
	tnode *n = aalloc(&ps->ar, sizeof *n);
	n->kind = (uint8_t)kind;
	n->at = (uint32_t)(at - ps->src);
	return n;
}

static bool eof(tp *ps) { return ps->p >= ps->end; }
static int peek(tp *ps, size_t k) { return ps->p + k < ps->end ? (uint8_t)ps->p[k] : -1; }

static void ws(tp *ps) {
	while (!eof(ps) && (*ps->p == ' ' || *ps->p == '\t')) ps->p++;
}

static bool is_newline(tp *ps) { return peek(ps, 0) == '\n' || (peek(ps, 0) == '\r' && peek(ps, 1) == '\n'); }

static void newline(tp *ps) { ps->p += *ps->p == '\r' ? 2 : 1; }

static bool is_control(int c) { return (c >= 0 && c < 0x20 && c != '\t') || c == 0x7f; }

// comment reads a comment, from its # to the end of its line (not the
// newline).
static void comment(tp *ps) {
	ps->p++;
	while (!eof(ps) && !is_newline(ps)) {
		int c = (uint8_t)*ps->p;
		if (is_control(c)) {
			tfail(ps, ps->p, c == '\r' ? "a carriage return without a line feed" : "a control character in a comment");
			return;
		}
		ps->p++;
	}
}

// line_end reads what may end a line after a key/value pair or a
// header: blanks, a comment, the newline (or the end).
static void line_end(tp *ps, const char *what) {
	ws(ps);
	if (!eof(ps) && *ps->p == '#') comment(ps);
	if (ps->failed || eof(ps)) return;
	if (is_newline(ps)) {
		newline(ps);
		return;
	}
	tfail(ps, ps->p, "expected the end of the line after %s", what);
}

// blanks skips blanks, newlines and comments (in an array).
static void blanks(tp *ps) {
	for (;;) {
		ws(ps);
		if (eof(ps)) return;
		if (*ps->p == '#') {
			comment(ps);
			if (ps->failed) return;
		} else if (is_newline(ps)) {
			newline(ps);
		} else {
			return;
		}
	}
}

static bool bare_char(int c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'; }
static bool digit(int c) { return c >= '0' && c <= '9'; }

static void sb_utf8(tp *ps, uint32_t c) {
	char t[4];
	size_t n = ak_utf8_encode(c, t);
	ak_buf_put(&ps->sb, t, n);
}

static int hexval(int c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

// escape reads the escape at p (its backslash) into sb.
static void escape(tp *ps) {
	const char *st = ps->p;
	int c = peek(ps, 1);
	ps->p += 2;
	switch (c) {
	case 'b': ak_buf_u8(&ps->sb, '\b'); return;
	case 't': ak_buf_u8(&ps->sb, '\t'); return;
	case 'n': ak_buf_u8(&ps->sb, '\n'); return;
	case 'f': ak_buf_u8(&ps->sb, '\f'); return;
	case 'r': ak_buf_u8(&ps->sb, '\r'); return;
	case '"': ak_buf_u8(&ps->sb, '"'); return;
	case '\\': ak_buf_u8(&ps->sb, '\\'); return;
	case 'u':
	case 'U': {
		int k = c == 'u' ? 4 : 8;
		uint32_t v = 0;
		for (int i = 0; i < k; i++) {
			int h = hexval(peek(ps, 0));
			if (h < 0) {
				tfail(ps, st, "a \\%c escape takes %d hexadecimal digits", c, k);
				return;
			}
			v = v << 4 | (uint32_t)h;
			ps->p++;
		}
		if (v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff)) {
			tfail(ps, st, "\\%c%.*s isn't a Unicode scalar value", c, k, st + 2);
			return;
		}
		sb_utf8(ps, v);
		return;
	}
	}
	tfail(ps, st, "a bad escape: \\%c", c > 0x20 && c < 0x7f ? c : '?');
}

// string reads a string at p (any of the four kinds) into sb.
static void string(tp *ps, bool *multi) {
	const char *st = ps->p;
	char q = *ps->p;
	bool ml = peek(ps, 1) == q && peek(ps, 2) == q;
	if (multi) *multi = ml;
	ak_buf_reset(&ps->sb);
	ps->p += ml ? 3 : 1;
	if (ml && is_newline(ps)) newline(ps);  // a newline right after the quotes is trimmed
	for (;;) {
		if (eof(ps)) {
			tfail(ps, st, "a string that doesn't end");
			return;
		}
		int c = (uint8_t)*ps->p;
		if (c == q) {
			if (!ml) {
				ps->p++;
				return;
			}
			size_t k = 0;
			while (peek(ps, k) == q) k++;
			if (k >= 3) {
				if (k > 5) {
					tfail(ps, ps->p, "too many quotes at the end of a multi-line string");
					return;
				}
				for (size_t i = 3; i < k; i++) ak_buf_u8(&ps->sb, (uint8_t)q);
				ps->p += k;
				return;
			}
			for (size_t i = 0; i < k; i++) ak_buf_u8(&ps->sb, (uint8_t)q);
			ps->p += k;
			continue;
		}
		if (c == '\\' && q == '"') {
			if (ml) {
				// A line-ending backslash: the newline and the blanks
				// after it are trimmed.
				const char *r = ps->p + 1;
				while (r < ps->end && (*r == ' ' || *r == '\t')) r++;
				if (r < ps->end && (*r == '\n' || (*r == '\r' && r + 1 < ps->end && r[1] == '\n'))) {
					ps->p = r;
					while (!eof(ps) && (*ps->p == ' ' || *ps->p == '\t' || is_newline(ps))) {
						if (is_newline(ps)) newline(ps);
						else ps->p++;
					}
					continue;
				}
			}
			escape(ps);
			if (ps->failed) return;
			continue;
		}
		if (is_newline(ps)) {
			if (!ml) {
				tfail(ps, ps->p, "a string that doesn't end on its line");
				return;
			}
			ak_buf_u8(&ps->sb, '\n');
			newline(ps);
			continue;
		}
		if (is_control(c)) {
			tfail(ps, ps->p, c == '\r' ? "a carriage return without a line feed" : "a control character in a string");
			return;
		}
		ak_buf_u8(&ps->sb, (uint8_t)c);
		ps->p++;
	}
}

// simple_key reads a bare or quoted key.
static bool simple_key(tp *ps, tkey *k) {
	k->at = ps->p;
	int c = peek(ps, 0);
	if (c == '"' || c == '\'') {
		bool ml;
		string(ps, &ml);
		if (ps->failed) return false;
		if (ml) {
			tfail(ps, k->at, "a key can't be a multi-line string");
			return false;
		}
		k->s = acopy(&ps->ar, ps->sb.data, ps->sb.len);
		k->n = (uint32_t)ps->sb.len;
		return true;
	}
	const char *st = ps->p;
	while (!eof(ps) && bare_char((uint8_t)*ps->p)) ps->p++;
	if (ps->p == st) {
		if (eof(ps) || is_newline(ps)) tfail(ps, st, "expected a key");
		else if (c == '=') tfail(ps, st, "a key/value pair without its key");
		else tfail(ps, st, "expected a key, not '%c'", c > 0x20 && c < 0x7f ? c : '?');
		return false;
	}
	k->s = st;
	k->n = (uint32_t)(ps->p - st);
	return true;
}

typedef struct {
	tkey *k;
	uint32_t n, cap;
} tkeys;

// key reads a key, dotted or not.
static bool key(tp *ps, tkeys *ks) {
	ks->n = 0;
	for (;;) {
		if (ks->n == ks->cap) {
			uint32_t cap = ks->cap ? ks->cap * 2 : 4;
			tkey *k = aalloc(&ps->ar, sizeof(tkey) * cap);
			if (ks->n) memcpy(k, ks->k, sizeof(tkey) * ks->n);
			ks->k = k;
			ks->cap = cap;
		}
		if (!simple_key(ps, &ks->k[ks->n])) return false;
		ks->n++;
		ws(ps);
		if (peek(ps, 0) != '.') return true;
		ps->p++;
		ws(ps);
	}
}

static tnode *value(tp *ps, uint32_t depth);

static bool leap(int64_t y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

static int days_in(int64_t y, int m) {
	static const int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	return m == 2 && leap(y) ? 29 : d[m - 1];
}

// digits reads k digits at p as a number; -1 when they aren't.
static int digits(tp *ps, int k) {
	int v = 0;
	for (int i = 0; i < k; i++) {
		int c = peek(ps, (size_t)i);
		if (!digit(c)) return -1;
		v = v * 10 + (c - '0');
	}
	ps->p += k;
	return v;
}

// time_of reads HH:MM:SS[.frac] into sb; false (and the failure) when
// it isn't one.
static bool time_of(tp *ps) {
	const char *st = ps->p;
	int h = digits(ps, 2);
	if (h < 0 || peek(ps, 0) != ':') goto bad;
	ps->p++;
	int mi = digits(ps, 2);
	if (mi < 0 || peek(ps, 0) != ':') goto bad;
	ps->p++;
	int se = digits(ps, 2);
	if (se < 0) goto bad;
	if (h > 23 || mi > 59 || se > 60) {
		tfail(ps, st, "a time out of range: %.8s", st);
		return false;
	}
	ak_buf_put(&ps->sb, st, 8);
	if (peek(ps, 0) == '.') {
		const char *f = ps->p;
		ps->p++;
		if (!digit(peek(ps, 0))) {
			tfail(ps, f, "a fraction of a second without its digits");
			return false;
		}
		while (digit(peek(ps, 0))) ps->p++;
		ak_buf_put(&ps->sb, f, (size_t)(ps->p - f));
	}
	return true;
bad:
	tfail(ps, st, "a bad time: expected HH:MM:SS");
	return false;
}

// datetime reads a date, a date-time or a time.
static tnode *datetime(tp *ps) {
	const char *st = ps->p;
	ak_buf_reset(&ps->sb);
	int kind;
	if (peek(ps, 2) == ':') {
		if (!time_of(ps)) return NULL;
		kind = T_LT;
	} else {
		int y = digits(ps, 4);
		if (y < 0 || peek(ps, 0) != '-') goto bad;
		ps->p++;
		int m = digits(ps, 2);
		if (m < 0 || peek(ps, 0) != '-') goto bad;
		ps->p++;
		int d = digits(ps, 2);
		if (d < 0) goto bad;
		if (m < 1 || m > 12 || d < 1 || d > days_in(y, m)) {
			tfail(ps, st, "a date out of range: %.10s", st);
			return NULL;
		}
		ak_buf_put(&ps->sb, st, 10);
		int c = peek(ps, 0);
		bool time = c == 'T' || c == 't' || (c == ' ' && digit(peek(ps, 1)) && digit(peek(ps, 2)) && peek(ps, 3) == ':');
		if (!time) {
			kind = T_LD;
		} else {
			ps->p++;
			ak_buf_u8(&ps->sb, 'T');
			if (!time_of(ps)) return NULL;
			kind = T_LDT;
			c = peek(ps, 0);
			if (c == 'Z' || c == 'z') {
				ps->p++;
				ak_buf_u8(&ps->sb, 'Z');
				kind = T_ODT;
			} else if (c == '+' || c == '-') {
				const char *o = ps->p;
				ps->p++;
				int oh = digits(ps, 2);
				if (oh < 0 || peek(ps, 0) != ':') {
					tfail(ps, o, "a bad offset: expected +HH:MM, -HH:MM or Z");
					return NULL;
				}
				ps->p++;
				int om = digits(ps, 2);
				if (om < 0 || oh > 23 || om > 59) {
					tfail(ps, o, "a bad offset: expected +HH:MM, -HH:MM or Z");
					return NULL;
				}
				ak_buf_put(&ps->sb, o, 6);
				kind = T_ODT;
			}
		}
	}
	if (bare_char(peek(ps, 0)) || peek(ps, 0) == ':' || peek(ps, 0) == '.' || peek(ps, 0) == '+') {
		tfail(ps, ps->p, "a bad date or time");
		return NULL;
	}
	tnode *n = mk(ps, kind, st);
	n->s.p = acopy(&ps->ar, ps->sb.data, ps->sb.len);
	n->s.n = (uint32_t)ps->sb.len;
	return n;
bad:
	tfail(ps, st, "a bad date: expected YYYY-MM-DD");
	return NULL;
}

// num_digits reads digits (of base) with underscores between them into
// sb; false when there are none, or an underscore isn't between two.
static bool num_digits(tp *ps, int base, bool *leading_zero) {
	const char *st = ps->p;
	size_t n = 0;
	bool under = false;
	for (;;) {
		int c = peek(ps, 0);
		int v = c == '_' ? -2 : hexval(c);
		if (v == -2) {
			if (!n || under) {
				tfail(ps, ps->p, "an underscore in a number must be between digits");
				return false;
			}
			under = true;
			ps->p++;
			continue;
		}
		if (v < 0 || v >= base) break;
		if (base == 16 && !(digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) break;
		ak_buf_u8(&ps->sb, (uint8_t)c);
		n++;
		under = false;
		ps->p++;
	}
	if (under) {
		tfail(ps, ps->p - 1, "an underscore in a number must be between digits");
		return false;
	}
	if (!n) {
		tfail(ps, st, "a number without its digits");
		return false;
	}
	if (leading_zero) *leading_zero = n > 1 && *st == '0';
	return true;
}

// number reads an integer or a float.
static tnode *number(tp *ps) {
	const char *st = ps->p;
	ak_buf_reset(&ps->sb);
	int c = peek(ps, 0);
	bool sign = c == '+' || c == '-';
	if (sign) {
		ak_buf_u8(&ps->sb, (uint8_t)c);
		ps->p++;
	}
	if (sign && peek(ps, 0) == '0' && (peek(ps, 1) == 'x' || peek(ps, 1) == 'o' || peek(ps, 1) == 'b')) {
		tfail(ps, st, "a hexadecimal, octal or binary integer has no sign");
		return NULL;
	}
	if (peek(ps, 0) == 'i' || peek(ps, 0) == 'n') {
		bool inf = ps->end - ps->p >= 3 && memcmp(ps->p, "inf", 3) == 0;
		bool nan = ps->end - ps->p >= 3 && memcmp(ps->p, "nan", 3) == 0;
		if (!inf && !nan) {
			tfail(ps, st, "expected a value");
			return NULL;
		}
		ps->p += 3;
		tnode *n = mk(ps, T_FLOAT, st);
		n->f = nan ? (c == '-' ? -NAN : NAN) : (c == '-' ? -INFINITY : INFINITY);
		if (bare_char(peek(ps, 0))) goto bad;
		return n;
	}
	if (!sign && c == '0' && (peek(ps, 1) == 'x' || peek(ps, 1) == 'o' || peek(ps, 1) == 'b')) {
		int base = peek(ps, 1) == 'x' ? 16 : peek(ps, 1) == 'o' ? 8 : 2;
		ps->p += 2;
		if (!num_digits(ps, base, NULL)) return NULL;
		uint64_t v = 0;
		for (size_t i = 0; i < ps->sb.len; i++) {
			uint64_t dg = (uint64_t)hexval(ps->sb.data[i]);
			if (v > ((uint64_t)INT64_MAX - dg) / (uint64_t)base) {
				tfail(ps, st, "an integer out of range");
				return NULL;
			}
			v = v * (uint64_t)base + dg;
		}
		if (bare_char(peek(ps, 0)) || peek(ps, 0) == '.') goto bad;
		tnode *n = mk(ps, T_INT, st);
		n->i = (int64_t)v;
		return n;
	}
	bool lz;
	if (!num_digits(ps, 10, &lz)) return NULL;
	if (lz) {
		tfail(ps, st, "a number with a leading zero");
		return NULL;
	}
	bool fl = false;
	if (peek(ps, 0) == '.') {
		fl = true;
		ak_buf_u8(&ps->sb, '.');
		ps->p++;
		if (!digit(peek(ps, 0))) {
			tfail(ps, ps->p, "a decimal point must be followed by digits");
			return NULL;
		}
		if (!num_digits(ps, 10, NULL)) return NULL;
	}
	if (peek(ps, 0) == 'e' || peek(ps, 0) == 'E') {
		fl = true;
		ak_buf_u8(&ps->sb, 'e');
		ps->p++;
		if (peek(ps, 0) == '+' || peek(ps, 0) == '-') {
			ak_buf_u8(&ps->sb, (uint8_t)*ps->p);
			ps->p++;
		}
		if (!digit(peek(ps, 0))) {
			tfail(ps, ps->p, "an exponent without its digits");
			return NULL;
		}
		if (!num_digits(ps, 10, NULL)) return NULL;
	}
	if (bare_char(peek(ps, 0)) || peek(ps, 0) == '.') goto bad;
	ak_buf_u8(&ps->sb, 0);
	const char *txt = (const char *)ps->sb.data;
	if (fl) {
		errno = 0;
		char *e;
		double f = strtod(txt, &e);
		if (isinf(f)) {
			tfail(ps, st, "a float out of range");
			return NULL;
		}
		tnode *n = mk(ps, T_FLOAT, st);
		n->f = f;
		return n;
	}
	bool neg = txt[0] == '-';
	const char *d = txt + (sign ? 1 : 0);
	uint64_t v = 0, limit = neg ? (uint64_t)INT64_MAX + 1 : (uint64_t)INT64_MAX;
	for (; *d; d++) {
		uint64_t dg = (uint64_t)(*d - '0');
		if (v > (limit - dg) / 10) {
			tfail(ps, st, "an integer out of range");
			return NULL;
		}
		v = v * 10 + dg;
	}
	tnode *n = mk(ps, T_INT, st);
	n->i = neg ? (int64_t)(0 - v) : (int64_t)v;
	return n;
bad:
	tfail(ps, st, "a bad number");
	return NULL;
}

static bool keyval(tp *ps, tnode *base, bool in_inline);

// deep fails when a value at depth would nest too deeply.
static bool deep(tp *ps, uint32_t depth) {
	if (depth <= MAX_DEPTH) return false;
	tfail(ps, ps->p, "nested too deeply");
	return true;
}

// array reads an array whose depth (tables and arrays above it) is
// depth.
static tnode *array(tp *ps, uint32_t depth) {
	const char *st = ps->p;
	if (deep(ps, depth)) return NULL;
	ps->p++;
	tnode *a = mk(ps, T_ARR, st);
	a->flags = F_FROZEN;
	a->depth = (uint16_t)depth;
	for (;;) {
		blanks(ps);
		if (ps->failed) return NULL;
		if (peek(ps, 0) == ']') {
			ps->p++;
			break;
		}
		tnode *v = value(ps, depth + 1);
		if (!v) return NULL;
		arr_add(&ps->ar, a, v);
		blanks(ps);
		if (ps->failed) return NULL;
		if (peek(ps, 0) == ',') {
			ps->p++;
			continue;
		}
		if (peek(ps, 0) == ']') {
			ps->p++;
			break;
		}
		tfail(ps, eof(ps) ? st : ps->p, eof(ps) ? "an array that doesn't end" : "expected ',' or ']' in an array");
		return NULL;
	}
	return a;
}

static tnode *inline_table(tp *ps, uint32_t depth) {
	const char *st = ps->p;
	if (deep(ps, depth)) return NULL;
	ps->p++;
	tnode *t = mk(ps, T_TAB, st);
	t->depth = (uint16_t)depth;
	ws(ps);
	if (peek(ps, 0) == '}') {
		ps->p++;
	} else {
		for (;;) {
			ws(ps);
			if (eof(ps) || is_newline(ps)) {
				tfail(ps, eof(ps) ? st : ps->p, "an inline table must be on one line");
				return NULL;
			}
			if (!keyval(ps, t, true)) return NULL;
			ws(ps);
			if (peek(ps, 0) == ',') {
				ps->p++;
				ws(ps);
				if (peek(ps, 0) == '}') {
					tfail(ps, ps->p, "an inline table can't end with a comma");
					return NULL;
				}
				continue;
			}
			if (peek(ps, 0) == '}') {
				ps->p++;
				break;
			}
			if (eof(ps) || is_newline(ps)) tfail(ps, eof(ps) ? st : ps->p, "an inline table must be on one line");
			else tfail(ps, ps->p, "expected ',' or '}' in an inline table");
			return NULL;
		}
	}
	t->flags = F_FROZEN;
	return t;
}

// value reads a value whose depth is depth.
static tnode *value(tp *ps, uint32_t depth) {
	if (eof(ps) || is_newline(ps) || peek(ps, 0) == '#') {
		tfail(ps, ps->p, "expected a value");
		return NULL;
	}
	const char *st = ps->p;
	int c = (uint8_t)*ps->p;
	switch (c) {
	case '"':
	case '\'': {
		string(ps, NULL);
		if (ps->failed) return NULL;
		tnode *n = mk(ps, T_STR, st);
		n->s.p = acopy(&ps->ar, ps->sb.data, ps->sb.len);
		n->s.n = (uint32_t)ps->sb.len;
		return n;
	}
	case '[':
		return array(ps, depth);
	case '{':
		return inline_table(ps, depth);
	case 't':
	case 'f': {
		bool t = c == 't';
		const char *w = t ? "true" : "false";
		size_t k = strlen(w);
		if ((size_t)(ps->end - ps->p) >= k && memcmp(ps->p, w, k) == 0 && !bare_char(peek(ps, k))) {
			ps->p += k;
			tnode *n = mk(ps, T_BOOL, st);
			n->b = t;
			return n;
		}
		break;
	}
	default:
		if (digit(c) && digit(peek(ps, 1)) && (peek(ps, 2) == ':' || (digit(peek(ps, 2)) && digit(peek(ps, 3)) && peek(ps, 4) == '-'))) return datetime(ps);
		if (digit(c) || c == '+' || c == '-' || c == 'i' || c == 'n') return number(ps);
	}
	tfail(ps, st, "expected a value");
	return NULL;
}

static tnode *new_table(tp *ps, tnode *parent, const char *at) {
	if (parent->depth >= MAX_DEPTH) {
		tfail(ps, at, "nested too deeply");
		return NULL;
	}
	tnode *t = mk(ps, T_TAB, at);
	t->depth = (uint16_t)(parent->depth + 1);
	return t;
}

static bool keyval(tp *ps, tnode *base, bool in_inline) {
	tkeys ks = {0};
	if (!key(ps, &ks)) return false;
	ws(ps);
	if (peek(ps, 0) != '=') {
		tfail(ps, ps->p, eof(ps) || is_newline(ps) ? "a key without a value: expected '='" : "expected '=' after a key");
		return false;
	}
	ps->p++;
	ws(ps);
	tnode *v = value(ps, base->depth + ks.n);
	if (!v) return false;
	tnode *t = base;
	for (uint32_t i = 0; i + 1 < ks.n; i++) {
		tkey *k = &ks.k[i];
		tent *e = tab_find(t, k->s, k->n);
		if (!e) {
			tnode *nt = new_table(ps, t, k->at);
			if (!nt) return false;
			tab_add(&ps->ar, t, k->s, k->n, nt);
			if (!in_inline) ak_vec_push(&ps->pending, nt);
			t = nt;
			continue;
		}
		if (e->v->kind != T_TAB) {
			tfail(ps, k->at, "'%.*s' is %s, not a table", (int)k->n, k->s, e->v->kind == T_ARR ? "an array" : "a value already");
			return false;
		}
		if (e->v->flags & F_FROZEN) {
			tfail(ps, k->at, "'%.*s' is an inline table: it can't be added to", (int)k->n, k->s);
			return false;
		}
		if (e->v->flags & F_EXPLICIT) {
			tfail(ps, k->at, "'%.*s' is a table defined above: dotted keys can't add to it", (int)k->n, k->s);
			return false;
		}
		t = e->v;
	}
	tkey *last = &ks.k[ks.n - 1];
	if (tab_find(t, last->s, last->n)) {
		tfail(ps, last->at, "a key defined twice: '%.*s'", (int)last->n, last->s);
		return false;
	}
	tab_add(&ps->ar, t, last->s, last->n, v);
	return true;
}

// header reads [a.b] or [[a.b]] and makes it the table key/value pairs
// go into.
static void header(tp *ps) {
	const char *st = ps->p;
	bool aot = peek(ps, 1) == '[';
	ps->p += aot ? 2 : 1;
	// The tables dotted keys made in the section before: closed.
	for (uint32_t i = 0; i < ps->pending.len; i++) ps->pending.data[i]->flags |= F_EXPLICIT;
	ps->pending.len = 0;
	ws(ps);
	tkeys ks = {0};
	if (!key(ps, &ks)) return;
	ws(ps);
	if (peek(ps, 0) != ']' || (aot && peek(ps, 1) != ']')) {
		tfail(ps, ps->p, aot ? "expected ']]' at the end of a header" : "expected ']' at the end of a header");
		return;
	}
	ps->p += aot ? 2 : 1;
	tnode *t = ps->root;
	for (uint32_t i = 0; i + 1 < ks.n; i++) {
		tkey *k = &ks.k[i];
		tent *e = tab_find(t, k->s, k->n);
		if (!e) {
			tnode *nt = new_table(ps, t, k->at);
			if (!nt) return;
			tab_add(&ps->ar, t, k->s, k->n, nt);
			t = nt;
			continue;
		}
		tnode *v = e->v;
		if (v->kind == T_ARR && (v->flags & F_AOT)) {
			t = v->a.items[v->a.n - 1];
			continue;
		}
		if (v->kind != T_TAB) {
			tfail(ps, k->at, "'%.*s' is %s, not a table", (int)k->n, k->s, v->kind == T_ARR ? "an array" : "a value already");
			return;
		}
		if (v->flags & F_FROZEN) {
			tfail(ps, k->at, "'%.*s' is an inline table: it can't be added to", (int)k->n, k->s);
			return;
		}
		t = v;
	}
	tkey *k = &ks.k[ks.n - 1];
	tent *e = tab_find(t, k->s, k->n);
	if (aot) {
		tnode *arr;
		if (!e) {
			if (t->depth >= MAX_DEPTH) {
				tfail(ps, k->at, "nested too deeply");
				return;
			}
			arr = mk(ps, T_ARR, k->at);
			arr->flags = F_AOT;
			arr->depth = (uint16_t)(t->depth + 1);
			tab_add(&ps->ar, t, k->s, k->n, arr);
		} else if (e->v->kind == T_ARR && (e->v->flags & F_AOT)) {
			arr = e->v;
		} else {
			tfail(ps, k->at, "'%.*s' is %s, not an array of tables", (int)k->n, k->s, e->v->kind == T_TAB ? "a table" : e->v->kind == T_ARR ? "an array" : "a value");
			return;
		}
		tnode *nt = new_table(ps, arr, st);
		if (!nt) return;
		nt->flags = F_EXPLICIT;
		arr_add(&ps->ar, arr, nt);
		ps->cur = nt;
	} else if (!e) {
		tnode *nt = new_table(ps, t, st);
		if (!nt) return;
		nt->flags = F_EXPLICIT;
		tab_add(&ps->ar, t, k->s, k->n, nt);
		ps->cur = nt;
	} else if (e->v->kind != T_TAB) {
		tfail(ps, k->at, "'%.*s' is %s, not a table", (int)k->n, k->s, e->v->kind == T_ARR ? "an array" : "a value already");
		return;
	} else if (e->v->flags & (F_EXPLICIT | F_FROZEN)) {
		tfail(ps, st, "a table defined twice: [%.*s]", (int)(ps->p - st - 2), st + 1);
		return;
	} else {
		e->v->flags |= F_EXPLICIT;
		e->v->at = (uint32_t)(st - ps->src);
		ps->cur = e->v;
	}
	line_end(ps, "a header");
}

// tparse reads text into ps->root; false (ps->msg, ps->errp) when it
// isn't TOML.
static bool tparse(tp *ps, const char *text, size_t n) {
	ps->src = ps->p = text;
	ps->end = text + n;
	ps->root = ps->cur = mk(ps, T_TAB, text);
	// Not UTF-8 is an error wherever it is.
	for (size_t i = 0; i < n;) {
		size_t at = i;
		uint32_t c = ak_utf8_decode(text, n, &i);
		if (c == 0xfffd && !(i - at == 3 && (uint8_t)text[at] == 0xef)) {
			tfail(ps, text + at, "not UTF-8");
			return false;
		}
	}
	if (n >= 3 && memcmp(text, "\xef\xbb\xbf", 3) == 0) ps->p += 3;
	while (!ps->failed) {
		ws(ps);
		if (eof(ps)) break;
		int c = (uint8_t)*ps->p;
		if (is_newline(ps)) {
			newline(ps);
		} else if (c == '#') {
			comment(ps);
		} else if (c == '[') {
			header(ps);
		} else if (is_control(c)) {
			tfail(ps, ps->p, c == '\r' ? "a carriage return without a line feed" : "a control character");
		} else {
			if (keyval(ps, ps->cur, false)) line_end(ps, "a value");
		}
	}
	return !ps->failed;
}

static void tp_free(tp *ps) {
	afree(&ps->ar);
	ak_vec_free(&ps->pending);
	ak_buf_free(&ps->sb);
}

// tthrow throws ps's failure: "toml: line:column: why".
static void tthrow(tp *ps) {
	int line, col;
	line_col(ps->src, (size_t)(ps->end - ps->src), (size_t)(ps->errp - ps->src), &line, &col);
	ak_throw_msg("toml: %d:%d: %s", line, col, ps->msg);
}

// ---- toml-test's JSON (core/test/toml_test.c) ----

static void tag_str(ak_buf *b, const char *s, size_t n) {
	ak_buf_u8(b, '"');
	for (size_t i = 0; i < n; i++) {
		uint8_t c = (uint8_t)s[i];
		if (c == '"' || c == '\\') {
			ak_buf_u8(b, '\\');
			ak_buf_u8(b, c);
		} else if (c < 0x20 || c == 0x7f) {
			ak_buf_printf(b, "\\u%04x", c);
		} else {
			ak_buf_u8(b, c);
		}
	}
	ak_buf_u8(b, '"');
}

static int ent_cmp(const void *a, const void *b) {
	const tent *x = *(tent *const *)a, *y = *(tent *const *)b;
	uint32_t n = x->kn < y->kn ? x->kn : y->kn;
	int c = memcmp(x->k, y->k, n);
	if (c) return c;
	return x->kn < y->kn ? -1 : x->kn > y->kn;
}

static void tagged(ak_buf *b, const tnode *n) {
	static const char *const types[] = {"string", "integer", "float", "bool", "datetime", "datetime-local", "date-local", "time-local"};
	char tmp[64];
	switch (n->kind) {
	case T_ARR:
		ak_buf_u8(b, '[');
		for (uint32_t i = 0; i < n->a.n; i++) {
			if (i) ak_buf_u8(b, ',');
			tagged(b, n->a.items[i]);
		}
		ak_buf_u8(b, ']');
		return;
	case T_TAB: {
		tent **es = ak_alloc(sizeof(tent *) * (n->t.n + 1));
		for (uint32_t i = 0; i < n->t.n; i++) es[i] = &n->t.e[i];
		qsort(es, n->t.n, sizeof *es, ent_cmp);
		ak_buf_u8(b, '{');
		for (uint32_t i = 0; i < n->t.n; i++) {
			if (i) ak_buf_u8(b, ',');
			tag_str(b, es[i]->k, es[i]->kn);
			ak_buf_u8(b, ':');
			tagged(b, es[i]->v);
		}
		ak_buf_u8(b, '}');
		ak_free(es);
		return;
	}
	}
	ak_buf_printf(b, "{\"type\":\"%s\",\"value\":", types[n->kind]);
	switch (n->kind) {
	case T_STR:
		tag_str(b, n->s.p, n->s.n);
		break;
	case T_INT:
		snprintf(tmp, sizeof tmp, "\"%lld\"", (long long)n->i);
		ak_buf_put(b, tmp, strlen(tmp));
		break;
	case T_FLOAT: {
		uint64_t bits;
		memcpy(&bits, &n->f, 8);
		if (n->f != n->f) snprintf(tmp, sizeof tmp, "\"nan\"");
		else snprintf(tmp, sizeof tmp, "\"%llu\"", (unsigned long long)bits);
		ak_buf_put(b, tmp, strlen(tmp));
		break;
	}
	case T_BOOL:
		ak_buf_put(b, n->b ? "\"true\"" : "\"false\"", n->b ? 6 : 7);
		break;
	default: {
		// A date-time, its fraction's trailing zeros dropped.
		AK_VEC(char) s = {0};
		for (uint32_t i = 0; i < n->s.n; i++) {
			if (n->s.p[i] == '.') {
				uint32_t j = i + 1;
				while (j < n->s.n && digit(n->s.p[j])) j++;
				uint32_t e = j;
				while (e > i + 1 && n->s.p[e - 1] == '0') e--;
				if (e > i + 1)
					for (uint32_t k = i; k < e; k++) ak_vec_push(&s, n->s.p[k]);
				i = j - 1;
				continue;
			}
			ak_vec_push(&s, n->s.p[i]);
		}
		tag_str(b, s.data ? s.data : "", s.len);
		ak_vec_free(&s);
	}
	}
	ak_buf_u8(b, '}');
}

bool ak_toml_tagged(const char *text, size_t n, ak_buf *out, char *err, size_t errn) {
	tp ps = {0};
	bool ok = tparse(&ps, text, n);
	if (ok) {
		tagged(out, ps.root);
	} else if (err) {
		int line, col;
		line_col(text, n, (size_t)(ps.errp - text), &line, &col);
		snprintf(err, errn, "%d:%d: %s", line, col, ps.msg);
	}
	tp_free(&ps);
	return ok;
}

// ---- Reading into a type ----

static ak_kind kind_of(const char *d) {
	switch (*d) {
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

static size_t name_len(const char *d) {
	const char *q = strchr(d + 1, ';');
	return q ? (size_t)(q - d - 1) : strlen(d + 1);
}

typedef struct {
	tp *ps;
	const ak_json_types *t;
	bool failed;
	uint32_t at;
	char msg[300];
	char path[256];
	size_t plen;
} dx;

static const char *what(const tnode *n) {
	switch (n->kind) {
	case T_STR: return "a string";
	case T_INT: return "an integer";
	case T_FLOAT: return "a float";
	case T_BOOL: return "a boolean";
	case T_ODT: return "a date-time";
	case T_LDT: return "a local date-time";
	case T_LD: return "a local date";
	case T_LT: return "a local time";
	case T_ARR: return "an array";
	default: return "a table";
	}
}

AK_PRINTF(3, 4) static void dfail(dx *x, const tnode *n, const char *fmt, ...) {
	if (x->failed) return;
	x->failed = true;
	x->at = n->at;
	char m[240];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(m, sizeof m, fmt, ap);
	va_end(ap);
	if (x->plen) snprintf(x->msg, sizeof x->msg, "%.*s: %s", (int)x->plen, x->path, m);
	else snprintf(x->msg, sizeof x->msg, "%s", m);
}

// path_push adds a key (or an index) to the path errors name; it returns
// the length to go back to.
static size_t path_key(dx *x, const char *k, size_t n) {
	size_t old = x->plen;
	bool bare = n > 0;
	for (size_t i = 0; i < n; i++)
		if (!bare_char((uint8_t)k[i])) bare = false;
	int w = snprintf(x->path + x->plen, sizeof x->path - x->plen, bare ? "%s%.*s" : "%s\"%.*s\"", old ? "." : "", (int)n, k);
	if (w > 0) x->plen = x->plen + (size_t)w < sizeof x->path ? x->plen + (size_t)w : sizeof x->path - 1;
	return old;
}

static size_t path_index(dx *x, uint32_t i) {
	size_t old = x->plen;
	int w = snprintf(x->path + x->plen, sizeof x->path - x->plen, "[%u]", i);
	if (w > 0) x->plen = x->plen + (size_t)w < sizeof x->path ? x->plen + (size_t)w : sizeof x->path - 1;
	return old;
}

static void json_of(ak_buf *b, const tnode *n);

static const ak_shape *shape_named(dx *x, const char *d) {
	return x->t && x->t->shape ? x->t->shape(x->t->ctx, d + 1, name_len(d)) : NULL;
}

static ak_val conv(dx *x, const tnode *n, const char *d);

// loose is a field's name and a key compared as config files write
// names: case and dashes and underscores aside (rust-version reads into
// rustVersion).
static bool loose_eq(const char *a, const char *k, uint32_t kn) {
	size_t i = 0, j = 0, an = strlen(a);
	for (;;) {
		while (i < an && (a[i] == '-' || a[i] == '_')) i++;
		while (j < kn && (k[j] == '-' || k[j] == '_')) j++;
		if (i == an || j == kn) return i == an && j == kn;
		char p = a[i], q = k[j];
		if (p >= 'A' && p <= 'Z') p = (char)(p - 'A' + 'a');
		if (q >= 'A' && q <= 'Z') q = (char)(q - 'A' + 'a');
		if (p != q) return false;
		i++, j++;
	}
}

static ak_val any_value(dx *x, const tnode *n, const ak_shape *s);

static int case_of(const ak_shape *s, char first) {
	for (uint16_t c = 0; c < s->ncases; c++)
		if (s->cases[c].name[0] == first) return c;
	return -1;
}

static ak_val any_case(const ak_shape *s, char first, ak_val v) {
	ak_enum *e = ak_enum_new(s, (uint16_t)case_of(s, first));
	e->f[0] = v;
	return (ak_val){.p = e};
}

// any_value is n as std/json's Value (shape s).
static ak_val any_value(dx *x, const tnode *n, const ak_shape *s) {
	switch (n->kind) {
	case T_INT: return any_case(s, 'i', (ak_val){.i = n->i});
	case T_FLOAT: return any_case(s, 'f', (ak_val){.f = n->f});
	case T_BOOL: return any_case(s, 'b', (ak_val){.i = n->b});
	case T_ARR: {
		ak_arr *a = ak_arr_new(AK_K_REF, s, n->a.n);
		for (uint32_t i = 0; i < n->a.n; i++) ak_arr_push(&a, any_value(x, n->a.items[i], s));
		return any_case(s, 'a', (ak_val){.p = a});
	}
	case T_TAB: {
		ak_map *m = ak_map_new(AK_K_REF, true, AK_K_REF, s);
		for (uint32_t i = 0; i < n->t.n; i++) ak_map_set(&m, (ak_val){.p = ak_str_new(n->t.e[i].k, n->t.e[i].kn)}, any_value(x, n->t.e[i].v, s));
		return any_case(s, 'o', (ak_val){.p = m});
	}
	default:
		return any_case(s, 's', (ak_val){.p = ak_str_new(n->s.p, n->s.n)});
	}
}

// zero is a field the table leaves out (std/json's rule): none, a
// scalar's zero, an empty string or collection, json.Value's null;
// false for one it can't leave out.
static bool zero(dx *x, const char *d, ak_val *out) {
	*out = (ak_val){0};
	if (*d == '?' || kind_of(d) != AK_K_REF) return true;
	switch (*d) {
	case 's': out->p = ak_str_empty(); return true;
	case '[': {
		ak_arr *a = ak_arr_new(kind_of(d + 1), d[1] == 'S' || d[1] == 'E' ? shape_named(x, d + 1) : NULL, 0);
		if (d[1] == 's') a->h.flags = 1;
		out->p = a;
		return true;
	}
	case '{': out->p = ak_map_new(AK_K_REF, true, kind_of(d + 1), d[1] == 'S' || d[1] == 'E' ? shape_named(x, d + 1) : NULL); return true;
	case '<': out->p = ak_set_new(kind_of(d + 1), d[1] == 's'); return true;
	case 'J': {
		const ak_shape *s = shape_named(x, d);
		if (!s || case_of(s, 'n') < 0) return false;
		out->p = ak_enum_new(s, (uint16_t)case_of(s, 'n'));
		return true;
	}
	}
	return false;
}

// via_json reads n as JSON does (what TOML has no form of its own for:
// an enum's case with a payload, bytes as Base64).
static ak_val via_json(dx *x, const tnode *n, const char *d) {
	ak_buf b = {0};
	json_of(&b, n);
	ak_val v = ak_json_decode((const char *)b.data, b.len, d, x->t);
	ak_buf_free(&b);
	if (ak_error_pending()) {
		void *err = ak_error_take();
		ak_str *m = ak_error_message(err);
		const char *why = m ? m->data : "not this type";
		// Its position is in the JSON: what it says is enough.
		const char *c = strstr(why, ": ");
		if (c && strncmp(why, "json: ", 6) == 0 && (c = strstr(c + 2, ": "))) why = c + 2;
		dfail(x, n, "%s", why);
		ak_release(err);
		return (ak_val){0};
	}
	return v;
}

static ak_val conv(dx *x, const tnode *n, const char *d) {
	switch (*d) {
	case '?': {
		ak_val v = conv(x, n, d + 1);
		if (x->failed || kind_of(d + 1) == AK_K_REF) return v;
		return (ak_val){.p = ak_box_new(kind_of(d + 1), v)};
	}
	case 'i':
		if (n->kind == T_INT) return (ak_val){.i = n->i};
		dfail(x, n, "expected an integer, not %s", what(n));
		return (ak_val){0};
	case 'f':
		if (n->kind == T_FLOAT) return (ak_val){.f = n->f};
		if (n->kind == T_INT) return (ak_val){.f = (double)n->i};
		dfail(x, n, "expected a float, not %s", what(n));
		return (ak_val){0};
	case 'b':
		if (n->kind == T_BOOL) return (ak_val){.i = n->b};
		dfail(x, n, "expected true or false, not %s", what(n));
		return (ak_val){0};
	case 's':
		if (n->kind == T_STR || (n->kind >= T_ODT && n->kind <= T_LT)) return (ak_val){.p = ak_str_new(n->s.p, n->s.n)};
		dfail(x, n, "expected a string, not %s", what(n));
		return (ak_val){0};
	case '[':
	case '<': {
		if (n->kind != T_ARR) {
			dfail(x, n, "expected an array, not %s", what(n));
			return (ak_val){0};
		}
		const char *ed = d + 1;
		ak_arr *a = NULL;
		ak_map *set = NULL;
		if (*d == '[') {
			a = ak_arr_new(kind_of(ed), *ed == 'S' || *ed == 'E' ? shape_named(x, ed) : NULL, n->a.n);
			if (*ed == 's') a->h.flags = 1;
		} else {
			set = ak_set_new(kind_of(ed), *ed == 's');
		}
		for (uint32_t i = 0; i < n->a.n; i++) {
			size_t old = path_index(x, i);
			ak_val v = conv(x, n->a.items[i], ed);
			x->plen = old;
			if (x->failed) {
				ak_release(a);
				ak_release(set);
				return (ak_val){0};
			}
			if (a) ak_arr_push(&a, v);
			else ak_set_insert(&set, v);
		}
		return (ak_val){.p = a ? (void *)a : (void *)set};
	}
	case '{': {
		if (n->kind != T_TAB) {
			dfail(x, n, "expected a table, not %s", what(n));
			return (ak_val){0};
		}
		const char *vd = d + 1;
		ak_map *m = ak_map_new(AK_K_REF, true, kind_of(vd), *vd == 'S' || *vd == 'E' ? shape_named(x, vd) : NULL);
		for (uint32_t i = 0; i < n->t.n; i++) {
			size_t old = path_key(x, n->t.e[i].k, n->t.e[i].kn);
			ak_val v = conv(x, n->t.e[i].v, vd);
			x->plen = old;
			if (x->failed) {
				ak_release(m);
				return (ak_val){0};
			}
			ak_map_set(&m, (ak_val){.p = ak_str_new(n->t.e[i].k, n->t.e[i].kn)}, v);
		}
		return (ak_val){.p = m};
	}
	case 'S': {
		const ak_shape *s = shape_named(x, d);
		if (!s) {
			dfail(x, n, "no type %.*s", (int)name_len(d), d + 1);
			return (ak_val){0};
		}
		if (n->kind != T_TAB) {
			dfail(x, n, "expected a table, not %s", what(n));
			return (ak_val){0};
		}
		ak_struct *o = ak_struct_new(s);
		for (uint16_t i = 0; i < s->nfields; i++) {
			const char *name = s->names ? s->names[i] : "";
			const char *fd = ak_json_field_desc(s, i);
			tent *e = tab_find((tnode *)n, name, (uint32_t)strlen(name));
			for (uint32_t j = 0; !e && j < n->t.n; j++)
				if (loose_eq(name, n->t.e[j].k, n->t.e[j].kn)) e = &n->t.e[j];
			if (!e) {
				if (!zero(x, fd, &o->f[i])) {
					dfail(x, n, "a missing key %s", name);
					break;
				}
				continue;
			}
			size_t old = path_key(x, e->k, e->kn);
			o->f[i] = conv(x, e->v, fd);
			x->plen = old;
			if (x->failed) break;
		}
		if (x->failed) {
			ak_release(o);
			return (ak_val){0};
		}
		return (ak_val){.p = o};
	}
	case 'e': {
		const char *const *cases;
		uint32_t nc;
		if (!x->t || !x->t->cases || !x->t->cases(x->t->ctx, d + 1, name_len(d), &cases, &nc)) {
			dfail(x, n, "no enum %.*s", (int)name_len(d), d + 1);
			return (ak_val){0};
		}
		if (n->kind != T_STR) {
			dfail(x, n, "expected a case's name, not %s", what(n));
			return (ak_val){0};
		}
		for (uint32_t i = 0; i < nc; i++)
			if (strlen(cases[i]) == n->s.n && memcmp(cases[i], n->s.p, n->s.n) == 0) return (ak_val){.i = i};
		dfail(x, n, "not a case: %.*s", (int)n->s.n, n->s.p);
		return (ak_val){0};
	}
	case 'E': {
		const ak_shape *s = shape_named(x, d);
		if (s && n->kind == T_STR) {
			for (uint16_t c = 0; c < s->ncases; c++)
				if (s->cases[c].nfields == 0 && strlen(s->cases[c].name) == n->s.n && memcmp(s->cases[c].name, n->s.p, n->s.n) == 0) return (ak_val){.p = ak_enum_new(s, c)};
			dfail(x, n, "not a case without a payload: %.*s", (int)n->s.n, n->s.p);
			return (ak_val){0};
		}
		return via_json(x, n, d);
	}
	case 'J': {
		const ak_shape *s = shape_named(x, d);
		if (!s || case_of(s, 'n') < 0) {
			dfail(x, n, "no type %.*s", (int)name_len(d), d + 1);
			return (ak_val){0};
		}
		return any_value(x, n, s);
	}
	default:
		return via_json(x, n, d);
	}
}

// toml.decode(text, desc): text's TOML as a value of desc's type.
static ak_val decode_fn(ak_val *args, uint32_t nargs) {
	AK_UNUSED(nargs);
	ak_str *text = args[0].p;
	const char *d = ((ak_str *)args[1].p)->data;
	tp ps = {0};
	if (!tparse(&ps, text->data, text->len)) {
		tthrow(&ps);
		tp_free(&ps);
		return (ak_val){0};
	}
	ak_json_types t;
	extern void ak_vm_json_types(ak_json_types *out);
	ak_vm_json_types(&t);
	dx x = {.ps = &ps, .t = &t};
	ak_val v = conv(&x, ps.root, d);
	if (x.failed) {
		int line, col;
		line_col(text->data, text->len, x.at, &line, &col);
		ak_throw_msg("toml: %d:%d: %s", line, col, x.msg);
	}
	tp_free(&ps);
	return v;
}

// ---- JSON of a tree (via_json) ----

static void json_of(ak_buf *b, const tnode *n) {
	switch (n->kind) {
	case T_INT: ak_buf_printf(b, "%lld", (long long)n->i); return;
	case T_FLOAT: {
		if (n->f != n->f || isinf(n->f)) {
			ak_buf_put(b, "null", 4);
			return;
		}
		ak_str *s = ak_str_from_float(n->f);
		ak_buf_put(b, s->data, s->len);
		ak_release(s);
		return;
	}
	case T_BOOL: ak_buf_put(b, n->b ? "true" : "false", n->b ? 4 : 5); return;
	case T_ARR:
		ak_buf_u8(b, '[');
		for (uint32_t i = 0; i < n->a.n; i++) {
			if (i) ak_buf_u8(b, ',');
			json_of(b, n->a.items[i]);
		}
		ak_buf_u8(b, ']');
		return;
	case T_TAB:
		ak_buf_u8(b, '{');
		for (uint32_t i = 0; i < n->t.n; i++) {
			if (i) ak_buf_u8(b, ',');
			tag_str(b, n->t.e[i].k, n->t.e[i].kn);
			ak_buf_u8(b, ':');
			json_of(b, n->t.e[i].v);
		}
		ak_buf_u8(b, '}');
		return;
	default:
		tag_str(b, n->s.p, n->s.n);
	}
}

// ---- Writing ----

typedef struct {
	const ak_json_types *t;
	arena ar;
	bool failed;
	char msg[300];
	char path[256];
	size_t plen;
	int depth;
} ex;

AK_PRINTF(2, 3) static void efail(ex *x, const char *fmt, ...) {
	if (x->failed) return;
	x->failed = true;
	char m[240];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(m, sizeof m, fmt, ap);
	va_end(ap);
	if (x->plen) snprintf(x->msg, sizeof x->msg, "%.*s: %s", (int)x->plen, x->path, m);
	else snprintf(x->msg, sizeof x->msg, "%s", m);
}

static size_t epath(ex *x, const char *fmt, const char *k, size_t n, uint32_t i) {
	size_t old = x->plen;
	int w;
	if (k) {
		bool bare = n > 0;
		for (size_t j = 0; j < n; j++)
			if (!bare_char((uint8_t)k[j])) bare = false;
		w = snprintf(x->path + x->plen, sizeof x->path - x->plen, bare ? "%s%.*s" : "%s\"%.*s\"", old ? "." : "", (int)n, k);
	} else {
		w = snprintf(x->path + x->plen, sizeof x->path - x->plen, fmt, i);
	}
	if (w > 0) x->plen = x->plen + (size_t)w < sizeof x->path ? x->plen + (size_t)w : sizeof x->path - 1;
	return old;
}

static tnode *enode(ex *x, int kind) {
	tnode *n = aalloc(&x->ar, sizeof *n);
	n->kind = (uint8_t)kind;
	return n;
}

static tnode *estr(ex *x, const char *s, size_t len) {
	tnode *n = enode(x, T_STR);
	n->s.p = acopy(&x->ar, s, len);
	n->s.n = (uint32_t)len;
	return n;
}

// Reading JSON (what ak_json_encode wrote of what TOML has no form of
// its own for) into a tree.
typedef struct {
	const char *p, *end;
} jr;

static void jws(jr *r) {
	while (r->p < r->end && (*r->p == ' ' || *r->p == '\n' || *r->p == '\t' || *r->p == '\r')) r->p++;
}

static tnode *jvalue(ex *x, jr *r, int depth);

static bool jstr(ex *x, jr *r, ak_buf *b) {
	AK_UNUSED(x);
	r->p++;
	while (r->p < r->end && *r->p != '"') {
		if (*r->p == '\\' && r->p + 1 < r->end) {
			char c = r->p[1];
			r->p += 2;
			switch (c) {
			case 'n': ak_buf_u8(b, '\n'); break;
			case 't': ak_buf_u8(b, '\t'); break;
			case 'r': ak_buf_u8(b, '\r'); break;
			case 'b': ak_buf_u8(b, '\b'); break;
			case 'f': ak_buf_u8(b, '\f'); break;
			case 'u': {
				uint32_t v = 0;
				for (int i = 0; i < 4 && r->p < r->end; i++, r->p++) v = v << 4 | (uint32_t)(hexval((uint8_t)*r->p) & 15);
				if (v >= 0xd800 && v < 0xdc00 && r->end - r->p >= 6 && r->p[0] == '\\' && r->p[1] == 'u') {
					uint32_t lo = 0;
					for (int i = 2; i < 6; i++) lo = lo << 4 | (uint32_t)(hexval((uint8_t)r->p[i]) & 15);
					if (lo >= 0xdc00 && lo < 0xe000) {
						v = 0x10000 + ((v - 0xd800) << 10) + (lo - 0xdc00);
						r->p += 6;
					}
				}
				char t[4];
				ak_buf_put(b, t, ak_utf8_encode(v, t));
				break;
			}
			default: ak_buf_u8(b, (uint8_t)c);
			}
			continue;
		}
		ak_buf_u8(b, (uint8_t)*r->p++);
	}
	if (r->p >= r->end) return false;
	r->p++;
	return true;
}

static tnode *jvalue(ex *x, jr *r, int depth) {
	jws(r);
	if (r->p >= r->end || depth > MAX_DEPTH) return NULL;
	char c = *r->p;
	if (c == '"') {
		ak_buf b = {0};
		bool ok = jstr(x, r, &b);
		tnode *n = ok ? estr(x, (const char *)b.data, b.len) : NULL;
		ak_buf_free(&b);
		return n;
	}
	if (c == '[') {
		r->p++;
		tnode *a = enode(x, T_ARR);
		jws(r);
		if (r->p < r->end && *r->p == ']') {
			r->p++;
			return a;
		}
		for (;;) {
			tnode *v = jvalue(x, r, depth + 1);
			if (!v) return NULL;
			arr_add(&x->ar, a, v);
			jws(r);
			if (r->p < r->end && *r->p == ',') {
				r->p++;
				continue;
			}
			if (r->p < r->end && *r->p == ']') {
				r->p++;
				return a;
			}
			return NULL;
		}
	}
	if (c == '{') {
		r->p++;
		tnode *t = enode(x, T_TAB);
		jws(r);
		if (r->p < r->end && *r->p == '}') {
			r->p++;
			return t;
		}
		for (;;) {
			jws(r);
			if (r->p >= r->end || *r->p != '"') return NULL;
			ak_buf b = {0};
			bool ok = jstr(x, r, &b);
			const char *k = acopy(&x->ar, b.data, b.len);
			uint32_t kn = (uint32_t)b.len;
			ak_buf_free(&b);
			jws(r);
			if (!ok || r->p >= r->end || *r->p != ':') return NULL;
			r->p++;
			jws(r);
			if (r->end - r->p >= 4 && memcmp(r->p, "null", 4) == 0) {
				r->p += 4;  // none: left out
			} else {
				tnode *v = jvalue(x, r, depth + 1);
				if (!v) return NULL;
				tab_add(&x->ar, t, k, kn, v);
			}
			jws(r);
			if (r->p < r->end && *r->p == ',') {
				r->p++;
				continue;
			}
			if (r->p < r->end && *r->p == '}') {
				r->p++;
				return t;
			}
			return NULL;
		}
	}
	if (r->end - r->p >= 4 && memcmp(r->p, "true", 4) == 0) {
		r->p += 4;
		tnode *n = enode(x, T_BOOL);
		n->b = true;
		return n;
	}
	if (r->end - r->p >= 5 && memcmp(r->p, "false", 5) == 0) {
		r->p += 5;
		return enode(x, T_BOOL);
	}
	if (r->end - r->p >= 4 && memcmp(r->p, "null", 4) == 0) {
		r->p += 4;
		efail(x, "TOML has no null");
		return NULL;
	}
	const char *st = r->p;
	bool fl = false;
	while (r->p < r->end && (digit((uint8_t)*r->p) || *r->p == '-' || *r->p == '+' || *r->p == '.' || *r->p == 'e' || *r->p == 'E')) {
		if (*r->p == '.' || *r->p == 'e' || *r->p == 'E') fl = true;
		r->p++;
	}
	if (r->p == st) return NULL;
	char buf[64];
	size_t k = (size_t)(r->p - st) < sizeof buf - 1 ? (size_t)(r->p - st) : sizeof buf - 1;
	memcpy(buf, st, k);
	buf[k] = 0;
	tnode *n = enode(x, fl ? T_FLOAT : T_INT);
	if (fl) n->f = strtod(buf, NULL);
	else n->i = strtoll(buf, NULL, 10);
	return n;
}

// tree_via_json is v as JSON writes it, as a tree (what TOML has no
// form of its own for).
static tnode *tree_via_json(ex *x, ak_val v, const char *d) {
	ak_str *j = ak_json_encode(v, d, x->t, 0);
	jr r = {j->data, j->data + j->len};
	tnode *n = jvalue(x, &r, x->depth);
	ak_release(j);
	if (!n) efail(x, "can't be written as TOML");
	return n;
}

// tree_of is v (of descriptor d) as a tree; NULL for none (and on
// failure, x->failed).
static tnode *tree_of(ex *x, ak_val v, const char *d) {
	while (*d == '?') {
		if (!v.p) return NULL;
		if (kind_of(d + 1) != AK_K_REF) v = ((ak_box *)v.p)->v;
		d++;
	}
	switch (*d) {
	case 'i': {
		tnode *n = enode(x, T_INT);
		n->i = v.i;
		return n;
	}
	case 'f': {
		tnode *n = enode(x, T_FLOAT);
		n->f = v.f;
		return n;
	}
	case 'b': {
		tnode *n = enode(x, T_BOOL);
		n->b = v.i != 0;
		return n;
	}
	case 's': {
		ak_str *s = v.p;
		return s ? estr(x, s->data, s->len) : estr(x, "", 0);
	}
	case 'e': {
		const char *const *cases;
		uint32_t nc;
		if (x->t && x->t->cases && x->t->cases(x->t->ctx, d + 1, name_len(d), &cases, &nc) && v.i >= 0 && (uint64_t)v.i < nc) return estr(x, cases[v.i], strlen(cases[v.i]));
		efail(x, "no enum %.*s", (int)name_len(d), d + 1);
		return NULL;
	}
	case '[':
	case '<': {
		if (++x->depth > MAX_DEPTH) {
			efail(x, "nested too deeply");
			return NULL;
		}
		tnode *a = enode(x, T_ARR);
		ak_arr *src = *d == '[' ? v.p : NULL;
		ak_arr *keys = *d == '<' && v.p ? ak_map_keys(v.p) : NULL;
		ak_arr *items = src ? src : keys;
		for (uint32_t i = 0; items && i < items->len && !x->failed; i++) {
			size_t old = epath(x, "[%u]", NULL, 0, i);
			tnode *e = tree_of(x, items->data[i], d + 1);
			if (!e && !x->failed) efail(x, "TOML has no null: an array's element can't be none");
			x->plen = old;
			if (e) arr_add(&x->ar, a, e);
		}
		ak_release(keys);
		x->depth--;
		return x->failed ? NULL : a;
	}
	case '{':
	case 'S': {
		if (++x->depth > MAX_DEPTH) {
			efail(x, "nested too deeply");
			return NULL;
		}
		tnode *t = enode(x, T_TAB);
		if (*d == '{') {
			ak_map *m = v.p;
			ak_arr *ks = m ? ak_map_keys(m) : NULL;
			ak_arr *vs = m ? ak_map_values(m) : NULL;
			for (uint32_t i = 0; ks && i < ks->len && !x->failed; i++) {
				ak_str *k = ks->data[i].p;
				size_t old = epath(x, NULL, k->data, k->len, 0);
				tnode *e = tree_of(x, vs->data[i], d + 1);
				x->plen = old;
				if (e) tab_add(&x->ar, t, acopy(&x->ar, k->data, k->len), k->len, e);
			}
			ak_release(ks);
			ak_release(vs);
		} else {
			ak_struct *o = v.p;
			const ak_shape *s = o ? o->shape : NULL;
			for (uint16_t i = 0; s && i < s->nfields && !x->failed; i++) {
					const char *name = s->names ? s->names[i] : "";
				size_t old = epath(x, NULL, name, strlen(name), 0);
				tnode *e = tree_of(x, o->f[i], ak_json_field_desc(s, i));
				x->plen = old;
				if (e) tab_add(&x->ar, t, name, (uint32_t)strlen(name), e);
			}
		}
		x->depth--;
		return x->failed ? NULL : t;
	}
	case 'J': {
		ak_enum *en = v.p;
		const ak_case *c = en ? &en->shape->cases[ak_enum_case(en)] : NULL;
		switch (c ? c->name[0] : 'n') {
		case 'b': return tree_of(x, en->f[0], "b");
		case 'i': return tree_of(x, en->f[0], "i");
		case 'f': return tree_of(x, en->f[0], "f");
		case 's': return tree_of(x, en->f[0], "s");
		case 'a': {
			if (++x->depth > MAX_DEPTH) {
				efail(x, "nested too deeply");
				return NULL;
			}
			tnode *a = enode(x, T_ARR);
			ak_arr *items = en->f[0].p;
			for (uint32_t i = 0; items && i < items->len && !x->failed; i++) {
				size_t old = epath(x, "[%u]", NULL, 0, i);
				tnode *e = tree_of(x, items->data[i], d);
				if (!e && !x->failed) efail(x, "TOML has no null: an array's element can't be null");
				x->plen = old;
				if (e) arr_add(&x->ar, a, e);
			}
			x->depth--;
			return x->failed ? NULL : a;
		}
		case 'o': {
			if (++x->depth > MAX_DEPTH) {
				efail(x, "nested too deeply");
				return NULL;
			}
			tnode *t = enode(x, T_TAB);
			ak_map *m = en->f[0].p;
			ak_arr *ks = m ? ak_map_keys(m) : NULL;
			ak_arr *vs = m ? ak_map_values(m) : NULL;
			for (uint32_t i = 0; ks && i < ks->len && !x->failed; i++) {
				ak_str *k = ks->data[i].p;
				size_t old = epath(x, NULL, k->data, k->len, 0);
				tnode *e = tree_of(x, vs->data[i], d);
				x->plen = old;
				if (e) tab_add(&x->ar, t, acopy(&x->ar, k->data, k->len), k->len, e);
			}
			ak_release(ks);
			ak_release(vs);
			x->depth--;
			return x->failed ? NULL : t;
		}
		}
		return NULL;  // null: left out
	}
	case 'E': {
		ak_enum *en = v.p;
		const ak_case *c = &en->shape->cases[ak_enum_case(en)];
		if (c->nfields == 0) return estr(x, c->name, strlen(c->name));
		return tree_via_json(x, v, d);  // a case with a payload, as JSON writes it
	}
	case 'y':
		return tree_via_json(x, v, d);
	}
	efail(x, "TOML can't say this type (%.1s)", d);
	return NULL;
}

static bool key_bare(const char *k, uint32_t n) {
	if (!n) return false;
	for (uint32_t i = 0; i < n; i++)
		if (!bare_char((uint8_t)k[i])) return false;
	return true;
}

static void w_str(ak_buf *b, const char *s, size_t n) {
	ak_buf_u8(b, '"');
	for (size_t i = 0; i < n; i++) {
		uint8_t c = (uint8_t)s[i];
		switch (c) {
		case '"': ak_buf_put(b, "\\\"", 2); break;
		case '\\': ak_buf_put(b, "\\\\", 2); break;
		case '\n': ak_buf_put(b, "\\n", 2); break;
		case '\t': ak_buf_put(b, "\\t", 2); break;
		case '\r': ak_buf_put(b, "\\r", 2); break;
		case '\b': ak_buf_put(b, "\\b", 2); break;
		case '\f': ak_buf_put(b, "\\f", 2); break;
		default:
			if (c < 0x20 || c == 0x7f) ak_buf_printf(b, "\\u%04X", c);
			else ak_buf_u8(b, c);
		}
	}
	ak_buf_u8(b, '"');
}

static void w_key(ak_buf *b, const char *k, uint32_t n) {
	if (key_bare(k, n)) ak_buf_put(b, k, n);
	else w_str(b, k, n);
}

static bool is_aot(const tnode *n) {
	if (n->kind != T_ARR || n->a.n == 0) return false;
	for (uint32_t i = 0; i < n->a.n; i++)
		if (n->a.items[i]->kind != T_TAB) return false;
	return true;
}

static void w_inline(ak_buf *b, const tnode *n) {
	switch (n->kind) {
	case T_STR: w_str(b, n->s.p, n->s.n); return;
	case T_INT: ak_buf_printf(b, "%lld", (long long)n->i); return;
	case T_BOOL: ak_buf_put(b, n->b ? "true" : "false", n->b ? 4 : 5); return;
	case T_FLOAT: {
		if (n->f != n->f) {
			ak_buf_put(b, "nan", 3);
			return;
		}
		if (isinf(n->f)) {
			ak_buf_put(b, n->f < 0 ? "-inf" : "inf", n->f < 0 ? 4 : 3);
			return;
		}
		ak_str *s = ak_str_from_float(n->f);
		ak_buf_put(b, s->data, s->len);
		if (!strpbrk(s->data, ".e")) ak_buf_put(b, ".0", 2);
		ak_release(s);
		return;
	}
	case T_ARR:
		ak_buf_u8(b, '[');
		for (uint32_t i = 0; i < n->a.n; i++) {
			if (i) ak_buf_put(b, ", ", 2);
			w_inline(b, n->a.items[i]);
		}
		ak_buf_u8(b, ']');
		return;
	case T_TAB:
		if (!n->t.n) {
			ak_buf_put(b, "{}", 2);
			return;
		}
		ak_buf_put(b, "{ ", 2);
		for (uint32_t i = 0; i < n->t.n; i++) {
			if (i) ak_buf_put(b, ", ", 2);
			w_key(b, n->t.e[i].k, n->t.e[i].kn);
			ak_buf_put(b, " = ", 3);
			w_inline(b, n->t.e[i].v);
		}
		ak_buf_put(b, " }", 2);
		return;
	default:
		ak_buf_put(b, n->s.p, n->s.n);  // a date-time, as it is
	}
}

typedef struct {
	const char *k;
	uint32_t n;
} wkey;

static void w_header(ak_buf *b, const wkey *path, uint32_t np, bool aot) {
	if (b->len) ak_buf_u8(b, '\n');
	ak_buf_put(b, aot ? "[[" : "[", aot ? 2 : 1);
	for (uint32_t i = 0; i < np; i++) {
		if (i) ak_buf_u8(b, '.');
		w_key(b, path[i].k, path[i].n);
	}
	ak_buf_put(b, aot ? "]]\n" : "]\n", aot ? 3 : 2);
}

// w_table writes a table's plain keys, then its tables as sections.
static void w_table(ak_buf *b, const tnode *t, wkey *path, uint32_t np) {
	for (uint32_t i = 0; i < t->t.n; i++) {
		const tnode *v = t->t.e[i].v;
		if (v->kind == T_TAB || is_aot(v)) continue;
		w_key(b, t->t.e[i].k, t->t.e[i].kn);
		ak_buf_put(b, " = ", 3);
		w_inline(b, v);
		ak_buf_u8(b, '\n');
	}
	for (uint32_t i = 0; i < t->t.n; i++) {
		const tnode *v = t->t.e[i].v;
		if (v->kind != T_TAB && !is_aot(v)) continue;
		path[np] = (wkey){t->t.e[i].k, t->t.e[i].kn};
		if (v->kind == T_TAB) {
			// A table of tables alone needs no header of its own.
			bool plain = v->t.n == 0;
			for (uint32_t j = 0; j < v->t.n; j++)
				if (v->t.e[j].v->kind != T_TAB && !is_aot(v->t.e[j].v)) plain = true;
			if (plain) w_header(b, path, np + 1, false);
			w_table(b, v, path, np + 1);
		} else {
			for (uint32_t j = 0; j < v->a.n; j++) {
				w_header(b, path, np + 1, true);
				w_table(b, v->a.items[j], path, np + 1);
			}
		}
	}
}

// toml.encode(value, desc): value as a TOML document.
static ak_val encode_fn(ak_val *args, uint32_t nargs) {
	AK_UNUSED(nargs);
	const char *d = ((ak_str *)args[1].p)->data;
	ak_json_types t;
	extern void ak_vm_json_types(ak_json_types *out);
	ak_vm_json_types(&t);
	ex x = {.t = &t};
	tnode *root = tree_of(&x, args[0], d);
	if (!x.failed && (!root || root->kind != T_TAB)) efail(&x, "a TOML document is a table: encode takes a struct, a [string: T] or a json.Value object");
	if (x.failed) {
		ak_throw_msg("toml: %s", x.msg);
		afree(&x.ar);
		return (ak_val){0};
	}
	ak_buf b = {0};
	wkey *path = ak_alloc(sizeof(wkey) * (MAX_DEPTH + 2));
	w_table(&b, root, path, 0);
	ak_free(path);
	ak_str *s = ak_str_new((const char *)b.data, b.len);
	ak_buf_free(&b);
	afree(&x.ar);
	return (ak_val){.p = s};
}

const ak_lib_entry ak_std_toml[] = {
	{"toml.decode", decode_fn},
	{"toml.encode", encode_fn},
	{NULL, NULL},
};
