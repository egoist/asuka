// field.c: text fields: a GtkEntry (GtkPasswordEntry when Secure), or for
// a Multiline field a GtkTextView in a scrolled window, styled as
// libadwaita styles an entry (filled, rounded, the accent's focus ring)
// unless Plain, with a dim label for its placeholder while it's empty. The
// text view's text takes the node's font and color from a tag of its own;
// a Rich field's styles are a tag per style over it, from and to the
// core's styled text (RichText's Content encoding). Ctrl+B, Ctrl+I and
// Ctrl+U toggle formats, as the Format command does; typed text takes the
// style before it, or what a Format at the cursor set.
//
// Selection offsets cross as UTF-16 (the core's) and characters (GTK's).
// PasteTypes: a paste of files, an image (as PNG) or data of a type the
// field takes is read asynchronously into the app's caches and reported
// with Pasted instead of going in.
#include "host.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

void lx_run_attrs(node *n, const ak_rich_style *s, void (*add)(PangoAttribute *a, void *ctx), void *ctx);
void lx_read_clipboard_text(void (*done)(const char *text, bool ok, void *ctx), void *ctx);
void lx_set_a11y_string(gpointer w, int prop, const char *s);

#define FIELD_PAD_X 9
#define FIELD_PAD_Y 8
#define ENTRY_HEIGHT 34  // libadwaita's entry's least height

// A style of a rich field's, its link owned.
typedef struct {
	ak_rich_style s;
	char *link;
	GtkTextTag *tag;
} rstyle;

typedef struct {
	AK_VEC(rstyle) styles;  // the buffer's tag of each style
	bool setting;           // the host is setting its text: no events
	ak_rich_style typing;   // what's typed next, when typing_set
	char *typing_link;
	bool typing_set;
	uint8_t formats;  // as last reported
	char *link;
	bool known;
} rich_field;

// Text going into a text view's buffer: its offset and length in
// characters, and the style it takes when typed in a rich field.
typedef struct {
	int at, count;
	ak_rich_style style;
	char *link;
	bool typed;
} insertion;

struct field {
	GtkWidget *editable;  // the GtkEditable (entry), or NULL
	GtkWidget *view;      // the GtkTextView (multiline), or NULL
	GtkTextBuffer *buffer;
	GtkWidget *hint;    // a multiline field's placeholder label
	GtkTextTag *base;   // a multiline field's tag of the node's font and color
	int32_t keyboard, ret, autocap, autocorrect;
	char *content_type;
	ak_buf paste_buf;
	ak_str_vec paste_types;
	uint32_t sel_start, sel_end;  // as last reported, UTF-16
	AK_VEC(insertion) pending;    // inserted, not yet tagged
	rich_field *rich;
};

static void fpad(node *n, int *x, int *y) {
	*x = n->plain ? 0 : FIELD_PAD_X;
	*y = n->plain ? 0 : FIELD_PAD_Y;
}

static void rich_reset(rich_field *rf) {
	for (uint32_t i = 0; i < rf->styles.len; i++) free(rf->styles.data[i].link);
	rf->styles.len = 0;
	rf->typing_set = false;
	rf->known = false;
}

static void free_pending(field *f) {
	for (uint32_t i = 0; i < f->pending.len; i++) free(f->pending.data[i].link);
	f->pending.len = 0;
}

void lx_release_field(node *n) {
	field *f = n->field;
	if (!f) return;
	if (f->rich) {
		rich_reset(f->rich);
		ak_vec_free(&f->rich->styles);
		free(f->rich->typing_link);
		free(f->rich->link);
		free(f->rich);
	}
	free_pending(f);
	ak_vec_free(&f->pending);
	free(f->content_type);
	ak_buf_free(&f->paste_buf);
	ak_vec_free(&f->paste_types);
	free(f);
	n->field = NULL;
}

// ---- Text and offsets ----

// field_text is the field's text (malloc'd).
static char *field_text(node *n) {
	field *f = n->field;
	if (f->editable) return strdup(gtk_editable_get_text((GtkEditable *)f->editable));
	GtkTextIter s, e;
	gtk_text_buffer_get_start_iter(f->buffer, &s);
	gtk_text_buffer_get_end_iter(f->buffer, &e);
	char *t = gtk_text_buffer_get_text(f->buffer, &s, &e, 1);
	char *out = strdup(t ? t : "");
	g_free(t);
	return out;
}

// chars_to_utf16 converts a character offset into s to UTF-16 units.
static uint32_t chars_to_utf16(const char *s, int chars) {
	uint32_t u = 0;
	for (int i = 0; *s && i < chars; i++) {
		unsigned char c = (unsigned char)*s;
		int len = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
		u += len == 4 ? 2 : 1;
		for (int k = 0; k < len && *s; k++) s++;
	}
	return u;
}

// utf16_to_chars converts a UTF-16 offset into s to characters (one inside
// a surrogate pair goes to the character's start).
static int utf16_to_chars(const char *s, uint32_t u) {
	int c = 0;
	while (*s && u > 0) {
		unsigned char b = (unsigned char)*s;
		int len = b < 0x80 ? 1 : b < 0xe0 ? 2 : b < 0xf0 ? 3 : 4;
		uint32_t l = len == 4 ? 2 : 1;
		if (l > u) break;
		u -= l;
		c++;
		for (int k = 0; k < len && *s; k++) s++;
	}
	return c;
}

// selection is the selection's bounds in characters, start first.
static void selection(node *n, int *start, int *end) {
	field *f = n->field;
	if (f->editable) {
		int s, e;
		if (gtk_editable_get_selection_bounds((GtkEditable *)f->editable, &s, &e)) {
			*start = s < e ? s : e;
			*end = s < e ? e : s;
			return;
		}
		*start = *end = gtk_editable_get_position((GtkEditable *)f->editable);
		return;
	}
	GtkTextIter s, e;
	gtk_text_buffer_get_selection_bounds(f->buffer, &s, &e);
	*start = gtk_text_iter_get_offset(&s);
	*end = gtk_text_iter_get_offset(&e);
}

static void report_selection(node *n) {
	if (n->updating || !listening(n, AK_EVENT_SELECTION_CHANGED)) return;
	int s, e;
	selection(n, &s, &e);
	char *text = field_text(n);  // now: the selection may move before "changed"
	uint32_t us = chars_to_utf16(text, s), ue = chars_to_utf16(text, e);
	free(text);
	if (us == n->field->sel_start && ue == n->field->sel_end) return;
	n->field->sel_start = us;
	n->field->sel_end = ue;
	uint8_t b[8];
	for (int i = 0; i < 4; i++) {
		b[i] = (uint8_t)(us >> (8 * i));
		b[4 + i] = (uint8_t)(ue >> (8 * i));
	}
	lx_emit(n, (ak_event){.kind = AK_EVENT_SELECTION_CHANGED, .s = ak_strn(b, 8)});
}

static void set_selection(node *n, uint32_t start, uint32_t end) {
	field *f = n->field;
	char *text = field_text(n);
	int s = utf16_to_chars(text, start), e = utf16_to_chars(text, end);
	f->sel_start = chars_to_utf16(text, s);
	f->sel_end = chars_to_utf16(text, e);
	free(text);
	n->updating = true;
	if (f->editable) {
		gtk_editable_select_region((GtkEditable *)f->editable, s, e);
	} else {
		GtkTextIter a, b;
		gtk_text_buffer_get_iter_at_offset(f->buffer, &a, e);
		gtk_text_buffer_get_iter_at_offset(f->buffer, &b, s);
		gtk_text_buffer_select_range(f->buffer, &a, &b);  // the cursor at the end
	}
	n->updating = false;
}

static void update_hint(node *n) {
	field *f = n->field;
	if (!f->hint) return;
	gtk_widget_set_visible(f->hint, !str_empty(n->placeholder) && gtk_text_buffer_get_char_count(f->buffer) == 0);
}

