// literal.c: values as the language writes them (literal.h).
#include "vm/literal.h"
#include "rt/json.h"
#include "rt/unicode.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

ak_lit ak_lit_short(ak_buf *b, ak_module *mod) {
	return (ak_lit){.b = b, .limit = b->len + AK_LIT_MAX, .depth = AK_LIT_DEPTH, .items = AK_LIT_ITEMS, .str = AK_LIT_STR, .mod = mod};
}

ak_lit ak_lit_full(ak_buf *b, ak_module *mod) {
	return (ak_lit){.b = b, .limit = SIZE_MAX, .depth = INT_MAX, .items = UINT32_MAX, .str = SIZE_MAX, .mod = mod};
}

static void put(ak_lit *s, const char *p) { ak_buf_put(s->b, p, strlen(p)); }
static bool full(const ak_lit *s) { return s->b->len >= s->limit; }

ak_kind ak_lit_desc_kind(const char *d) {
	switch (*d) {
	case 'i':
	case 'e':
	case 'd':
	case 'c':
	case 'r':
	case 'n':
		return AK_K_INT;
	case 'f':
		return AK_K_FLOAT;
	case 'b':
		return AK_K_BOOL;
	default:
		return AK_K_REF;
	}
}

const char *ak_lit_desc_end(const char *d) {
	for (;;) {
		switch (*d) {
		case '?':
		case '[':
		case '{':
		case '<':
			d++;
			continue;
		case 'S':
		case 'E':
		case 'J':
		case 'e': {
			const char *q = strchr(d, ';');
			return q ? q + 1 : d + strlen(d);
		}
		case 'n':
			return d[1] ? d + 2 : d + 1;
		case 'm':
			return ak_lit_desc_end(ak_lit_desc_end(d + 1));
		case '\0':
			return d;
		default:
			return d + 1;
		}
	}
}

void ak_lit_type_name(ak_buf *b, const char *name, size_t n) {
	const char *seg = name, *end = name + n;
	for (const char *p = name;; p++) {
		char c = p < end ? *p : 0;
		if (c == '<' || c == '>' || c == ',' || c == '(' || c == ')' || c == '?' || c == '[' || c == ']' || c == ' ' || c == ':' || c == 0) {
			const char *dot = NULL;
			for (const char *q = seg; q < p; q++)
				if (*q == '.') dot = q;
			const char *from = dot ? dot + 1 : seg;
			ak_buf_put(b, from, (size_t)(p - from));
			if (!c) return;
			ak_buf_put(b, p, 1);
			seg = p + 1;
		}
	}
}

// A field's name is followed by its JSON descriptor, then by how the
// language writes it when that's other than JSON's ("wait\0i\0d": a
// duration), in a module's shapes (load.c's rname, sh->lits).
const char *ak_lit_field_desc(const ak_shape *sh, const char *name) {
	if (!name || !sh->lits) return "-";
	const char *d = name + strlen(name) + 1;
	const char *l = d + strlen(d) + 1;
	return *l ? l : *d ? d : "-";
}

void ak_lit_peel(const ak_lit *s, ak_val *v, ak_kind *k, const char **d) {
	for (int guard = 0; guard < 16; guard++) {
		if (**d == '?' && *k != AK_K_REF) {
			(*d)++;
			continue;
		}
		if (*k != AK_K_REF || !v->p) return;
		const ak_obj *o = v->p;
		if (o->tag == AK_TAG_CELL || o->tag == AK_TAG_MEMO || o->tag == AK_TAG_ACC) {
			ak_val pv;
			ak_kind pk;
			if (!s->peek || !ak_vm_dbg_peek(v->p, &pv, &pk)) return;
			*v = pv;
			*k = pk;
			continue;
		}
		if (**d == '?') {
			(*d)++;
			if (ak_lit_desc_kind(*d) != AK_K_REF && o->tag == AK_TAG_BOX) {
				*k = (ak_kind)o->aux;
				*v = ((const ak_box *)o)->v;
			}
			continue;
		}
		if (o->tag == AK_TAG_BOX && ak_lit_desc_kind(*d) == AK_K_REF && **d != 'J') {
			// An optional scalar whose type the descriptor doesn't say.
			*k = (ak_kind)o->aux;
			*v = ((const ak_box *)o)->v;
			continue;
		}
		return;
	}
}

