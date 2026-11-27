#include "vm/keep.h"
#include "vm/vm_int.h"

// ---- Encoding ----
//
// value := tag, then by tag:
//   'i' i64 | 'f' f64 | 'b' u8 | 'n' (none)
//   's' str | 'y' bytes
//   'B' kind value                      a boxed optional scalar
//   'a' kind flags str count value*     an array (elements of kind; str:
//                                       their shape's name, or "")
//   'm' kind kind flags str count (value value)*
//                                       a map (str: its values' shape's)
//   'S' str count (str kind value)*     a struct: its shape's name, fields by name
//   'E' str str count (kind value)*     an enum: its shape's and its case's names
//   'T' mode kind kind str count (value value?)*
//                                       a B-tree (rt.h's ak_tree): its keys'
//                                       shape's name, when it has one ("": none),
//                                       its entries in order, values but a set's
// str := u32 length, bytes

static void wstr(ak_buf *b, const char *s) { ak_buf_str(b, s ? s : "", s ? strlen(s) : 0); }

bool ak_keep_encode(ak_buf *b, ak_val v, ak_kind k) {
	switch (k) {
	case AK_K_INT:
		ak_buf_u8(b, 'i');
		ak_buf_u64(b, (uint64_t)v.i);
		return true;
	case AK_K_BOOL:
		ak_buf_u8(b, 'b');
		ak_buf_u8(b, v.i != 0);
		return true;
	case AK_K_FLOAT:
		ak_buf_u8(b, 'f');
		ak_buf_f64(b, v.f);
		return true;
	case AK_K_REF:
		break;
	default:
		ak_buf_u8(b, 'n');
		return true;
	}
	ak_obj *o = v.p;
	if (!o) {
		ak_buf_u8(b, 'n');
		return true;
	}
	switch ((ak_tag)o->tag) {
	case AK_TAG_STR: {
		ak_str *s = (ak_str *)o;
		ak_buf_u8(b, 's');
		ak_buf_str(b, s->data, s->len);
		return true;
	}
	case AK_TAG_BYTES: {
		ak_bytes *s = (ak_bytes *)o;
		ak_buf_u8(b, 'y');
		ak_buf_str(b, (const char *)s->data, s->len);
		return true;
	}
	case AK_TAG_BOX: {
		ak_box *x = (ak_box *)o;
		ak_buf_u8(b, 'B');
		ak_buf_u8(b, (uint8_t)x->h.aux);
		return ak_keep_encode(b, x->v, (ak_kind)x->h.aux);
	}
	case AK_TAG_ARR: {
		ak_arr *a = (ak_arr *)o;
		ak_buf_u8(b, 'a');
		ak_buf_u8(b, (uint8_t)AK_ARR_KIND(ak_arr_kind(a), a->elem));  // its storage too: a packed [u8] comes back packed
		ak_buf_u8(b, (uint8_t)a->h.flags);
		wstr(b, a->shape ? a->shape->name : NULL);
		ak_buf_u32(b, a->len);
		for (uint32_t i = 0; i < a->len; i++)
			if (!ak_keep_encode(b, ak_arr_get(a, i), ak_arr_kind(a))) return false;
		return true;
	}
	case AK_TAG_MAP: {
		ak_map *m = (ak_map *)o;
		ak_kind kk = (ak_kind)(m->h.aux & 0xf), vk = (ak_kind)(m->h.aux >> 4);
		ak_buf_u8(b, 'm');
		ak_buf_u8(b, (uint8_t)kk);
		ak_buf_u8(b, (uint8_t)vk);
		ak_buf_u8(b, (uint8_t)m->h.flags);
		wstr(b, m->vshape ? m->vshape->name : NULL);
		ak_buf_u32(b, m->live);
		for (uint32_t i = 0; i < m->len; i++) {
			if (m->dead[i]) continue;
			if (!ak_keep_encode(b, m->keys[i], kk) || !ak_keep_encode(b, m->vals[i], vk)) return false;
		}
		return true;
	}
	case AK_TAG_STRUCT: {
		ak_struct *s = (ak_struct *)o;
		ak_buf_u8(b, 'S');
		wstr(b, s->shape->name);
		ak_buf_u32(b, s->shape->nfields);
		for (uint16_t i = 0; i < s->shape->nfields; i++) {
			wstr(b, s->shape->names ? s->shape->names[i] : "");
			ak_buf_u8(b, s->shape->kinds[i]);
			if (!ak_keep_encode(b, s->f[i], (ak_kind)s->shape->kinds[i])) return false;
		}
		return true;
	}
	case AK_TAG_ENUM: {
		ak_enum *e = (ak_enum *)o;
		const ak_case *c = &e->shape->cases[ak_enum_case(e)];
		ak_buf_u8(b, 'E');
		wstr(b, e->shape->name);
		wstr(b, c->name);
		ak_buf_u32(b, c->nfields);
		for (uint16_t i = 0; i < c->nfields; i++) {
			ak_buf_u8(b, c->kinds[i]);
			if (!ak_keep_encode(b, e->f[i], (ak_kind)c->kinds[i])) return false;
		}
		return true;
	}
	case AK_TAG_TREE: {
		ak_tree *t = (ak_tree *)o;
		ak_buf_u8(b, 'T');
		ak_buf_u8(b, t->mode);
		ak_buf_u8(b, t->kk);
		ak_buf_u8(b, t->vk);
		wstr(b, t->kshape ? t->kshape->name : NULL);
		ak_buf_u64(b, (uint64_t)t->count);
		ak_titer it;
		ak_titer_start(&it, t);
		const ak_tnode *n;
		uint32_t i;
		while (ak_titer_next(&it, &n, &i)) {
			if (!ak_keep_encode(b, ak_tnode_keys(n)[i], (ak_kind)t->kk)) return false;
			if (!ak_tree_is_set(t) && !ak_keep_encode(b, ak_tnode_vals(n)[i], (ak_kind)t->vk)) return false;
		}
		return true;
	}
	default:
		return false;  // closures, cells, views, tasks: not values
	}
}