static void set_field_text(node *n, const char *s) {
	field *f = n->field;
	str_setc(&n->text, s);
	char *cur = field_text(n);
	bool same = strcmp(cur, S(s)) == 0;
	free(cur);
	if (same) return;
	n->updating = true;
	if (f->editable) {
		gtk_editable_set_text((GtkEditable *)f->editable, S(s));
		gtk_editable_set_position((GtkEditable *)f->editable, -1);
	} else {
		if (f->rich) f->rich->setting = true;
		gtk_text_buffer_set_text(f->buffer, S(s), -1);
		if (f->rich) f->rich->setting = false;
	}
	n->updating = false;
	update_hint(n);
}

// lx_field_put_text sets the field's text, the cursor at its end, as the
// app does (no TextChanged): a suggestion chosen (suggest.c).
void lx_field_put_text(node *n, const char *s) {
	if (n->field) set_field_text(n, s);
}

// ---- Rich fields' styles ----

// rich_format is a style's TextFormat bits.
static uint8_t rich_format(const ak_rich_style *s) {
	uint8_t f = 0;
	if (s->weight >= AK_FONT_WEIGHT_SEMIBOLD) f |= 1;
	if (s->flags & AK_RICH_ITALIC) f |= 2;
	if (s->flags & AK_RICH_UNDERLINE) f |= 4;
	if (s->flags & AK_RICH_STRIKE) f |= 8;
	if (s->flags & AK_RICH_MONO) f |= 16;
	return f;
}

static ak_rich_style rich_formatted(ak_rich_style s, uint8_t bits, bool on) {
	if (bits & 1) {
		if (on)
			s.weight = s.weight > AK_FONT_WEIGHT_BOLD ? s.weight : AK_FONT_WEIGHT_BOLD;
		else if (s.weight >= AK_FONT_WEIGHT_SEMIBOLD)
			s.weight = 0;
	}
	static const uint8_t map[][2] = {{2, AK_RICH_ITALIC}, {16, AK_RICH_MONO}, {4, AK_RICH_UNDERLINE}, {8, AK_RICH_STRIKE}};
	for (size_t i = 0; i < AK_LEN(map); i++)
		if (bits & map[i][0]) s.flags = on ? (uint8_t)(s.flags | map[i][1]) : (uint8_t)(s.flags & ~map[i][1]);
	return s;
}

static bool style_empty(const ak_rich_style *s) {
	static const ak_rich_style zero;
	return ak_rich_style_eq(s, &zero);
}

static void set_boxed(gpointer o, const char *name, GType t, gconstpointer p) {
	GValue v = {0};
	g_value_init(&v, t);
	g_value_set_boxed(&v, p);
	g_object_set_property(o, name, &v);
	g_value_unset(&v);
}

// style_tag_props sets a tag's look from a style, over the field's own
// (its base tag's): what the style sets, as a RichText draws it.
static void style_tag_props(node *n, GtkTextTag *t, const ak_rich_style *s) {
	PangoFontDescription *desc = pango_font_description_new();
	if (s->text_style || s->size || s->weight) {
		node x = {.text_style = n->text_style, .font_size = n->font_size, .font_weight = n->font_weight};
		if (s->text_style) x.text_style = s->text_style;
		if (s->size) x.font_size = s->size;
		if (s->weight) x.font_weight = s->weight;
		float size;
		int weight;
		lx_font(&x, &size, &weight);
		pango_font_description_set_absolute_size(desc, size * 1024.0);
		pango_font_description_set_weight(desc, (PangoWeight)weight);
	}
	if (s->flags & AK_RICH_ITALIC) pango_font_description_set_style(desc, PANGO_STYLE_ITALIC);
	if (s->flags & AK_RICH_MONO) pango_font_description_set_family(desc, "monospace");
	set_boxed(t, "font-desc", pango_font_description_get_type(), desc);
	pango_font_description_free(desc);
	g_object_set(t, "underline", (s->flags & AK_RICH_UNDERLINE) || s->link.len ? PANGO_UNDERLINE_SINGLE : PANGO_UNDERLINE_NONE,
		"strikethrough", (gboolean)((s->flags & AK_RICH_STRIKE) != 0), NULL);
	GdkRGBA c;
	bool ok = lx_resolve(s->color, &c);
	if (!ok && s->link.len) {
		c = H.theme.accent;
		ok = true;
	}
	if (ok)
		set_boxed(t, "foreground-rgba", gdk_rgba_get_type(), &c);
	else
		g_object_set(t, "foreground-set", 0, NULL);
	GdkRGBA bg;
	if (lx_resolve(s->background, &bg) && bg.alpha > 0)
		set_boxed(t, "background-rgba", gdk_rgba_get_type(), &bg);
	else
		g_object_set(t, "background-set", 0, NULL);
}

// style_tag is the buffer's tag of a style (NULL: the field's own style,
// no tag), made the first time a run has it.
static GtkTextTag *style_tag(node *n, const ak_rich_style *s) {
	rich_field *rf = n->field->rich;
	if (!rf || style_empty(s)) return NULL;
	for (uint32_t i = 0; i < rf->styles.len; i++)
		if (ak_rich_style_eq(&rf->styles.data[i].s, s)) return rf->styles.data[i].tag;
	GtkTextTag *t = gtk_text_tag_new(NULL);
	gtk_text_tag_table_add(gtk_text_buffer_get_tag_table(n->field->buffer), t);
	g_object_unref(t);  // the table's
	rstyle r = {*s, str_dup(s->link), t};
	r.s.link = ak_cstr(r.link);
	ak_vec_push(&rf->styles, r);
	style_tag_props(n, t, &r.s);
	return t;
}

static void restyle_tags(node *n) {
	rich_field *rf = n->field->rich;
	if (!rf) return;
	for (uint32_t i = 0; i < rf->styles.len; i++) style_tag_props(n, rf->styles.data[i].tag, &rf->styles.data[i].s);
}

// style_at is the style of the character at it (its link the field's),
// and its tag.
static ak_rich_style style_at(node *n, GtkTextIter *it, GtkTextTag **tag) {
	rich_field *rf = n->field->rich;
	*tag = NULL;
	ak_rich_style none = {0};
	if (!rf) return none;
	GSList *l = gtk_text_iter_get_tags(it);
	ak_rich_style out = none;
	for (GSList *p = l; p; p = p->next)
		for (uint32_t i = 0; i < rf->styles.len; i++)
			if (rf->styles.data[i].tag == p->data) {
				out = rf->styles.data[i].s;
				*tag = rf->styles.data[i].tag;
				g_slist_free(l);
				return out;
			}
	g_slist_free(l);
	return out;
}

static ak_rich_style style_at_offset(node *n, int off) {
	GtkTextIter it;
	gtk_text_buffer_get_iter_at_offset(n->field->buffer, &it, off);
	GtkTextTag *t;
	return style_at(n, &it, &t);
}

typedef void (*run_fn)(node *n, int start, int end, const ak_rich_style *s, GtkTextTag *tag, void *ctx);

// field_runs calls fn for each run of one style in [from, to) (character
// offsets).
static void field_runs(node *n, int from, int to, run_fn fn, void *ctx) {
	GtkTextBuffer *buf = n->field->buffer;
	int total = gtk_text_buffer_get_char_count(buf);
	if (to > total) to = total;
	for (int off = from; off < to;) {
		GtkTextIter it, next;
		gtk_text_buffer_get_iter_at_offset(buf, &it, off);
		GtkTextTag *tag;
		ak_rich_style s = style_at(n, &it, &tag);
		next = it;
		int end = to;
		if (gtk_text_iter_forward_to_tag_toggle(&next, NULL)) {
			int o = gtk_text_iter_get_offset(&next);
			end = o < to ? o : to;
		}
		if (end <= off) end = off + 1;
		fn(n, off, end, &s, tag, ctx);
		off = end;
	}
}

// field_slice is the text in [from, to) (character offsets, malloc'd).
static char *field_slice(node *n, int from, int to) {
	GtkTextIter a, b;
	gtk_text_buffer_get_iter_at_offset(n->field->buffer, &a, from);
	gtk_text_buffer_get_iter_at_offset(n->field->buffer, &b, to);
	char *t = gtk_text_buffer_get_text(n->field->buffer, &a, &b, 1);
	char *out = strdup(t ? t : "");
	g_free(t);
	return out;
}

