// path.c: the nodes of the paths readers follow below a state, and what
// a write tells them (path.h, rfcs/0013).
#include "vm/path.h"

#include <string.h>

// A node's kind: the step from its parent.
enum { PN_ROOT, PN_FIELD, PN_INDEX, PN_KEY, PN_LEN, PN_KEYS, PN_COL };

// One node per followed path below a state. Its signal's readers are the
// path's; its children are the steps below it, by kind: fields and
// indexes in an array by number (a node's value is a struct or an
// array, never both), map keys and set members in a hash, the length in
// a slot of its own, and the keys a for follows (one per field path) in
// a list, as are the columns: a field path of every element, what a
// loop over the array follows (rfcs/0013's collection methods, a key's
// function): one node for all the elements, not one per element. The
// root is the state's container: its signal is unused (the
// state's own is the root's), and it goes with its last child.
struct ak_pnode {
	ak_wsignal sig;  // first: its readers
	ak_pnode *parent;
	ak_pnode **home;  // the root's: where the state keeps it
	uint8_t kind;
	uint8_t kk;     // KEY: the key's kind
	uint16_t nf;    // KEYS: the field path's length
	uint32_t nkids;
	int64_t i;      // FIELD, INDEX
	ak_val key;     // KEY: retained
	uint64_t hash;  // KEY
	uint16_t *fields;  // KEYS: the field path; and the keys as the for saw them last
	ak_val *cache;
	uint32_t ncache;
	uint8_t ck;        // their kind
	ak_pnode *fkids;   // FIELD children: a list through next (a struct's followed fields are few)
	ak_pnode **num;    // INDEX children, by index
	uint32_t ncap;
	ak_pnode **keyed;  // KEY children: buckets, chained through next
	uint32_t kcap, klive;
	ak_pnode *len, *keys;
	ak_pnode *next;  // a FIELD's, a KEYS' or a column's in its parent's list, a KEY's in its bucket
};

// ---- Values ----

// pv is a value at a path: none where the path doesn't reach (a missing
// key, an index out of range, a none optional).
typedef struct {
	ak_val v;
	ak_kind k;
	bool none;
} pv;

static const pv pv_none = {.none = true};

static inline bool is_tag(ak_val v, ak_tag t) { return v.p && ((ak_obj *)v.p)->tag == t; }

static pv field_of(pv x, int64_t f) {
	if (x.none || x.k != AK_K_REF || !is_tag(x.v, AK_TAG_STRUCT)) return pv_none;
	ak_struct *s = x.v.p;
	if (f >= s->shape->nfields) return pv_none;
	return (pv){s->f[f], (ak_kind)s->shape->kinds[f], false};
}

static pv index_of(pv x, int64_t i) {
	if (x.none || x.k != AK_K_REF || !is_tag(x.v, AK_TAG_ARR)) return pv_none;
	ak_arr *a = x.v.p;
	if (i < 0 || i >= a->len) return pv_none;
	return (pv){a->data[i], ak_arr_kind(a), false};
}

// key_of is a map's value at key, or a set's member's presence.
static pv key_of(pv x, ak_val key) {
	if (x.none || x.k != AK_K_REF || !is_tag(x.v, AK_TAG_MAP)) return pv_none;
	ak_map *m = x.v.p;
	ak_val v;
	bool has = ak_map_get(m, key, &v);
	if (ak_map_is_set(m)) return (pv){{.i = has}, AK_K_BOOL, false};
	if (!has) return pv_none;
	return (pv){v, (ak_kind)(m->h.aux >> 4), false};
}

static pv len_of(pv x) {
	if (x.none || x.k != AK_K_REF || !x.v.p) return pv_none;
	ak_obj *o = x.v.p;
	if (o->tag == AK_TAG_ARR) return (pv){{.i = ((ak_arr *)o)->len}, AK_K_INT, false};
	if (o->tag == AK_TAG_MAP) return (pv){{.i = ((ak_map *)o)->live}, AK_K_INT, false};
	return pv_none;
}

// child_val is the value at c given its parent's, x.
static pv child_val(pv x, const ak_pnode *c) {
	switch (c->kind) {
	case PN_FIELD: return field_of(x, c->i);
	case PN_INDEX: return index_of(x, c->i);
	case PN_KEY: return key_of(x, c->key);
	case PN_LEN: return len_of(x);
	}
	return x;  // KEYS: the array's
}