static void show_float(ak_lit *s, double f) {
	if (f != f) {
		put(s, "nan");
		return;
	}
	if (isinf(f)) {
		put(s, f > 0 ? "inf" : "-inf");
		return;
	}
	ak_strb t = {0};
	ak_strb_float(&t, f);  // in its small buffer: no allocation
	const char *p = t.b.data ? (const char *)t.b.data : t.small;
	size_t n = ak_strb_len(&t);
	ak_buf_put(s->b, p, n);
	if (!memchr(p, '.', n) && !memchr(p, 'e', n)) put(s, ".0");  // a float says it's one
	ak_strb_free(&t);
}

static void show_duration(ak_lit *s, int64_t ns) {
	static const struct {
		int64_t n;
		const char *unit;
	} units[] = {{3600000000000LL, "h"}, {60000000000LL, "min"}, {1000000000LL, "s"}, {1000000LL, "ms"}, {1000LL, "us"}};
	if (ns == 0) {
		put(s, "0s");
		return;
	}
	for (size_t i = 0; i < sizeof units / sizeof *units; i++) {
		if (ns % units[i].n == 0) {
			ak_buf_printf(s->b, "%lld%s", (long long)(ns / units[i].n), units[i].unit);
			return;
		}
	}
	ak_buf_printf(s->b, "%lldns", (long long)ns);
}

static void show_int(ak_lit *s, int64_t i, const char *d) {
	switch (*d) {
	case 'd':
		show_duration(s, i);
		return;
	case 'c': {
		ak_builtin_fn hex = ak_builtin_find("color.hex");
		if (hex) {
			ak_val arg = {.i = i}, r = hex(&arg, 1);
			if (ak_error_pending()) ak_release(ak_error_take());
			if (r.p) {
				ak_buf_put(s->b, ((ak_str *)r.p)->data, ((ak_str *)r.p)->len);
				ak_release(r.p);
				return;
			}
		}
		break;
	}
	case 'r': {
		char u[4];
		size_t n = i >= 0 && i <= 0x10FFFF ? ak_utf8_encode((uint32_t)i, u) : 0;
		ak_buf_u8(s->b, '\'');
		if (i == '\n') put(s, "\\n");
		else if (i == '\t') put(s, "\\t");
		else if (i == '\r') put(s, "\\r");
		else if (i == '\'' || i == '\\') ak_buf_printf(s->b, "\\%c", (char)i);
		else ak_buf_put(s->b, u, n);
		ak_buf_u8(s->b, '\'');
		return;
	}
	case 'n':
		if (d[1] == '7') {
			ak_buf_printf(s->b, "%llu", (unsigned long long)i);
			return;
		}
		break;
	case 'e': {
		const char *q = strchr(d, ';');
		ak_json_types t;
		const char *const *cases;
		uint32_t n;
		if (s->mod && q) {
			ak_vm_module_json_types(s->mod, &t);
			if (t.cases(t.ctx, d + 1, (size_t)(q - d - 1), &cases, &n) && i >= 0 && (uint64_t)i < n) {
				ak_buf_printf(s->b, ".%s", cases[i]);
				return;
			}
		}
		break;
	}
	}
	ak_buf_printf(s->b, "%lld", (long long)i);
}

// show_str writes a string literal: its escapes, "{" as "\{" (a literal's
// interpolation), its first s->str bytes and "…".
static void show_str(ak_lit *s, const char *p, size_t n) {
	ak_buf_u8(s->b, '"');
	size_t shown = n > s->str ? s->str : n;
	while (shown < n && shown > 0 && ((unsigned char)p[shown] & 0xC0) == 0x80) shown--;  // a whole character
	size_t from = 0;
	for (size_t i = 0; i < shown; i++) {
		unsigned char c = (unsigned char)p[i];
		const char *esc = NULL;
		char u[12];
		switch (c) {
		case '"': esc = "\\\""; break;
		case '\\': esc = "\\\\"; break;
		case '{': esc = "\\{"; break;
		case '\n': esc = "\\n"; break;
		case '\t': esc = "\\t"; break;
		case '\r': esc = "\\r"; break;
		case 0: esc = "\\0"; break;
		default:
			if (c < 0x20 || c == 0x7f) {
				snprintf(u, sizeof u, "\\u{%x}", c);
				esc = u;
			}
		}
		if (!esc) continue;
		ak_buf_put(s->b, p + from, i - from);
		put(s, esc);
		from = i + 1;
	}
	ak_buf_put(s->b, p + from, shown - from);
	if (shown < n) put(s, "…");
	ak_buf_u8(s->b, '"');
}

