// csv.c: std/csv's builtins. CSV (RFC 4180) read as Go's encoding/csv
// reads it: fields separated by a character (a comma, or any other but a
// quote or a line break), a field in quotes may hold the separator, line
// breaks and quotes (doubled); CRLF or LF; empty lines skipped, and
// lines that start with a comment character when one is given; a UTF-8
// byte order mark at the start dropped. Rows read as [string] (or [X],
// each cell an X), or into a struct, its fields by the header row's
// names; rows written back with the fields quoted only when they must be.
#include "std/std.h"
#include "rt/json.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>

// json.c's: a shape's field's descriptor.
extern const char *ak_json_field_desc(const ak_shape *s, int i);

#define STR(i) ((ak_str *)args[i].p)

// ---- Reading ----

typedef struct {
	const char *src, *p, *end;
	char sep[4];
	size_t seplen;
	char com[4];
	size_t comlen;
	bool trim, lazy;
	int64_t line;  // the line p is on
	// The record read: its fields' text one after another, where each
	// ends, and where each starts (its line and column).
	ak_buf rec;
	AK_VEC(uint32_t) ends;
	AK_VEC(int64_t) lines;
	AK_VEC(int64_t) cols;
	int64_t rec_line;
	bool open;  // the text ended in a quoted field
	bool failed;
	char msg[300];
} cr;

AK_PRINTF(4, 5) static void cfail(cr *r, int64_t line, int64_t col, const char *fmt, ...) {
	if (r->failed) return;
	r->failed = true;
	char m[200];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(m, sizeof m, fmt, ap);
	va_end(ap);
	snprintf(r->msg, sizeof r->msg, "csv: %lld:%lld: %s", (long long)line, (long long)col, m);
}

// eol reports whether q is at the end of a line: a newline, CRLF, or the
// end of the text (a lone CR just before it too, as Go drops one).
static bool eol(const cr *r, const char *q) {
	if (q >= r->end || *q == '\n') return true;
	return *q == '\r' && (q + 1 == r->end || q[1] == '\n');
}

static const char *past_eol(const cr *r, const char *q) {
	if (q < r->end && *q == '\r') q++;
	if (q < r->end && *q == '\n') q++;
	return q;
}

static bool at(const cr *r, const char *q, const char *s, size_t n) { return n && (size_t)(r->end - q) >= n && memcmp(q, s, n) == 0; }

// chars counts the characters from a to b (columns).
static int64_t chars(const char *a, const char *b) {
	int64_t n = 0;
	for (; a < b; a++)
		if (((uint8_t)*a & 0xc0) != 0x80) n++;
	return n;
}

// space reports whether the character at q is a space, as Go's
// unicode.IsSpace says (the line's end aside); its length in *n.
static bool space(const cr *r, const char *q, size_t *n) {
	uint8_t c = (uint8_t)*q;
	if (c == ' ' || c == '\t' || c == '\v' || c == '\f') {
		*n = 1;
		return true;
	}
	if (c < 0x80) return false;
	size_t i = (size_t)(q - r->src);
	uint32_t cp = ak_utf8_decode(r->src, (size_t)(r->end - r->src), &i);
	*n = i - (size_t)(q - r->src);
	return cp == 0x85 || cp == 0xa0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200a) || cp == 0x2028 || cp == 0x2029 || cp == 0x202f || cp == 0x205f || cp == 0x3000;
}

static void end_field(cr *r, int64_t line, int64_t col) {
	ak_vec_push(&r->ends, (uint32_t)r->rec.len);
	ak_vec_push(&r->lines, line);
	ak_vec_push(&r->cols, col);
}

