// host.c: the nodes the core makes, its batches applied to them, the
// events sent back, and work posted to the main thread.
#include "host.h"
#include "parts.h"

wh_host H;

// ---- Nodes ----

node *wh_node(uint32_t id) { return wh_map_get(&H.nodes, id); }

node *wh_node_of(HWND hwnd) { return hwnd ? wh_map_get(&H.by_hwnd, (uint64_t)(uintptr_t)hwnd) : NULL; }

void wh_bind(node *n, HWND hwnd) {
	if (hwnd) wh_map_put(&H.by_hwnd, (uint64_t)(uintptr_t)hwnd, n);
}

void wh_unbind(HWND hwnd) {
	if (hwnd) wh_map_del(&H.by_hwnd, (uint64_t)(uintptr_t)hwnd);
}

node *wh_node_new(uint32_t id, uint16_t kind) {
	node *n = wh_alloc(sizeof *n);
	n->id = id;
	n->kind = kind;
	n->opacity = 1;
	n->xf.sx = n->xf.sy = 1;
	return n;
}

uint32_t wh_host_id(void) { return AK_HOST_ID_BIT | ++H.next_cell; }

void wh_node_add(node *n) {
	if (n->id) wh_map_put(&H.nodes, n->id, n);
	wh_bind(n, n->hwnd);
	wh_bind(n, n->ctl);
	wh_bind(n, n->ctl2);
}

static void node_free(node *n) {
	free(n->kids);
	free(n->font_family);
	free(n->gradient);
	if (n->conic) GdipDisposeImage((GpImage *)n->conic);
	wh_free_text(n);
	free(n->shadow);
	actions_free(&n->menu);
	actions_free(&n->acts);
	free(n->transition);
	free(n->match);
	wh_free_cstrings(n->drop_accept, n->ndrop_accept);
	free(n->drag_items);
	wstr_free(&n->title);
	wstr_free(&n->message);
	free(n->icon);
	wstr_free(&n->badge);
	wstr_free(&n->text);
	wstr_free(&n->placeholder);
	wh_free_cstrings(n->paste_types, n->npaste_types);
	wh_free_strings(n->items, n->nitems);
	wstr_free(&n->picker_label);
	free(n->symbol);
	wstr_free(&n->tooltip);
	free(n->keys);
	free(n->canvas);
	wstr_free(&n->a11y_label);
	wstr_free(&n->a11y_hint);
	wstr_free(&n->a11y_value);
	wh_free_strings(n->a11y_actions, n->na11y_actions);
	if (n->clip_rgn) DeleteObject(n->clip_rgn);
	wh_field_free(n);
	wh_nav_free(n);
	// A node named after it's freed reads this, not what it held: its
	// window handles and pointers are nothing anyone has.
	memset(n, 0xDD, sizeof *n);
	free(n);
}

// ---- Posting ----

static void jobs_push(wh_jobs *j, void (*fn)(void *), void *ctx) {
	if (j->n == j->cap) {
		j->cap = j->cap ? j->cap * 2 : 16;
		j->v = wh_realloc(j->v, j->cap * sizeof *j->v);
	}
	j->v[j->n++] = (wh_job){fn, ctx};
}

void wh_post(void (*fn)(void *ctx), void *ctx) {
	AcquireSRWLockExclusive(&H.post_mu);
	jobs_push(&H.posted, fn, ctx);
	bool wake = !H.post_sent;
	H.post_sent = true;
	ReleaseSRWLockExclusive(&H.post_mu);
	if (wake) PostMessageW(H.msg_win, WM_RUN_POSTED, 0, 0);
}

void wh_run_posted(void) {
	// Taken whole, then run: what they post runs on the next wake-up.
	static wh_jobs run;
	AcquireSRWLockExclusive(&H.post_mu);
	wh_jobs t = run;
	run = H.posted;
	H.posted = t;
	H.posted.n = 0;
	H.post_sent = false;
	ReleaseSRWLockExclusive(&H.post_mu);
	for (uint32_t i = 0; i < run.n; i++) wh_enter(run.v[i].fn, run.v[i].ctx);
	run.n = 0;
}

void wh_after_core(void (*fn)(void *ctx), void *ctx) {
	if (H.depth == 0) {
		fn(ctx);
		return;
	}
	jobs_push(&H.later, fn, ctx);
}

