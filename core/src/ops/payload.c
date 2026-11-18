#include "ops/ops.h"

#define NO_PARENT UINT32_MAX
// menu_depth bounds submenus' nesting: deeper ones are cut (ops.menuDepth).
#define MENU_DEPTH 16

static inline ak_reader reader(const uint8_t *b, size_t n) { return (ak_reader){b, n, 0, false}; }

// Actions.

void ak_append_actions(ak_buf *b, const ak_action_item *items, uint32_t n) {
	ak_buf_u32(b, n);
	for (uint32_t i = 0; i < n; i++) {
		const ak_action_item *it = &items[i];
		uint8_t flags = it->flags & (uint8_t)~AK_MENU_SUBMENU;
		if (it->count > 0 || (flags & AK_MENU_INLINE)) flags |= AK_MENU_SUBMENU;
		ak_buf_u8(b, it->role);
		ak_buf_wstr(b, it->title);
		ak_buf_wstr(b, it->icon);
		ak_buf_u8(b, flags);
		if (flags & AK_MENU_SUBMENU) ak_append_actions(b, it->items, it->count);
	}
}

static void decode_actions(ak_reader *r, ak_action_vec *out, uint32_t depth, uint32_t parent) {
	uint32_t count = ak_read_u32(r);
	for (uint32_t i = 0; i < count && !r->err; i++) {
		ak_action it = {.index = out->len, .depth = depth, .parent = parent};
		it.role = ak_read_u8(r);
		it.title = ak_read_str(r);
		it.icon = ak_read_str(r);
		it.flags = ak_read_u8(r);
		if (r->err) break;
		bool sub = it.flags & AK_MENU_SUBMENU;
		if (sub && depth >= MENU_DEPTH) {
			r->err = true;
			break;
		}
		uint32_t at = out->len;
		ak_vec_push(out, it);
		if (parent != NO_PARENT) out->data[parent].count++;
		if (sub) decode_actions(r, out, depth + 1, at);
	}
}

bool ak_decode_actions(const uint8_t *b, size_t n, ak_action_vec *out) {
	if (n == 0) return true;
	ak_reader r = reader(b, n);
	decode_actions(&r, out, 0, NO_PARENT);
	return !r.err;
}

uint32_t ak_count_actions(const ak_action_item *items, uint32_t n) {
	uint32_t c = 0;
	for (uint32_t i = 0; i < n; i++) c += 1 + ak_count_actions(items[i].items, items[i].count);
	return c;
}

int64_t ak_action_named(const ak_action *items, uint32_t n, ak_span title) {
	for (uint32_t i = 0; i < n; i++)
		if (ak_span_eq(items[i].title, title)) return items[i].index;
	return -1;
}

// Menus.

void ak_append_menu(ak_buf *b, const ak_menu_item *items, uint32_t n) {
	ak_buf_u32(b, n);
	for (uint32_t i = 0; i < n; i++) {
		const ak_menu_item *it = &items[i];
		uint8_t flags = it->flags & (uint8_t)~AK_MENU_SUBMENU;
		if (it->count > 0) flags |= AK_MENU_SUBMENU;
		ak_buf_u8(b, flags);
		ak_buf_wstr(b, it->title);
		ak_buf_wstr(b, it->key);
		ak_buf_u8(b, it->mods);
		ak_buf_u8(b, it->role);
		if (flags & AK_MENU_SUBMENU) ak_append_menu(b, it->items, it->count);
	}
}

void ak_append_menu_bar(ak_buf *b, const ak_menu_bar_menu *menus, uint32_t n) {
	ak_buf_u32(b, n);
	for (uint32_t i = 0; i < n; i++) {
		ak_buf_wstr(b, menus[i].title);
		ak_append_menu(b, menus[i].items, menus[i].count);
	}
}

static void decode_menu(ak_reader *r, ak_menu_entry_vec *out, uint32_t menu, uint32_t depth, uint32_t parent) {
	uint32_t count = ak_read_u32(r);
	for (uint32_t i = 0; i < count && !r->err; i++) {
		ak_menu_entry it = {.index = out->len, .depth = depth, .parent = parent, .menu = menu};
		it.flags = ak_read_u8(r);
		it.title = ak_read_str(r);
		it.key = ak_read_str(r);
		it.mods = ak_read_u8(r);
		it.role = ak_read_u8(r);
		if (r->err) break;
		bool sub = it.flags & AK_MENU_SUBMENU;
		if (sub && depth >= MENU_DEPTH) {
			r->err = true;
			break;
		}
		uint32_t at = out->len;
		ak_vec_push(out, it);
		if (parent != NO_PARENT) out->data[parent].count++;
		if (sub) decode_menu(r, out, menu, depth + 1, at);
	}
}