// record reads the next record into r->rec; false at the end of the
// text, or on a failure (r->failed).
static bool record(cr *r) {
	ak_buf_reset(&r->rec);
	r->ends.len = r->lines.len = r->cols.len = 0;
	// Empty lines and comments are skipped.
	for (;;) {
		if (r->p >= r->end) return false;
		if (eol(r, r->p) || at(r, r->p, r->com, r->comlen)) {
			while (!eol(r, r->p)) r->p++;
			r->p = past_eol(r, r->p);
			r->line++;
			continue;
		}
		break;
	}
	r->rec_line = r->line;
	const char *ls = r->p;  // the line's start, for columns
	for (;;) {
		if (r->trim) {
			size_t n;
			while (r->p < r->end && !eol(r, r->p) && space(r, r->p, &n)) r->p += n;
		}
		int64_t col = chars(ls, r->p) + 1;
		if (r->p >= r->end || *r->p != '"') {
			// A field that isn't quoted: to the separator or the line's end.
			const char *st = r->p;
			while (!eol(r, r->p) && !at(r, r->p, r->sep, r->seplen)) {
				if (*r->p == '"' && !r->lazy) {
					cfail(r, r->line, chars(ls, r->p) + 1, "a bare \" in a field that isn't quoted (quote the field, and double the quote: \"a\"\"b\")");
					return false;
				}
				r->p++;
			}
			ak_buf_put(&r->rec, st, (size_t)(r->p - st));
			end_field(r, r->line, col);
			if (at(r, r->p, r->sep, r->seplen)) {
				r->p += r->seplen;
				continue;
			}
			r->p = past_eol(r, r->p);
			r->line++;
			return true;
		}
		// A quoted field: to its closing quote, over lines.
		int64_t fline = r->line;
		r->p++;
		for (;;) {
			const char *st = r->p;
			while (r->p < r->end && *r->p != '"' && *r->p != '\n' && !(*r->p == '\r' && r->p + 1 < r->end && r->p[1] == '\n')) r->p++;
			ak_buf_put(&r->rec, st, (size_t)(r->p - st));
			if (r->p >= r->end) {
				r->open = true;
				if (!r->lazy) {
					cfail(r, fline, col, "a quoted field that doesn't end");
					return false;
				}
				end_field(r, fline, col);
				return true;
			}
			if (*r->p != '"') {
				// A line break in the field: kept, as \n.
				ak_buf_u8(&r->rec, '\n');
				r->p = past_eol(r, r->p);
				r->line++;
				ls = r->p;
				continue;
			}
			const char *q = r->p++;
			if (r->p < r->end && *r->p == '"') {
				ak_buf_u8(&r->rec, '"');
				r->p++;
			} else if (at(r, r->p, r->sep, r->seplen)) {
				r->p += r->seplen;
				end_field(r, fline, col);
				break;
			} else if (eol(r, r->p)) {
				end_field(r, fline, col);
				r->p = past_eol(r, r->p);
				r->line++;
				return true;
			} else if (r->lazy) {
				ak_buf_u8(&r->rec, '"');
			} else {
				cfail(r, r->line, chars(ls, q) + 1, "a \" in a quoted field must be doubled (\"a\"\"b\")");
				return false;
			}
		}
	}
}

static const char *field(const cr *r, uint32_t i, size_t *n) {
	uint32_t st = i ? r->ends.data[i - 1] : 0;
	*n = r->ends.data[i] - st;
	return (const char *)r->rec.data + st;
}

static void cr_free(cr *r) {
	ak_buf_free(&r->rec);
	ak_vec_free(&r->ends);
	ak_vec_free(&r->lines);
	ak_vec_free(&r->cols);
}

// one_char takes a separator's or a comment's character: false when s
// isn't one character.
static bool one_char(const ak_str *s, char out[4], size_t *n) {
	*n = 0;
	if (!s->len) return true;
	size_t i = 0;
	uint32_t c = ak_utf8_decode(s->data, s->len, &i);
	if (i != s->len || (c == 0xfffd && s->len != 3)) return false;
	memcpy(out, s->data, i);
	*n = i;
	return true;
}