static bool rname(ak_reader *r, const uint8_t **p, uint32_t *n) {
	*p = ak_read_bytes(r, n);
	return !r->err;
}

static bool name_is(const uint8_t *p, uint32_t n, const char *s) { return s && strlen(s) == n && memcmp(p, s, n) == 0; }

// shape_named reads a shape's name ("" for none) and finds it.
static const ak_shape *shape_named(ak_reader *r, ak_keep_shape_fn find, void *ctx) {
	uint32_t n;
	const uint8_t *p = ak_read_bytes(r, &n);
	if (r->err || n == 0 || n >= 256 || !find) return NULL;
	char name[256];
	memcpy(name, p, n);
	name[n] = 0;
	return find(ctx, name);
}

// skip reads past one encoded value.
static bool skip(ak_reader *r) {
	uint8_t tag = ak_read_u8(r);
	uint32_t n;
	switch (tag) {
	case 'i':
	case 'f':
		ak_read_u64(r);
		break;
	case 'b':
		ak_read_u8(r);
		break;
	case 'n':
		break;
	case 's':
	case 'y':
		ak_read_bytes(r, &n);
		break;
	case 'B':
		ak_read_u8(r);
		return skip(r);
	case 'a':
		ak_read_u8(r);
		ak_read_u8(r);
		ak_read_bytes(r, &n);
		n = ak_read_u32(r);
		for (uint32_t i = 0; i < n && !r->err; i++)
			if (!skip(r)) return false;
		break;
	case 'm':
		ak_read_u8(r);
		ak_read_u8(r);
		ak_read_u8(r);
		ak_read_bytes(r, &n);
		n = ak_read_u32(r);
		for (uint32_t i = 0; i < n && !r->err; i++)
			if (!skip(r) || !skip(r)) return false;
		break;
	case 'S':
		ak_read_bytes(r, &n);
		n = ak_read_u32(r);
		for (uint32_t i = 0; i < n && !r->err; i++) {
			uint32_t m;
			ak_read_bytes(r, &m);
			ak_read_u8(r);
			if (!skip(r)) return false;
		}
		break;
	case 'E':
		ak_read_bytes(r, &n);
		ak_read_bytes(r, &n);
		n = ak_read_u32(r);
		for (uint32_t i = 0; i < n && !r->err; i++) {
			ak_read_u8(r);
			if (!skip(r)) return false;
		}
		break;
	case 'T': {
		ak_read_u8(r);
		ak_read_u8(r);
		bool set = ak_read_u8(r) == AK_K_VOID;
		ak_read_bytes(r, &n);
		uint64_t c = ak_read_u64(r);
		for (uint64_t i = 0; i < c && !r->err; i++)
			if (!skip(r) || (!set && !skip(r))) return false;
		break;
	}
	default:
		return false;
	}
	return !r->err;
}