bool ak_decode_menu(const uint8_t *b, size_t n, ak_menu_entry_vec *out) {
	ak_reader r = reader(b, n);
	decode_menu(&r, out, 0, 0, NO_PARENT);
	return !r.err;
}

bool ak_decode_menu_bar(const uint8_t *b, size_t n, ak_menu_title_vec *menus, ak_menu_entry_vec *items) {
	ak_reader r = reader(b, n);
	uint32_t count = ak_read_u32(&r);
	for (uint32_t i = 0; i < count && !r.err; i++) {
		ak_menu_title m = {.title = ak_read_str(&r), .first = items->len};
		decode_menu(&r, items, menus->len, 0, NO_PARENT);
		m.total = items->len - m.first;
		if (r.err && m.total == 0) break;
		ak_vec_push(menus, m);
	}
	return !r.err;
}

// Alerts.

void ak_append_alert_fields(ak_buf *b, const ak_alert_field *fs, uint32_t n) {
	ak_buf_u32(b, n);
	for (uint32_t i = 0; i < n; i++) {
		ak_buf_wstr(b, fs[i].placeholder);
		ak_buf_wstr(b, fs[i].text);
		ak_buf_u8(b, fs[i].flags);
		ak_buf_u8(b, fs[i].keyboard);
		ak_buf_wstr(b, fs[i].content_type);
	}
}

bool ak_decode_alert_fields(const uint8_t *b, size_t n, ak_alert_field_vec *out) {
	if (n < 4) return n == 0;
	ak_reader r = reader(b, n);
	uint32_t count = ak_read_u32(&r);
	for (uint32_t i = 0; i < count && !r.err; i++) {
		ak_alert_field f;
		f.placeholder = ak_read_str(&r);
		f.text = ak_read_str(&r);
		f.flags = ak_read_u8(&r);
		f.keyboard = ak_read_u8(&r);
		f.content_type = ak_read_str(&r);
		if (r.err) break;
		ak_vec_push(out, f);
	}
	return !r.err;
}

void ak_append_field_changed(ak_buf *b, uint32_t index, ak_span text) {
	ak_buf_u32(b, index);
	ak_buf_wstr(b, text);
}

bool ak_decode_field_changed(const uint8_t *b, size_t n, uint32_t *index, ak_span *text) {
	ak_reader r = reader(b, n);
	uint32_t i = ak_read_u32(&r);
	ak_span s = ak_read_str(&r);
	if (r.err) {
		*index = 0;
		*text = (ak_span){0};
		return false;
	}
	*index = i;
	*text = s;
	return true;
}

int64_t ak_default_alert_action(const ak_action *items, uint32_t n) {
	for (uint32_t i = 0; i < n; i++)
		if (items[i].depth == 0 && items[i].role == AK_ALERT_BUTTON_ROLE_DEFAULT) return items[i].index;
	return -1;
}

// Keys.

void ak_append_keys(ak_buf *b, const ak_key *keys, uint32_t n) {
	ak_buf_u32(b, n);
	for (uint32_t i = 0; i < n; i++) {
		ak_buf_wstr(b, keys[i].name);
		ak_buf_u8(b, keys[i].mods);
	}
}

bool ak_decode_keys(const uint8_t *b, size_t n, ak_key_vec *out) {
	ak_reader r = reader(b, n);
	uint32_t count = ak_read_u32(&r);
	if (r.err || count > n) return false;
	for (uint32_t i = 0; i < count; i++) {
		ak_key k;
		k.name = ak_read_str(&r);
		k.mods = ak_read_u8(&r);
		if (r.err) break;
		ak_vec_push(out, k);
	}
	return !r.err;
}