// cr_init reads a call's text and options: text, separator, comment,
// trim, lazy, the first line's number.
static bool cr_init(cr *r, ak_val *args) {
	*r = (cr){0};
	ak_str *text = STR(0);
	r->src = r->p = text->data;
	r->end = text->data + text->len;
	if (text->len >= 3 && memcmp(text->data, "\xef\xbb\xbf", 3) == 0) r->p += 3;
	if (!one_char(STR(1), r->sep, &r->seplen) || r->seplen == 0 || r->sep[0] == '"' || r->sep[0] == '\n' || r->sep[0] == '\r') {
		ak_throw_msg("csv: a separator is one character, not a quote or a line break: \"%s\"", STR(1)->data);
		return false;
	}
	if (!one_char(STR(2), r->com, &r->comlen) || (r->comlen && (r->com[0] == '"' || r->com[0] == '\n' || r->com[0] == '\r' || (r->comlen == r->seplen && memcmp(r->com, r->sep, r->seplen) == 0)))) {
		ak_throw_msg("csv: a comment is one character, not a quote, a line break or the separator: \"%s\"", STR(2)->data);
		return false;
	}
	r->trim = args[3].i != 0;
	r->lazy = args[4].i != 0;
	r->line = args[6].i;
	return true;
}

// ---- Cells as values ----

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

static const ak_shape *shape_named(const ak_json_types *t, const char *d) {
	if (*d != 'S' && *d != 'E' && *d != 'J') return NULL;
	return t && t->shape ? t->shape(t->ctx, d + 1, name_len(d)) : NULL;
}

static const ak_json_types *types(void) {
	static AK_TLS ak_json_types t;
	extern void ak_vm_json_types(ak_json_types *out);
	ak_vm_json_types(&t);
	return &t;
}

// short_name is a type's name without its package's (std/csv.Sale: Sale).
static const char *short_name(const char *name) {
	const char *s = name;
	for (const char *p = name; *p; p++)
		if (*p == '.' || *p == '/') s = p + 1;
	return s;
}

// trimmed is s without the spaces around it (a number's, a bool's).
static void trimmed(const char **s, size_t *n) {
	while (*n && (**s == ' ' || **s == '\t')) (*s)++, (*n)--;
	while (*n && ((*s)[*n - 1] == ' ' || (*s)[*n - 1] == '\t')) (*n)--;
}

static bool word_is(const char *s, size_t n, const char *w) {
	if (strlen(w) != n) return false;
	for (size_t i = 0; i < n; i++) {
		char c = s[i];
		if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
		if (c != w[i]) return false;
	}
	return true;
}