// same reports two values that are the same: scalars equal, references
// the same object (or both none). Nothing under them can differ.
static inline bool same(pv a, pv b) {
	if (a.none || b.none) return a.none == b.none;
	if (a.k != b.k) return false;
	if (a.k == AK_K_REF) return a.v.p == b.v.p;
	return ak_val_eq(a.v, b.v, a.k);
}

static inline bool pv_eq(pv a, pv b) {
	if (a.none || b.none) return a.none == b.none;
	return a.k == b.k && ak_val_eq(a.v, b.v, a.k);
}

// item_key is an item's key: its value at the field path fields.
static pv item_key(pv item, const uint16_t *fields, uint16_t nf) {
	for (uint16_t j = 0; j < nf; j++) item = field_of(item, fields[j]);
	return item;
}

static void cache_clear(ak_pnode *n) {
	if (n->ck == AK_K_REF)
		for (uint32_t i = 0; i < n->ncache; i++) ak_release(n->cache[i].p);
	n->ncache = 0;
}

// keys_changed reports whether the keys of arr, from lo to hi (hi -1:
// to the end), differ from those the for following kc saw last, or its
// length does.
static bool keys_changed(const ak_pnode *kc, pv arr, int64_t lo, int64_t hi) {
	pv len = len_of(arr);
	int64_t n = len.none ? 0 : len.v.i;
	if (n != kc->ncache) return true;
	if (hi < 0 || hi > n) hi = n;
	for (int64_t i = lo < 0 ? 0 : lo; i < hi; i++) {
		pv k = item_key(index_of(arr, i), kc->fields, kc->nf);
		if (k.none || k.k != (ak_kind)kc->ck || !ak_val_eq(k.v, kc->cache[i], k.k)) return true;
	}
	return false;
}

// cols_differ reports whether two arrays' values at a column's field
// path differ: in number, or one by one.
static bool cols_differ(const ak_pnode *c, pv a, pv b) {
	pv la = len_of(a), lb = len_of(b);
	if (!pv_eq(la, lb)) return true;
	if (la.none) return false;
	for (int64_t i = 0; i < la.v.i; i++)
		if (!pv_eq(item_key(index_of(a, i), c->fields, c->nf), item_key(index_of(b, i), c->fields, c->nf))) return true;
	return false;
}

// keys_seen refreshes kc's keys: arr's, as its for sees them now.
static void keys_seen(ak_pnode *kc, pv arr) {
	cache_clear(kc);
	pv len = len_of(arr);
	uint32_t n = len.none ? 0 : (uint32_t)len.v.i;
	if (n == 0) return;
	kc->cache = ak_realloc(kc->cache, n * sizeof *kc->cache);
	for (uint32_t i = 0; i < n; i++) {
		pv k = item_key(index_of(arr, i), kc->fields, kc->nf);
		if (k.none) {
			// (a field path to none: the IR spells none of those)
			kc->cache[i] = (ak_val){0};
			k.k = (ak_kind)kc->ck;
		} else {
			kc->ck = (uint8_t)k.k;
			ak_retain_val(k.v, k.k);
			kc->cache[i] = k.v;
		}
		kc->ncache = i + 1;
	}
}

// ---- Nodes ----

static void unwatched(ak_wsignal *s);

static ak_pnode *node_new(ak_pnode *parent, uint8_t kind) {
	ak_pnode *n = ak_alloc(sizeof *n);
	memset(n, 0, sizeof *n);
	ak_wsignal_init(&n->sig, unwatched);
	ak_signal_born(&n->sig.node);
	n->parent = parent;
	n->kind = kind;
	if (parent) parent->nkids++;
	return n;
}

static void node_free(ak_pnode *n) {
	if (n->kind == PN_KEY) ak_release_val(n->key, (ak_kind)n->kk);
	cache_clear(n);
	ak_free(n->cache);
	ak_free(n->fields);
	ak_free(n->num);
	ak_free(n->keyed);
	ak_free(n);
}