// rune decodes the UTF-8 rune at s, its length in *len (0: invalid).
static uint32_t rune(const uint8_t *s, uint32_t n, uint32_t *len) {
	*len = 0;
	if (n == 0) return 0;
	uint8_t c = s[0];
	if (c < 0x80) {
		*len = 1;
		return c;
	}
	uint32_t l, v, min;
	if ((c & 0xe0) == 0xc0) l = 2, v = c & 0x1f, min = 0x80;
	else if ((c & 0xf0) == 0xe0) l = 3, v = c & 0x0f, min = 0x800;
	else if ((c & 0xf8) == 0xf0) l = 4, v = c & 0x07, min = 0x10000;
	else return 0;
	if (l > n) return 0;
	for (uint32_t i = 1; i < l; i++) {
		if ((s[i] & 0xc0) != 0x80) return 0;
		v = v << 6 | (s[i] & 0x3f);
	}
	if (v < min || v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff)) return 0;
	*len = l;
	return v;
}

static uint32_t put_rune(char *buf, uint32_t r) {
	if (r < 0x80) {
		buf[0] = (char)r;
		return 1;
	}
	if (r < 0x800) {
		buf[0] = (char)(0xc0 | r >> 6);
		buf[1] = (char)(0x80 | (r & 0x3f));
		return 2;
	}
	if (r < 0x10000) {
		buf[0] = (char)(0xe0 | r >> 12);
		buf[1] = (char)(0x80 | (r >> 6 & 0x3f));
		buf[2] = (char)(0x80 | (r & 0x3f));
		return 3;
	}
	buf[0] = (char)(0xf0 | r >> 18);
	buf[1] = (char)(0x80 | (r >> 12 & 0x3f));
	buf[2] = (char)(0x80 | (r >> 6 & 0x3f));
	buf[3] = (char)(0x80 | (r & 0x3f));
	return 4;
}

// lower is unicode.ToLower for the alphabets keyboards type: Latin
// (Basic, Latin-1, Extended-A, Extended Additional), Greek, Cyrillic,
// Armenian and the full-width Latin letters.
static uint32_t lower(uint32_t r) {
	if (r >= 'A' && r <= 'Z') return r + 32;
	if (r < 0xc0) return r;
	if (r <= 0xde) return r == 0xd7 ? r : r + 32;
	if (r == 0x130) return 'i';
	if (r >= 0x100 && r <= 0x137) return r | 1;
	if (r >= 0x139 && r <= 0x148) return (r & 1) ? r + 1 : r;
	if (r >= 0x14a && r <= 0x177) return r | 1;
	if (r == 0x178) return 0xff;
	if (r >= 0x179 && r <= 0x17e) return (r & 1) ? r + 1 : r;
	if (r == 0x386) return 0x3ac;
	if (r >= 0x388 && r <= 0x38a) return r + 37;
	if (r == 0x38c) return 0x3cc;
	if (r == 0x38e || r == 0x38f) return r + 63;
	if (r >= 0x391 && r <= 0x3ab && r != 0x3a2) return r + 32;
	if (r >= 0x400 && r <= 0x40f) return r + 80;
	if (r >= 0x410 && r <= 0x42f) return r + 32;
	if ((r >= 0x460 && r <= 0x481) || (r >= 0x48a && r <= 0x4bf)) return r | 1;
	if (r >= 0x531 && r <= 0x556) return r + 48;
	if ((r >= 0x1e00 && r <= 0x1e95) || (r >= 0x1ea0 && r <= 0x1eff)) return r | 1;
	if (r >= 0xff21 && r <= 0xff3a) return r + 32;
	return r;
}

ak_span ak_key_name(ak_span name, char buf[4]) {
	uint32_t l;
	uint32_t r = rune((const uint8_t *)name.p, name.len, &l);
	if (l == 0 || l != name.len) return name;
	return (ak_span){buf, put_rune(buf, lower(r))};
}

int ak_match_key(const ak_key *keys, uint32_t n, ak_span name, uint8_t mods) {
	char buf[4];
	name = ak_key_name(name, buf);
	for (uint32_t i = 0; i < n; i++)
		if (keys[i].mods == mods && ak_span_eq(keys[i].name, name)) return (int)i;
	return -1;
}

// Items.

static void item_head(ak_buf *b, const ak_item *it) {
	ak_buf_u8(b, it->kind);
	ak_buf_wstr(b, it->text);
	ak_buf_wstr(b, it->path);
	ak_buf_wstr(b, it->name);
	ak_buf_wstr(b, it->mime);
}

static void read_item_head(ak_reader *r, ak_item *it) {
	it->kind = ak_read_u8(r);
	it->text = ak_read_str(r);
	it->path = ak_read_str(r);
	it->name = ak_read_str(r);
	it->mime = ak_read_str(r);
}