void wh_core_begin(void) { H.depth++; }

// wh_core_end ends a call into the core: once none is in progress, what
// waited for it runs (work that re-enters the core, modal loops), and the
// events raised meanwhile go.
void wh_core_end(void) {
	H.depth--;
	while (H.depth == 0 && (H.later.n > 0 || H.pending_at < H.pending.len)) {
		if (H.later.n > 0) {
			wh_jobs t = H.later;
			H.later = (wh_jobs){0};
			for (uint32_t i = 0; i < t.n; i++) t.v[i].fn(t.v[i].ctx);
			free(t.v);
			continue;
		}
		const uint8_t *p = H.pending.data + H.pending_at;
		uint32_t len = wh_u32(p);
		// Copied: the event may raise more, appended to the buffer.
		uint8_t small[256], *ev = len <= sizeof small ? small : wh_alloc(len);
		memcpy(ev, p + 4, len);
		H.pending_at += 4 + len;
		if (H.pending_at >= H.pending.len) {
			ak_buf_reset(&H.pending);
			H.pending_at = 0;
		}
		H.depth++;
		asuka_event(ev, len);
		H.depth--;
		if (ev != small) free(ev);
	}
}

void wh_enter(void (*fn)(void *ctx), void *ctx) {
	wh_core_begin();
	fn(ctx);
	wh_core_end();
}

// ---- Events ----

void wh_dispatch(const ak_event *e) {
	if (H.depth > 0) {
		// Encoded in place after its length: the core may be reading the
		// event that raised this one.
		size_t at = H.pending.len;
		ak_buf_u32(&H.pending, 0);
		ak_event_append(&H.pending, e);
		uint32_t len = (uint32_t)(H.pending.len - at - 4);
		memcpy(H.pending.data + at, &len, 4);
		return;
	}
	ak_buf_reset(&H.evbuf);
	ak_event_append(&H.evbuf, e);
	wh_core_begin();
	asuka_event(H.evbuf.data, H.evbuf.len);
	wh_core_end();
}

void wh_emit(node *n, ak_event *e) {
	if (!n || n->dead) return;
	if (n->search_of) {
		wh_search_event(n, e);  // a header's search field: its screen's
		return;
	}
	if (n->alert_of) {
		wh_alert_field_event(n, e);  // an alert's field: the alert's
		return;
	}
	if (!node_listening(n, e->kind)) return;
	e->id = n->id;
	wh_dispatch(e);
}

void wh_emit_kind(node *n, int kind) {
	ak_event e = {.kind = (ak_event_kind)kind};
	wh_emit(n, &e);
}

void wh_emit_bool(node *n, int kind, bool b) {
	ak_event e = {.kind = (ak_event_kind)kind, .b = b};
	wh_emit(n, &e);
}

void wh_emit_int(node *n, int kind, int64_t i) {
	ak_event e = {.kind = (ak_event_kind)kind, .i = i};
	wh_emit(n, &e);
}

void wh_emit_float(node *n, int kind, double f) {
	ak_event e = {.kind = (ak_event_kind)kind, .f = f};
	wh_emit(n, &e);
}

void wh_emit_str(node *n, int kind, const char *s, size_t len) {
	ak_event e = {.kind = (ak_event_kind)kind, .s = ak_strn(s, len)};
	wh_emit(n, &e);
}

void wh_emit_wstr(node *n, int kind, const wchar_t *s, int len) {
	int n8;
	char *u = wh_utf8(s, len, &n8);
	wh_emit_str(n, kind, u, (size_t)n8);
	free(u);
}

void wh_emit_bytes(node *n, int kind, const void *b, size_t len) { wh_emit_str(n, kind, b, len); }

// ---- Batches ----