static ak_val zero_field(ak_kind k, bool *ok) {
	// A new field: scalars start at zero; a reference has no zero to
	// start from unless it's optional, which the shape doesn't say, so
	// the conversion fails.
	if (k == AK_K_REF) *ok = false;
	return (ak_val){0};
}

bool ak_keep_decode(ak_reader *r, ak_kind k, ak_keep_shape_fn find, void *ctx, ak_val *out) {
	*out = (ak_val){0};
	uint8_t tag = ak_read_u8(r);
	if (r->err) return false;
	switch (tag) {
	case 'i':
		out->i = (int64_t)ak_read_u64(r);
		return !r->err && k == AK_K_INT;
	case 'b':
		out->i = ak_read_u8(r);
		return !r->err && k == AK_K_BOOL;
	case 'f':
		out->f = ak_read_f64(r);
		return !r->err && k == AK_K_FLOAT;
	case 'n':
		return k == AK_K_REF;
	}
	if (k != AK_K_REF) {
		// The type was a reference and is a scalar now: read past it.
		r->off--;
		skip(r);
		return false;
	}
	switch (tag) {
	case 's':
	case 'y': {
		uint32_t n;
		const uint8_t *p = ak_read_bytes(r, &n);
		if (r->err) return false;
		out->p = tag == 's' ? (void *)ak_str_new((const char *)p, n) : (void *)ak_bytes_new(p, n);
		return true;
	}
	case 'B': {
		ak_kind bk = (ak_kind)ak_read_u8(r);
		ak_val v;
		if (!ak_keep_decode(r, bk, find, ctx, &v)) return false;
		out->p = ak_box_new(bk, v);
		return true;
	}
	case 'a': {
		ak_kind ak = (ak_kind)ak_read_u8(r), ek = (ak_kind)(ak & 0xf);  // AK_ARR_KIND's
		uint8_t flags = ak_read_u8(r);
		const ak_shape *es = shape_named(r, find, ctx);
		uint32_t n = ak_read_u32(r);
		if (r->err || n > r->len) return false;
		ak_arr *a = ak_arr_new(ak, es, n);
		a->h.flags = flags;
		bool ok = true;
		for (uint32_t i = 0; i < n; i++) {
			ak_val v;
			if (!ak_keep_decode(r, ek, find, ctx, &v)) {
				ok = false;
				break;
			}
			ak_arr_put(a, a->len++, v);
			if (ek == AK_K_REF && v.p) {
				ak_obj *eo = v.p;
				if (eo->tag == AK_TAG_STR) a->h.flags = 1;
				if (!a->shape && eo->tag == AK_TAG_STRUCT) a->shape = ((ak_struct *)eo)->shape;
				if (!a->shape && eo->tag == AK_TAG_ENUM) a->shape = ((ak_enum *)eo)->shape;
			}
		}
		if (!ok) {
			ak_release(a);
			return false;
		}
		out->p = a;
		return true;
	}
	case 'm': {
		ak_kind kk = (ak_kind)ak_read_u8(r), vk = (ak_kind)ak_read_u8(r);
		uint8_t flags = ak_read_u8(r);
		const ak_shape *vs = shape_named(r, find, ctx);
		uint32_t n = ak_read_u32(r);
		if (r->err || n > r->len) return false;
		ak_map *m = ak_map_new(kk, flags & AK_MAP_STRKEYS, vk, vs);
		m->h.flags = flags & (AK_MAP_STRKEYS | AK_MAP_SET);
		for (uint32_t i = 0; i < n; i++) {
			ak_val key, val;
			if (!ak_keep_decode(r, kk, find, ctx, &key)) goto map_fail;
			if (!ak_keep_decode(r, vk, find, ctx, &val)) {
				ak_release_val(key, kk);
				goto map_fail;
			}
			ak_map_set(&m, key, val);
		}
		out->p = m;
		return true;
	map_fail:
		ak_release(m);
		return false;
	}
	case 'T': {
		// Read back in its order, which may be a closure's the tree
		// doesn't keep: appended, not compared.
		ak_tree_mode mode = (ak_tree_mode)ak_read_u8(r);
		ak_kind kk = (ak_kind)ak_read_u8(r), vk = (ak_kind)ak_read_u8(r);
		uint32_t named;
		const uint8_t *nm = ak_read_bytes(r, &named);
		const ak_shape *ks = NULL;  // the new program's
		if (!r->err && named) {
			char name[256];
			if (named >= sizeof name || !find) return false;
			memcpy(name, nm, named);
			name[named] = 0;
			if (!(ks = find(ctx, name))) return false;
		}
		uint64_t c = ak_read_u64(r);
		if (r->err || c > r->len || mode > AK_TM_CMP || kk > AK_K_REF || vk > AK_K_REF) return false;
		ak_tree *t = ak_tree_new(mode, kk, vk);
		t->kshape = ks;
		for (uint64_t i = 0; i < c; i++) {
			ak_val key, val = {0};
			if (!ak_keep_decode(r, kk, find, ctx, &key)) goto tree_fail;
			if (vk != AK_K_VOID && !ak_keep_decode(r, vk, find, ctx, &val)) {
				ak_release_val(key, kk);
				goto tree_fail;
			}
			ak_tree_append(t, key, val);
		}
		out->p = t;
		return true;
	tree_fail:
		ak_release(t);
		return false;
	}
	case 'S': {
		const uint8_t *sn;
		uint32_t snl;
		if (!rname(r, &sn, &snl)) return false;
		char name[256];
		if (snl >= sizeof name) return false;
		memcpy(name, sn, snl);
		name[snl] = 0;
		const ak_shape *s = find ? find(ctx, name) : NULL;
		uint32_t nf = ak_read_u32(r);
		if (r->err) return false;
		ak_struct *o = s ? ak_struct_new(s) : NULL;
		bool *seen = NULL;
		if (o && s->nfields) seen = ak_alloc(s->nfields);
		bool ok = o != NULL && s->ncases == 0;
		for (uint32_t i = 0; i < nf; i++) {
			const uint8_t *fn;
			uint32_t fnl;
			if (!rname(r, &fn, &fnl)) {
				ok = false;
				break;
			}
			ak_kind fk = (ak_kind)ak_read_u8(r);
			int idx = -1;
			if (ok)
				for (uint16_t j = 0; j < s->nfields; j++)
					if (s->names && name_is(fn, fnl, s->names[j])) idx = j;
			if (idx >= 0 && s->kinds[idx] == fk) {
				ak_val v;
				if (!ak_keep_decode(r, fk, find, ctx, &v)) {
					ok = false;
					break;
				}
				o->f[idx] = v;
				seen[idx] = true;
			} else {
				// A field removed, or now another kind: read past it.
				if (!skip(r)) {
					ok = false;
					break;
				}
				if (idx >= 0) ok = false;  // changed kind: start again
			}
		}
		if (ok)
			for (uint16_t j = 0; j < s->nfields; j++)
				if (!seen[j]) o->f[j] = zero_field((ak_kind)s->kinds[j], &ok);
		ak_free(seen);
		if (!ok) {
			ak_release(o);
			return false;
		}
		out->p = o;
		return true;
	}
	case 'E': {
		const uint8_t *sn, *cn;
		uint32_t snl, cnl;
		if (!rname(r, &sn, &snl) || !rname(r, &cn, &cnl)) return false;
		char name[256];
		if (snl >= sizeof name) return false;
		memcpy(name, sn, snl);
		name[snl] = 0;
		const ak_shape *s = find ? find(ctx, name) : NULL;
		uint32_t nf = ak_read_u32(r);
		if (r->err) return false;
		int ci = -1;
		if (s)
			for (uint16_t c = 0; c < s->ncases; c++)
				if (name_is(cn, cnl, s->cases[c].name)) ci = c;
		bool ok = ci >= 0 && s->cases[ci].nfields == nf;
		ak_val fields[32];
		uint32_t got = 0;
		if (nf > 32) ok = false;
		for (uint32_t i = 0; i < nf; i++) {
			ak_kind fk = (ak_kind)ak_read_u8(r);
			if (ok && s->cases[ci].kinds[i] == fk) {
				if (!ak_keep_decode(r, fk, find, ctx, &fields[got])) {
					ok = false;
					break;
				}
				got++;
			} else {
				ok = false;
				if (!skip(r)) break;
			}
		}
		if (!ok) {
			for (uint32_t i = 0; i < got; i++) ak_release_val(fields[i], (ak_kind)s->cases[ci].kinds[i]);
			return false;
		}
		ak_enum *e = ak_enum_new(s, (uint16_t)ci);
		for (uint32_t i = 0; i < nf; i++) e->f[i] = fields[i];
		out->p = e;
		return true;
	}
	}
	return false;
}