typedef struct {
	AK_VEC(ak_rich_run) runs;
	AK_VEC(char *) texts;
} enc_ctx;

static void enc_run(node *n, int start, int end, const ak_rich_style *s, GtkTextTag *tag, void *data) {
	(void)tag;
	enc_ctx *c = data;
	char *t = field_slice(n, start, end);
	ak_vec_push(&c->texts, t);
	ak_rich_run r = {*s, ak_cstr(t)};
	ak_vec_push(&c->runs, r);
}

// encode_rich is the field's styled text as the core encodes it
// (canonical), into b.
static void encode_rich(node *n, ak_buf *b) {
	field *f = n->field;
	if (!f || !f->buffer) return;
	enc_ctx c = {0};
	field_runs(n, 0, gtk_text_buffer_get_char_count(f->buffer), enc_run, &c);
	ak_append_rich(b, c.runs.data, c.runs.len);
	for (uint32_t i = 0; i < c.texts.len; i++) free(c.texts.data[i]);
	ak_vec_free(&c.texts);
	ak_vec_free(&c.runs);
}

static void report_formats(node *n);

static void rich_changed(node *n) {
	field *f = n->field;
	if (!f || !f->rich || f->rich->setting) return;
	ak_buf b = {0};
	encode_rich(n, &b);
	lx_emit(n, (ak_event){.kind = AK_EVENT_RICH_CHANGED, .s = ak_strn(b.data, b.len)});
	ak_buf_free(&b);
	report_formats(n);
}

// typing_style is what's typed at offset at: a Format's at the cursor, or
// the style of the character before it (the first's, at the start).
static ak_rich_style typing_style(node *n, int at) {
	rich_field *rf = n->field->rich;
	if (rf->typing_set) return rf->typing;
	if (at > 0) return style_at_offset(n, at - 1);
	if (gtk_text_buffer_get_char_count(n->field->buffer) > 0) return style_at_offset(n, 0);
	return (ak_rich_style){0};
}

typedef struct {
	uint8_t all;
	ak_span link;
	bool first;
} fmt_ctx;

static void fmt_run(node *n, int start, int end, const ak_rich_style *s, GtkTextTag *tag, void *data) {
	(void)n;
	(void)start;
	(void)end;
	(void)tag;
	fmt_ctx *c = data;
	c->all &= rich_format(s);
	if (c->first) {
		c->link = s->link;
		c->first = false;
	} else if (!ak_span_eq(c->link, s->link)) {
		c->link = (ak_span){0};
	}
}

// formats_of is the styles at the cursor (what's typed next) or that all
// the selection has, and its link (into *link: the style's).
static uint8_t formats_of(node *n, ak_span *link) {
	int s, e;
	selection(n, &s, &e);
	if (s == e) {
		ak_rich_style st = typing_style(n, s);
		*link = st.link;
		return rich_format(&st);
	}
	fmt_ctx c = {31, {0}, true};
	field_runs(n, s < e ? s : e, s < e ? e : s, fmt_run, &c);
	*link = c.link;
	return c.all;
}

static void report_formats(node *n) {
	field *f = n->field;
	if (!f || !f->rich || !f->buffer || !listening(n, AK_EVENT_FORMATS)) return;
	ak_span link;
	uint8_t bits = formats_of(n, &link);
	rich_field *rf = f->rich;
	if (rf->known && bits == rf->formats && ak_span_eq(link, ak_cstr(S(rf->link)))) return;
	rf->known = true;
	rf->formats = bits;
	str_set(&rf->link, link);
	ak_buf b = {0};
	ak_buf_u8(&b, bits);
	ak_buf_str(&b, link.p, link.len);
	lx_emit(n, (ak_event){.kind = AK_EVENT_FORMATS, .s = ak_strn(b.data, b.len)});
	ak_buf_free(&b);
}

typedef ak_rich_style (*restyle_fn)(ak_rich_style s, const void *arg);

typedef struct {
	int start, end;
	GtkTextTag *old, *new_;
} change;

typedef struct {
	restyle_fn fn;
	const void *arg;
	AK_VEC(change) changes;
} restyle_ctx;

static void restyle_run(node *n, int start, int end, const ak_rich_style *s, GtkTextTag *tag, void *data) {
	restyle_ctx *c = data;
	ak_rich_style ns = c->fn(*s, c->arg);
	if (ak_rich_style_eq(&ns, s)) return;
	change ch = {start, end, tag, style_tag(n, &ns)};
	ak_vec_push(&c->changes, ch);
}

typedef struct {
	int at, idx, count;
	struct {
		int start, end;
		ak_span link;
	} runs[512];
} link_ctx;

static void link_run(node *n, int start, int end, const ak_rich_style *s, GtkTextTag *tag, void *data) {
	(void)n;
	(void)tag;
	link_ctx *c = data;
	if (c->count >= 512) return;
	if (c->at >= start && c->at < end) c->idx = c->count;
	c->runs[c->count].start = start;
	c->runs[c->count].end = end;
	c->runs[c->count].link = s->link;
	c->count++;
}

// restyle changes each run of the selection through fn, as one undoable
// action; at the cursor, what's typed next (with whole_link, the link the
// cursor is in).
static void restyle(node *n, restyle_fn fn, const void *arg, bool whole_link) {
	field *f = n->field;
	GtkTextBuffer *buf = f->buffer;
	int ss, se;
	selection(n, &ss, &se);
	int s = ss < se ? ss : se, e = ss < se ? se : ss;
	if (s == e && whole_link) {
		int total = gtk_text_buffer_get_char_count(buf);
		if (!total) return;
		link_ctx *c = calloc(1, sizeof *c);
		if (!c) abort();
		c->at = s > 0 ? s - 1 : 0;
		if (c->at > total - 1) c->at = total - 1;
		c->idx = -1;
		field_runs(n, 0, total, link_run, c);
		if (c->idx < 0 || !c->runs[c->idx].link.len) {
			free(c);
			return;
		}
		int lo = c->idx, hi = c->idx;
		while (lo > 0 && ak_span_eq(c->runs[lo - 1].link, c->runs[c->idx].link)) lo--;
		while (hi < c->count - 1 && ak_span_eq(c->runs[hi + 1].link, c->runs[c->idx].link)) hi++;
		s = c->runs[lo].start;
		e = c->runs[hi].end;
		free(c);
	}
	if (s == e) {
		rich_field *rf = f->rich;
		ak_rich_style t = fn(typing_style(n, s), arg);
		str_set(&rf->typing_link, t.link);
		t.link = ak_cstr(rf->typing_link);
		rf->typing = t;
		rf->typing_set = true;
		report_formats(n);
		return;
	}
	restyle_ctx c = {fn, arg, {0}};
	field_runs(n, s, e, restyle_run, &c);
	if (!c.changes.len) {
		report_formats(n);
		return;
	}
	gtk_text_buffer_begin_user_action(buf);
	for (uint32_t i = 0; i < c.changes.len; i++) {
		GtkTextIter a, b;
		gtk_text_buffer_get_iter_at_offset(buf, &a, c.changes.data[i].start);
		gtk_text_buffer_get_iter_at_offset(buf, &b, c.changes.data[i].end);
		if (c.changes.data[i].old) gtk_text_buffer_remove_tag(buf, c.changes.data[i].old, &a, &b);
		if (c.changes.data[i].new_) gtk_text_buffer_apply_tag(buf, c.changes.data[i].new_, &a, &b);
	}
	gtk_text_buffer_end_user_action(buf);
	ak_vec_free(&c.changes);
	rich_changed(n);
}

typedef struct {
	uint8_t bits;
	bool on;
} fmt_arg;

static ak_rich_style apply_format(ak_rich_style s, const void *arg) {
	const fmt_arg *a = arg;
	return rich_formatted(s, a->bits, a->on);
}

static ak_rich_style apply_link(ak_rich_style s, const void *arg) {
	s.link = *(const ak_span *)arg;
	return s;
}