static ak_pnode *kid_field(ak_pnode *n, int64_t f, bool make) {
	for (ak_pnode *c = n->fkids; c; c = c->next)
		if (c->i == f) return c;
	if (!make) return NULL;
	ak_pnode *c = node_new(n, PN_FIELD);
	c->i = f;
	c->next = n->fkids;
	n->fkids = c;
	return c;
}

static ak_pnode *kid_num(ak_pnode *n, uint8_t kind, int64_t i, bool make) {
	if (kind == PN_FIELD) return kid_field(n, i, make);
	if (i < n->ncap && n->num[i]) return n->num[i];
	if (!make) return NULL;
	if (i >= n->ncap) {
		uint32_t cap = n->ncap ? n->ncap : 4;
		while (cap <= i) cap *= 2;
		n->num = ak_realloc(n->num, cap * sizeof *n->num);
		memset(n->num + n->ncap, 0, (cap - n->ncap) * sizeof *n->num);
		n->ncap = cap;
	}
	ak_pnode *c = node_new(n, kind);
	c->i = i;
	n->num[i] = c;
	return c;
}

static void keyed_grow(ak_pnode *n) {
	uint32_t cap = n->kcap ? n->kcap * 2 : 8;
	ak_pnode **b = ak_alloc(cap * sizeof *b);
	memset(b, 0, cap * sizeof *b);
	for (uint32_t j = 0; j < n->kcap; j++)
		for (ak_pnode *c = n->keyed[j], *next; c; c = next) {
			next = c->next;
			c->next = b[c->hash & (cap - 1)];
			b[c->hash & (cap - 1)] = c;
		}
	ak_free(n->keyed);
	n->keyed = b;
	n->kcap = cap;
}

static ak_pnode *kid_key(ak_pnode *n, ak_val key, ak_kind kk, bool make) {
	uint64_t h = ak_val_hash(key, kk);
	if (n->kcap)
		for (ak_pnode *c = n->keyed[h & (n->kcap - 1)]; c; c = c->next)
			if (c->hash == h && c->kk == kk && ak_val_eq(c->key, key, kk)) return c;
	if (!make) return NULL;
	if (n->klive + 1 > n->kcap) keyed_grow(n);
	ak_pnode *c = node_new(n, PN_KEY);
	ak_retain_val(key, kk);
	c->key = key;
	c->kk = (uint8_t)kk;
	c->hash = h;
	c->next = n->keyed[h & (n->kcap - 1)];
	n->keyed[h & (n->kcap - 1)] = c;
	n->klive++;
	return c;
}

static ak_pnode *kid_len(ak_pnode *n, bool make) {
	if (!n->len && make) n->len = node_new(n, PN_LEN);
	return n->len;
}

static ak_pnode *kid_keys(ak_pnode *n, uint8_t kind, const uint16_t *fields, uint16_t nf) {
	for (ak_pnode *c = n->keys; c; c = c->next)
		if (c->kind == kind && c->nf == nf && memcmp(c->fields, fields, nf * sizeof *fields) == 0) return c;
	ak_pnode *c = node_new(n, kind);
	c->nf = nf;
	c->fields = ak_alloc((nf ? nf : 1) * sizeof *fields);
	memcpy(c->fields, fields, nf * sizeof *fields);
	c->next = n->keys;
	n->keys = c;
	return c;
}

// detach takes c out of its parent's children.
static void detach(ak_pnode *c) {
	ak_pnode *n = c->parent;
	switch (c->kind) {
	case PN_FIELD: {
		ak_pnode **p = &n->fkids;
		while (*p != c) p = &(*p)->next;
		*p = c->next;
		break;
	}
	case PN_INDEX:
		n->num[c->i] = NULL;
		break;
	case PN_KEY: {
		ak_pnode **p = &n->keyed[c->hash & (n->kcap - 1)];
		while (*p != c) p = &(*p)->next;
		*p = c->next;
		n->klive--;
		break;
	}
	case PN_LEN:
		n->len = NULL;
		break;
	case PN_KEYS:
	case PN_COL: {
		ak_pnode **p = &n->keys;
		while (*p != c) p = &(*p)->next;
		*p = c->next;
		break;
	}
	}
	n->nkids--;
}

// prune frees n, and the nodes above it, while they have neither readers
// nor children; the root goes with its last child.
static void prune(ak_pnode *n) {
	while (n && !n->nkids && !n->sig.node.subs) {
		ak_pnode *parent = n->parent;
		if (parent) detach(n);
		else *n->home = NULL;
		node_free(n);
		n = parent;
	}
}