static void apply_op(const ak_op *op) {
	node *n;
	switch (op->code) {
	case AK_OP_CREATE:
		wh_create(op->id, op->kind);
		break;
	case AK_OP_DESTROY:
		wh_destroy(op->id);
		break;
	case AK_OP_INSERT: {
		node *p = wh_node(op->id), *c = wh_node(op->child);
		if (p && c) wh_insert(p, c, op->index);
		break;
	}
	case AK_OP_REMOVE: {
		node *p = wh_node(op->id), *c = wh_node(op->child);
		if (p && c) wh_remove(p, c);
		break;
	}
	case AK_OP_SET_PROP:
		if ((n = wh_node(op->id))) {
			if (op->prop < AK_FIRST_KIND_PROP) wh_set_common(n, op->prop, &op->value);
			else wh_set_own(n, op->prop, &op->value);
		}
		break;
	case AK_OP_SET_FRAME:
		if ((n = wh_node(op->id))) wh_set_frame(n, op->frame);
		break;
	case AK_OP_LISTEN:
		if ((n = wh_node(op->id)) && op->event < 128) {
			if ((op->event == AK_EVENT_HOVER || op->event == AK_EVENT_HOVER_MOVE) && !node_listening(n, op->event)) wh_hover_views(1);
			n->listens[op->event / 64] |= (uint64_t)1 << (op->event % 64);
			wh_listen(n, op->event);
		}
		break;
	case AK_OP_UNLISTEN:
		if ((n = wh_node(op->id)) && op->event < 128) {
			if ((op->event == AK_EVENT_HOVER || op->event == AK_EVENT_HOVER_MOVE) && node_listening(n, op->event)) wh_hover_views(-1);
			n->listens[op->event / 64] &= ~((uint64_t)1 << (op->event % 64));
		}
		break;
	case AK_OP_COMMAND:
		if ((n = wh_node(op->id))) wh_command(n, op->cmd, (const uint8_t *)op->value.s.p, op->value.s.len);
		break;
	case AK_OP_SET_ROOT:
		if ((n = wh_node(op->id))) wh_set_root(n);
		break;
	}
}

void wh_apply(const uint8_t *buf, size_t len) {
	H.depth++;
	// Frames first noted, so a transition knows the batch places what it
	// makes.
	ak_batch_reader br;
	ak_op op;
	H.batch_frames = false;
	ak_batch_reader_init(&br, buf, len);
	while (ak_batch_next(&br, &op))
		if (op.code == AK_OP_SET_FRAME) {
			H.batch_frames = true;
			break;
		}
	ak_batch_reader_init(&br, buf, len);
	while (ak_batch_next(&br, &op)) apply_op(&op);
	if (br.err != AK_OK) wh_log("a batch the host can't read: %s", ak_err_string(br.err));
	wh_match_batch_done();
	wh_refresh_hover();
	wh_overlays_dirty();  // where the overlays' anchors are now
	H.depth--;
}

// ---- The tree ----

void wh_destroy(uint32_t id) {
	node *n = wh_node(id);
	if (!n) return;
	wh_map_del(&H.nodes, id);
	n->dead = true;
	wh_destroy_native(n);
	wh_forget_interaction(n);
	wh_focus_forget(n);
	wh_a11y_forget(n);
	if (n->kind == AK_KIND_OVERLAY) wh_overlay_gone(n);
	if (wh_destroy_nav(n)) return;
	if (n->tooltip.n) wh_set_tooltip(n, NULL);
	// Going out to its match or with a transition, or inside a view that
	// is: on screen until it's gone.
	if (wh_match_out(n) || wh_exit_transition(n)) return;
	if (wh_in_exiting(n->hwnd)) {
		if (n->parent) {
			node *p = n->parent;
			for (uint32_t i = 0; i < p->nkids; i++)
				if (p->kids[i] == n) {
					memmove(p->kids + i, p->kids + i + 1, (p->nkids - i - 1) * sizeof *p->kids);
					p->nkids--;
					break;
				}
			n->parent = NULL;
		}
		wh_gone_later(n);  // its window goes with the one it's in
		return;
	}
	if (n->parent) wh_remove(n->parent, n);
	for (uint32_t i = 0; i < n->nkids; i++) n->kids[i]->parent = NULL;
	wh_release(n);
	wh_free_node(n);
}

// wh_free_node frees what's left of a node whose windows are gone, once
// the message being handled has returned: the handler may still name it
// (an event it emitted ran the core, whose flush destroyed it).
void wh_free_node(node *n) {
	n->dead = true;
	if (H.ndead == H.capdead) {
		H.capdead = H.capdead ? H.capdead * 2 : 16;
		H.dead = wh_realloc(H.dead, H.capdead * sizeof *H.dead);
	}
	H.dead[H.ndead++] = n;
}