// format_field is the Format command: u8 bits, u8 how (0 off, 1 on, 2
// toggle).
static void format_field(node *n, const uint8_t *args, size_t len) {
	field *f = n->field;
	if (!f || !f->rich || !f->buffer || len < 2) return;
	uint8_t bits = args[0], how = args[1];
	ak_span link;
	uint8_t has = formats_of(n, &link);
	bool on = how == 1;
	if (how == 2) on = (has & bits) != bits;
	fmt_arg a = {bits, on};
	restyle(n, apply_format, &a, false);
}

void lx_field_command(node *n, ak_cmd c, ak_span args) {
	field *f = n->field;
	switch (c) {
	case AK_CMD_FORMAT:
		format_field(n, (const uint8_t *)args.p, args.len);
		break;
	case AK_CMD_SET_LINK:
		if (f && f->rich && f->buffer) {
			char *url = str_dup(args);
			ak_span s = ak_cstr(url);
			restyle(n, apply_link, &s, true);
			free(url);
		}
		break;
	default:
		lx_command_more(n, c, args);
	}
}

// ---- Typing ----

// on_inserting notes text going into a text view's buffer (its
// "insert-text", before it's in): where, how long, and in a rich field
// the style it's typed in.
static void on_inserting(GtkTextBuffer *buf, GtkTextIter *it, const char *text, int length, node *n) {
	(void)buf;
	field *f = n->field;
	insertion ins = {.at = gtk_text_iter_get_offset(it), .count = length > 0 ? (int)g_utf8_strlen(text, length) : 0};
	if (f->rich && !f->rich->setting) {
		ins.style = typing_style(n, ins.at);
		ins.link = str_dup(ins.style.link);
		ins.style.link = ak_cstr(ins.link);
		ins.typed = true;
	}
	ak_vec_push(&f->pending, ins);
}

typedef struct {
	GtkTextTag *t;
} untagged_ctx;

static void tag_untagged(node *n, int start, int end, const ak_rich_style *s, GtkTextTag *tag, void *data) {
	(void)s;
	untagged_ctx *c = data;
	if (tag) return;
	GtkTextIter a, b;
	gtk_text_buffer_get_iter_at_offset(n->field->buffer, &a, start);
	gtk_text_buffer_get_iter_at_offset(n->field->buffer, &b, end);
	gtk_text_buffer_apply_tag(n->field->buffer, c->t, &a, &b);
}

// field_inserted tags what went in: the node's font, and in a rich field
// the style typed text takes (where it has none).
static void field_inserted(node *n) {
	field *f = n->field;
	if (!f->buffer || !f->pending.len) return;
	GtkTextBuffer *buf = f->buffer;
	int total = gtk_text_buffer_get_char_count(buf);
	uint32_t count = f->pending.len;
	insertion *pending = malloc(count * sizeof *pending);
	if (!pending) abort();
	memcpy(pending, f->pending.data, count * sizeof *pending);
	f->pending.len = 0;
	for (uint32_t i = 0; i < count; i++) {
		insertion *ins = &pending[i];
		int s = ins->at < total ? ins->at : total, e = ins->at + ins->count < total ? ins->at + ins->count : total;
		if (s < e) {
			GtkTextIter a, b;
			gtk_text_buffer_get_iter_at_offset(buf, &a, s);
			gtk_text_buffer_get_iter_at_offset(buf, &b, e);
			gtk_text_buffer_apply_tag(buf, f->base, &a, &b);
			if (ins->typed) {
				untagged_ctx c = {style_tag(n, &ins->style)};
				if (c.t) field_runs(n, s, e, tag_untagged, &c);
			}
		}
		free(ins->link);
	}
	free(pending);
}

static void on_changed(gpointer w, node *n) {
	(void)w;
	field_inserted(n);
	update_hint(n);
	if (n->updating) return;
	char *t = field_text(n);
	free(n->text);
	n->text = t;
	lx_emit(n, (ak_event){.kind = AK_EVENT_TEXT_CHANGED, .s = ak_cstr(t)});
	rich_changed(n);
	report_selection(n);  // a multiline one grows: the core measures it again
	lx_suggest_typed(n);  // its suggestions show (suggest.c)
}

static void on_activate(GtkEntry *e, node *n) {
	(void)e;
	lx_emit(n, (ak_event){.kind = AK_EVENT_SUBMIT});
}

static void on_sel_prop(GObject *o, GParamSpec *ps, node *n) {
	(void)o;
	(void)ps;
	report_selection(n);
}

static void on_mark_set(GtkTextBuffer *buf, GtkTextIter *it, GtkTextMark *mark, node *n) {
	(void)it;
	if (mark != gtk_text_buffer_get_insert(buf) && mark != gtk_text_buffer_get_selection_bound(buf)) return;
	field *f = n->field;
	if (f->rich && !f->rich->setting && !f->pending.len) f->rich->typing_set = false;  // the cursor moved
	report_selection(n);
	report_formats(n);
}

static void on_focus_enter(GtkEventControllerFocus *c, node *n) {
	(void)c;
	lx_suggest_focus(n, true);
	lx_emit(n, (ak_event){.kind = AK_EVENT_FOCUS});
}

static void on_focus_leave(GtkEventControllerFocus *c, node *n) {
	(void)c;
	lx_suggest_focus(n, false);
	lx_emit(n, (ak_event){.kind = AK_EVENT_BLUR});
}

// rich_key: Ctrl+B, Ctrl+I and Ctrl+U toggle bold, italic and underline.
static gboolean on_rich_key(GtkEventControllerKey *k, guint keyval, guint keycode, GdkModifierType state, node *n) {
	(void)k;
	(void)keycode;
	if (!n->field || !n->field->rich || (state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SHIFT_MASK | GDK_SUPER_MASK)) != GDK_CONTROL_MASK) return 0;
	uint8_t bits;
	switch (keyval) {
	case 'b':
	case 'B':
		bits = 1;
		break;
	case 'i':
	case 'I':
		bits = 2;
		break;
	case 'u':
	case 'U':
		bits = 4;
		break;
	default:
		return 0;
	}
	uint8_t args[2] = {bits, 2};
	format_field(n, args, 2);
	return 1;
}

static void paste_content(node *n, GtkWidget *w);

static void on_paste(GtkWidget *w, node *n) { paste_content(n, w); }

static void on_preedit(GtkWidget *w, const char *s, gpointer d) {
	(void)d;
	g_object_set_data((GObject *)w, "asuka-preedit", s && *s ? (gpointer)1 : NULL);
}

// lx_watch_preedit follows an input method composing in a widget (keys.c
// leaves keys to it meanwhile).
void lx_watch_preedit(GtkWidget *w) {
	if (w) g_signal_connect(w, "preedit-changed", G_CALLBACK(on_preedit), NULL);
}

bool lx_composing(GtkWidget *w);
bool lx_composing(GtkWidget *w) { return w && g_object_get_data((GObject *)w, "asuka-preedit") != NULL; }

static void connect_field(node *n) {
	field *f = n->field;
	GtkEventController *focus = gtk_event_controller_focus_new();
	lx_connect(n, focus, "enter", G_CALLBACK(on_focus_enter));
	lx_connect(n, focus, "leave", G_CALLBACK(on_focus_leave));
	if (f->editable) {
		GtkWidget *e = f->editable;
		lx_connect(n, e, "changed", G_CALLBACK(on_changed));
		lx_connect(n, e, "activate", G_CALLBACK(on_activate));
		lx_connect(n, e, "notify::cursor-position", G_CALLBACK(on_sel_prop));
		lx_connect(n, e, "notify::selection-bound", G_CALLBACK(on_sel_prop));
		GtkEditable *d = gtk_editable_get_delegate((GtkEditable *)e);
		if (d) {
			lx_connect(n, d, "paste-clipboard", G_CALLBACK(on_paste));
			lx_watch_preedit((GtkWidget *)d);
		}
		gtk_widget_add_controller(e, focus);
		return;
	}
	GtkTextBuffer *buf = f->buffer;
	GtkWidget *tv = f->view;
	lx_connect(n, buf, "insert-text", G_CALLBACK(on_inserting));
	lx_connect(n, buf, "changed", G_CALLBACK(on_changed));
	lx_connect(n, buf, "mark-set", G_CALLBACK(on_mark_set));
	lx_connect(n, tv, "paste-clipboard", G_CALLBACK(on_paste));
	lx_watch_preedit(tv);
	GtkEventController *keys = gtk_event_controller_key_new();
	gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
	lx_connect(n, keys, "key-pressed", G_CALLBACK(on_rich_key));
	gtk_widget_add_controller(tv, keys);
	gtk_widget_add_controller(tv, focus);
}