// cell makes a value of descriptor d from a cell's text; false (and
// why) when it can't be one.
static bool cell(const char *s, size_t n, const char *d, const ak_json_types *t, ak_val *out, char *why, size_t whyn) {
	*out = (ak_val){0};
	switch (*d) {
	case '?': {
		if (n == 0) return true;  // none
		if (!cell(s, n, d + 1, t, out, why, whyn)) return false;
		if (kind_of(d + 1) != AK_K_REF) *out = (ak_val){.p = ak_box_new(kind_of(d + 1), *out)};
		return true;
	}
	case 's':
		out->p = ak_str_new(s, n);
		return true;
	case 'i': {
		trimmed(&s, &n);
		size_t i = 0;
		bool neg = false;
		if (i < n && (s[i] == '+' || s[i] == '-')) neg = s[i++] == '-';
		uint64_t v = 0, limit = neg ? (uint64_t)INT64_MAX + 1 : (uint64_t)INT64_MAX;
		bool ok = i < n;
		for (; ok && i < n; i++) {
			if (s[i] < '0' || s[i] > '9') ok = false;
			else {
				uint64_t dg = (uint64_t)(s[i] - '0');
				if (v > (limit - dg) / 10) {
					snprintf(why, whyn, "%.*s is out of an int's range", (int)(n < 60 ? n : 60), s);
					return false;
				}
				v = v * 10 + dg;
			}
		}
		if (!ok) {
			snprintf(why, whyn, "\"%.*s\" isn't an integer", (int)(n < 60 ? n : 60), s);
			return false;
		}
		out->i = neg ? (int64_t)(0 - v) : (int64_t)v;
		return true;
	}
	case 'f': {
		trimmed(&s, &n);
		char buf[128];
		bool ok = n > 0 && n < sizeof buf;
		for (size_t i = 0; ok && i < n; i++)
			if (s[i] == 'x' || s[i] == 'X' || s[i] == '_') ok = false;
		if (ok) {
			memcpy(buf, s, n);
			buf[n] = 0;
			char *e;
			out->f = strtod(buf, &e);
			ok = e == buf + n;
		}
		if (!ok) {
			snprintf(why, whyn, "\"%.*s\" isn't a number", (int)(n < 60 ? n : 60), s);
			return false;
		}
		return true;
	}
	case 'b': {
		trimmed(&s, &n);
		static const char *const yes[] = {"true", "t", "1", "yes", "y"}, *const no[] = {"false", "f", "0", "no", "n"};
		for (int i = 0; i < 5; i++) {
			if (word_is(s, n, yes[i])) {
				out->i = 1;
				return true;
			}
			if (word_is(s, n, no[i])) return true;
		}
		snprintf(why, whyn, "\"%.*s\" isn't true or false", (int)(n < 60 ? n : 60), s);
		return false;
	}
	case 'e': {
		const char *const *cases;
		uint32_t nc;
		if (t && t->cases && t->cases(t->ctx, d + 1, name_len(d), &cases, &nc)) {
			for (uint32_t i = 0; i < nc; i++)
				if (strlen(cases[i]) == n && memcmp(cases[i], s, n) == 0) {
					out->i = i;
					return true;
				}
		}
		// The enum's name, without its package's.
		const char *nm = d + 1;
		size_t nl = name_len(d);
		for (size_t i = 0; i < nl; i++)
			if (d[1 + i] == '.' || d[1 + i] == '/') nm = d + 2 + i;
		snprintf(why, whyn, "\"%.*s\" isn't a case of %.*s", (int)(n < 60 ? n : 60), s, (int)(nl - (size_t)(nm - d - 1)), nm);
		return false;
	}
	case 'E': {
		const ak_shape *sh = shape_named(t, d);
		for (uint16_t c = 0; sh && c < sh->ncases; c++)
			if (sh->cases[c].nfields == 0 && strlen(sh->cases[c].name) == n && memcmp(sh->cases[c].name, s, n) == 0) {
				out->p = ak_enum_new(sh, c);
				return true;
			}
		break;  // a case with a payload: as JSON
	}
	}
	// Anything else (a struct, an array, a map, a json.Value) is the
	// cell's JSON.
	*out = ak_json_decode(s, n, d, t);
	if (ak_error_pending()) {
		void *err = ak_error_take();
		ak_str *m = ak_error_message(err);
		snprintf(why, whyn, "%s", m ? m->data : "not JSON");
		ak_release(err);
		*out = (ak_val){0};
		return false;
	}
	return true;
}

// loose compares a field's name and a header's as people write headers:
// case, spaces, dashes and underscores aside ("First Name" is firstName).
static bool loose_eq(const char *a, const char *k, size_t kn) {
	size_t i = 0, j = 0, an = strlen(a);
	for (;;) {
		while (i < an && (a[i] == '-' || a[i] == '_' || a[i] == ' ')) i++;
		while (j < kn && (k[j] == '-' || k[j] == '_' || k[j] == ' ')) j++;
		if (i == an || j == kn) return i == an && j == kn;
		char p = a[i], q = k[j];
		if (p >= 'A' && p <= 'Z') p = (char)(p - 'A' + 'a');
		if (q >= 'A' && q <= 'Z') q = (char)(q - 'A' + 'a');
		if (p != q) return false;
		i++, j++;
	}
}