void ak_append_items(ak_buf *b, const ak_item *items, uint32_t n) {
	ak_buf_u32(b, n);
	for (uint32_t i = 0; i < n; i++) {
		item_head(b, &items[i]);
		ak_buf_u64(b, (uint64_t)items[i].size);
	}
}

static bool decode_items(ak_reader *r, ak_item_vec *out) {
	uint32_t count = ak_read_u32(r);
	for (uint32_t i = 0; i < count && !r->err; i++) {
		ak_item it = {0};
		read_item_head(r, &it);
		it.size = (int64_t)ak_read_u64(r);
		if (r->err) break;
		ak_vec_push(out, it);
	}
	return !r->err;
}

bool ak_decode_items(const uint8_t *b, size_t n, ak_item_vec *out) {
	ak_reader r = reader(b, n);
	return decode_items(&r, out);
}

void ak_append_drag_items(ak_buf *b, const ak_item *items, uint32_t n) {
	ak_buf_u32(b, n);
	for (uint32_t i = 0; i < n; i++) {
		item_head(b, &items[i]);
		ak_buf_u8(b, items[i].value ? 1 : 0);
	}
}

bool ak_decode_drag_items(const uint8_t *b, size_t n, ak_item_vec *out) {
	ak_reader r = reader(b, n);
	uint32_t count = ak_read_u32(&r);
	for (uint32_t i = 0; i < count && !r.err; i++) {
		ak_item it = {.size = -1};
		read_item_head(&r, &it);
		it.value = ak_read_u8(&r) & 1;
		if (r.err) break;
		ak_vec_push(out, it);
	}
	return !r.err;
}

void ak_append_drop(ak_buf *b, const ak_drop *d, const ak_item *items, uint32_t n) {
	ak_buf_f32(b, d->x);
	ak_buf_f32(b, d->y);
	ak_buf_u32(b, d->source);
	ak_buf_u8(b, d->copied ? 1 : 0);
	ak_append_items(b, items, n);
}

bool ak_decode_drop(const uint8_t *b, size_t n, ak_drop *d, ak_item_vec *items) {
	*d = (ak_drop){0};
	if (n < 13) return false;
	ak_reader r = reader(b, n);
	d->x = ak_read_f32(&r);
	d->y = ak_read_f32(&r);
	d->source = ak_read_u32(&r);
	d->copied = ak_read_u8(&r) & 1;
	decode_items(&r, items);
	return true;
}

void ak_append_strings(ak_buf *b, const ak_span *s, uint32_t n) {
	ak_buf_u32(b, n);
	for (uint32_t i = 0; i < n; i++) ak_buf_wstr(b, s[i]);
}

bool ak_decode_strings(const uint8_t *b, size_t n, ak_str_vec *out) {
	ak_reader r = reader(b, n);
	uint32_t count = ak_read_u32(&r);
	for (uint32_t i = 0; i < count && !r.err; i++) {
		ak_span s = ak_read_str(&r);
		if (r.err) break;
		ak_vec_push(out, s);
	}
	return !r.err;
}

static bool space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'; }

// base_mime is a MIME type without its parameters and spaces.
static ak_span base_mime(ak_span m) {
	const char *semi = m.len ? memchr(m.p, ';', m.len) : NULL;
	if (semi) m.len = (uint32_t)(semi - m.p);
	while (m.len && space(m.p[0])) m.p++, m.len--;
	while (m.len && space(m.p[m.len - 1])) m.len--;
	return m;
}

static bool fold_eq(const char *a, const char *b, uint32_t n) {
	for (uint32_t i = 0; i < n; i++) {
		char x = a[i], y = b[i];
		if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
		if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
		if (x != y) return false;
	}
	return true;
}

bool ak_match_mime(const ak_span *patterns, uint32_t n, ak_span mime) {
	mime = base_mime(mime);
	for (uint32_t i = 0; i < n; i++) {
		ak_span p = base_mime(patterns[i]);
		if (ak_span_eqc(p, "*") || ak_span_eqc(p, "*/*")) return true;
		if (p.len >= 2 && p.p[p.len - 2] == '/' && p.p[p.len - 1] == '*') {
			uint32_t k = p.len - 1;
			if (mime.len > k && fold_eq(mime.p, p.p, k)) return true;
		} else if (p.len == mime.len && fold_eq(p.p, mime.p, p.len)) {
			return true;
		}
	}
	return false;
}