// ---- Making and styling ----

static void apply_placeholder(node *n) {
	field *f = n->field;
	if (f->editable && !n->secure)
		gtk_entry_set_placeholder_text((GtkEntry *)f->editable, S(n->placeholder));
	else if (f->editable)
		g_object_set(f->editable, "placeholder-text", S(n->placeholder), NULL);
	else if (f->hint) {
		gtk_label_set_text((GtkLabel *)f->hint, S(n->placeholder));
		update_hint(n);
	}
}

// apply_purpose tells the input method what the field takes (its
// keyboard, its content type) and how to help (capitals, spelling).
static void apply_purpose(node *n) {
	field *f = n->field;
	GtkInputPurpose purpose = GTK_INPUT_PURPOSE_FREE_FORM;
	switch (f->keyboard) {
	case AK_KEYBOARD_NUMBER: purpose = GTK_INPUT_PURPOSE_DIGITS; break;
	case AK_KEYBOARD_DECIMAL: purpose = GTK_INPUT_PURPOSE_NUMBER; break;
	case AK_KEYBOARD_EMAIL: purpose = GTK_INPUT_PURPOSE_EMAIL; break;
	case AK_KEYBOARD_URL: purpose = GTK_INPUT_PURPOSE_URL; break;
	case AK_KEYBOARD_PHONE: purpose = GTK_INPUT_PURPOSE_PHONE; break;
	}
	unsigned hints = 0;
	switch (f->autocap) {
	case AK_AUTOCAPITALIZE_SENTENCES: hints |= GTK_INPUT_HINT_UPPERCASE_SENTENCES; break;
	case AK_AUTOCAPITALIZE_WORDS: hints |= GTK_INPUT_HINT_UPPERCASE_WORDS; break;
	case AK_AUTOCAPITALIZE_CHARACTERS: hints |= GTK_INPUT_HINT_UPPERCASE_CHARS; break;
	}
	switch (f->autocorrect) {
	case AK_AUTOCORRECT_YES: hints |= GTK_INPUT_HINT_SPELLCHECK | GTK_INPUT_HINT_WORD_COMPLETION; break;
	case AK_AUTOCORRECT_NO: hints |= GTK_INPUT_HINT_NO_SPELLCHECK; break;
	}
	const char *ct = S(f->content_type);
	const unsigned soft = GTK_INPUT_HINT_UPPERCASE_SENTENCES | GTK_INPUT_HINT_UPPERCASE_WORDS | GTK_INPUT_HINT_SPELLCHECK;
	if (!strcmp(ct, "password") || !strcmp(ct, "newPassword")) {
		purpose = GTK_INPUT_PURPOSE_PASSWORD;
		hints = GTK_INPUT_HINT_NO_SPELLCHECK | GTK_INPUT_HINT_PRIVATE;
	} else if (!strcmp(ct, "oneTimeCode")) {
		purpose = GTK_INPUT_PURPOSE_PIN;
		hints = GTK_INPUT_HINT_NO_SPELLCHECK;
	} else if (!strcmp(ct, "email")) {
		purpose = GTK_INPUT_PURPOSE_EMAIL;
		hints = (hints & ~soft) | GTK_INPUT_HINT_NO_SPELLCHECK;
	} else if (!strcmp(ct, "phone")) {
		purpose = GTK_INPUT_PURPOSE_PHONE;
	} else if (!strcmp(ct, "name") || !strcmp(ct, "givenName") || !strcmp(ct, "familyName")) {
		purpose = GTK_INPUT_PURPOSE_NAME;
	} else if (!strcmp(ct, "cardNumber") || !strcmp(ct, "postalCode")) {
		if (!strcmp(ct, "cardNumber")) purpose = GTK_INPUT_PURPOSE_DIGITS;
		hints |= GTK_INPUT_HINT_NO_SPELLCHECK;
	} else if (!strcmp(ct, "username") || !strcmp(ct, "url")) {
		if (!strcmp(ct, "url")) purpose = GTK_INPUT_PURPOSE_URL;
		hints = (hints & ~soft) | GTK_INPUT_HINT_NO_SPELLCHECK;
	}
	if (f->view) {
		gtk_text_view_set_input_purpose((GtkTextView *)f->view, purpose);
		gtk_text_view_set_input_hints((GtkTextView *)f->view, (GtkInputHints)hints);
	} else if (f->editable && !n->secure) {
		gtk_entry_set_input_purpose((GtkEntry *)f->editable, purpose);
		gtk_entry_set_input_hints((GtkEntry *)f->editable, (GtkInputHints)hints);
	}
}

// style_base_tag sets the base tag's look: the node's font and color.
static void style_base_tag(node *n) {
	GtkTextTag *t = n->field->base;
	if (!t) return;
	float size;
	int weight;
	lx_font(n, &size, &weight);
	PangoFontDescription *desc = pango_font_description_new();
	const char *fam = lx_family(n);
	if (*fam) pango_font_description_set_family(desc, fam);
	pango_font_description_set_absolute_size(desc, size * 1024.0);
	pango_font_description_set_weight(desc, (PangoWeight)weight);
	pango_font_description_set_style(desc, n->font_traits & 1 ? PANGO_STYLE_ITALIC : PANGO_STYLE_NORMAL);
	set_boxed(t, "font-desc", pango_font_description_get_type(), desc);
	pango_font_description_free(desc);
	// Its letter spacing and font features, as a label's attributes.
	float k = H.font_scale > 0 ? H.font_scale : 1;
	g_object_set(t, "letter-spacing", (int)lroundf(n->letter_spacing * k * 1024), NULL);
	if (n->font_traits & 2)
		g_object_set(t, "font-features", "tnum=1", NULL);
	else
		g_object_set(t, "font-features-set", 0, NULL);
	g_object_set(t, "variant", n->font_traits & 4 ? PANGO_VARIANT_SMALL_CAPS : PANGO_VARIANT_NORMAL, NULL);
	GdkRGBA c;
	if (lx_resolve(n->color, &c))
		set_boxed(t, "foreground-rgba", gdk_rgba_get_type(), &c);
	else
		g_object_set(t, "foreground-set", 0, NULL);
}

// lx_style_field sets a field's font, color and alignment: an entry's as
// attributes, a text view's as its base tag (and a rich field's styles
// again, which resolve colors), and its placeholder's.
void lx_style_field(node *n) {
	field *f = n->field;
	if (!f) return;
	PangoAttrList *attrs = lx_attrs(n, true);
	float x = lx_xalign(n);
	if (f->editable && !n->secure) {
		gtk_entry_set_attributes((GtkEntry *)f->editable, attrs);
		gtk_entry_set_alignment((GtkEntry *)f->editable, x);
	} else if (f->editable) {
		GtkEditable *d = gtk_editable_get_delegate((GtkEditable *)f->editable);
		if (d) gtk_text_set_attributes((GtkText *)d, attrs);
	} else if (f->view) {
		GtkJustification j = GTK_JUSTIFY_LEFT;
		switch (lx_physical_align(n)) {
		case 1: j = GTK_JUSTIFY_CENTER; break;
		case 2: j = GTK_JUSTIFY_RIGHT; break;
		}
		gtk_text_view_set_justification((GtkTextView *)f->view, j);
		style_base_tag(n);
		restyle_tags(n);
		if (f->hint) {
			PangoAttrList *ha = lx_attrs(n, false);
			gtk_label_set_attributes((GtkLabel *)f->hint, ha);
			pango_attr_list_unref(ha);
			gtk_label_set_xalign((GtkLabel *)f->hint, x);
			gtk_label_set_justify((GtkLabel *)f->hint, j);
		}
	}
	pango_attr_list_unref(attrs);
}