// csv.read(text, separator, comment, trim, lazy, flexible, line, header,
// fields, desc): text's records as [T]. T is [X] (each record's cells,
// each an X) or a struct: its fields by the header's names, header
// given (a stream's later records) or the first record. fields is the
// number of fields each record must have (-1: as the first does);
// flexible, any.
static ak_val read_fn(ak_val *args, uint32_t nargs) {
	AK_UNUSED(nargs);
	const char *d = STR(9)->data;
	cr r;
	if (!cr_init(&r, args)) return (ak_val){0};
	bool flexible = args[5].i != 0;
	ak_arr *given = args[7].p;
	int64_t fields = args[8].i;
	const ak_json_types *t = types();
	const ak_shape *sh = NULL;
	if (*d == 'S') {
		sh = shape_named(t, d);
		if (!sh) {
			ak_throw_msg("csv: no type %.*s", (int)name_len(d), d + 1);
			return (ak_val){0};
		}
	} else if (*d != '[') {
		ak_throw_msg("csv: rows read as a struct or an array ([string]), not %s", d);
		return (ak_val){0};
	}
	ak_arr *out = ak_arr_new(kind_of(d), sh, 0);
	// A struct's fields' columns: -1 when the header hasn't one.
	int32_t *colof = sh ? ak_alloc(sizeof(int32_t) * (sh->nfields + 1u)) : NULL;
	bool have_header = !sh;
	if (sh && given && given->len) {
		have_header = true;
		for (uint16_t i = 0; i < sh->nfields; i++) {
			colof[i] = -1;
			for (uint32_t pass = 0; pass < 2 && colof[i] < 0; pass++)
				for (uint32_t c = 0; c < given->len && colof[i] < 0; c++) {
					ak_str *h = given->data[c].p;
					if (pass == 0 ? strcmp(h->data, sh->names[i]) == 0 : loose_eq(sh->names[i], h->data, h->len)) colof[i] = (int32_t)c;
				}
		}
	}
	char why[300];
	while (record(&r)) {
		uint32_t nf = r.ends.len;
		if (!flexible) {
			if (fields < 0) fields = nf;
			else if (nf != fields) {
				cfail(&r, r.rec_line, 1, "a row of %u fields, not %lld as the first", nf, (long long)fields);
				break;
			}
		}
		if (!have_header) {
			// The header: each field's column, by name (as it is, then
			// loosely).
			have_header = true;
			for (uint16_t i = 0; i < sh->nfields; i++) {
				colof[i] = -1;
				for (uint32_t pass = 0; pass < 2 && colof[i] < 0; pass++)
					for (uint32_t c = 0; c < nf && colof[i] < 0; c++) {
						size_t n;
						const char *h = field(&r, c, &n);
						if (pass == 0 ? (strlen(sh->names[i]) == n && memcmp(h, sh->names[i], n) == 0) : loose_eq(sh->names[i], h, n)) colof[i] = (int32_t)c;
					}
			}
			for (uint16_t i = 0; i < sh->nfields && !r.failed; i++)
				if (colof[i] < 0 && ak_json_field_desc(sh, i)[0] != '?') cfail(&r, r.rec_line, 1, "no column %s in the header for %s's field (make the field optional for a column that may be missing)", sh->names[i], short_name(sh->name));
			if (r.failed) break;
			continue;
		}
		if (!sh) {
			const char *ed = d + 1;
			ak_arr *row = ak_arr_new(kind_of(ed), shape_named(t, ed), nf);
			if (*ed == 's') row->h.flags = 1;
			for (uint32_t c = 0; c < nf && !r.failed; c++) {
				size_t n;
				const char *s = field(&r, c, &n);
				ak_val v;
				if (!cell(s, n, ed, t, &v, why, sizeof why)) cfail(&r, r.lines.data[c], r.cols.data[c], "%s", why);
				else ak_arr_push(&row, v);
			}
			if (r.failed) {
				ak_release(row);
				break;
			}
			ak_arr_push(&out, (ak_val){.p = row});
			continue;
		}
		ak_struct *o = ak_struct_new(sh);
		for (uint16_t i = 0; i < sh->nfields && !r.failed; i++) {
			const char *fd = ak_json_field_desc(sh, i);
			if (colof[i] < 0 || (uint32_t)colof[i] >= nf) {
				if (*fd != '?') cfail(&r, r.rec_line, 1, "no %s: the row has %u fields", sh->names[i], nf);
				continue;
			}
			size_t n;
			const char *s = field(&r, (uint32_t)colof[i], &n);
			if (!cell(s, n, fd, t, &o->f[i], why, sizeof why)) cfail(&r, r.lines.data[colof[i]], r.cols.data[colof[i]], "%s: %s", sh->names[i], why);
		}
		if (r.failed) {
			ak_release(o);
			break;
		}
		ak_arr_push(&out, (ak_val){.p = o});
	}
	ak_free(colof);
	if (r.failed) {
		ak_throw_msg("%s", r.msg);
		ak_release(out);
		cr_free(&r);
		return (ak_val){0};
	}
	cr_free(&r);
	return (ak_val){.p = out};
}