// Styled text.

bool ak_rich_style_eq(const ak_rich_style *a, const ak_rich_style *b) {
	return a->text_style == b->text_style && a->size == b->size && a->weight == b->weight && a->flags == b->flags &&
	       a->color == b->color && a->background == b->background && ak_span_eq(a->link, b->link);
}

typedef struct {
	uint32_t style;
	const char *p;
	uint32_t len;
	bool merged;  // goes on the run before it
} rich_seg;

void ak_append_rich(ak_buf *b, const ak_rich_run *runs, uint32_t n) {
	AK_VEC(uint32_t) styles = {0};  // a run that has each style
	AK_VEC(rich_seg) segs = {0};
	AK_VEC(uint32_t) paras = {0};  // each paragraph's first seg
	ak_vec_push(&paras, 0);
	for (uint32_t i = 0; i < n; i++) {
		const ak_rich_run *r = &runs[i];
		const char *p = r->text.len ? r->text.p : "", *end = p + r->text.len;
		for (uint32_t part = 0;; part++) {
			const char *nl = p < end ? memchr(p, '\n', (size_t)(end - p)) : NULL;
			const char *stop = nl ? nl : end;
			if (part > 0) ak_vec_push(&paras, segs.len);
			if (stop > p) {
				uint32_t k = 0;
				while (k < styles.len && !ak_rich_style_eq(&runs[styles.data[k]].style, &r->style)) k++;
				if (k == styles.len) ak_vec_push(&styles, i);
				bool merged = segs.len > paras.data[paras.len - 1] && segs.data[segs.len - 1].style == k;
				ak_vec_push(&segs, ((rich_seg){k, p, (uint32_t)(stop - p), merged}));
			}
			if (!nl) break;
			p = nl + 1;
		}
	}
	ak_buf_u32(b, styles.len);
	for (uint32_t k = 0; k < styles.len; k++) {
		const ak_rich_style *s = &runs[styles.data[k]].style;
		ak_buf_u8(b, s->text_style);
		ak_buf_f32(b, s->size);
		ak_buf_u8(b, s->weight);
		ak_buf_u8(b, s->flags);
		ak_buf_u64(b, s->color);
		ak_buf_u64(b, s->background);
		ak_buf_wstr(b, s->link);
	}
	ak_buf_u32(b, paras.len);
	for (uint32_t p = 0; p < paras.len; p++) {
		uint32_t first = paras.data[p], end = p + 1 < paras.len ? paras.data[p + 1] : segs.len;
		uint32_t count = 0;
		for (uint32_t i = first; i < end; i++) count += !segs.data[i].merged;
		ak_buf_u32(b, count);
		for (uint32_t i = first; i < end;) {
			uint32_t j = i + 1, len = segs.data[i].len;
			while (j < end && segs.data[j].merged) len += segs.data[j++].len;
			ak_buf_u32(b, segs.data[i].style);
			ak_buf_u32(b, len);
			for (; i < j; i++) ak_buf_put(b, segs.data[i].p, segs.data[i].len);
		}
	}
	ak_vec_free(&styles);
	ak_vec_free(&segs);
	ak_vec_free(&paras);
}

bool ak_decode_rich(const uint8_t *b, size_t n, ak_rich_run_vec *out) {
	uint32_t start = out->len;
	AK_VEC(ak_rich_style) styles = {0};
	ak_reader r = reader(b, n);
	bool ok = false;
	uint32_t count = ak_read_u32(&r);
	if (r.err) goto done;
	for (uint32_t i = 0; i < count; i++) {
		if (r.len - r.off < 23) goto fail;
		ak_rich_style s;
		s.text_style = ak_read_u8(&r);
		s.size = ak_read_f32(&r);
		s.weight = ak_read_u8(&r);
		s.flags = ak_read_u8(&r);
		s.color = ak_read_u64(&r);
		s.background = ak_read_u64(&r);
		s.link = ak_read_str(&r);
		if (r.err) goto fail;
		ak_vec_push(&styles, s);
	}
	uint32_t paras = ak_read_u32(&r);
	if (r.err) goto fail;
	ak_rich_style last = {0};
	for (uint32_t p = 0; p < paras; p++) {
		if (p > 0) ak_vec_push(out, ((ak_rich_run){last, AK_STR("\n")}));
		uint32_t runs = ak_read_u32(&r);
		if (r.err) goto done;
		for (uint32_t i = 0; i < runs; i++) {
			uint32_t k = ak_read_u32(&r);
			ak_span text = ak_read_str(&r);
			if (r.err || k >= styles.len) {
				r.err = true;
				goto done;
			}
			last = styles.data[k];
			ak_vec_push(out, ((ak_rich_run){last, text}));
		}
	}
	ok = true;
	goto done;
fail:
	out->len = start;
done:
	ak_vec_free(&styles);
	return ok;
}

