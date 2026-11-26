// inspect.c: the app's views as asuka dev's inspector shows them
// (AK_DEV_INSPECT): each view's element, its native view's id, its text,
// its frame and its children, as JSON. The roots are the root's element
// and the providers of no parent (a sheet's content, a popover's, a
// window's), and a List's cells are its children, though neither is a
// child of the view it shows in.
#include "ui/ui_ext.h"
#include "rt/sched.h"

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

// root_at is the tree's i'th root: the root's element, then the
// providers of no parent (sheets', popovers', windows' contents).
static ak_view *root_at(ak_ui_root *r, uint32_t i) {
	if (!r) return NULL;
	if (r->el && !r->el->dead) {
		if (i == 0) return r->el;
		i--;
	}
	for (uint32_t j = 0; j < r->provs.len; j++) {
		ak_view *v = r->provs.data[j];
		if (v && !v->dead && !v->parent && v != r->el && i-- == 0) return v;
	}
	return NULL;
}

void ak_ui_inspect(ak_buf *out) {
	ak_ui_root *r = ak_ui.root;
	ak_buf_put(out, "{\"roots\":[", 10);
	walk w = {out, true, 0};
	for (uint32_t i = 0;; i++) {
		ak_view *v = root_at(r, i);
		if (!v) break;
		view_json(&w, v);
	}
	ak_buf_put(out, "]}", 2);
}

// The i'th child as the tree has them: its children that aren't dead,
// then a List's cells.
typedef struct {
	uint32_t i;
	ak_view *found;
} nth;

static void cell_nth(ak_view *c, void *ctx) {
	nth *n = ctx;
	if (n->found || !c || c->dead) return;
	if (n->i == 0) n->found = c;
	else n->i--;
}

static ak_view *kid_at(ak_view *e, uint32_t i) {
	for (uint32_t j = 0; j < e->kids.len; j++) {
		ak_view *k = e->kids.data[j];
		if (k && !k->dead && i-- == 0) return k;
	}
	nth n = {i, NULL};
	if (e->kind == AK_KIND_LIST && e->el == AK_EL_LIST) ak_list_each_cell(e, cell_nth, &n);
	return n.found;
}

// ak_view_native_rect finds the view with a native view v is in (v
// itself, or for a view without one of its own, a layout's, the nearest
// around it that has one) and where v is in it: x, y, width, height in
// points; a width of -1 for all of it (v's own native view). False for a
// fragment, which has no box, or a view that's gone.
bool ak_view_native_rect(ak_view *v, ak_view **anchor, float rect[4]) {
	ak_view *a = v;
	rect[0] = rect[1] = 0;
	rect[2] = rect[3] = -1;
	if (v && !v->real) {
		if (!v->box) return false;
		ak_lframe fr = ak_lnode_frame(v->box);
		rect[0] = fr.x, rect[1] = fr.y, rect[2] = fr.w, rect[3] = fr.h;
		for (a = v->parent; a && !a->real; a = a->parent) {
			if (a->box) {
				ak_lframe pf = ak_lnode_frame(a->box);
				rect[0] += pf.x;
				rect[1] += pf.y;
			}
		}
	}
	// A view with a native view of its own is all of it (-1): the host's
	// bounds for it, as it placed it.
	if (!a || a->dead || !a->real || !a->id || !a->r || a->r->disposed) return false;
	*anchor = a;
	return true;
}

// What the highlight last showed: a view's id and the rect in it.
static struct {
	bool on;
	ak_id id;
	float rect[4];
	bool posted;
} H;

static void highlight_flush(void *ctx) {
	AK_UNUSED(ctx);
	H.posted = false;
	ak_ui_flush();
}

static void highlight_clear(ak_ui_root *r) {
	if (!H.on) return;
	H.on = false;
	if (r && r->el && !r->disposed) ak_batch_command(r->batch, r->el->id, AK_CMD_DEV_HIGHLIGHT_CLEAR, NULL, 0);
}

void ak_ui_highlight(ak_span path) {
	ak_ui_root *r = ak_ui.root;
	ak_view *v = NULL;
	if (path.len) {
		// "0.2.1": a root's index, then a child's at each level.
		uint32_t i = 0;
		bool digits = false, first = true;
		for (uint32_t k = 0; k <= path.len; k++) {
			char c = k < path.len ? path.p[k] : '.';
			if (c >= '0' && c <= '9' && i < 100000000) {
				i = i * 10 + (uint32_t)(c - '0');
				digits = true;
			} else if (c == '.' && digits) {
				v = first ? root_at(r, i) : kid_at(v, i);
				if (!v) break;
				first = false;
				i = 0;
				digits = false;
			} else {
				v = NULL;
				break;
			}
		}
	}
	ak_view *a = NULL;
	float rect[4];
	if (!v || !ak_view_native_rect(v, &a, rect)) a = NULL;
	if (!a || a->dead || !a->real || !a->id || !a->r || a->r->disposed) {
		highlight_clear(r);
	} else if (!H.on || H.id != a->id || memcmp(H.rect, rect, sizeof rect) != 0) {
		H.on = true;
		H.id = a->id;
		memcpy(H.rect, rect, sizeof rect);
		ak_buf args = {0};
		for (int k = 0; k < 4; k++) ak_buf_f32(&args, rect[k]);
		ak_batch_command(a->r->batch, a->id, AK_CMD_DEV_HIGHLIGHT, args.data, args.len);
		ak_buf_free(&args);
	}
	if (!H.posted) {
		H.posted = true;
		ak_post(highlight_flush, NULL);
	}
}