static void unwatched(ak_wsignal *s) { prune((ak_pnode *)s); }

static ak_pnode *root_of(ak_pnode **tree) {
	if (!*tree) {
		ak_pnode *r = node_new(NULL, PN_ROOT);
		r->home = tree;
		*tree = r;
	}
	return *tree;
}

// each_kid runs fn on each of n's children. fn mustn't free them.
#define EACH_KID(n, c, ...) \
	do { \
		for (ak_pnode *c = (n)->fkids; c; c = c->next) { __VA_ARGS__; } \
		for (uint32_t j_ = 0; j_ < (n)->ncap; j_++) { \
			ak_pnode *c = (n)->num[j_]; \
			if (c) { __VA_ARGS__; } \
		} \
		for (uint32_t j_ = 0; j_ < (n)->kcap; j_++) \
			for (ak_pnode *c = (n)->keyed[j_]; c; c = c->next) { __VA_ARGS__; } \
		if ((n)->len) { ak_pnode *c = (n)->len; __VA_ARGS__; } \
		for (ak_pnode *c = (n)->keys; c; c = c->next) { __VA_ARGS__; } \
	} while (0)

static void drop_all(ak_pnode *n) {
	for (ak_pnode *c = n->fkids, *next; c; c = next) {
		next = c->next;
		drop_all(c);
	}
	for (uint32_t j = 0; j < n->ncap; j++)
		if (n->num[j]) drop_all(n->num[j]);
	for (uint32_t j = 0; j < n->kcap; j++)
		for (ak_pnode *c = n->keyed[j], *next; c; c = next) {
			next = c->next;
			drop_all(c);
		}
	if (n->len) drop_all(n->len);
	for (ak_pnode *c = n->keys, *next; c; c = next) {
		next = c->next;
		drop_all(c);
	}
	ak_signal_drop(&n->sig.node);
	node_free(n);
}

void ak_path_free(ak_pnode **tree) {
	ak_pnode *r = *tree;
	if (!r) return;
	*tree = NULL;
	drop_all(r);
}

static uint32_t count(const ak_pnode *n) {
	uint32_t k = 1;
	EACH_KID(n, c, k += count(c));
	return k;
}

uint32_t ak_path_nodes(const ak_pnode *tree) { return tree ? count(tree) - 1 : 0; }

static bool read_by(const ak_pnode *n, const ak_comp *c) {
	if (ak_comp_reads(c, &n->sig.node)) return true;
	bool hit = false;
	EACH_KID(n, k, hit = hit || read_by(k, c));
	return hit;
}

bool ak_path_read_by(const ak_pnode *tree, const ak_comp *c) { return tree && read_by(tree, c); }

// ---- Reads ----

static inline uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

// ret is v as a read returns it: retained, boxed when an optional step
// led to a scalar.
static ak_val ret(pv x, bool opt) {
	if (x.none) return (ak_val){0};
	if (opt && x.k != AK_K_REF) return (ak_val){.p = ak_box_new(x.k, x.v)};
	ak_retain_val(x.v, x.k);
	return x.v;
}

