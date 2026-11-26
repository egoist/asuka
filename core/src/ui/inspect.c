// inspect.c: the app's views as asuka dev's inspector shows them
// (AK_DEV_INSPECT): each view's element, its native view's id, its text,
// its frame and its children, as JSON. The roots are the root's element
// and the providers of no parent (a sheet's content, a popover's, a
// window's), and a List's cells are its children, though neither is a
// child of the view it shows in.
#include "ui/ui_ext.h"

#include <stdio.h>

// json_str writes s as a JSON string.
static void json_str(ak_buf *b, const char *s, size_t n) {
	ak_buf_u8(b, '"');
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)s[i];
		switch (c) {
		case '"': ak_buf_put(b, "\\\"", 2); break;
		case '\\': ak_buf_put(b, "\\\\", 2); break;
		case '\n': ak_buf_put(b, "\\n", 2); break;
		case '\r': ak_buf_put(b, "\\r", 2); break;
		case '\t': ak_buf_put(b, "\\t", 2); break;
		default:
			if (c < 0x20) ak_buf_printf(b, "\\u%04x", c);
			else ak_buf_u8(b, c);
		}
	}
	ak_buf_u8(b, '"');
}

typedef struct {
	ak_buf *b;
	bool first;
	int depth;
} walk;

static void view_json(walk *w, ak_view *e);

static void cell_json(ak_view *c, void *ctx) { view_json(ctx, c); }

// A cap on depth: a view can't hold itself, but a bug in a hook mustn't
// take the app down with the inspector.
#define MAX_DEPTH 256

static void view_json(walk *w, ak_view *e) {
	if (!e || e->dead || w->depth > MAX_DEPTH) return;
	ak_buf *b = w->b;
	if (!w->first) ak_buf_u8(b, ',');
	w->first = false;
	ak_buf_printf(b, "{\"el\":");
	const char *name = e->cls && e->cls->name ? e->cls->name : (e->frag ? "Fragment" : "View");
	json_str(b, name, strlen(name));
	ak_buf_printf(b, ",\"id\":%u", (unsigned)e->id);
	if (e->text) {
		ak_buf_put(b, ",\"text\":", 8);
		json_str(b, e->text->data, e->text->len);
	}
	if (e->text2) {
		ak_buf_put(b, ",\"text2\":", 9);
		json_str(b, e->text2->data, e->text2->len);
	}
	if (e->real) ak_buf_printf(b, ",\"frame\":[%g,%g,%g,%g]", (double)e->sent[0], (double)e->sent[1], (double)e->sent[2], (double)e->sent[3]);
	if (e->frag) ak_buf_put(b, ",\"fragment\":true", 16);
	ak_buf_put(b, ",\"kids\":[", 9);
	walk inner = {b, true, w->depth + 1};
	for (uint32_t i = 0; i < e->kids.len; i++) view_json(&inner, e->kids.data[i]);
	if (e->kind == AK_KIND_LIST && e->el == AK_EL_LIST) ak_list_each_cell(e, cell_json, &inner);
	ak_buf_put(b, "]}", 2);
}

void ak_ui_inspect(ak_buf *out) {
	ak_ui_root *r = ak_ui.root;
	ak_buf_put(out, "{\"roots\":[", 10);
	walk w = {out, true, 0};
	if (r) {
		view_json(&w, r->el);
		// Sheets', popovers' and windows' contents: providers of no parent.
		for (uint32_t i = 0; i < r->provs.len; i++) {
			ak_view *v = r->provs.data[i];
			if (v && !v->dead && !v->parent && v != r->el) view_json(&w, v);
		}
	}
	ak_buf_put(out, "]}", 2);
}