// ---- The dev loop's registry ----

typedef struct {
	ak_str *key;
	void *cell;     // live, weak; NULL once gone
	ak_buf saved;   // its value, saved before a reload
	uint8_t kind;
	bool has;
} entry;

static AK_VEC(entry) entries;
static bool on;
static ak_keep_shape_fn find_shape;
static void *find_ctx;
static uint32_t nsaved, nrestored;

extern bool (*ak_vm_keep_find)(ak_str *key, ak_kind k, ak_val *out);
extern void (*ak_vm_keep_add)(ak_str *key, void *cell);
ak_kind ak_vm_cell_kind(void *cell);

static entry *lookup(ak_str *key) {
	for (uint32_t i = 0; i < entries.len; i++)
		if (ak_str_eq(entries.data[i].key, key)) return &entries.data[i];
	return NULL;
}

ak_str *(*ak_keep_qualify)(ak_str *key);

static bool keep_find(ak_str *key, ak_kind k, ak_val *out) {
	ak_str *q = ak_keep_qualify ? ak_keep_qualify(key) : ak_retain(key);
	entry *e = lookup(q);
	ak_release(q);
	if (!e || !e->has) return false;
	ak_reader r = {e->saved.data, e->saved.len, 0, false};
	bool ok = ak_keep_decode(&r, k, find_shape, find_ctx, out);
	// Each saved value is used once: the next cell with the key starts
	// fresh (two instances of a component in one place).
	e->has = false;
	if (ok) nrestored++;
	return ok;
}