// Sidebars.

void ak_append_sidebar(ak_buf *b, const ak_sidebar_section *s, uint32_t n) {
	ak_buf_u32(b, n);
	for (uint32_t i = 0; i < n; i++) {
		ak_buf_wstr(b, s[i].title);
		ak_buf_u32(b, s[i].count);
		for (uint32_t j = 0; j < s[i].count; j++) {
			ak_buf_wstr(b, s[i].items[j].title);
			ak_buf_wstr(b, s[i].items[j].icon);
			ak_buf_wstr(b, s[i].items[j].badge);
		}
	}
}

bool ak_decode_sidebar(const uint8_t *b, size_t n, ak_sidebar_heading_vec *sections, ak_sidebar_item_vec *items) {
	if (n == 0) return true;
	ak_reader r = reader(b, n);
	uint32_t count = ak_read_u32(&r);
	for (uint32_t i = 0; i < count && !r.err; i++) {
		ak_sidebar_heading s = {.title = ak_read_str(&r), .first = items->len};
		uint32_t k = ak_read_u32(&r);
		if (r.err) break;
		for (uint32_t j = 0; j < k; j++) {
			ak_sidebar_item it;
			it.title = ak_read_str(&r);
			it.icon = ak_read_str(&r);
			it.badge = ak_read_str(&r);
			if (r.err) break;
			ak_vec_push(items, it);
		}
		s.count = items->len - s.first;
		ak_vec_push(sections, s);
	}
	return !r.err;
}

bool ak_same_sidebar_shape(const ak_sidebar_heading *a, uint32_t na, const ak_sidebar_heading *b, uint32_t nb) {
	if (na != nb) return false;
	for (uint32_t i = 0; i < na; i++)
		if (a[i].count != b[i].count) return false;
	return true;
}

bool ak_badge_number(ak_span s, int32_t *n) {
	*n = 0;
	if (s.len == 0 || s.len > 9) return false;
	int32_t v = 0;
	for (uint32_t i = 0; i < s.len; i++) {
		char c = s.p[i];
		if (c < '0' || c > '9') return false;
		v = v * 10 + (c - '0');
	}
	*n = v;
	return true;
}

// Windows.

void ak_append_window_config(ak_buf *b, const ak_window_config *c) {
	const float fs[] = {c->width, c->height, c->min_width, c->min_height, c->max_width, c->max_height, c->x, c->y};
	for (size_t i = 0; i < AK_LEN(fs); i++) ak_buf_f32(b, fs[i]);
	ak_buf_u32(b, c->flags);
	ak_buf_u8(b, c->title_bar);
	ak_buf_u64(b, c->background);
	ak_buf_u32(b, c->parent);
	ak_buf_wstr(b, c->state_key);
	ak_buf_put(b, c->title.p, c->title.len);
}

ak_window_config ak_decode_window_config(const uint8_t *b, size_t n) {
	ak_window_config c = {0};
	ak_reader r = reader(b, n);
	if (n < 32) {
		float *old[] = {&c.width, &c.height, &c.min_width, &c.min_height};
		for (size_t i = 0; i < AK_LEN(old); i++) *old[i] = ak_read_f32(&r);
		if (!r.err) c.title = ak_strn(b + 16, n - 16);
		return c;
	}
	float *fs[] = {&c.width, &c.height, &c.min_width, &c.min_height, &c.max_width, &c.max_height, &c.x, &c.y};
	for (size_t i = 0; i < AK_LEN(fs); i++) *fs[i] = ak_read_f32(&r);
	c.flags = ak_read_u32(&r);
	c.title_bar = ak_read_u8(&r);
	c.background = ak_read_u64(&r);
	c.parent = ak_read_u32(&r);
	c.state_key = ak_read_str(&r);
	if (!r.err) c.title = ak_strn(b + r.off, n - r.off);
	else c.state_key = (ak_span){0};
	return c;
}