// wh_free_dead frees the nodes destroyed so far. The main loop calls it
// between messages, where no handler is running to name one.
void wh_free_dead(void) {
	while (H.ndead) {
		node *n = H.dead[--H.ndead];
		node_free(n);
	}
}

// wh_release lets go of n's windows: DestroyWindow takes its native
// descendants with it, so they're forgotten first (a window handle is
// reused once its window is gone).
void wh_release(node *n) {
	if (n->list) wh_list_release(n);
	if (n->image) wh_image_close(n);
	if (n->shader) wh_shader_release(n);
	if (n->shape) wh_shape_free(n);
	if (n->rich) wh_rich_release(n);
	if (n->pane_node) wh_split_release(n);
	if (n->clip_rgn) {
		DeleteObject(n->clip_rgn);
		n->clip_rgn = NULL;
		H.round_clips--;
	}
	if (n->material) {
		n->material = 0;
		H.materials--;
	}
	if (n->pict) wh_drop_picture(n);
	if (n->zoom != 0) {
		n->zoom = 0;
		H.zoomed--;
	}
	if (n->hwnd) {
		HWND hw = n->hwnd;
		wh_forget_tree(hw);
		if (n->web) wh_web_close(n, hw);  // its window goes after its controller
		else DestroyWindow(hw);
	} else if (n->web) {
		wh_web_close(n, NULL);
	}
	n->hwnd = n->ctl = n->ctl2 = NULL;
}

static BOOL CALLBACK collect(HWND hwnd, LPARAM lp) {
	struct {
		HWND *v;
		uint32_t n, cap;
	} *l = (void *)lp;
	if (l->n == l->cap) {
		l->cap = l->cap ? l->cap * 2 : 32;
		l->v = wh_realloc(l->v, l->cap * sizeof *l->v);
	}
	l->v[l->n++] = hwnd;
	return TRUE;
}

// wh_children lists a window's descendants (free *out).
uint32_t wh_children(HWND hwnd, HWND **out) {
	struct {
		HWND *v;
		uint32_t n, cap;
	} l = {0};
	EnumChildWindows(hwnd, collect, (LPARAM)&l);
	*out = l.v;
	return l.n;
}

void wh_forget_tree(HWND hwnd) {
	HWND *list;
	uint32_t n = wh_children(hwnd, &list);
	list = wh_realloc(list, (n + 1) * sizeof *list);
	list[n++] = hwnd;
	for (uint32_t i = 0; i < n; i++) {
		HWND w = list[i];
		wh_unsubclass(w);
		wh_exiting_forget(w);  // a handle goes with its window
		node *x = wh_node_of(w);
		if (!x) continue;
		if (x->hwnd == w) {
			wh_drop_provider(x, w);
			x->hwnd = NULL;
		}
		if (x->ctl == w) {
			if (x->kind == AK_KIND_PICKER || x->kind == AK_KIND_TEXT_FIELD || x->kind == AK_KIND_TOGGLE) wh_clear_accessible_name(w);
			x->ctl = NULL;
		}
		if (x->ctl2 == w) x->ctl2 = NULL;
		KillTimer(w, TIMER_LONG_PRESS);
		KillTimer(w, TIMER_SPIN);
		KillTimer(w, TIMER_BARS);
		KillTimer(w, TIMER_SHADER);
		wh_unbind(w);
	}
	free(list);
}

static void kids_insert(node *p, node *c, uint32_t i) {
	if (p->nkids == p->capkids) {
		p->capkids = p->capkids ? p->capkids * 2 : 4;
		p->kids = wh_realloc(p->kids, p->capkids * sizeof *p->kids);
	}
	if (i > p->nkids) i = p->nkids;
	memmove(p->kids + i + 1, p->kids + i, (p->nkids - i) * sizeof *p->kids);
	p->kids[i] = c;
	p->nkids++;
}