// csv.incomplete(text, separator, trim, lazy): whether text ends inside
// a quoted field, so a record read line by line goes on on the next.
static ak_val incomplete_fn(ak_val *args, uint32_t nargs) {
	AK_UNUSED(nargs);
	ak_str *text = STR(0);
	cr r = {.src = text->data, .p = text->data, .end = text->data + text->len, .trim = args[2].i != 0, .lazy = args[3].i != 0};
	if (!one_char(STR(1), r.sep, &r.seplen)) r.seplen = 0;
	while (record(&r)) {}
	bool open = r.open;
	cr_free(&r);
	return (ak_val){.i = open};
}

// ---- Writing ----

typedef struct {
	ak_buf b;
	char sep[4];
	size_t seplen;
	bool crlf;
} cw;

// put writes a field, quoted when it must be: when it holds the
// separator, a quote or a line break, or starts with a space (as Go's
// encoding/csv), or is \. (a PostgreSQL end of data).
static void put(cw *w, bool first, const char *s, size_t n) {
	if (!first) ak_buf_put(&w->b, w->sep, w->seplen);
	bool quote = n == 2 && s[0] == '\\' && s[1] == '.';
	for (size_t i = 0; i < n && !quote; i++)
		if (s[i] == '"' || s[i] == '\n' || s[i] == '\r' || (w->seplen && i + w->seplen <= n && memcmp(s + i, w->sep, w->seplen) == 0)) quote = true;
	if (!quote && n) {
		size_t k;
		cr r = {.src = s, .end = s + n};
		quote = space(&r, s, &k);
	}
	if (!quote) {
		ak_buf_put(&w->b, s, n);
		return;
	}
	ak_buf_u8(&w->b, '"');
	for (size_t i = 0; i < n; i++) {
		if (s[i] == '"') ak_buf_put(&w->b, "\"\"", 2);
		else if (s[i] == '\n' && w->crlf && !(i > 0 && s[i - 1] == '\r')) ak_buf_put(&w->b, "\r\n", 2);
		else ak_buf_u8(&w->b, (uint8_t)s[i]);
	}
	ak_buf_u8(&w->b, '"');
}

static void row_end(cw *w) {
	if (w->crlf) ak_buf_put(&w->b, "\r\n", 2);
	else ak_buf_u8(&w->b, '\n');
}