bool ak_path_read(ak_pnode **tree, ak_node *root, ak_val v, ak_kind k, const uint8_t *p, uint32_t n, const ak_val *args, bool len, bool track, ak_val *out) {
	pv x = {v, k, false};
	bool opt = false;
	// Tracking: the node of the path read so far (NULL: the root's
	// container, made only when a step is followed).
	ak_pnode *at = NULL;
	ak_node *follow = root;  // what the read subscribes to
	bool made = false;
	bool grow = track;  // making nodes: until a column's
	uint32_t a = 0;
	// With len, a length step after p's.
	for (uint32_t j = 0; j < n || (len && j == n);) {
		uint8_t s = j < n ? p[j] : AK_PS_LEN;
		j++;
		ak_pnode *next = NULL;
		switch (s) {
		case AK_PS_FIELD:
		case AK_PS_OFIELD: {
			uint16_t f = u16(p + j);
			j += 2;
			if (s == AK_PS_OFIELD) opt = true;
			x = field_of(x, f);
			if (grow) next = kid_num(at ? at : root_of(tree), PN_FIELD, f, true);
			break;
		}
		case AK_PS_EACH:
			if (grow) {
				// A loop's element: the column of the field path that
				// follows, for every element; nothing deeper is followed.
				uint16_t cf[64];
				uint16_t nf = 0;
				for (uint32_t e = j; e < n && (p[e] == AK_PS_FIELD || p[e] == AK_PS_OFIELD) && nf < 64; e += 3) cf[nf++] = u16(p + e + 1);
				next = kid_keys(at ? at : root_of(tree), PN_COL, cf, nf);
				at = next;
				follow = &at->sig.node;
				made = true;
				grow = false;  // the rest is only read
			}
			// fallthrough
		case AK_PS_INDEX:
		case AK_PS_GET: {
			int64_t i = args[a++].i;
			pv arr = x;
			x = index_of(x, i);
			if (s == AK_PS_GET) opt = true;
			if (x.none && !arr.none) {
				// Out of range: what may make it exist changes the length.
				if (grow) {
					ak_pnode *base = at ? at : root_of(tree);
					next = kid_len(base, true);
					at = next;
					follow = &at->sig.node;
					made = true;
				}
				if (s == AK_PS_INDEX) {
					if (track) {
						ak_signal_read(follow);
						if (!follow->subs) prune(at);
					}
					ak_throw_msg("index %lld out of range (length %u)", (long long)i, ((ak_arr *)arr.v.p)->len);
					return false;
				}
				j = n + 1;  // none: nothing further to follow
				continue;
			}
			if (grow) next = kid_num(at ? at : root_of(tree), PN_INDEX, i, true);
			break;
		}
		case AK_PS_KEY:
		case AK_PS_VALUE:
		case AK_PS_HAS:
		case AK_PS_MEMBER: {
			ak_kind kk = (ak_kind)p[j++];
			ak_val key = args[a++];
			pv m = x;
			x = key_of(x, key);
			if (s == AK_PS_KEY) {
				opt = true;
			} else if (s == AK_PS_VALUE) {
				if (x.none) x = (pv){{0}, AK_K_INT, false};  // missing: the caller asked has first
			} else {
				bool has = s == AK_PS_HAS ? !x.none : (!x.none && x.v.i);
				x = m.none ? pv_none : (pv){{.i = has}, AK_K_BOOL, false};
				opt = false;
			}
			if (grow) next = kid_key(at ? at : root_of(tree), key, kk, true);
			break;
		}
		case AK_PS_LEN:
			x = len_of(x);
			if (grow) next = kid_len(at ? at : root_of(tree), true);
			break;
		default:
			ak_panic("path: a step %c in a read", s);
		}
		if (grow && next) {
			at = next;
			follow = &at->sig.node;
			made = true;
		}
	}
	if (track) {
		ak_signal_read(follow);
		if (made && !follow->subs) prune(at);  // nothing subscribed (a disposed reader)
	}
	*out = ret(x, opt);
	return true;
}

void ak_path_keys(ak_pnode **tree, ak_val v, ak_kind k, const uint8_t *p, uint32_t n, const ak_val *args, const uint16_t *fields, uint16_t nf) {
	ak_pnode *at = root_of(tree);
	pv x = {v, k, false};
	uint32_t a = 0;
	for (uint32_t j = 0; j < n;) {
		uint8_t s = p[j++];
		switch (s) {
		case AK_PS_FIELD:
		case AK_PS_OFIELD:
			x = field_of(x, u16(p + j));
			at = kid_num(at, PN_FIELD, u16(p + j), true);
			j += 2;
			break;
		case AK_PS_INDEX:
		case AK_PS_GET: {
			int64_t i = args[a++].i;
			if (i < 0) i = 0;  // an index below 0 never reads: its row's for has none
			x = index_of(x, i);
			at = kid_num(at, PN_INDEX, i, true);
			break;
		}
		case AK_PS_KEY:
			j++;
			x = key_of(x, args[a]);
			at = kid_key(at, args[a], (ak_kind)p[j - 1], true);
			a++;
			break;
		default:
			ak_panic("path: a step %c in a for's", s);
		}
	}
	ak_pnode *kc = kid_keys(at, PN_KEYS, fields, nf);
	ak_signal_read(&kc->sig.node);
	if (!kc->sig.node.subs) {
		prune(kc);
		return;
	}
	keys_seen(kc, x);
}