void ak_lit_map_descs(const char *d, const char **kd, const char **vd) {
	*kd = *vd = "-";
	if (*d == '{') *kd = "s", *vd = d + 1;
	else if (*d == 'm') *kd = d + 1, *vd = ak_lit_desc_end(d + 1);
	else if (*d == '<') *kd = d + 1;
}

// show_case writes an enum's case: .name, or for an error its type's
// name and the case's (MailError.offline), or its type's alone for a
// struct's (ParseError, stored as an enum's one case named as the type).
static void show_case(ak_lit *s, const ak_shape *sh, const ak_case *cs, bool bare) {
	if (!sh->is_error) {
		if (!bare) put(s, ".");
		put(s, cs->name);
		return;
	}
	size_t at = s->b->len;
	ak_lit_type_name(s->b, sh->name, strlen(sh->name));
	const char *t = (const char *)s->b->data + at, *lt = memchr(t, '<', s->b->len - at);
	size_t tn = lt ? (size_t)(lt - t) : s->b->len - at;
	if (sh->ncases == 2 && strlen(cs->name) == tn && memcmp(cs->name, t, tn) == 0) return;
	put(s, ".");
	put(s, cs->name);
}

// The parts being written, outermost first: the writer keeps them on a
// stack of its own, not the C stack, as json.c's encoder does, so a value
// nests as deep as the app made it (a long linked list of a recursive
// enum) and writing it takes no C stack per level.
enum { P_ARR, P_MAP, P_STRUCT, P_CASE };
typedef struct {
	const char *d;   // the part's descriptor
	const void *o;   // the array, the map, the struct, the enum
	uint32_t i;      // its next item
	uint32_t shown;  // the items written
	uint8_t what;    // P_*
	bool val;        // a map's: item i's key is written, its value next
} part;

typedef struct {
	part *p;
	uint32_t n, cap;
	part small[16];
	AK_VEC(ak_val) held;  // what types' own toJSON gave, let go of at the end
} parts;

static void push(parts *ps, part p) {
	if (ps->n == ps->cap) {
		uint32_t cap = ps->cap * 2;
		part *np = ak_alloc(sizeof(part) * cap);
		memcpy(np, ps->p, sizeof(part) * ps->n);
		if (ps->p != ps->small) ak_free(ps->p);
		ps->p = np;
		ps->cap = cap;
	}
	ps->p[ps->n++] = p;
}

// own_value is what a value of shape sh says of itself (its description,
// written; or what its toJSON gives, to write in its place, held), when
// it says something: true.
static bool own_value(ak_lit *s, parts *ps, const ak_shape *sh, ak_val *v, ak_kind *k, const char **d) {
	if (!s->own || !(ak_shape_has(sh, AK_HOOK_SHOW) || ak_shape_has(sh, AK_HOOK_TOJSON))) return false;
	bool text = ak_shape_has(sh, AK_HOOK_SHOW);
	ak_val r = ak_shape_call(sh, text ? AK_HOOK_SHOW : AK_HOOK_TOJSON, v, 1);
	if (ak_error_pending()) {
		ak_release(ak_error_take());
		return false;
	}
	if (text) {
		if (r.p) ak_buf_put(s->b, ((const ak_str *)r.p)->data, ((const ak_str *)r.p)->len);
		ak_release(r.p);
		*k = AK_K_VOID;  // written
		return true;
	}
	*k = ak_lit_desc_kind(sh->jdesc);
	*d = sh->jdesc;
	*v = r;
	if (*k == AK_K_REF && r.p) ak_vec_push(&ps->held, r);
	return true;
}