void ak_append_window_state(ak_buf *b, const ak_window_state *s) {
	ak_buf_f32(b, s->x);
	ak_buf_f32(b, s->y);
	ak_buf_f32(b, s->width);
	ak_buf_f32(b, s->height);
	ak_buf_u8(b, s->flags);
}

ak_window_state ak_decode_window_state(const uint8_t *b, size_t n) {
	ak_reader r = reader(b, n);
	ak_window_state s;
	s.x = ak_read_f32(&r);
	s.y = ak_read_f32(&r);
	s.width = ak_read_f32(&r);
	s.height = ak_read_f32(&r);
	s.flags = ak_read_u8(&r);
	if (r.err) return (ak_window_state){0};
	return s;
}

static bool window_action_args(uint8_t a) { return a == AK_WINDOW_ACTION_SIZE || a == AK_WINDOW_ACTION_POSITION || a == AK_WINDOW_ACTION_FIT; }

void ak_append_window_action(ak_buf *b, uint8_t action, float x, float y) {
	ak_buf_u8(b, action);
	if (window_action_args(action)) {
		ak_buf_f32(b, x);
		ak_buf_f32(b, y);
	}
}

bool ak_decode_window_action(const uint8_t *b, size_t n, uint8_t *action, float *x, float *y) {
	*action = 0;
	*x = *y = 0;
	if (n == 0) return false;
	uint8_t a = b[0];
	if (window_action_args(a)) {
		if (n < 9) return false;
		ak_reader r = reader(b + 1, n - 1);
		*x = ak_read_f32(&r);
		*y = ak_read_f32(&r);
	}
	*action = a;
	return true;
}

void ak_append_flyout(ak_buf *b, const ak_flyout *f) {
	ak_buf_u32(b, f->tray);
	ak_buf_f32(b, f->x);
	ak_buf_f32(b, f->y);
	ak_buf_f32(b, f->width);
	ak_buf_f32(b, f->height);
	ak_buf_u8(b, f->placement);
	ak_buf_f32(b, f->gap);
}

bool ak_decode_flyout(const uint8_t *b, size_t n, ak_flyout *f) {
	ak_reader r = reader(b, n);
	ak_flyout v;
	v.tray = ak_read_u32(&r);
	v.x = ak_read_f32(&r);
	v.y = ak_read_f32(&r);
	v.width = ak_read_f32(&r);
	v.height = ak_read_f32(&r);
	v.placement = ak_read_u8(&r);
	v.gap = ak_read_f32(&r);
	*f = r.err ? (ak_flyout){0} : v;
	return !r.err;
}

// Locations.

void ak_append_location(ak_buf *b, const ak_location *l) {
	ak_buf_u8(b, l->action);
	ak_buf_u32(b, (uint32_t)l->delta);
	ak_buf_wstr(b, l->href);
	ak_buf_wstr(b, l->state);
	ak_buf_wstr(b, l->window);
}

bool ak_decode_location(const uint8_t *b, size_t n, ak_location *l) {
	ak_reader r = reader(b, n);
	l->action = ak_read_u8(&r);
	l->delta = (int32_t)ak_read_u32(&r);
	l->href = ak_read_str(&r);
	l->state = ak_read_str(&r);
	l->window = ak_read_str(&r);
	return !r.err;
}

void ak_append_location_event(ak_buf *b, ak_span href, ak_span state) {
	ak_buf_wstr(b, href);
	ak_buf_wstr(b, state);
}

bool ak_decode_location_event(const uint8_t *b, size_t n, ak_span *href, ak_span *state) {
	ak_reader r = reader(b, n);
	*href = ak_read_str(&r);
	*state = ak_read_str(&r);
	return !r.err;
}

// The error overlay.

void ak_append_dev_error(ak_buf *b, const ak_dev_error *e) {
	ak_buf_u8(b, e->flags);
	ak_buf_wstr(b, e->title);
	ak_buf_wstr(b, e->message);
	ak_buf_wstr(b, e->source);
	ak_buf_wstr(b, e->details);
}

bool ak_decode_dev_error(const uint8_t *b, size_t n, ak_dev_error *e) {
	ak_reader r = reader(b, n);
	e->flags = ak_read_u8(&r);
	e->title = ak_read_str(&r);
	e->message = ak_read_str(&r);
	e->source = ak_read_str(&r);
	e->details = ak_read_str(&r);
	return !r.err;
}