// put_value writes v (of descriptor d) as a cell: a string as it is, a
// number as it prints, none empty, an enum's case by its name, anything
// else as its JSON.
static void put_value(cw *w, bool first, ak_val v, const char *d, const ak_json_types *t) {
	while (*d == '?') {
		if (!v.p) {
			put(w, first, "", 0);
			return;
		}
		if (kind_of(d + 1) != AK_K_REF) v = ((ak_box *)v.p)->v;
		d++;
	}
	ak_str *s = NULL;
	switch (*d) {
	case 's': {
		ak_str *x = v.p;
		put(w, first, x ? x->data : "", x ? x->len : 0);
		return;
	}
	case 'i': s = ak_str_from_int(v.i); break;
	case 'f': s = ak_str_from_float(v.f); break;
	case 'b': s = ak_str_c(v.i ? "true" : "false"); break;
	case 'E': {
		ak_enum *en = v.p;
		const ak_case *c = &en->shape->cases[ak_enum_case(en)];
		if (c->nfields == 0) s = ak_str_c(c->name);
		break;
	}
	}
	if (!s) {
		s = ak_json_encode(v, d, t, 0);
		// An enum stored as an int writes its case's name: a JSON string.
		if (*d == 'e' && s->len >= 2 && s->data[0] == '"') {
			ak_str *u = ak_str_new(s->data + 1, s->len - 2);
			ak_release(s);
			s = u;
		}
	}
	put(w, first, s->data, s->len);
	ak_release(s);
}

// csv.write(rows, separator, header, crlf, desc): rows as CSV. A struct's
// rows have a header of its fields' names first (when header is true).
static ak_val write_fn(ak_val *args, uint32_t nargs) {
	AK_UNUSED(nargs);
	ak_arr *rows = args[0].p;
	const char *d = STR(4)->data;
	cw w = {.crlf = args[3].i != 0};
	if (!one_char(STR(1), w.sep, &w.seplen) || w.seplen == 0 || w.sep[0] == '"' || w.sep[0] == '\n' || w.sep[0] == '\r') {
		ak_throw_msg("csv: a separator is one character, not a quote or a line break: \"%s\"", STR(1)->data);
		return (ak_val){0};
	}
	const ak_json_types *t = types();
	if (*d == 'S') {
		const ak_shape *sh = shape_named(t, d);
		if (!sh) {
			ak_throw_msg("csv: no type %.*s", (int)name_len(d), d + 1);
			return (ak_val){0};
		}
		if (args[2].i) {
			for (uint16_t i = 0; i < sh->nfields; i++) put(&w, i == 0, sh->names[i], strlen(sh->names[i]));
			row_end(&w);
		}
		for (uint32_t r = 0; rows && r < rows->len; r++) {
			ak_struct *o = rows->data[r].p;
			for (uint16_t i = 0; i < sh->nfields; i++) put_value(&w, i == 0, o->f[i], ak_json_field_desc(sh, i), t);
			row_end(&w);
		}
	} else if (*d == '[') {
		for (uint32_t r = 0; rows && r < rows->len; r++) {
			ak_arr *row = rows->data[r].p;
			for (uint32_t i = 0; row && i < row->len; i++) put_value(&w, i == 0, row->data[i], d + 1, t);
			row_end(&w);
		}
	} else {
		ak_throw_msg("csv: rows are written from structs or arrays ([string]), not %s", d);
		return (ak_val){0};
	}
	ak_str *s = ak_str_new((const char *)w.b.data, w.b.len);
	ak_buf_free(&w.b);
	return (ak_val){.p = s};
}

// csv.isStruct(desc): whether T is a struct, whose first row is a header.
static ak_val is_struct_fn(ak_val *args, uint32_t nargs) {
	AK_UNUSED(nargs);
	return (ak_val){.i = STR(0)->data[0] == 'S'};
}

const ak_lib_entry ak_std_csv[] = {
	{"csv.read", read_fn},
	{"csv.incomplete", incomplete_fn},
	{"csv.write", write_fn},
	{"csv.isStruct", is_struct_fn},
	{NULL, NULL},
};