// open_value writes v, or what opens it when it has parts (pushed, to write as
// write's loop goes on).
static void open_value(ak_lit *s, parts *ps, ak_val v, ak_kind k, const char *d, bool bare) {
	for (;;) {
		ak_lit_peel(s, &v, &k, &d);
		switch (k) {
		case AK_K_INT: show_int(s, v.i, d); return;
		case AK_K_FLOAT: show_float(s, v.f); return;
		case AK_K_BOOL: put(s, v.i ? "true" : "false"); return;
		case AK_K_VOID: put(s, "()"); return;
		default: break;
		}
		const ak_obj *o = v.p;
		if (!o) {
			put(s, "none");
			return;
		}
		if (ps->n > (uint32_t)s->depth && o->tag != AK_TAG_STR && o->tag != AK_TAG_BOX) {
			put(s, "…");
			return;
		}
		if (*d == 'J' && s->own && s->mod) {
			// A json.Value: as JSON.
			ak_json_types t;
			ak_vm_module_json_types(s->mod, &t);
			ak_str *j = ak_json_encode(v, d, &t, 0);
			ak_buf_put(s->b, j->data, j->len);
			ak_release(j);
			return;
		}
		switch (o->tag) {
		case AK_TAG_STR: {
			const ak_str *str = v.p;
			show_str(s, str->data, str->len);
			return;
		}
		case AK_TAG_BYTES:
			ak_buf_printf(s->b, "<%u bytes>", ((const ak_bytes *)o)->len);
			return;
		case AK_TAG_ARR:
			put(s, "[");
			push(ps, (part){.d = *d == '[' ? d : "-", .o = o, .what = P_ARR});
			return;
		case AK_TAG_MAP: {
			const ak_map *m = v.p;
			bool set = ak_map_is_set(m);
			if (!set && !m->live) {
				put(s, "[:]");
				return;
			}
			put(s, set ? "Set([" : "[");
			push(ps, (part){.d = d, .o = o, .what = P_MAP});
			return;
		}
		case AK_TAG_STRUCT: {
			const ak_struct *st = v.p;
			const ak_shape *sh = st->shape;
			if (own_value(s, ps, sh, &v, &k, &d)) {
				if (k == AK_K_VOID) return;
				continue;  // what its toJSON gave
			}
			if (sh == &ak_shape_error && st->f[0].p) {
				// Error("…"), as it's made.
				put(s, "Error(");
				const ak_str *m = st->f[0].p;
				show_str(s, m->data, m->len);
				put(s, ")");
				return;
			}
			if (sh->name[0] != '(') ak_lit_type_name(s->b, sh->name, strlen(sh->name));  // a tuple's has none
			put(s, "(");
			push(ps, (part){.d = d, .o = o, .what = P_STRUCT});
			return;
		}
		case AK_TAG_ENUM: {
			const ak_enum *e = v.p;
			const ak_shape *sh = e->shape;
			uint16_t c = ak_enum_case(e);
			if (c >= sh->ncases) {
				put(s, "<enum>");
				return;
			}
			if (own_value(s, ps, sh, &v, &k, &d)) {
				if (k == AK_K_VOID) return;
				continue;
			}
			const ak_case *cs = &sh->cases[c];
			show_case(s, sh, cs, bare);
			if (cs->nfields) {
				put(s, "(");
				push(ps, (part){.d = d, .o = o, .what = P_CASE});
			}
			return;
		}
		case AK_TAG_BOX:
			v = ((const ak_box *)o)->v;
			k = (ak_kind)o->aux;
			d = "-";
			continue;
		case AK_TAG_CLOSURE: {
			// The debugger names its function; text has none of it.
			const ak_closure *c = v.p;
			if (!s->peek) put(s, "<fn>");
			else if (c->invoke == ak_vm_closure_invoke) ak_buf_printf(s->b, "fn %s", ((const ak_func *)c->fn)->name->data);
			else put(s, "fn");
			return;
		}
		case AK_TAG_CELL:
		case AK_TAG_MEMO:
		case AK_TAG_ACC: put(s, "<not computed>"); return;
		case AK_TAG_TASK: put(s, "<task>"); return;
		case AK_TAG_VIEW: put(s, "<view>"); return;
		case AK_TAG_STORE: put(s, "<store>"); return;
		case AK_TAG_TREE: ak_buf_printf(s->b, "<%lld entries>", (long long)((const ak_tree *)o)->count); return;
		case AK_TAG_NATIVE: put(s, "<native object>"); return;
		default: put(s, "<object>"); return;
		}
	}
}

static const char *close_of(const part *p) {
	if (p->what == P_ARR) return "]";
	if (p->what == P_MAP) return ak_map_is_set(p->o) ? "])" : "]";
	return ")";
}