// ---- Writes ----

static inline void write(ak_pnode *n) {
	if (n->sig.node.subs) ak_signal_write(&n->sig.node);
}

// write_all writes n and everything below it (v its value now): the
// keys a for follows, when they changed from those it saw.
static void write_all(ak_pnode *n, pv v) {
	write(n);
	EACH_KID(n, c, {
		if (c->kind == PN_KEYS) {
			if (keys_changed(c, v, 0, -1)) write(c);
		} else if (c->kind == PN_COL) {
			write(c);
		} else {
			write_all(c, child_val(v, c));
		}
	});
}

// compare writes the nodes below n whose values differ between old and
// now (n's, before and after), skipping what's the same object.
static void compare(ak_pnode *n, pv old, pv now) {
	EACH_KID(n, c, {
		if (c->kind == PN_KEYS) {
			if (!same(old, now) && keys_changed(c, now, 0, -1)) write(c);
		} else if (c->kind == PN_COL) {
			if (!same(old, now) && cols_differ(c, old, now)) write(c);
		} else {
			pv o = child_val(old, c), w = child_val(now, c);
			if (!same(o, w) && !(c->sig.node.subs && pv_eq(o, w))) {
				write(c);
				compare(c, o, w);
			}
		}
	});
}

void ak_path_set(ak_pnode **tree, ak_node *root, ak_val old, ak_val now, ak_kind k) {
	ak_batch_begin();
	ak_signal_write(root);
	if (*tree && !ak_signal_in_memo()) compare(*tree, (pv){old, k, false}, (pv){now, k, false});
	ak_batch_end();
}

// A place's steps, as a write walks them: the node of each (NULL past
// the last one that's followed) and the value there now.
typedef struct {
	ak_pnode *node;
	pv val;
	int64_t index;  // an INDEX step's
} pstep;

#define MAX_STEPS 32

// related reports whether a write at an item's path g (gn fields; the
// write's steps after the item) may change its key at f: one is the
// other's beginning.
static bool related(const uint16_t *g, uint16_t gn, const uint16_t *f, uint16_t fn) {
	uint16_t m = gn < fn ? gn : fn;
	for (uint16_t j = 0; j < m; j++)
		if (g[j] != f[j]) return false;
	return true;
}