static void set_rich_field(node *n, const uint8_t *b, size_t len);

void lx_make_field(node *n) {
	if (!n->field) {
		n->field = calloc(1, sizeof *n->field);
		if (!n->field) abort();
	}
	field *f = n->field;
	ak_buf keep = {0};  // a rich field's styled text, into the new view
	bool kept = false;
	if (f->rich && f->buffer) {
		encode_rich(n, &keep);
		rich_reset(f->rich);
		kept = true;
	}
	char *text = n->text ? strdup(n->text) : NULL;
	f->editable = f->view = f->hint = NULL;
	f->buffer = NULL;
	f->base = NULL;
	free_pending(f);
	GtkWidget *w;
	if (n->multiline) {
		GtkWidget *sw = gtk_scrolled_window_new();
		gtk_scrolled_window_set_policy((GtkScrolledWindow *)sw, GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
		GtkWidget *tv = gtk_text_view_new();
		gtk_text_view_set_wrap_mode((GtkTextView *)tv, GTK_WRAP_WORD_CHAR);
		gtk_text_view_set_accepts_tab((GtkTextView *)tv, 0);
		gtk_widget_add_css_class(tv, "asuka-clear");
		gtk_scrolled_window_set_child((GtkScrolledWindow *)sw, tv);
		int px, py;
		fpad(n, &px, &py);
		gtk_text_view_set_top_margin((GtkTextView *)tv, py);
		gtk_text_view_set_bottom_margin((GtkTextView *)tv, py);
		gtk_text_view_set_left_margin((GtkTextView *)tv, px);
		gtk_text_view_set_right_margin((GtkTextView *)tv, px);
		if (!n->plain) {
			gtk_widget_add_css_class(sw, "asuka-field");
			gtk_widget_set_overflow(sw, GTK_OVERFLOW_HIDDEN);
		}
		// The placeholder: a dim label where the text starts, shown while
		// there's none.
		GtkWidget *hint = gtk_label_new("");
		gtk_widget_add_css_class(hint, "dim-label");
		gtk_label_set_wrap((GtkLabel *)hint, 1);
		gtk_label_set_wrap_mode((GtkLabel *)hint, PANGO_WRAP_WORD_CHAR);
		gtk_label_set_xalign((GtkLabel *)hint, 0);
		gtk_widget_set_can_target(hint, 0);
		gtk_widget_set_valign(hint, GTK_ALIGN_START);
		gtk_widget_set_margin_start(hint, px);
		gtk_widget_set_margin_end(hint, px);
		gtk_widget_set_margin_top(hint, py);
		GtkWidget *ov = gtk_overlay_new();
		gtk_overlay_set_child((GtkOverlay *)ov, sw);
		gtk_overlay_add_overlay((GtkOverlay *)ov, hint);
		f->view = tv;
		f->buffer = gtk_text_view_get_buffer((GtkTextView *)tv);
		f->hint = hint;
		// The node's font: the lowest tag, on all the text.
		f->base = gtk_text_tag_new(NULL);
		gtk_text_tag_table_add(gtk_text_buffer_get_tag_table(f->buffer), f->base);
		g_object_unref(f->base);
		w = ov;
	} else if (n->secure) {
		w = gtk_password_entry_new();
		gtk_password_entry_set_show_peek_icon((GtkPasswordEntry *)w, 1);
		f->editable = w;
	} else {
		w = gtk_entry_new();
		f->editable = w;
	}
	lx_set_inner(n, w);
	if (n->plain && f->editable) {
		if (!n->secure) gtk_entry_set_has_frame((GtkEntry *)w, 0);
		gtk_widget_add_css_class(w, "asuka-plain");
	}
	connect_field(n);
	lx_style_field(n);
	set_field_text(n, text);
	free(text);
	if (kept) set_rich_field(n, keep.data, keep.len);
	ak_buf_free(&keep);
	apply_placeholder(n);
	apply_purpose(n);
	lx_suggest_attach(n);
	if (!str_empty(n->a11y_label)) lx_update_a11y(n);
	if (n->disabled) gtk_widget_set_sensitive(n->widget, 0);
}

// set_rich_field is the Rich prop: the field's text, styled.
static void set_rich_field(node *n, const uint8_t *b, size_t len) {
	field *f = n->field;
	if (!f->rich) {
		f->rich = calloc(1, sizeof *f->rich);
		if (!f->rich) abort();
	}
	if (!f->view) {
		n->multiline = true;
		lx_make_field(n);
		f = n->field;
	}
	ak_buf cur = {0};
	encode_rich(n, &cur);
	bool same = cur.len == len && (len == 0 || memcmp(cur.data, b, len) == 0);
	ak_buf_free(&cur);
	if (same) return;
	rich_field *rf = f->rich;
	GtkTextBuffer *buf = f->buffer;
	int ss, se;
	selection(n, &ss, &se);
	rf->setting = n->updating = true;
	gtk_text_buffer_begin_irreversible_action(buf);
	gtk_text_buffer_set_text(buf, "", 0);
	ak_rich_run_vec runs = {0};
	ak_decode_rich(b, len, &runs);
	for (uint32_t i = 0; i < runs.len; i++) {
		ak_rich_run *run = &runs.data[i];
		if ((run->style.flags & AK_RICH_INLINE) || !run->text.len) continue;
		int start = gtk_text_buffer_get_char_count(buf);
		GtkTextIter end;
		gtk_text_buffer_get_end_iter(buf, &end);
		gtk_text_buffer_insert(buf, &end, run->text.p, (int)run->text.len);
		GtkTextTag *t = style_tag(n, &run->style);
		if (t) {
			GtkTextIter a;
			gtk_text_buffer_get_iter_at_offset(buf, &a, start);
			gtk_text_buffer_get_end_iter(buf, &end);
			gtk_text_buffer_apply_tag(buf, t, &a, &end);
		}
	}
	ak_vec_free(&runs);
	gtk_text_buffer_end_irreversible_action(buf);
	int count = gtk_text_buffer_get_char_count(buf);
	GtkTextIter a, c;
	gtk_text_buffer_get_iter_at_offset(buf, &a, ss < count ? ss : count);
	gtk_text_buffer_get_iter_at_offset(buf, &c, se < count ? se : count);
	gtk_text_buffer_select_range(buf, &a, &c);
	rf->typing_set = false;
	rf->setting = n->updating = false;
	free(n->text);
	n->text = field_text(n);
	update_hint(n);
	report_formats(n);
}

void lx_set_field_prop(node *n, ak_prop p, const ak_value *v) {
	field *f = n->field;
	switch (p) {
	case AK_PROP_TEXT_FIELD_VALUE: {
		char *t = str_dup(v->s);
		set_field_text(n, t);
		free(t);
		break;
	}
	case AK_PROP_TEXT_FIELD_PLACEHOLDER:
		str_set(&n->placeholder, v->s);
		apply_placeholder(n);
		break;
	case AK_PROP_TEXT_FIELD_SECURE:
		if (n->secure != (v->i != 0)) {
			n->secure = v->i != 0;
			lx_make_field(n);
		}
		break;
	case AK_PROP_TEXT_FIELD_MULTILINE:
		if (n->multiline != (v->i != 0)) {
			n->multiline = v->i != 0;
			lx_make_field(n);
		}
		break;
	case AK_PROP_TEXT_FIELD_PLAIN:
		if (n->plain != (v->i != 0)) {
			n->plain = v->i != 0;
			lx_make_field(n);
		}
		break;
	case AK_PROP_TEXT_FIELD_KEYBOARD:
		f->keyboard = (int32_t)v->i;
		apply_purpose(n);
		break;
	case AK_PROP_TEXT_FIELD_RETURN_KEY:
		f->ret = (int32_t)v->i;  // a touch keyboard's label: none on the desktop
		break;
	case AK_PROP_TEXT_FIELD_AUTOCAPITALIZE:
		f->autocap = (int32_t)v->i;
		apply_purpose(n);
		break;
	case AK_PROP_TEXT_FIELD_AUTOCORRECT:
		f->autocorrect = (int32_t)v->i;
		apply_purpose(n);
		break;
	case AK_PROP_TEXT_FIELD_CONTENT_TYPE:
		str_set(&f->content_type, v->s);
		apply_purpose(n);
		break;
	case AK_PROP_TEXT_FIELD_SELECTION: {
		ak_reader r = {(const uint8_t *)v->s.p, v->s.len, 0, false};
		uint32_t s = ak_read_u32(&r), e = ak_read_u32(&r);
		set_selection(n, s, e);
		break;
	}
	case AK_PROP_TEXT_FIELD_RICH:
		set_rich_field(n, (const uint8_t *)v->s.p, v->s.len);
		break;
	case AK_PROP_TEXT_FIELD_PASTE_TYPES:
		ak_buf_reset(&f->paste_buf);
		ak_buf_put(&f->paste_buf, v->s.p, v->s.len);
		f->paste_types.len = 0;
		ak_decode_strings(f->paste_buf.data, f->paste_buf.len, &f->paste_types);
		break;
	default:
		lx_set_own_more(n, p, v);
	}
}

// ---- Measuring ----

typedef struct {
	ak_buf text;
	PangoAttrList *list;
	node *n;
	size_t from, to;
} layout_ctx;

static void add_attr(PangoAttribute *a, void *data) {
	layout_ctx *c = data;
	a->start_index = (guint)c->from;
	a->end_index = (guint)c->to;
	pango_attr_list_insert(c->list, a);
}

static void layout_run(node *n, int start, int end, const ak_rich_style *s, GtkTextTag *tag, void *data) {
	(void)tag;
	layout_ctx *c = data;
	char *t = field_slice(n, start, end);
	c->from = c->text.len;
	ak_buf_put(&c->text, t, strlen(t));
	c->to = c->text.len;
	free(t);
	lx_run_attrs(n, s, add_attr, c);
}

// lx_measure_multiline is a multiline field's height at width w: its text
// laid out as the text view lays it out (its font, a rich field's styles)
// inside its padding, at least a line, at most its line limit; -1 for a
// field that isn't multiline.
float lx_measure_multiline(node *n, float w) {
	field *f = n->field;
	if (!f || !f->view) return -1;
	int px, py;
	fpad(n, &px, &py);
	layout_ctx c = {.list = lx_attrs(n, false), .n = n};
	if (f->rich) {
		field_runs(n, 0, gtk_text_buffer_get_char_count(f->buffer), layout_run, &c);
	} else {
		char *t = field_text(n);
		ak_buf_put(&c.text, t, strlen(t));
		free(t);
	}
	PangoLayout *pl = gtk_widget_create_pango_layout(f->view, lx_buf_cstr(&c.text));
	pango_layout_set_attributes(pl, c.list);
	pango_attr_list_unref(c.list);
	ak_buf_free(&c.text);
	pango_layout_set_wrap(pl, PANGO_WRAP_WORD_CHAR);
	float inner = w - 2 * px;
	pango_layout_set_width(pl, (int)((inner > 1 ? inner : 1) * 1024));
	int pw, ph;
	pango_layout_get_pixel_size(pl, &pw, &ph);
	g_object_unref(pl);
	float ht = (float)ph, line = lx_line_height(n);
	if (n->line_limit > 0 && ht > line * n->line_limit) ht = line * n->line_limit;
	ht = (ht > line ? ht : line) + 2 * py;
	if (!n->plain && ht < ENTRY_HEIGHT) ht = ENTRY_HEIGHT;  // a line: as tall as an entry
	return ht;
}

// ---- Commands ----

// lx_insert_text replaces the selection with s, as if typed: one undoable
// change, reported as typing is.
void lx_insert_text(node *n, ak_span s) {
	field *f = n->field;
	if (!f) return;
	char *t = str_dup(s);
	if (f->editable) {
		gtk_editable_delete_selection((GtkEditable *)f->editable);
		int pos = gtk_editable_get_position((GtkEditable *)f->editable);
		gtk_editable_insert_text((GtkEditable *)f->editable, t, -1, &pos);
		gtk_editable_set_position((GtkEditable *)f->editable, pos);
	} else {
		gtk_text_buffer_begin_user_action(f->buffer);
		gtk_text_buffer_delete_selection(f->buffer, 1, 1);
		gtk_text_buffer_insert_interactive_at_cursor(f->buffer, t, -1, 1);
		gtk_text_buffer_end_user_action(f->buffer);
	}
	free(t);
}

void lx_focus_field(node *n) {
	field *f = n->field;
	if (!f) return;
	if (f->editable)
		gtk_widget_grab_focus(f->editable);
	else if (f->view)
		gtk_widget_grab_focus(f->view);
}

node *lx_field_of_widget(GtkWidget *w) {
	for (GtkWidget *p = w; p; p = gtk_widget_get_parent(p)) {
		node *v = lx_view_node(p);
		if (v && v->kind == AK_KIND_TEXT_FIELD && v->field && (v->field->editable == w || v->field->view == w || v->inner == w)) return v;
		if (v) return NULL;
	}
	return NULL;
}

// lx_field_widget is the widget a field's accessibility is the
// control's.
GtkWidget *lx_field_widget(node *n);
GtkWidget *lx_field_widget(node *n) { return n->field ? (n->field->editable ? n->field->editable : n->field->view) : NULL; }

// ---- Pastes of content the field takes ----

static uint8_t share_kind(const char *m) {
	if (!strncmp(m, "image/", 6)) return AK_SHARE_KIND_IMAGE;
	if (!strncmp(m, "video/", 6)) return AK_SHARE_KIND_VIDEO;
	if (!strncmp(m, "audio/", 6)) return AK_SHARE_KIND_AUDIO;
	return AK_SHARE_KIND_FILE;
}

// paste_dir is a new folder for a paste's files in the app's caches
// (malloc'd).
static char *paste_dir(void) {
	static int seq;
	char name[64];
	snprintf(name, sizeof name, "%lld-%d", (long long)g_get_real_time() * 1000, ++seq);
	char *base = dsk_join(g_get_user_cache_dir(), S(H.app_id));
	char *pastes = dsk_join(base, "Pastes");
	char *dir = dsk_join(pastes, name);
	free(base);
	free(pastes);
	dsk_mkdirs(dir);
	return dir;
}

static char *write_paste(const char *name, const void *data, size_t n) {
	char *dir = paste_dir();
	char *p = dsk_join(dir, name);
	free(dir);
	if (!dsk_write_file(p, data, n)) {
		free(p);
		return NULL;
	}
	return p;
}

// A paste in flight: its field by ID (it may go meanwhile).
typedef struct {
	ak_id id;
	GdkClipboard *cb;
	bool text;
	char *mime;
	GInputStream *in;
	GOutputStream *out;
} paste;

static node *paste_node(paste *p) {
	node *n = node_get(p->id);
	return n && n->field ? n : NULL;
}

static void insert_text_cb(const char *s, bool ok, void *ctx) {
	node *n = node_get((ak_id)(uintptr_t)ctx);
	if (ok && n && n->field) lx_insert_text(n, ak_cstr(s));
}

// pasted reports the items pasted into a field still there; with none,
// its text paste goes ahead after all (a file of a type it doesn't take).
static void pasted(paste *p, const ak_item *items, uint32_t count) {
	node *n = paste_node(p);
	if (n) {
		if (count) {
			ak_buf b = {0};
			ak_append_items(&b, items, count);
			lx_emit(n, (ak_event){.kind = AK_EVENT_PASTED, .s = ak_strn(b.data, b.len)});
			ak_buf_free(&b);
		} else if (p->text) {
			lx_read_clipboard_text(insert_text_cb, (void *)(uintptr_t)p->id);
		}
	}
	free(p->mime);
	free(p);
}

static char *guess_mime(const char *path) {
	char *ct = g_content_type_guess(path, NULL, 0, NULL);
	char *m = ct ? g_content_type_get_mime_type(ct) : NULL;
	g_free(ct);
	char *out = strdup(m && *m ? m : "application/octet-stream");
	g_free(m);
	return out;
}

// copy_to_pastes copies another app's file into the app's caches.
static char *copy_to_pastes(const char *src, int64_t *size) {
	size_t n;
	char *data = dsk_read_file(src, &n);
	if (!data) return NULL;
	char *p = write_paste(dsk_base(src), data, n);
	free(data);
	*size = (int64_t)n;
	return p;
}

static void files_read(GObject *src, GAsyncResult *res, gpointer data) {
	paste *p = data;
	GError *err = NULL;
	const GValue *v = gdk_clipboard_read_value_finish((GdkClipboard *)src, res, &err);
	if (err) g_error_free(err);
	AK_VEC(ak_item) items = {0};
	AK_VEC(char *) owned = {0};
	node *n = paste_node(p);
	if (v && n) {
		GdkFileList *list = g_value_get_boxed(v);
		GSList *files = list ? gdk_file_list_get_files(list) : NULL;
		for (GSList *l = files; l; l = l->next) {
			char *path = g_file_get_path(l->data);
			if (!path) continue;
			char *m = guess_mime(path);
			if (ak_match_mime(n->field->paste_types.data, n->field->paste_types.len, ak_cstr(m))) {
				int64_t size = 0;
				char *dst = copy_to_pastes(path, &size);
				if (dst) {
					char *name = strdup(dsk_base(path));
					ak_vec_push(&owned, dst);
					ak_vec_push(&owned, name);
					ak_vec_push(&owned, m);
					ak_item it = {.kind = share_kind(m), .path = ak_cstr(dst), .name = ak_cstr(name), .mime = ak_cstr(m), .size = size};
					ak_vec_push(&items, it);
					m = NULL;
				}
			}
			free(m);
			g_free(path);
		}
		g_slist_free(files);
	}
	pasted(p, items.data, items.len);
	for (uint32_t i = 0; i < owned.len; i++) free(owned.data[i]);
	ak_vec_free(&owned);
	ak_vec_free(&items);
}

static void texture_read(GObject *src, GAsyncResult *res, gpointer data) {
	paste *p = data;
	GError *err = NULL;
	GdkTexture *t = gdk_clipboard_read_texture_finish((GdkClipboard *)src, res, &err);
	if (err) g_error_free(err);
	ak_item it = {0};
	uint32_t count = 0;
	char *path = NULL;
	if (t) {
		GBytes *b = gdk_texture_save_to_png_bytes(t);
		g_object_unref(t);
		if (b) {
			gsize size;
			const void *d = g_bytes_get_data(b, &size);
			path = write_paste("Image.png", d, size);
			if (path) {
				it = (ak_item){.kind = AK_SHARE_KIND_IMAGE, .path = ak_cstr(path), .name = AK_STR("Image.png"), .mime = AK_STR("image/png"), .size = (int64_t)size};
				count = 1;
			}
			g_bytes_unref(b);
		}
	}
	p->text = false;
	pasted(p, &it, count);
	free(path);
}

static void data_spliced(GObject *src, GAsyncResult *res, gpointer data) {
	paste *p = data;
	GError *err = NULL;
	g_output_stream_splice_finish((GOutputStream *)src, res, &err);
	if (err) g_error_free(err);
	GBytes *b = g_memory_output_stream_steal_as_bytes((GMemoryOutputStream *)p->out);
	g_object_unref(p->out);
	g_object_unref(p->in);
	ak_item it = {0};
	uint32_t count = 0;
	char *path = NULL, name[96] = "Pasted";
	if (b) {
		gsize size;
		const void *d = g_bytes_get_data(b, &size);
		// A file name with the type's extension, as the system knows it.
		char *ct = g_content_type_from_mime_type(p->mime);
		if (ct) {
			GList *globs = NULL;
			(void)globs;
			const char *slash = strchr(p->mime, '/');
			if (slash && strlen(slash + 1) < 40) snprintf(name, sizeof name, "Pasted.%s", slash + 1);
			g_free(ct);
		}
		if (size) {
			path = write_paste(name, d, size);
			if (path) {
				it = (ak_item){.kind = share_kind(p->mime), .path = ak_cstr(path), .name = ak_cstr(name), .mime = ak_cstr(p->mime), .size = (int64_t)size};
				count = 1;
			}
		}
		g_bytes_unref(b);
	}
	p->text = false;
	pasted(p, &it, count);
	free(path);
}

static void data_read(GObject *src, GAsyncResult *res, gpointer data) {
	paste *p = data;
	GError *err = NULL;
	const char *out_mime = NULL;
	p->in = gdk_clipboard_read_finish((GdkClipboard *)src, res, &out_mime, &err);
	if (err) g_error_free(err);
	if (!p->in) {
		p->text = false;
		pasted(p, NULL, 0);
		return;
	}
	p->out = g_memory_output_stream_new_resizable();
	g_output_stream_splice_async(p->out, p->in, G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE | G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET, 0, NULL, data_spliced, p);
}

typedef struct {
	ak_id id;
} text_paste;

static void text_pasted(const char *s, bool ok, void *ctx) {
	node *n = node_get((ak_id)(uintptr_t)ctx);
	if (!ok || !n) return;
	ak_item it = {.kind = AK_SHARE_KIND_TEXT, .text = ak_cstr(s), .mime = AK_STR("text/plain"), .size = -1};
	ak_buf b = {0};
	ak_append_items(&b, &it, 1);
	lx_emit(n, (ak_event){.kind = AK_EVENT_PASTED, .s = ak_strn(b.data, b.len)});
	ak_buf_free(&b);
}

// paste_content is a field's paste-clipboard: it takes the clipboard's
// content of the field's types, if it has any, and stops the paste of
// text.
static void paste_content(node *n, GtkWidget *w) {
	field *f = n->field;
	if (!f || !f->paste_types.len || !listening(n, AK_EVENT_PASTED)) return;
	GdkClipboard *cb = gdk_display_get_clipboard(gdk_display_get_default());
	GdkContentFormats *formats = gdk_clipboard_get_formats(cb);
	if (!formats) return;
	gsize count = 0;
	const char *const *mimes = gdk_content_formats_get_mime_types(formats, &count);
	bool has_text = false;
	for (gsize i = 0; i < count; i++)
		if (!strncmp(mimes[i], "text/plain", 10) || !strcmp(mimes[i], "UTF8_STRING") || !strcmp(mimes[i], "STRING")) has_text = true;
	paste *p = calloc(1, sizeof *p);
	if (!p) abort();
	p->id = n->id;
	p->cb = cb;
	if (gdk_content_formats_contain_gtype(formats, gdk_file_list_get_type())) {
		// Files (a file manager's copy): those of the field's types.
		g_signal_stop_emission_by_name(w, "paste-clipboard");
		p->text = has_text;
		gdk_clipboard_read_value_async(cb, gdk_file_list_get_type(), 0, NULL, files_read, p);
		return;
	}
	// Data of a type the field takes, as it is (a GIF stays a GIF).
	for (gsize i = 0; i < count; i++) {
		const char *m = mimes[i];
		if (!strncmp(m, "text/", 5) || !strchr(m, '/') || !strncmp(m, "application/x-", 14) || !ak_match_mime(f->paste_types.data, f->paste_types.len, ak_cstr(m))) continue;
		g_signal_stop_emission_by_name(w, "paste-clipboard");
		p->mime = strdup(m);
		const char *list[2] = {p->mime, NULL};
		gdk_clipboard_read_async(cb, list, 0, NULL, data_read, p);
		return;
	}
	if (gdk_content_formats_contain_gtype(formats, gdk_texture_get_type()) && ak_match_mime(f->paste_types.data, f->paste_types.len, AK_STR("image/png"))) {
		// An image GDK can read (as another app's copy offers it): a PNG.
		g_signal_stop_emission_by_name(w, "paste-clipboard");
		gdk_clipboard_read_texture_async(cb, NULL, texture_read, p);
		return;
	}
	if (has_text && ak_match_mime(f->paste_types.data, f->paste_types.len, AK_STR("text/plain"))) {
		g_signal_stop_emission_by_name(w, "paste-clipboard");
		lx_read_clipboard_text(text_pasted, (void *)(uintptr_t)n->id);
	}
	free(p);
}