static void keep_add(ak_str *key, void *cell) {
	ak_str *q = ak_keep_qualify ? ak_keep_qualify(key) : ak_retain(key);
	entry *e = lookup(q);
	if (!e) {
		ak_vec_push(&entries, ((entry){.key = q}));
		e = &entries.data[entries.len - 1];
	} else {
		ak_release(q);
	}
	e->cell = cell;
	((ak_obj *)cell)->flags |= 1;  // keyed: forget it when it goes
}

void ak_keep_forget(void *cell) {
	for (uint32_t i = 0; i < entries.len; i++)
		if (entries.data[i].cell == cell) entries.data[i].cell = NULL;
}

void ak_keep_start(void) {
	on = true;
	ak_vm_keep_find = keep_find;
	ak_vm_keep_add = keep_add;
}

static bool holding;  // ak_keep_hold's

void ak_keep_hold(bool hold) { holding = hold; }

void ak_keep_save(void) {
	nsaved = 0;
	uint32_t n = 0;
	for (uint32_t i = 0; i < entries.len; i++) {
		entry *e = &entries.data[i];
		if (!e->cell && holding) {
			// Held (a preview showed): the app's, for when it's back.
			entries.data[n++] = *e;
			continue;
		}
		if (!e->cell) {
			// No live cell: what it saved at an earlier reload and no cell
			// took since goes (a state whose initializer was edited, then
			// edited back, starts from it, not from a value two reloads
			// old), and so does the entry.
			ak_release(e->key);
			ak_buf_free(&e->saved);
			continue;
		}
		entries.data[n++] = *e;
		e = &entries.data[n - 1];
		ak_buf_reset(&e->saved);
		ak_kind k = ak_vm_cell_kind(e->cell);
		ak_val v = ak_vm_cell_get(e->cell, false);
		if (ak_error_pending()) {
			// A follow whose initializer failed: nothing to keep.
			ak_release(ak_error_take());
			e->has = false;
		} else {
			e->has = ak_keep_encode(&e->saved, v, k);
			ak_release_val(v, k);
		}
		e->kind = (uint8_t)k;
		e->cell = NULL;
		if (e->has) nsaved++;
	}
	entries.len = n;
}

void ak_keep_discard(void) {
	nsaved = 0;
	for (uint32_t i = 0; i < entries.len; i++) {
		entry *e = &entries.data[i];
		ak_buf_reset(&e->saved);
		e->has = false;
		e->cell = NULL;
	}
}

void ak_keep_use(ak_keep_shape_fn find, void *ctx) {
	find_shape = find;
	find_ctx = ctx;
	nrestored = 0;
}

void ak_keep_stop(void) {
	on = false;
	ak_vm_keep_find = NULL;
	ak_vm_keep_add = NULL;
	for (uint32_t i = 0; i < entries.len; i++) {
		ak_release(entries.data[i].key);
		ak_buf_free(&entries.data[i].saved);
	}
	ak_vec_free(&entries);
}

uint32_t ak_keep_saved(void) { return nsaved; }
uint32_t ak_keep_restored(void) { return nrestored; }