void wh_insert(node *p, node *c, uint32_t index) {
	if (wh_insert_nav(p, c, index)) return;
	uint32_t i = index < p->nkids ? index : p->nkids;
	kids_insert(p, c, i);
	c->parent = p;
	if (p->kind == AK_KIND_SCROLL) wh_zoom_child(p, c, false);  // a zoomed Scroll's content: at its zoom
	if (!c->hwnd || !p->hwnd) return;
	SetParent(c->hwnd, p->hwnd);
	wh_match_in(c);
	wh_adopt(p, c);
	if (p->opacity < 1 && !p->pict) wh_update_picture(p);  // faded with child windows now
	wh_place_frame(c);
	// Above the windows of the children before it, below those after.
	HWND after = HWND_TOP;
	for (uint32_t j = i + 1; j < p->nkids; j++) {
		node *next = p->kids[j];
		if (next->hwnd && GetParent(next->hwnd) == p->hwnd) {
			after = next->hwnd;
			break;
		}
	}
	if (after == HWND_TOP) after = wh_top_of_children(p);  // above the content, under what the host keeps on top
	SetWindowPos(c->hwnd, after, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	if (p->rich) wh_rich_place_inlines(p);
}

void wh_remove(node *p, node *c) {
	for (uint32_t i = 0; i < p->nkids; i++)
		if (p->kids[i] == c) {
			memmove(p->kids + i, p->kids + i + 1, (p->nkids - i - 1) * sizeof *p->kids);
			p->nkids--;
			break;
		}
	c->parent = NULL;
	if (p->kind == AK_KIND_SCROLL) wh_zoom_child(p, c, false);  // out of a zoomed Scroll: its own scale again
	if (wh_remove_nav(p, c)) return;
	if (c->hwnd) SetParent(c->hwnd, H.park);  // parked: it may be inserted again (a moved view)
	if (p->kind == AK_KIND_OVERLAY) wh_overlays_dirty();  // its region
}

void wh_set_root(node *n) {
	H.root = n;
	// The window changed before the root was set (WindowOptions, a menu
	// bar): the core hears of it now.
	if (memcmp(&H.env, &H.core_env, sizeof H.env) != 0) wh_report_environment(n, &H.env);
	if (n->hwnd) {
		SetParent(n->hwnd, H.window);
		RECT r;
		GetClientRect(H.window, &r);
		SetWindowPos(n->hwnd, HWND_BOTTOM, 0, 0, r.right, r.bottom, SWP_NOACTIVATE);
	}
}

float wh_scale_of(const node *n) { return n && n->k > 0 ? n->k : H.scale; }
float wh_scale_of_k(float k) { return k > 0 ? k : H.scale; }
int wh_dpi_of(const node *n) { return n && n->k > 0 ? (int)lroundf(n->k * 96) : H.dpi; }
int wh_px(const node *n, float v) { return (int)lroundf(v * wh_scale_of(n)); }
float wh_pt(const node *n, int v) { return (float)v / wh_scale_of(n); }

void wh_set_frame(node *n, const float f[4]) {
	if (n->cell) {
		wh_cell_set_height(n, f[3]);
		return;
	}
	if ((n->nav && wh_nav_sized_by_host(n)) || n->kind == AK_KIND_OVERLAY) return;  // the host sizes it
	if (n->parent && n->parent->rich) {
		// An inline component: its size is the core's, its place the text's.
		n->frame[2] = f[2], n->frame[3] = f[3];
		wh_rich_place_inlines(n->parent);
		return;
	}
	if (wh_enter_transition(n, f)) return;
	if (wh_animate_frame(n, f)) return;
	memcpy(n->frame, f, sizeof n->frame);
	n->placed = true;
	wh_place_frame(n);
	if (n->parent && n->parent->kind == AK_KIND_SCROLL) wh_update_scroll(n->parent);
}

// wh_place_frame moves n's window to its frame, in pixels, offset by its
// scroll view's position.
void wh_place_frame(node *n) {
	if (!n->hwnd || n == H.root || n->kind == AK_KIND_OVERLAY) return;
	if (n->parent && n->parent->pane_node) {
		wh_layout_split(n->parent);  // a SplitView's detail: beside its pane
		return;
	}
	wh_place_at(n, n->frame);
}

static bool has_xform(const node *n) {
	return n->xf.tx != 0 || n->xf.ty != 0 || n->xf.sx != 1 || n->xf.sy != 1 || n->xf.rot != 0;
}

void wh_place_at(node *n, const float f[4]) {
	float x = f[0], y = f[1];
	float z = n->parent && n->parent->kind == AK_KIND_SCROLL ? wh_scroll_zoom(n->parent) : 1;
	if (n->parent && n->parent->kind == AK_KIND_SCROLL) {
		x -= n->parent->scroll_x / z;  // its position is the zoomed content's
		y -= n->parent->scroll_y / z;
	}
	int x0 = wh_px(n, x), y0 = wh_px(n, y);
	int w = wh_px(n, x + f[2]) - x0, ht = wh_px(n, y + f[3]) - y0;
	if (z != 1) {
		// A zoomed Scroll's content: its scale (its k) is the zoom's, its
		// frame the zoomed content's, in the Scroll's pixels.
		float k = wh_scale_of(n->parent);
		x0 = (int)lroundf((f[0] * z - n->parent->scroll_x) * k), y0 = (int)lroundf((f[1] * z - n->parent->scroll_y) * k);
		w = (int)lroundf((f[0] + f[2]) * z * k) - (int)lroundf(f[0] * z * k), ht = (int)lroundf((f[1] + f[3]) * z * k) - (int)lroundf(f[1] * z * k);
	}
	if (has_xform(n)) {
		int x1, y1;
		if (z != 1) wh_transformed(n, f[0] * z - n->parent->scroll_x, f[1] * z - n->parent->scroll_y, f[2] * z, f[3] * z, &x0, &y0, &x1, &y1);
		else wh_transformed(n, x, y, f[2], f[3], &x0, &y0, &x1, &y1);
		w = x1 - x0, ht = y1 - y0;
	}
	bool shadow = n->shadow_len > 0 || n->has_text_shadow;
	if (shadow) wh_invalidate_shadow(n);  // where it was
	SetWindowPos(n->hwnd, NULL, x0, y0, w, ht, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOCOPYBITS);
	if (shadow) wh_invalidate_shadow(n);
	if (n->pict) wh_place_picture(n);
	wh_resized(n);
	wh_overlays_dirty();  // an overlay's child (its region), an anchor
}

// wh_resized lays out what's inside a view that changed size: a field's
// control, a list's rows, a stack's screens.
void wh_resized(node *n) {
	if (n->kind == AK_KIND_TEXT_FIELD) wh_layout_field(n);
	else if (n->list) wh_list_resized(n);
	else if (n->kind == AK_KIND_STACK || n->kind == AK_KIND_TABS) wh_layout_screens(n);
	else if (n->pane_node) wh_layout_split(n);
	else if (n->kind == AK_KIND_SCROLL) wh_update_scroll(n);
	else if (n->kind == AK_KIND_STEPPER) wh_layout_stepper(n);
	else if (n->rich) wh_rich_resized(n);
	else if (n->web) wh_web_resized(n);
	else if (n->shader) wh_render_shader(n);
	if (wh_round(n) || n->gradient || wh_has_border(n)) InvalidateRect(n->hwnd, NULL, FALSE);
}

void wh_measure(uint32_t id, float w, float h, float aw, float ah, float *ow, float *oh) {
	node *n = wh_node(id);
	*ow = *oh = 0;
	if (!n || !n->hwnd) return;
	wh_measure_node(n, w, h, aw, ah, ow, oh);
	if (*ow != *ow) *ow = 0;
	if (*oh != *oh) *oh = 0;
}

// wh_top_of_children is where a child inserted last goes in its parent's
// z-order: on top, but under the bar a stack or tabs keep above their
// screens.
HWND wh_top_of_children(node *p) {
	HWND bar = wh_nav_bar_window(p);
	return bar ? bar : HWND_TOP;
}

// wh_adopt gives a view inserted into p the scale of p's window, if it was
// at another's (made in the parking window, or moved from another window).
void wh_adopt(node *p, node *c) {
	float k = wh_k_for(p->k, wh_own_zoom(c));
	if (c->k == k || !c->hwnd) return;
	if (wh_scale_of(c) == wh_scale_of_k(k)) wh_set_scale(c->hwnd, p->k);
	else wh_rescale(c->hwnd, p->k);
}

bool wh_is_rtl(const node *n) {
	if (n) {
		if (n->dir == AK_LAYOUT_DIRECTION_LEFT_TO_RIGHT) return false;
		if (n->dir == AK_LAYOUT_DIRECTION_RIGHT_TO_LEFT) return true;
	}
	return H.rtl;
}