bool ak_path_put(ak_pnode **tree, ak_node *root, ak_val now, ak_kind k, const uint8_t *p, uint32_t n, const ak_val *args) {
	pstep steps[MAX_STEPS + 1];
	uint16_t fields[MAX_STEPS];  // each step's field (an item's tail, for keys)
	bool isfield[MAX_STEPS];
	uint32_t ns = 0, a = 0;
	steps[0] = (pstep){*tree, {now, k, false}, 0};
	uint8_t mode = 0;
	bool keyed = false;  // the place's last step is a map's key
	// The place: its steps' nodes and values.
	for (uint32_t j = 0; j < n;) {
		uint8_t s = p[j++];
		if (s == AK_PS_CHANGED || s == AK_PS_ALL || s == AK_PS_OLD) {
			mode = s;
			break;
		}
		if (ns == MAX_STEPS) {
			mode = AK_PS_ALL;  // deeper than nodes go: everything
			break;
		}
		pstep *cur = &steps[ns], *nx = &steps[ns + 1];
		ak_pnode *at = cur->node;
		isfield[ns] = false;
		keyed = s == AK_PS_KEY;
		switch (s) {
		case AK_PS_FIELD:
		case AK_PS_OFIELD: {
			uint16_t f = u16(p + j);
			j += 2;
			fields[ns] = f;
			isfield[ns] = true;
			nx->val = field_of(cur->val, f);
			nx->node = at ? kid_num(at, PN_FIELD, f, false) : NULL;
			break;
		}
		case AK_PS_INDEX:
		case AK_PS_GET: {
			int64_t i = args[a++].i;
			nx->val = index_of(cur->val, i);
			nx->index = i;
			nx->node = at && i >= 0 ? kid_num(at, PN_INDEX, i, false) : NULL;
			break;
		}
		case AK_PS_KEY: {
			ak_kind kk = (ak_kind)p[j++];
			ak_val key = args[a++];
			nx->val = key_of(cur->val, key);
			nx->node = at ? kid_key(at, key, kk, false) : NULL;
			break;
		}
		default:
			ak_panic("path: a step %c in a write", s);
		}
		ns++;
	}
	if (!mode) ak_panic("path: a write without its last step");
	pstep *place = &steps[ns];
	// Was it a change? An equal write, or a builtin that changed nothing,
	// tells no one.
	pv old = pv_none;
	ak_change ch = ak_last_change;
	ak_last_change = (ak_change){0};
	if (mode == AK_PS_OLD) {
		// The old value, of the place's kind; after a map's key, then
		// whether it was there.
		old = place->val.none ? pv_none : (pv){args[a], place->val.k, false};
		if (keyed && !args[a + 1].i) old = pv_none;
		if (pv_eq(old, place->val)) return false;
	} else if (mode == AK_PS_CHANGED && ch.what == AK_CHANGE_NONE) {
		return false;
	}
	if (mode == AK_PS_CHANGED && ch.what == AK_CHANGE_UNKNOWN) mode = AK_PS_ALL;
	ak_batch_begin();
	ak_signal_write(root);
	if (*tree && !ak_signal_in_memo()) {
		// The paths above the place: their values changed with it. And the
		// keys a for follows of each array on the way, when the write may
		// change an item's.
		for (uint32_t d = 0; d < ns; d++) {
			ak_pnode *at = steps[d].node;
			if (!at) break;
			if (d > 0) write(at);
			if (!at->keys || isfield[d] || d + 1 > ns) continue;
			// at is an array; the write is to its item steps[d+1], then
			// the fields after it (gn of them: past a non-field step, the
			// whole of what's under that field).
			uint16_t g[MAX_STEPS];
			uint16_t gn = 0;
			for (uint32_t e = d + 1; e < ns && isfield[e]; e++) g[gn++] = fields[e];
			int64_t i = steps[d + 1].index;
			for (ak_pnode *kc = at->keys; kc; kc = kc->next) {
				if (!related(g, gn, kc->fields, kc->nf)) continue;
				if (kc->kind == PN_KEYS) {
					if (keys_changed(kc, steps[d].val, i, i + 1)) write(kc);
					continue;
				}
				// A column: compared where the old value is at hand and the
				// column is below what was written.
				if (mode == AK_PS_OLD && gn < kc->nf && gn == ns - d - 1) {
					pv o = item_key(old, kc->fields + gn, (uint16_t)(kc->nf - gn));
					pv w = item_key(place->val, kc->fields + gn, (uint16_t)(kc->nf - gn));
					if (pv_eq(o, w)) continue;
				}
				write(kc);
			}
		}
		ak_pnode *pn = place->node;
		if (pn) {
			if (ns > 0) write(pn);
			switch (mode) {
			case AK_PS_OLD:
				compare(pn, old, place->val);
				break;
			case AK_PS_ALL:
				EACH_KID(pn, c, {
					if (c->kind == PN_KEYS) {
						if (keys_changed(c, place->val, 0, -1)) write(c);
					} else if (c->kind == PN_COL) {
						write(c);
					} else {
						write_all(c, child_val(place->val, c));
					}
				});
				break;
			case AK_PS_CHANGED:
				if (ch.len && pn->len) write(pn->len);
				if (ch.what == AK_CHANGE_RANGE) {
					for (ak_pnode *kc = pn->keys; kc; kc = kc->next)
						if (kc->kind == PN_COL || keys_changed(kc, place->val, ch.lo, ch.len ? -1 : ch.hi)) write(kc);
					int64_t hi = ch.hi < 0 || ch.len ? (int64_t)pn->ncap : ch.hi;
					for (int64_t i = ch.lo < 0 ? 0 : ch.lo; i < hi && i < pn->ncap; i++)
						if (pn->num[i] && pn->num[i]->kind == PN_INDEX) write_all(pn->num[i], index_of(place->val, i));
				} else if (pn->kcap) {
					for (ak_pnode *c = pn->keyed[ch.hash & (pn->kcap - 1)]; c; c = c->next)
						if (c->hash == ch.hash) write_all(c, child_val(place->val, c));
				}
				break;
			}
		}
	}
	ak_batch_end();
	return true;
}