// next writes part p's next item, or what closes it when there's none
// (false: p is done).
static bool next(ak_lit *s, parts *ps, part *p) {
	ak_val v = {0};
	ak_kind k = AK_K_REF;
	const char *d = "-";
	if (p->what == P_MAP && p->val) {
		const ak_map *m = p->o;
		const char *kd, *vd;
		ak_lit_map_descs(p->d, &kd, &vd);
		put(s, ": ");
		v = m->vals[p->i];
		k = (ak_kind)(m->h.aux >> 4);
		p->val = false;
		p->i++;
		open_value(s, ps, v, k, vd, false);  // p moves if it grows the stack: not used after
		return true;
	}
	switch (p->what) {
	case P_ARR: {
		const ak_arr *a = p->o;
		if (p->i >= a->len) return false;
		v = ak_arr_get(a, p->i);
		k = (ak_kind)(ak_arr_kind(a) & 0xf);
		d = *p->d == '[' ? p->d + 1 : "-";
		break;
	}
	case P_MAP: {
		const ak_map *m = p->o;
		while (p->i < m->len && m->dead[p->i]) p->i++;
		if (p->i >= m->len) return false;
		const char *vd;
		ak_lit_map_descs(p->d, &d, &vd);
		v = m->keys[p->i];
		k = (ak_kind)(m->h.aux & 0xf);
		break;
	}
	default: {
		// A struct's fields, an enum case's payload: name: v, or v alone
		// where the name is a position ("0").
		const ak_shape *sh;
		const ak_val *f;
		const uint8_t *kinds;
		const char *const *names;
		uint32_t n;
		if (p->what == P_STRUCT) {
			const ak_struct *st = p->o;
			sh = st->shape, f = st->f, kinds = sh->kinds, names = sh->names, n = sh->nfields;
		} else {
			const ak_enum *e = p->o;
			const ak_case *cs = &e->shape->cases[ak_enum_case(e)];
			sh = e->shape, f = e->f, kinds = cs->kinds, names = cs->names, n = cs->nfields;
		}
		while (p->i < n && names && names[p->i] && names[p->i][0] == '$') p->i++;  // the lowering's own (a component's owner)
		if (p->i >= n) return false;
		const char *name = names ? names[p->i] : NULL;
		if (p->shown) put(s, ", ");
		if (full(s)) {
			put(s, "…");
			return false;
		}
		p->shown++;
		if (name && *name && !(name[0] >= '0' && name[0] <= '9')) ak_buf_printf(s->b, "%s: ", name);
		v = f[p->i];
		k = (ak_kind)kinds[p->i];
		d = ak_lit_field_desc(sh, name);
		p->i++;
		open_value(s, ps, v, k, d, false);
		return true;
	}
	}
	// An array's item, a map's key.
	if (p->shown) put(s, ", ");
	if (p->shown >= s->items || full(s)) {
		put(s, "…");
		return false;
	}
	p->shown++;
	if (p->what == P_MAP && !ak_map_is_set(p->o)) p->val = true;
	else p->i++;
	open_value(s, ps, v, k, d, false);
	return true;
}

void ak_lit_write(ak_lit *s, ak_val v, ak_kind k, const char *d) {
	parts ps = {.cap = 16};
	ps.p = ps.small;
	open_value(s, &ps, v, k, d, s->bare);
	while (ps.n) {
		part *p = &ps.p[ps.n - 1];
		if (next(s, &ps, p)) continue;
		put(s, close_of(&ps.p[ps.n - 1]));
		ps.n--;
	}
	if (ps.p != ps.small) ak_free(ps.p);
	for (uint32_t i = 0; i < ps.held.len; i++) ak_release(ps.held.data[i].p);
	ak_vec_free(&ps.held);
}

void ak_lit_cut(ak_buf *b, size_t start) {
	if (b->len - start <= AK_LIT_MAX) return;
	size_t cut = start + AK_LIT_MAX;
	while (cut > start && (b->data[cut] & 0xC0) == 0x80) cut--;
	b->len = cut;
	ak_buf_put(b, "…", strlen("…"));
}

// ---- Builtins ----

// show.text(value, desc): value as interpolation and print write it
// when it's not a number, a bool or a string (lang/ir's describe): an
// enum's case without its dot, the rest as the language writes it, all
// of it.
static ak_val show_text(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	const ak_str *desc = args[1].p;
	ak_buf b = {0};
	ak_lit s = ak_lit_full(&b, ak_vm_running_module());
	s.own = true;
	s.bare = true;
	ak_lit_write(&s, args[0], ak_lit_desc_kind(desc->data), desc->data);
	return (ak_val){.p = ak_str_of_buf(&b)};
}

// error.text(e): an error's message when its type says none (no message
// field, no message method): the error as the language writes it, its
// type's name, its case's and its payload, cut short as the debugger
// cuts a value (MailError.server(code: 500), ParseError(line: 3)).
static ak_val error_text(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_buf b = {0};
	ak_lit s = ak_lit_short(&b, ak_vm_running_module());
	s.own = true;
	ak_lit_write(&s, args[0], AK_K_REF, "-");
	ak_lit_cut(&b, 0);
	return (ak_val){.p = ak_str_of_buf(&b)};
}

void ak_vm_literal_init(void) {
	ak_builtin_register("show.text", show_text);
	ak_builtin_register("error.text", error_text);
}
