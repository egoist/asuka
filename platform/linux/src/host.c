// host.c: the host's heart: the core's callbacks (asuka.h's apply,
// measure and post), events going up (queued while a call into the core
// is in progress, as GTK emits signals while a batch is applied), the
// nodes by ID, and the tree's inserts and removes.
#include "host.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

host H;

void str_set(char **dst, ak_span s) {
	free(*dst);
	*dst = str_dup(s);
}

void str_setc(char **dst, const char *s) {
	char *d = s ? strdup(s) : NULL;
	free(*dst);
	*dst = d;
}

const char *lx_buf_cstr(ak_buf *b) {
	ak_buf_reserve(b, 1);
	b->data[b->len] = 0;
	return (const char *)b->data;
}

char *str_dup(ak_span s) {
	char *d = malloc((size_t)s.len + 1);
	if (!d) abort();
	if (s.len) memcpy(d, s.p, s.len);
	d[s.len] = 0;
	return d;
}

void lx_log(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	fputs("asuka: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

void lx_fail(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	fputs("asuka: the Linux host ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
	abort();
}

// ---- Nodes by ID ----

static uint32_t slot(ak_id id, uint32_t cap) { return (id * 2654435761u) & (cap - 1); }

node *node_get(ak_id id) {
	if (!H.table || !id) return NULL;
	for (uint32_t i = slot(id, H.table_cap);; i = (i + 1) & (H.table_cap - 1)) {
		node *n = H.table[i];
		if (!n) return NULL;
		if (n != (node *)1 && n->id == id) return n;
	}
}

static void table_put(node *n);

static void table_grow(void) {
	uint32_t old = H.table_cap;
	node **t = H.table;
	H.table_cap = old ? old * 2 : 1024;
	H.table = calloc(H.table_cap, sizeof *H.table);
	if (!H.table) abort();
	H.table_len = 0;
	for (uint32_t i = 0; i < old; i++)
		if (t[i] && t[i] != (node *)1) table_put(t[i]);
	free(t);
}

static void table_put(node *n) {
	if ((H.table_len + 1) * 2 > H.table_cap) table_grow();
	for (uint32_t i = slot(n->id, H.table_cap);; i = (i + 1) & (H.table_cap - 1)) {
		if (!H.table[i] || H.table[i] == (node *)1) {
			H.table[i] = n;
			H.table_len++;
			return;
		}
	}
}

static void table_del(ak_id id) {
	if (!H.table) return;
	for (uint32_t i = slot(id, H.table_cap);; i = (i + 1) & (H.table_cap - 1)) {
		node *n = H.table[i];
		if (!n) return;
		if (n != (node *)1 && n->id == id) {
			H.table[i] = (node *)1;  // a tombstone
			return;
		}
	}
}

void lx_add_node(node *n);
void lx_add_node(node *n) { table_put(n); }

// lx_forget_node lets go of a view of the host's own (a list's row or
// cell): out of the nodes, its widget let go.
void lx_forget_node(node *n);
void lx_forget_node(node *n) {
	table_del(n->id);
	lx_release(n);
}

// lx_each_node calls f for every node.
void lx_each_node(void (*f)(node *n, void *ctx), void *ctx);
void lx_each_node(void (*f)(node *n, void *ctx), void *ctx) {
	for (uint32_t i = 0; i < H.table_cap; i++) {
		node *n = H.table[i];
		if (n && n != (node *)1) f(n, ctx);
	}
}

// ---- Calls into the core, events going up ----

static void drain(void);

void lx_enter_begin(void) { H.depth++; }

void lx_enter_end(void) {
	H.depth--;
	drain();
}

// drain sends what waited for the core to return: the work queued for
// then, and the events raised meanwhile, in order.
static void drain(void) {
	while (H.depth == 0 && (H.later.len > 0 || H.pending_off < H.pending.len)) {
		if (H.later.len > 0) {
			// Run in order; what they queue runs after.
			uint32_t n = H.later.len;
			later_t *fs = malloc(n * sizeof *fs);
			if (!fs) abort();
			memcpy(fs, H.later.data, n * sizeof *fs);
			memmove(H.later.data, H.later.data + n, (H.later.len - n) * sizeof *fs);
			H.later.len -= n;
			for (uint32_t i = 0; i < n; i++) fs[i].fn(fs[i].ctx);
			free(fs);
			continue;
		}
		ak_reader r = {H.pending.data, H.pending.len, H.pending_off, false};
		uint32_t len = ak_read_u32(&r);
		const uint8_t *p = H.pending.data + r.off;
		H.pending_off = r.off + len;
		// The event's bytes may move as more are queued: copy them out.
		uint8_t small[256];
		uint8_t *buf = len <= sizeof small ? small : malloc(len);
		if (!buf) abort();
		memcpy(buf, p, len);
		if (H.pending_off >= H.pending.len) {
			H.pending.len = 0;
			H.pending_off = 0;
		}
		H.depth++;
		asuka_event(buf, len);
		H.depth--;
		if (buf != small) free(buf);
	}
}

// lx_dispatch sends an event to the core, or queues it while a call
// into the core is in progress.
void lx_dispatch(const ak_event *ev) {
	if (!H.started) return;
	ak_buf_reset(&H.ev);
	ak_event_append(&H.ev, ev);
	if (H.depth > 0) {
		ak_buf_u32(&H.pending, (uint32_t)H.ev.len);
		ak_buf_put(&H.pending, H.ev.data, H.ev.len);
		return;
	}
	H.depth++;
	asuka_event(H.ev.data, H.ev.len);
	H.depth--;
	drain();
}

// lx_emit sends ev on n if the core listens for it there.
void lx_emit(node *n, ak_event ev) {
	if (!n || n->dead || !listening(n, ev.kind)) return;
	ev.id = n->id;
	lx_dispatch(&ev);
}

// lx_after_core runs fn once the call into the core in progress has
// returned (now, if none is).
void lx_after_core(later_fn fn, void *ctx) {
	if (H.depth == 0) {
		fn(ctx);
		return;
	}
	ak_vec_push(&H.later, ((later_t){fn, ctx}));
}

// ---- Posts: one FIFO ----
//
// What any thread posts (lx_post, asuka_desktop_post) and the core's own
// posts (its post callback: asuka_run_posted) run on the main thread in
// the order they were posted, from one queue drained by one idle source
// at a time (asuka_desktop.h: a package's notification posted before its
// async call answers runs before the answer).

typedef struct {
	later_fn fn;  // NULL: the core's (asuka_run_posted)
	void *ctx;
} posted;

static pthread_mutex_t post_mu = PTHREAD_MUTEX_INITIALIZER;
static AK_VEC(posted) post_queue;
static size_t post_head;
static bool post_scheduled;

static gboolean run_posts(gpointer data) {
	(void)data;
	for (;;) {
		pthread_mutex_lock(&post_mu);
		if (post_head == post_queue.len) {
			post_queue.len = 0;
			post_head = 0;
			post_scheduled = false;
			pthread_mutex_unlock(&post_mu);
			return 0;
		}
		posted p = post_queue.data[post_head++];
		pthread_mutex_unlock(&post_mu);
		H.depth++;
		if (p.fn)
			p.fn(p.ctx);
		else
			asuka_run_posted();
		H.depth--;
		drain();
	}
}

static void enqueue(later_fn fn, void *ctx) {
	pthread_mutex_lock(&post_mu);
	ak_vec_push(&post_queue, ((posted){fn, ctx}));
	bool schedule = !post_scheduled;
	post_scheduled = true;
	pthread_mutex_unlock(&post_mu);
	if (schedule) g_idle_add_full(G_PRIORITY_DEFAULT, run_posts, NULL, NULL);  // thread-safe
}

// lx_post runs fn(ctx) on the main thread soon, after what's posted
// before it. Any thread.
void lx_post(later_fn fn, void *ctx) { enqueue(fn, ctx); }

// ---- The core's callbacks ----

static void cb_post(void) { enqueue(NULL, NULL); }

static void cb_apply(const uint8_t *buf, size_t len) { lx_apply_batch(buf, len); }

static void cb_measure(uint32_t id, float width, float height, float aw, float ah, float *ow, float *oh) {
	lx_measure_cb(id, width, height, aw, ah, ow, oh);
}


asuka_host_callbacks lx_callbacks(void);
asuka_host_callbacks lx_callbacks(void) {
	asuka_host_callbacks cb = {0};
	cb.apply = cb_apply;
	cb.measure = cb_measure;
	cb.post = cb_post;
	asuka_native_callbacks(&cb);
	return cb;
}

// ---- Applying a batch ----

static void apply_op(const ak_op *op);
void lx_batch_applied(void);
void lx_destroy(ak_id id);
void lx_command(node *n, ak_cmd c, ak_span args);
void lx_set_root(node *n);
void lx_set_frame(node *n, const float f[4]);

void lx_apply_batch(const uint8_t *buf, size_t len) {
	ak_batch_reader br;
	ak_batch_reader_init(&br, buf, len);
	ak_op op;
	H.depth++;
	while (ak_batch_next(&br, &op)) apply_op(&op);
	if (br.err != AK_OK) lx_log("a batch the host can't read: %s", ak_err_string(br.err));
	lx_batch_applied();
	H.depth--;
	// What waited for the batch (events GTK raised as it was applied)
	// goes once the core's call that sent it returns: drain runs then,
	// from the call's own lx_enter_end or event loop turn.
	if (H.depth == 0) drain();
}

static void apply_op(const ak_op *op) {
	node *n;
	switch (op->code) {
	case AK_OP_CREATE:
		lx_create(op->id, op->kind);
		break;
	case AK_OP_DESTROY:
		lx_destroy(op->id);
		break;
	case AK_OP_INSERT: {
		node *p = node_get(op->id), *c = node_get(op->child);
		if (p && c) lx_insert(p, c, op->index);
		break;
	}
	case AK_OP_REMOVE: {
		node *p = node_get(op->id), *c = node_get(op->child);
		if (p && c) lx_remove(p, c);
		break;
	}
	case AK_OP_SET_PROP:
		if ((n = node_get(op->id))) {
			if (op->prop < AK_FIRST_KIND_PROP)
				lx_set_common(n, op->prop, &op->value);
			else
				lx_set_own(n, op->prop, &op->value);
		}
		break;
	case AK_OP_SET_FRAME:
		if ((n = node_get(op->id))) lx_set_frame(n, op->frame);
		break;
	case AK_OP_LISTEN:
		if ((n = node_get(op->id)) && op->event < 128) {
			n->listens[op->event / 64] |= UINT64_C(1) << (op->event % 64);
			lx_listen(n, op->event);
		}
		break;
	case AK_OP_UNLISTEN:
		if ((n = node_get(op->id)) && op->event < 128) {
			n->listens[op->event / 64] &= ~(UINT64_C(1) << (op->event % 64));
			lx_unlisten(n, op->event);
		}
		break;
	case AK_OP_COMMAND:
		if ((n = node_get(op->id))) lx_command(n, op->cmd, op->value.s);
		break;
	case AK_OP_SET_ROOT:
		if ((n = node_get(op->id))) lx_set_root(n);
		break;
	}
}

// ---- The tree ----

static void free_node(node *n);

static gboolean free_later(gpointer data) {
	free_node(data);
	return 0;
}

void lx_destroy(ak_id id) {
	node *n = node_get(id);
	if (!n) return;
	table_del(id);
	n->dead = true;
	if (lx_destroy_nav(n)) return;
	for (uint32_t i = 0; i < n->children.len; i++)
		if (n->children.data[i]->parent == n) n->children.data[i]->parent = NULL;
	// Going out with a transition: shown until it's gone.
	if (lx_exit(n)) {
		node *p = n->parent;
		if (p) {
			for (uint32_t i = 0; i < p->children.len; i++)
				if (p->children.data[i] == n) {
					memmove(p->children.data + i, p->children.data + i + 1, (p->children.len - i - 1) * sizeof(node *));
					p->children.len--;
					break;
				}
			n->parent = NULL;
		}
		return;
	}
	if (n->parent) lx_remove(n->parent, n);
	lx_release(n);
}

// lx_release lets go of n's widgets; its memory goes on the next idle
// turn, once no signal GTK is emitting can still name it.
void lx_release(node *n) {
	n->dead = true;
	lx_release_more(n);
	lx_release_events(n);
	lx_release_controls(n);
	if (n->inner) g_signal_handlers_disconnect_matched(n->inner, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, n);
	if (n->texture) {
		g_object_unref(n->texture);
		n->texture = NULL;
	}
	if (n->widget) {
		g_signal_handlers_disconnect_matched(n->widget, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, n);
		if (gtk_widget_get_parent(n->widget)) lx_detach(n->widget);
		lx_view_set_node(n->widget, NULL);
		g_object_unref(n->widget);  // the host's reference, taken as it was made
		n->widget = NULL;
	}
	if (H.root == n) H.root = NULL;
	g_idle_add_full(300, free_later, n, NULL);  // G_PRIORITY_DEFAULT_IDLE + 100
}

static void free_node(node *n) {
	lx_release_field(n);
	lx_release_a11y(n);
	lx_release_scroll(n);
	lx_release_image(n);
	lx_release_list(n);
	ak_vec_free(&n->children);
	free(n->font_family);
	free(n->title);
	free(n->message);
	free(n->icon);
	free(n->text);
	free(n->placeholder);
	free(n->source);
	free(n->symbol);
	free(n->a11y_label);
	for (uint32_t i = 0; i < n->items.len; i++) free(n->items.data[i]);
	ak_vec_free(&n->items);
	actions_free(&n->menu);
	actions_free(&n->actions);
	ak_vec_free(&n->gradient);
	ak_vec_free(&n->keys);
	ak_buf_free(&n->keys_buf);
	lx_nav_free(n);
	free(n);
}

// lx_detach takes a widget out of its parent, whatever kind the parent is.
void lx_detach(GtkWidget *w) {
	GtkWidget *p = gtk_widget_get_parent(w);
	if (!p) return;
	if (lx_view_node(p) || lx_is_view(p)) {
		gtk_widget_unparent(w);
		gtk_widget_queue_allocate(p);
		return;
	}
	if (AK_IS(p, gtk_viewport_get_type))
		gtk_viewport_set_child((GtkViewport *)p, NULL);
	else if (AK_IS(p, adw_navigation_page_get_type))
		adw_navigation_page_set_child((AdwNavigationPage *)p, NULL);
	else if (AK_IS(p, adw_toolbar_view_get_type))
		adw_toolbar_view_set_content((AdwToolbarView *)p, NULL);
	else if (AK_IS(p, adw_bin_get_type))
		adw_bin_set_child((AdwBin *)p, NULL);
	else if (AK_IS(p, adw_dialog_get_type))
		adw_dialog_set_child((AdwDialog *)p, NULL);
	else if (AK_IS(p, gtk_popover_get_type))
		gtk_popover_set_child((GtkPopover *)p, NULL);
	else if (AK_IS(p, gtk_box_get_type))
		gtk_box_remove((GtkBox *)p, w);
	else if (AK_IS(p, gtk_fixed_get_type))
		gtk_fixed_remove((GtkFixed *)p, w);  // a zoomable Scroll's content (scroll.c)
	else if (AK_IS(p, gtk_overlay_get_type) && gtk_overlay_get_child((GtkOverlay *)p) == w)
		gtk_overlay_set_child((GtkOverlay *)p, NULL);
	else if (AK_IS(p, gtk_overlay_get_type))
		gtk_overlay_remove_overlay((GtkOverlay *)p, w);
	else if (AK_IS(p, gtk_scrolled_window_get_type))
		gtk_scrolled_window_set_child((GtkScrolledWindow *)p, NULL);
	else
		gtk_widget_unparent(w);
}

void lx_add_child(node *p, node *c, uint32_t index) {
	uint32_t i = index < p->children.len ? index : p->children.len;
	ak_vec_push(&p->children, NULL);
	memmove(p->children.data + i + 1, p->children.data + i, (p->children.len - 1 - i) * sizeof(node *));
	p->children.data[i] = c;
	c->parent = p;
}

void lx_insert(node *p, node *c, uint32_t index) {
	if (lx_insert_nav(p, c, index)) return;
	if (lx_list_insert(p, c, index)) return;
	lx_add_child(p, c, index);
	if (!c->widget) return;
	if (p->scroll) {
		lx_scroll_set_content(p, c);
		return;
	}
	lx_detach(c->widget);
	// Above the widgets of the children before it, below those after: in
	// GTK, later siblings draw on top.
	GtkWidget *next = NULL;
	uint32_t i = index < p->children.len ? index : p->children.len - 1;
	for (uint32_t j = i + 1; j < p->children.len; j++) {
		node *x = p->children.data[j];
		if (x->widget && gtk_widget_get_parent(x->widget) == p->widget) {
			next = x->widget;
			break;
		}
	}
	if (!next) {
		// Last of p's own, under the overlays over p (overlay.c).
		for (GtkWidget *x = gtk_widget_get_last_child(p->widget); x; x = gtk_widget_get_prev_sibling(x)) {
			node *o = lx_view_node(x);
			if (!o || o->kind != AK_KIND_OVERLAY || o->parent) break;
			next = x;
		}
	}
	gtk_widget_insert_before(c->widget, p->widget, next);
	gtk_widget_queue_allocate(p->widget);
}

void lx_remove(node *p, node *c) {
	for (uint32_t i = 0; i < p->children.len; i++)
		if (p->children.data[i] == c) {
			memmove(p->children.data + i, p->children.data + i + 1, (p->children.len - i - 1) * sizeof(node *));
			p->children.len--;
			break;
		}
	c->parent = NULL;
	if (lx_remove_nav(p, c)) return;
	if (lx_list_remove(p, c)) return;
	if (p->scroll) {
		lx_scroll_set_content(p, NULL);
		return;
	}
	if (c->widget && gtk_widget_get_parent(c->widget) == p->widget) {
		gtk_widget_unparent(c->widget);
		gtk_widget_queue_allocate(p->widget);
	}
}

void lx_set_root(node *n) {
	H.root = n;
	lx_win_set_root(H.main, n);
	// The window may have changed since the core read the environment
	// (WindowOptions come in the same batch, before the root is set).
	lx_win_env(H.main, &H.env);
	ak_event ev = {.id = n->id, .kind = AK_EVENT_ENVIRONMENT, .env = H.env};
	lx_dispatch(&ev);
}

void lx_set_frame(node *n, const float f[4]) {
	if (lx_list_frame(n, f)) return;
	if (n->placed && memcmp(n->frame, f, sizeof n->frame) == 0) return;
	bool was = n->placed;
	float old[4];
	memcpy(old, n->frame, sizeof old);
	memcpy(n->frame, f, sizeof n->frame);
	n->placed = true;
	if (!n->widget) return;
	if (n->min_scale > 0 && (f[2] != old[2] || !was)) lx_text_frame(n);  // a shrinking text, at its new width
	if (n->parent && n->parent->scroll) {
		// A scroll's content: its size is what scrolls.
		gtk_widget_queue_resize(n->widget);
		return;
	}
	if (n->kind == AK_KIND_TABS) lx_tabs_resized(n);
	if (H.animating && was) {
		lx_animate(n, old);
		return;
	}
	if (H.animating && n->transition) lx_enter_from(n);
	lx_relayout(n);
}

void lx_measure_cb(uint32_t id, float width, float height, float aw, float ah, float *ow, float *oh) {
	node *n = node_get(id);
	*ow = *oh = 0;
	if (!n || !n->widget) return;
	lx_measure(n, width, height, aw, ah, ow, oh);
	if (*ow != *ow) *ow = 0;
	if (*oh != *oh) *oh = 0;
}

void lx_queue_draw(node *n) {
	if (n && n->widget) gtk_widget_queue_draw(n->widget);
}

// lx_connect connects a signal of obj (n's inner widget, a controller, an
// object of its own) with n as its data: n's release disconnects those
// on its widgets.
void lx_connect(node *n, gpointer obj, const char *sig, GCallback cb) {
	g_signal_connect_data(obj, sig, cb, n, NULL, (GConnectFlags)0);
}

bool lx_automating(void) {
	const char *keys[] = {"ASUKA_SNAPSHOT", "ASUKA_SCRIPT", "ASUKA_COMMANDS"};
	for (size_t i = 0; i < AK_LEN(keys); i++) {
		const char *v = getenv(keys[i]);
		if (v && *v) return true;
	}
	return false;
}

float lx_scale_of(node *n) {
	if (n && n->widget) {
		GtkNative *nat = gtk_widget_get_native(n->widget);
		if (nat) {
			GdkSurface *s = gtk_native_get_surface(nat);
			if (s) return gdk_surface_get_scale ? (float)gdk_surface_get_scale(s) : (float)gdk_surface_get_scale_factor(s);
		}
	}
	return H.env.scale > 0 ? H.env.scale : 1;
}

// ---- Actions ----

void actions_free(action_vec *v) {
	for (uint32_t i = 0; i < v->len; i++) {
		free(v->data[i].title);
		free(v->data[i].icon);
		actions_free(&v->data[i].items);
	}
	ak_vec_free(v);
}

// build reads the flat, depth-first list's items at depth from *i into
// out.
static void build(const ak_action *flat, uint32_t n, uint32_t *i, uint32_t count, action_vec *out, int64_t base) {
	for (uint32_t k = 0; k < count && *i < n; k++) {
		const ak_action *a = &flat[(*i)++];
		action x = {.role = a->role, .title = str_dup(a->title), .icon = str_dup(a->icon), .index = (int64_t)a->index + base, .flags = a->flags};
		if (a->flags & AK_MENU_SUBMENU) build(flat, n, i, a->count, &x.items, base);
		ak_vec_push(out, x);
	}
}

// actions_decode reads a menu (ops.DecodeActions), its items' numbers
// base + their own.
void actions_decode(action_vec *out, ak_span b, int64_t base) {
	actions_free(out);
	ak_action_vec flat = {0};
	ak_decode_actions((const uint8_t *)b.p, b.len, &flat);
	uint32_t top = 0;
	for (uint32_t i = 0; i < flat.len; i++)
		if (flat.data[i].depth == 0) top++;
	uint32_t i = 0;
	build(flat.data, flat.len, &i, top, out, base);
	ak_vec_free(&flat);
}

bool actions_same(const action_vec *a, const action_vec *b) {
	if (a->len != b->len) return false;
	for (uint32_t i = 0; i < a->len; i++) {
		const action *x = &a->data[i], *y = &b->data[i];
		if (x->role != y->role || !str_eq(x->title, y->title) || !str_eq(x->icon, y->icon) || x->index != y->index || x->flags != y->flags || !actions_same(&x->items, &y->items))
			return false;
	}
	return true;
}

void actions_copy(action_vec *dst, const action_vec *src) {
	actions_free(dst);
	for (uint32_t i = 0; i < src->len; i++) {
		const action *s = &src->data[i];
		action x = {.role = s->role, .title = s->title ? strdup(s->title) : NULL, .icon = s->icon ? strdup(s->icon) : NULL, .index = s->index, .flags = s->flags};
		actions_copy(&x.items, &s->items);
		ak_vec_push(dst, x);
	}
}
