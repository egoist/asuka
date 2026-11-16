// Interaction states, style variants, gestures, screen readers' actions,
// drags and drops, gestures (interaction.c).
#include "uih.h"

static ak_color sem(uint16_t c) { return ak_semantic_color(c); }

static ak_color bg(const ak_fake_view *v) { return (ak_color)ak_fake_int(v, AK_PROP_BACKGROUND); }
static ak_color border(const ak_fake_view *v) { return (ak_color)ak_fake_int(v, AK_PROP_BORDER_COLOR); }

static void set_state(ak_uitest *a, ak_fake_view *v, ak_event_kind k, bool on) {
	dispatch(a, (ak_event){.id = v->id, .kind = k, .b = on});
}

static int presses;

static ak_val press(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	presses++;
	return noval();
}

// ---- Variants (TestStyleVariants) ----

static ak_val variants_app(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_view *v = ak_ui_new(AK_EL_VIEW);
	on(v, AK_EV_VIEW_PRESS, press, NULL);
	set_i(v, AK_KEY_BACKGROUND, (int64_t)sem(AK_SEMANTIC_COLOR_SECONDARY_BACKGROUND));
	ak_ui_variant(v, AK_STATE_HOVERED, AK_KEY_BACKGROUND, (ak_val){.i = (int64_t)sem(AK_SEMANTIC_COLOR_SECONDARY_GROUPED_BACKGROUND)});
	ak_ui_variant(v, AK_STATE_HOVERED, AK_KEY_BORDER_COLOR, (ak_val){.i = (int64_t)sem(AK_SEMANTIC_COLOR_ACCENT)});
	ak_ui_variant(v, AK_STATE_PRESSED, AK_KEY_BACKGROUND, (ak_val){.i = (int64_t)sem(AK_SEMANTIC_COLOR_SEPARATOR)});
	return (ak_val){.p = v};
}

// A view's variants are laid over its style while it's in their states,
// in order (pressed over hovered), each setting only its keys.
TEST(style_variants) {
	presses = 0;
	ak_uitest *a = mount(variants_app, NULL);
	ak_fake_view *v = nth(a, AK_KIND_VIEW, 1);
	REQUIRE(v);
	CHECK(bg(v) == sem(AK_SEMANTIC_COLOR_SECONDARY_BACKGROUND) && border(v) == 0);
	set_state(a, v, AK_EVENT_HOVER, true);
	CHECK(bg(v) == sem(AK_SEMANTIC_COLOR_SECONDARY_GROUPED_BACKGROUND) && border(v) == sem(AK_SEMANTIC_COLOR_ACCENT));
	set_state(a, v, AK_EVENT_PRESSED, true);
	CHECK(bg(v) == sem(AK_SEMANTIC_COLOR_SEPARATOR) && border(v) == sem(AK_SEMANTIC_COLOR_ACCENT));
	set_state(a, v, AK_EVENT_PRESSED, false);
	set_state(a, v, AK_EVENT_HOVER, false);
	CHECK(bg(v) == sem(AK_SEMANTIC_COLOR_SECONDARY_BACKGROUND) && border(v) == 0);
	// The style's own value changing while a variant is laid over it.
	set_state(a, v, AK_EVENT_PRESSED, true);
	ak_fake_tap(ak_uitest_host(a), v);
	CHECK_EQ_INT(presses, 1);
	done(a);
}

static ak_val track_app(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_view *col = ak_ui_new(AK_EL_COLUMN);
	ak_view *hover = ak_ui_new(AK_EL_VIEW);
	on(hover, AK_EV_VIEW_PRESS, press, NULL);
	ak_ui_variant(hover, AK_STATE_HOVERED, AK_KEY_OPACITY, (ak_val){.f = 0.5});
	add(col, hover);
	ak_view *plain = ak_ui_new(AK_EL_VIEW);
	on(plain, AK_EV_VIEW_PRESS, press, NULL);
	set(plain, AK_KEY_PADDING, edges4(4, 4, 4, 4));
	add(col, plain);
	ak_view *pressed = ak_ui_new(AK_EL_VIEW);  // a variant alone makes a view
	ak_ui_variant(pressed, AK_STATE_PRESSED, AK_KEY_OPACITY, (ak_val){.f = 0.5});
	add(col, pressed);
	return (ak_val){.p = col};
}

// A view tracks only the states its variants name: one with no pressed
// variant doesn't report its pressed state (and keeps the platform's
// highlight).
TEST(variants_track_what_they_name) {
	ak_uitest *a = mount(track_app, NULL);
	REQUIRE(count_kind(a, AK_KIND_VIEW) == 4);
	ak_fake_view *hover = nth(a, AK_KIND_VIEW, 1), *plain = nth(a, AK_KIND_VIEW, 2), *pressed = nth(a, AK_KIND_VIEW, 3);
	CHECK(ak_fake_listens(hover, AK_EVENT_HOVER) && !ak_fake_listens(hover, AK_EVENT_PRESSED));
	CHECK(!ak_fake_listens(plain, AK_EVENT_HOVER) && !ak_fake_listens(plain, AK_EVENT_PRESSED));
	CHECK(ak_fake_listens(pressed, AK_EVENT_PRESSED) && !ak_fake_listens(pressed, AK_EVENT_HOVER));
	set_state(a, pressed, AK_EVENT_PRESSED, true);
	CHECK_EQ_F(ak_fake_float(pressed, AK_PROP_OPACITY), 0.5);
	set_state(a, pressed, AK_EVENT_PRESSED, false);
	CHECK_EQ_F(ak_fake_float(pressed, AK_PROP_OPACITY), 1);
	done(a);
}

static void *tint;

static ak_val read_tint(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	return ak_vm_cell_get(tint, true);
}

static ak_val bound_app(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_view *col = ak_ui_new(AK_EL_COLUMN);
	ak_view *v = ak_ui_new(AK_EL_VIEW);
	set(v, AK_KEY_PADDING, edges4(4, 4, 4, 4));
	ak_closure *t = fn(read_tint, NULL);
	ak_ui_variant_bind(v, AK_STATE_PRESSED, AK_KEY_BACKGROUND, t);
	ak_release(t);
	add(v, text("x"));
	add(col, v);
	return (ak_val){.p = col};
}

// A variant's value follows what it reads, while the state's on and
// after.
TEST(bound_variants) {
	tint = cell_int((int64_t)sem(AK_SEMANTIC_COLOR_SEPARATOR));
	ak_uitest *a = mount(bound_app, NULL);
	ak_fake_view *v = nth(a, AK_KIND_VIEW, 1);
	REQUIRE(v);
	CHECK(ak_fake_listens(v, AK_EVENT_PRESSED));
	CHECK_EQ_INT(v->children.len, 1);
	set_state(a, v, AK_EVENT_PRESSED, true);
	CHECK(bg(v) == sem(AK_SEMANTIC_COLOR_SEPARATOR));
	write_i(tint, (int64_t)sem(AK_SEMANTIC_COLOR_RED));
	ak_ui_flush();
	CHECK(bg(v) == sem(AK_SEMANTIC_COLOR_RED));
	set_state(a, v, AK_EVENT_PRESSED, false);
	CHECK(bg(v) == 0);
	done(a);
	ak_release(tint);
}

// ---- Layers: a style: list's conditional entries, the caller's ----

static void *flag_a, *flag_b, *flag_c;

static void *cell_bool(bool v) { return ak_vm_cell_new((ak_val){.i = v}, AK_K_BOOL, NULL).p; }
static void write_b(void *cell, bool v) { ak_vm_cell_set(cell, (ak_val){.i = v}); }

static ak_val read_a(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	return ak_vm_cell_get(flag_a, true);
}

static ak_val read_b(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	return ak_vm_cell_get(flag_b, true);
}

static ak_val read_c(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	return ak_vm_cell_get(flag_c, true);
}

static void when(ak_view *v, uint16_t layer, ak_invoke_fn f) {
	ak_closure *t = fn(f, NULL);
	ak_ui_layer_when(v, layer, t);
	ak_release(t);
}

static ak_view *layered;

static ak_val layers_app(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_view *col = ak_ui_new(AK_EL_COLUMN);
	// A Text: layers are any element's, not only a container's.
	ak_view *t = text("x");
	set_i(t, AK_KEY_BACKGROUND, (int64_t)sem(AK_SEMANTIC_COLOR_SECONDARY_BACKGROUND));
	uint16_t la = ak_ui_layer(t, AK_LAYER_OWN, 0);
	when(t, la, read_a);
	ak_ui_layer_set(t, la, AK_KEY_BACKGROUND, (ak_val){.i = (int64_t)sem(AK_SEMANTIC_COLOR_RED)});
	ak_ui_layer_set(t, la, AK_KEY_OPACITY, (ak_val){.f = 0.5});
	uint16_t lb = ak_ui_layer(t, AK_LAYER_OWN, 0);
	when(t, lb, read_b);
	ak_ui_layer_set(t, lb, AK_KEY_BACKGROUND, (ak_val){.i = (int64_t)sem(AK_SEMANTIC_COLOR_BLUE)});
	add(col, t);
	layered = t;
	return (ak_val){.p = col};
}

// A layer's keys are laid over the view's own while its condition holds,
// and go back (to the own value, or unset) when it stops; of two on, the
// one made later wins a key both set.
TEST(style_layers) {
	flag_a = cell_bool(false), flag_b = cell_bool(false);
	ak_uitest *a = mount(layers_app, NULL);
	ak_fake_view *v = nth(a, AK_KIND_TEXT, 0);
	REQUIRE(v);
	CHECK(bg(v) == sem(AK_SEMANTIC_COLOR_SECONDARY_BACKGROUND));
	CHECK_EQ_F(ak_fake_float(v, AK_PROP_OPACITY), 0);  // never sent: a layer that's off sends nothing
	write_b(flag_a, true);
	ak_ui_flush();
	CHECK(bg(v) == sem(AK_SEMANTIC_COLOR_RED));
	CHECK_EQ_F(ak_fake_float(v, AK_PROP_OPACITY), 0.5);
	write_b(flag_b, true);
	ak_ui_flush();
	CHECK(bg(v) == sem(AK_SEMANTIC_COLOR_BLUE));
	CHECK_EQ_F(ak_fake_float(v, AK_PROP_OPACITY), 0.5);
	write_b(flag_a, false);
	ak_ui_flush();
	CHECK(bg(v) == sem(AK_SEMANTIC_COLOR_BLUE));
	CHECK_EQ_F(ak_fake_float(v, AK_PROP_OPACITY), 1);
	write_b(flag_b, false);
	ak_ui_flush();
	CHECK(bg(v) == sem(AK_SEMANTIC_COLOR_SECONDARY_BACKGROUND));
	// The own value changing under a layer that's on.
	write_b(flag_a, true);
	ak_ui_flush();
	set_i(layered, AK_KEY_BACKGROUND, (int64_t)sem(AK_SEMANTIC_COLOR_GREEN));
	ak_ui_flush();
	CHECK(bg(v) == sem(AK_SEMANTIC_COLOR_RED));
	write_b(flag_a, false);
	ak_ui_flush();
	CHECK(bg(v) == sem(AK_SEMANTIC_COLOR_GREEN));
	done(a);
	ak_release(flag_a), ak_release(flag_b);
}

static ak_val tiers_app(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_view *col = ak_ui_new(AK_EL_COLUMN);
	ak_view *v = ak_ui_new(AK_EL_VIEW);
	set(v, AK_KEY_PADDING, edges4(4, 4, 4, 4));
	// The component's own: a layer on while a holds, and pressed.
	uint16_t own = ak_ui_layer(v, AK_LAYER_OWN, 0);
	when(v, own, read_a);
	ak_ui_layer_set(v, own, AK_KEY_BACKGROUND, (ak_val){.i = (int64_t)sem(AK_SEMANTIC_COLOR_RED)});
	ak_ui_layer_set(v, own, AK_KEY_OPACITY, (ak_val){.f = 0.5});
	ak_ui_variant(v, AK_STATE_PRESSED, AK_KEY_BACKGROUND, (ak_val){.i = (int64_t)sem(AK_SEMANTIC_COLOR_SEPARATOR)});
	// Its caller's: the background, and a layer on while c holds.
	ak_ui_set_outer(v, AK_KEY_BACKGROUND, (ak_val){.i = (int64_t)sem(AK_SEMANTIC_COLOR_GREEN)});
	uint16_t outer = ak_ui_layer(v, AK_LAYER_OUTER, 0);
	when(v, outer, read_c);
	ak_ui_layer_set(v, outer, AK_KEY_BORDER_COLOR, (ak_val){.i = (int64_t)sem(AK_SEMANTIC_COLOR_ACCENT)});
	ak_ui_layer_set(v, outer, AK_KEY_BACKGROUND, (ak_val){.i = (int64_t)sem(AK_SEMANTIC_COLOR_BLUE)});
	add(col, v);
	return (ak_val){.p = col};
}

// The order of the tiers: the caller's background is over the
// component's own layer (whose other keys still apply), the caller's
// layer over the caller's own values, and a variant over every layer.
TEST(style_layer_tiers) {
	flag_a = cell_bool(true), flag_c = cell_bool(false);
	ak_uitest *a = mount(tiers_app, NULL);
	ak_fake_view *v = nth(a, AK_KIND_VIEW, 1);
	REQUIRE(v);
	CHECK(bg(v) == sem(AK_SEMANTIC_COLOR_GREEN));
	CHECK_EQ_F(ak_fake_float(v, AK_PROP_OPACITY), 0.5);
	write_b(flag_c, true);
	ak_ui_flush();
	CHECK(bg(v) == sem(AK_SEMANTIC_COLOR_BLUE) && border(v) == sem(AK_SEMANTIC_COLOR_ACCENT));
	set_state(a, v, AK_EVENT_PRESSED, true);
	CHECK(bg(v) == sem(AK_SEMANTIC_COLOR_SEPARATOR));
	set_state(a, v, AK_EVENT_PRESSED, false);
	write_b(flag_c, false);
	ak_ui_flush();
	CHECK(bg(v) == sem(AK_SEMANTIC_COLOR_GREEN) && border(v) == 0);
	write_b(flag_a, false);
	ak_ui_flush();
	CHECK(bg(v) == sem(AK_SEMANTIC_COLOR_GREEN));
	CHECK_EQ_F(ak_fake_float(v, AK_PROP_OPACITY), 1);
	done(a);
	ak_release(flag_a), ak_release(flag_c);
}

static ak_val cond_variant_app(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_view *col = ak_ui_new(AK_EL_COLUMN);
	ak_view *v = ak_ui_new(AK_EL_VIEW);
	set(v, AK_KEY_PADDING, edges4(4, 4, 4, 4));
	// A style's pressed { … } in a conditional entry, then the block's.
	uint16_t l = ak_ui_layer(v, AK_LAYER_STATE, AK_STATE_PRESSED);
	when(v, l, read_a);
	ak_ui_layer_set(v, l, AK_KEY_OPACITY, (ak_val){.f = 0.25});
	ak_ui_layer_set(v, l, AK_KEY_BACKGROUND, (ak_val){.i = (int64_t)sem(AK_SEMANTIC_COLOR_RED)});
	ak_ui_variant(v, AK_STATE_PRESSED, AK_KEY_BACKGROUND, (ak_val){.i = (int64_t)sem(AK_SEMANTIC_COLOR_SEPARATOR)});
	add(col, v);
	return (ak_val){.p = col};
}

// A state's layer with a condition: on while both hold; the block's
// variant of the state is over the style's.
TEST(conditional_variants) {
	flag_a = cell_bool(false);
	ak_uitest *a = mount(cond_variant_app, NULL);
	ak_fake_view *v = nth(a, AK_KIND_VIEW, 1);
	REQUIRE(v);
	CHECK(ak_fake_listens(v, AK_EVENT_PRESSED));
	set_state(a, v, AK_EVENT_PRESSED, true);
	CHECK(bg(v) == sem(AK_SEMANTIC_COLOR_SEPARATOR));
	CHECK_EQ_F(ak_fake_float(v, AK_PROP_OPACITY), 0);  // unset: its layer is off
	write_b(flag_a, true);
	ak_ui_flush();
	CHECK(bg(v) == sem(AK_SEMANTIC_COLOR_SEPARATOR));
	CHECK_EQ_F(ak_fake_float(v, AK_PROP_OPACITY), 0.25);
	set_state(a, v, AK_EVENT_PRESSED, false);
	CHECK(bg(v) == 0);
	CHECK_EQ_F(ak_fake_float(v, AK_PROP_OPACITY), 1);
	done(a);
	ak_release(flag_a);
}

// Turning layers on and off allocates nothing (the fake host's copies
// of what it's sent aside).
static ak_fake *layer_host;

static void layer_apply(void *ctx, const uint8_t *buf, size_t len) {
	AK_UNUSED(ctx);
	uint64_t n = ak_alloc_count;
	ak_fake_apply(layer_host, buf, len);
	ak_alloc_count = n;
}

static void layer_measure(void *ctx, uint32_t id, float w, float h, float aw, float ah, float *ow, float *oh) {
	AK_UNUSED(ctx);
	uint64_t n = ak_alloc_count;
	ak_fake_measure(layer_host, id, w, h, aw, ah, ow, oh);
	ak_alloc_count = n;
}

TEST(toggling_layers_allocates_nothing) {
	setup();
	flag_a = cell_bool(false), flag_b = cell_bool(false);
	layer_host = ak_fake_new();
	ak_ui_init(&(ak_host){NULL, layer_apply, layer_measure});
	ak_ui_set_environment(&layer_host->env);
	ak_closure *c = fn(layers_app, NULL);
	ak_ui_mount(c);
	ak_release(c);
	for (int i = 1; i <= 4; i++) {
		ak_fake_reset(layer_host);
		write_b(flag_a, i % 2), write_b(flag_b, i % 3 == 0);
		ak_ui_flush();
	}
	uint64_t allocs = 0;
	for (int i = 0; i < 200; i++) {
		ak_fake_reset(layer_host);
		uint64_t before = ak_alloc_count;
		write_b(flag_a, i % 2), write_b(flag_b, i % 3 == 0);
		ak_ui_flush();
		allocs += ak_alloc_count - before;
	}
	if (allocs != 0) T_FAIL("%llu allocations in 200 updates", (unsigned long long)allocs);
	CHECK(ak_fake_count(layer_host, AK_OP_SET_PROP) > 0);  // the work was done
	ak_ui_unmount();
	ak_fake_free(layer_host);
	ak_release(flag_a), ak_release(flag_b);
}

// ---- A view's states as values (TestInteractionGroupAndPeer) ----

static ak_view *card;
static void *peer;

static ak_val title_color(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	return (ak_val){.i = (int64_t)sem(ak_ui_view_state(card, AK_STATE_HOVERED) ? AK_SEMANTIC_COLOR_ACCENT : AK_SEMANTIC_COLOR_LABEL)};
}

static ak_val down_text(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	return (ak_val){.p = ak_str_c(ak_ui_view_state(card, AK_STATE_PRESSED) ? "down" : "up")};
}

static ak_val peer_text(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_view *t = ak_ui_new(AK_EL_TEXT);
	bind(t, AK_KEY_TEXT_TEXT, down_text, NULL);
	return (ak_val){.p = t};
}

static ak_val group_app(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_view *col = ak_ui_new(AK_EL_COLUMN);
	ak_view *v = ak_ui_new(AK_EL_VIEW);
	on(v, AK_EV_VIEW_PRESS, press, NULL);
	card = ak_ui_ref(v);  // View as card { … }
	ak_view *title = text("Title");
	bind(title, AK_KEY_COLOR, title_color, NULL);
	add(v, title);
	add(col, v);
	ak_closure *cond = fn(read_cell, peer), *then = fn(peer_text, NULL);
	add(col, ak_ui_show(cond, then, NULL));
	ak_release(cond), ak_release(then);
	return (ak_val){.p = col};
}

// A view's states are values other views' bindings read: its children's
// (a group's hover), its siblings' (a peer's). The view reports a state
// from when something first reads it.
TEST(state_group_and_peer) {
	peer = ak_vm_cell_new((ak_val){.i = 0}, AK_K_BOOL, NULL).p;
	ak_uitest *a = mount(group_app, NULL);
	ak_fake_view *v = nth(a, AK_KIND_VIEW, 1), *title = one(a, AK_KIND_TEXT, "Title");
	REQUIRE(v && title);
	CHECK(ak_fake_listens(v, AK_EVENT_HOVER) && !ak_fake_listens(v, AK_EVENT_PRESSED));
	set_state(a, v, AK_EVENT_HOVER, true);
	CHECK((ak_color)ak_fake_int(title, AK_PROP_COLOR) == sem(AK_SEMANTIC_COLOR_ACCENT));
	set_state(a, v, AK_EVENT_HOVER, false);
	CHECK((ak_color)ak_fake_int(title, AK_PROP_COLOR) == sem(AK_SEMANTIC_COLOR_LABEL));
	write_i(peer, 1);  // reads card's pressed: it reports it from now on
	ak_ui_flush();
	CHECK(ak_fake_listens(v, AK_EVENT_PRESSED));
	CHECK(one(a, AK_KIND_TEXT, "up") != NULL);
	set_state(a, v, AK_EVENT_PRESSED, true);
	CHECK(one(a, AK_KIND_TEXT, "down") != NULL);
	set_state(a, v, AK_EVENT_PRESSED, false);
	ak_release(card);
	done(a);
	ak_release(peer);
}

// ---- Handlers, and leaving the states as the view goes ----

static char log_buf[256];

static void logf_(const char *s) {
	size_t n = strlen(log_buf);
	snprintf(log_buf + n, sizeof log_buf - n, "%s%s", n ? " " : "", s);
}

static ak_val log_bool(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(n);
	char s[32];
	snprintf(s, sizeof s, "%s:%s", (const char *)c->fn, a[0].i ? "on" : "off");
	logf_(s);
	return noval();
}

static void *shown;

static ak_val stateful_view(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_view *v = ak_ui_new(AK_EL_VIEW);
	on(v, AK_EV_VIEW_PRESSED_CHANGE, log_bool, "pressed");
	on(v, AK_EV_VIEW_HOVER_CHANGE, log_bool, "hovered");
	on(v, AK_EV_VIEW_DROP_OVER, log_bool, "over");
	ak_arr *types = ak_arr_new(AK_K_REF, NULL, 1);
	ak_arr_push(&types, (ak_val){.p = ak_str_c("*/*")});
	set(v, AK_KEY_VIEW_DROP_TYPES, (ak_val){.p = types});
	return (ak_val){.p = v};
}

static ak_val leave_app(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_closure *cond = fn(read_cell, shown), *then = fn(stateful_view, NULL);
	ak_view *s = ak_ui_show(cond, then, NULL);
	ak_release(cond), ak_release(then);
	ak_view *col = ak_ui_new(AK_EL_COLUMN);
	add(col, s);
	return (ak_val){.p = col};
}

// A view that goes away while pressed, hovered or under a drag leaves
// each state, so what follows it elsewhere doesn't stay stuck.
TEST(states_left_when_the_view_goes) {
	log_buf[0] = 0;
	shown = ak_vm_cell_new((ak_val){.i = 1}, AK_K_BOOL, NULL).p;
	ak_uitest *a = mount(leave_app, NULL);
	ak_fake_view *v = nth(a, AK_KIND_VIEW, 1);
	REQUIRE(v);
	CHECK(ak_fake_listens(v, AK_EVENT_PRESSED) && ak_fake_listens(v, AK_EVENT_HOVER) && ak_fake_listens(v, AK_EVENT_DRAG_OVER));
	set_state(a, v, AK_EVENT_PRESSED, true);
	set_state(a, v, AK_EVENT_PRESSED, true);  // a repeat changes nothing
	set_state(a, v, AK_EVENT_HOVER, true);
	dispatch(a, (ak_event){.id = v->id, .kind = AK_EVENT_DRAG_OVER, .g = {.state = AK_GESTURE_STATE_BEGAN, .x = 1, .y = 2}});
	write_i(shown, 0);
	ak_ui_flush();
	CHECK_EQ_STR(log_buf, "pressed:on hovered:on over:on pressed:off hovered:off over:off");
	// One that's in none reports nothing.
	log_buf[0] = 0;
	write_i(shown, 1);
	ak_ui_flush();
	write_i(shown, 0);
	ak_ui_flush();
	CHECK_EQ_STR(log_buf, "");
	done(a);
	ak_release(shown);
}

// ---- Gestures, screen readers' actions ----

static ak_val log_gesture(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(n);
	const ak_struct *g = a[0].p;
	char s[64];
	snprintf(s, sizeof s, "%s:%lld:%g:%g", (const char *)c->fn, (long long)g->f[0].i, g->f[3].f, g->f[7].f);
	logf_(s);
	return noval();
}

static ak_val log_int(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(n);
	char s[32];
	snprintf(s, sizeof s, "%s:%lld", (const char *)c->fn, (long long)a[0].i);
	logf_(s);
	return noval();
}

static ak_val log_run(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(a), AK_UNUSED(n);
	logf_((const char *)c->fn);
	return noval();
}

static const uint8_t action_kinds[] = {AK_K_REF, AK_K_REF};
static const ak_shape action_shape = {"A11yAction", 2, action_kinds, NULL, NULL, 0, NULL, false, NULL};

static ak_struct *action(const char *name) {
	ak_struct *s = ak_struct_new(&action_shape);
	s->f[0].p = ak_str_c(name);
	s->f[1].p = fn(log_run, name);
	return s;
}

static ak_val gesture_app(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_view *v = ak_ui_new(AK_EL_VIEW);
	set_i(v, AK_KEY_WIDTH, ak_length_pt(100));
	set_i(v, AK_KEY_HEIGHT, ak_length_pt(100));
	on(v, AK_EV_VIEW_PAN, log_gesture, "pan");
	on(v, AK_EV_VIEW_PINCH, log_gesture, "pinch");
	on(v, AK_EV_VIEW_SWIPE, log_int, "swipe");
	on(v, AK_EV_VIEW_ADJUST, log_int, "adjust");
	ak_arr *acts = ak_arr_new(AK_K_REF, &action_shape, 2);
	ak_arr_push(&acts, (ak_val){.p = action("Delete")});
	ak_arr_push(&acts, (ak_val){.p = action("Reply")});
	set(v, AK_KEY_ACTIONS, (ak_val){.p = acts});
	return (ak_val){.p = v};
}

// A gesture makes a view of its own; each reaches its handler with what
// the host reports.
TEST(gestures_and_actions) {
	log_buf[0] = 0;
	ak_uitest *a = mount(gesture_app, NULL);
	ak_fake_view *v = nth(a, AK_KIND_VIEW, 1);
	REQUIRE(v);
	CHECK(ak_fake_listens(v, AK_EVENT_PAN) && ak_fake_listens(v, AK_EVENT_SWIPE) && ak_fake_listens(v, AK_EVENT_PINCH));
	CHECK(!ak_fake_listens(v, AK_EVENT_ROTATE));
	CHECK(ak_fake_bool(v, AK_PROP_A11Y_ADJUSTABLE) && ak_fake_listens(v, AK_EVENT_A11Y_ACTION));
	ak_str_vec names = {0};
	ak_fake_strings(v, AK_PROP_A11Y_ACTIONS, &names);
	CHECK(names.len == 2 && ak_span_eqc(names.data[0], "Delete") && ak_span_eqc(names.data[1], "Reply"));
	ak_vec_free(&names);
	dispatch(a, (ak_event){.id = v->id, .kind = AK_EVENT_PAN, .g = {.state = AK_GESTURE_STATE_CHANGED, .translation_x = 12, .scale = 1}});
	dispatch(a, (ak_event){.id = v->id, .kind = AK_EVENT_PINCH, .g = {.state = AK_GESTURE_STATE_ENDED, .scale = 2}});
	dispatch(a, (ak_event){.id = v->id, .kind = AK_EVENT_SWIPE, .i = 2});  // left
	dispatch(a, (ak_event){.id = v->id, .kind = AK_EVENT_ADJUST, .i = -1});
	dispatch(a, (ak_event){.id = v->id, .kind = AK_EVENT_A11Y_ACTION, .i = 1});
	CHECK_EQ_STR(log_buf, "pan:1:12:1 pinch:2:0:2 swipe:1 adjust:-1 Reply");
	done(a);
}

// ---- Drags and drops ----

static ak_struct *drag_item(int64_t kind, const char *text, const char *path, const char *mime, const char *value) {
	ak_struct *s = ak_struct_new(&ak_ui_shape_drag_item);
	s->f[0].i = kind;
	s->f[1].p = ak_str_c(text);
	s->f[2].p = ak_str_c(path);
	s->f[3].p = ak_str_c("");
	s->f[4].p = ak_str_c(mime);
	s->f[5].i = -1;
	s->f[6].p = ak_str_c(value);
	return s;
}

static ak_arr *strs(const char *a, const char *b) {
	ak_arr *x = ak_arr_new(AK_K_REF, NULL, 2);
	x->h.flags = 1;
	if (a) ak_arr_push(&x, (ak_val){.p = ak_str_c(a)});
	if (b) ak_arr_push(&x, (ak_val){.p = ak_str_c(b)});
	return x;
}

static void *card_title;

static ak_val card_items(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_val t = ak_vm_cell_get(card_title, true);
	ak_arr *items = ak_arr_new(AK_K_REF, &ak_ui_shape_drag_item, 1);
	ak_arr_push(&items, (ak_val){.p = drag_item(0, ((ak_str *)t.p)->data, "", "application/x-card", "card-7")});
	ak_release(t.p);
	return (ak_val){.p = items};
}

static ak_val log_move(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(n);
	char s[32];
	snprintf(s, sizeof s, "move:%g,%g", a[0].f, a[1].f);
	logf_(s);
	return noval();
}

static ak_val log_drop(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(n);
	const ak_struct *d = a[0].p;
	const ak_arr *items = d->f[0].p;
	char s[160];
	int k = snprintf(s, sizeof s, "drop:%g,%g:%s%s:", d->f[1].f, d->f[2].f, d->f[3].i ? "local" : "other", d->f[4].i ? ",copied" : "");
	for (uint32_t i = 0; i < items->len && k < (int)sizeof s; i++) {
		const ak_struct *it = items->data[i].p;
		k += snprintf(s + k, sizeof s - (size_t)k, "[%lld %s %s %s %s %lld %s]", (long long)it->f[0].i, ((ak_str *)it->f[1].p)->data,
		              ((ak_str *)it->f[2].p)->data, ((ak_str *)it->f[3].p)->data, ((ak_str *)it->f[4].p)->data, (long long)it->f[5].i,
		              ((ak_str *)it->f[6].p)->data);
	}
	logf_(s);
	return noval();
}

static ak_val drag_app(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_view *col = ak_ui_new(AK_EL_COLUMN);
	ak_view *src = ak_ui_new(AK_EL_VIEW);
	bind(src, AK_KEY_VIEW_DRAG_ITEMS, card_items, NULL);
	on(src, AK_EV_VIEW_DRAG_PHASE, log_int, "phase");
	add(col, src);
	ak_view *target = ak_ui_new(AK_EL_VIEW);
	set(target, AK_KEY_VIEW_DROP_TYPES, (ak_val){.p = strs("application/x-card", NULL)});
	on(target, AK_EV_VIEW_DROP_OVER, log_bool, "over");
	on(target, AK_EV_VIEW_DROP_MOVE, log_move, NULL);
	on(target, AK_EV_VIEW_DROP, log_drop, NULL);
	add(col, target);
	ak_view *images = ak_ui_new(AK_EL_VIEW);
	set(images, AK_KEY_VIEW_DROP_TYPES, (ak_val){.p = strs("image/*", NULL)});
	on(images, AK_EV_VIEW_DROP, log_drop, NULL);
	add(col, images);
	return (ak_val){.p = col};
}

static void send_drop(ak_uitest *a, ak_fake_view *v, ak_drop d, const ak_item *items, uint32_t n) {
	ak_buf b = {0};
	ak_append_drop(&b, &d, items, n);
	dispatch(a, (ak_event){.id = v->id, .kind = AK_EVENT_DROP, .s = ak_strn(b.data, b.len)});
	ak_buf_free(&b);
}

// A drag within the app carries the app's values: the target that takes
// their type gets them as they were when the drag began, and the source
// hears it was dropped.
TEST(drag_within_the_app) {
	log_buf[0] = 0;
	card_title = cell_ref(ak_str_c("Write tests"));
	ak_uitest *a = mount(drag_app, NULL);
	ak_fake_view *src = nth(a, AK_KIND_VIEW, 1), *target = nth(a, AK_KIND_VIEW, 2), *images = nth(a, AK_KIND_VIEW, 3);
	REQUIRE(src && target && images);
	ak_item_vec items = {0};
	ak_span b = bytes_of(src, AK_PROP_VIEW_DRAG_ITEMS);
	ak_decode_drag_items((const uint8_t *)b.p, b.len, &items);
	CHECK(items.len == 1 && items.data[0].value && ak_span_eqc(items.data[0].mime, "application/x-card") &&
	      ak_span_eqc(items.data[0].text, "Write tests") && items.data[0].kind == AK_SHARE_KIND_TEXT);
	ak_vec_free(&items);
	CHECK(ak_fake_listens(target, AK_EVENT_DROP) && ak_fake_listens(src, AK_EVENT_DRAGGING));
	// Dropped on a target that takes none of it: cancelled.
	dispatch(a, (ak_event){.id = src->id, .kind = AK_EVENT_DRAGGING, .i = AK_DRAG_PHASE_BEGAN});
	send_drop(a, images, (ak_drop){.x = 5, .y = 5, .source = src->id}, NULL, 0);
	dispatch(a, (ak_event){.id = src->id, .kind = AK_EVENT_DRAGGING, .i = AK_DRAG_PHASE_CANCELLED});
	CHECK_EQ_STR(log_buf, "phase:0 phase:2");
	// The drag carries what it began with.
	log_buf[0] = 0;
	dispatch(a, (ak_event){.id = src->id, .kind = AK_EVENT_DRAGGING, .i = AK_DRAG_PHASE_BEGAN});
	write_p(card_title, ak_str_c("Changed meanwhile"));
	ak_ui_flush();
	dispatch(a, (ak_event){.id = target->id, .kind = AK_EVENT_DRAG_OVER, .g = {.state = AK_GESTURE_STATE_BEGAN, .x = 10, .y = 20}});
	dispatch(a, (ak_event){.id = target->id, .kind = AK_EVENT_DRAG_OVER, .g = {.state = AK_GESTURE_STATE_CHANGED, .x = 11, .y = 21}});
	send_drop(a, target, (ak_drop){.x = 11, .y = 21, .source = src->id}, NULL, 0);
	dispatch(a, (ak_event){.id = target->id, .kind = AK_EVENT_DRAG_OVER, .g = {.state = AK_GESTURE_STATE_CANCELLED, .x = 11, .y = 21}});
	dispatch(a, (ak_event){.id = src->id, .kind = AK_EVENT_DRAGGING, .i = AK_DRAG_PHASE_DROPPED});
	CHECK_EQ_STR(log_buf, "phase:0 over:on move:10,20 move:11,21 over:off drop:11,21:local:[0 Write tests   application/x-card -1 card-7] phase:1");
	b = bytes_of(src, AK_PROP_VIEW_DRAG_ITEMS);
	ak_decode_drag_items((const uint8_t *)b.p, b.len, &items);
	CHECK(items.len == 1 && ak_span_eqc(items.data[0].text, "Changed meanwhile"));  // the source's items follow it
	ak_vec_free(&items);
	done(a);
	ak_release(card_title);
}

static ak_val drop_app(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_view *v = ak_ui_new(AK_EL_VIEW);
	set(v, AK_KEY_VIEW_DROP_TYPES, (ak_val){.p = strs("image/*", "text/uri-list")});
	on(v, AK_EV_VIEW_DROP_OVER, log_bool, "over");
	on(v, AK_EV_VIEW_DROP, log_drop, NULL);
	return (ak_val){.p = v};
}

// A drop from another app gets the items its target takes, in order.
TEST(drop_from_another_app) {
	log_buf[0] = 0;
	ak_uitest *a = mount(drop_app, NULL);
	ak_fake_view *v = nth(a, AK_KIND_VIEW, 1);
	REQUIRE(v);
	ak_item items[] = {
		{.kind = AK_SHARE_KIND_IMAGE, .path = AK_STR("/caches/IMG_1.heic"), .name = AK_STR("IMG_1.heic"), .mime = AK_STR("image/heic"), .size = 1200},
		{.kind = AK_SHARE_KIND_TEXT, .text = AK_STR("hello"), .mime = AK_STR("text/plain; charset=utf-8"), .size = -1},
		{.kind = AK_SHARE_KIND_LINK, .text = AK_STR("https://asuka.dev"), .mime = AK_STR("text/uri-list"), .size = -1},
	};
	dispatch(a, (ak_event){.id = v->id, .kind = AK_EVENT_DRAG_OVER, .g = {.state = AK_GESTURE_STATE_BEGAN, .x = 3, .y = 4}});
	send_drop(a, v, (ak_drop){.x = 3, .y = 4, .copied = true}, items, 3);
	CHECK_EQ_STR(log_buf,
	             "over:on over:off drop:3,4:other,copied:[2  /caches/IMG_1.heic IMG_1.heic image/heic 1200 ][1 https://asuka.dev   text/uri-list -1 ]");
	done(a);
}

static void *photo_path;

static ak_val wire_items(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_val p = ak_vm_cell_get(photo_path, true);
	ak_arr *items = ak_arr_new(AK_K_REF, &ak_ui_shape_drag_item, 4);
	ak_arr_push(&items, (ak_val){.p = drag_item(0, "", ((ak_str *)p.p)->data, "", "")});
	ak_arr_push(&items, (ak_val){.p = drag_item(0, "plain words", "", "", "")});
	ak_arr_push(&items, (ak_val){.p = drag_item(AK_SHARE_KIND_LINK, "https://asuka.dev", "", "", "")});
	ak_arr_push(&items, (ak_val){.p = drag_item(0, "", "", "", "42")});
	ak_release(p.p);
	return (ak_val){.p = items};
}

static ak_val wire_app(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_view *v = ak_ui_new(AK_EL_VIEW);
	bind(v, AK_KEY_VIEW_DRAG_ITEMS, wire_items, NULL);
	return (ak_val){.p = v};
}

// What a drag carries goes to the host with its kind, path, name and type
// filled in, and again only when it changes.
TEST(drag_items_on_the_wire) {
	photo_path = cell_ref(ak_str_c("photos/cat.png"));
	ak_uitest *a = mount(wire_app, NULL);
	ak_fake_view *v = nth(a, AK_KIND_VIEW, 1);
	REQUIRE(v);
	ak_item_vec items = {0};
	ak_span b = bytes_of(v, AK_PROP_VIEW_DRAG_ITEMS);
	ak_decode_drag_items((const uint8_t *)b.p, b.len, &items);
	REQUIRE(items.len == 4);
	ak_item *cat = &items.data[0];
	CHECK(cat->kind == AK_SHARE_KIND_IMAGE && cat->path.len > 15 && cat->path.p[0] == '/' && ak_span_eqc(cat->name, "cat.png") &&
	      ak_span_eqc(cat->mime, "image/png") && cat->size == -1 && !cat->value);
	CHECK(memcmp(cat->path.p + cat->path.len - 15, "/photos/cat.png", 15) == 0);
	CHECK(items.data[1].kind == AK_SHARE_KIND_TEXT && ak_span_eqc(items.data[1].mime, "text/plain"));
	CHECK(items.data[2].kind == AK_SHARE_KIND_LINK && ak_span_eqc(items.data[2].mime, "text/uri-list"));
	CHECK(items.data[3].value && ak_span_eqc(items.data[3].mime, "application/octet-stream"));
	ak_vec_free(&items);
	reset(a);
	write_p(photo_path, ak_str_c("photos/cat.png"));  // the same: nothing sent
	ak_ui_flush();
	write_p(photo_path, ak_str_c("notes/a.pdf"));
	ak_ui_flush();
	CHECK_EQ_INT(sets(a), 1);
	b = bytes_of(v, AK_PROP_VIEW_DRAG_ITEMS);
	ak_decode_drag_items((const uint8_t *)b.p, b.len, &items);
	CHECK(items.len && items.data[0].kind == AK_SHARE_KIND_FILE && ak_span_eqc(items.data[0].mime, "application/pdf") &&
	      ak_span_eqc(items.data[0].name, "a.pdf"));
	ak_vec_free(&items);
	done(a);
	ak_release(photo_path);
}

static ak_val row_items(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_arr *items = ak_arr_new(AK_K_REF, &ak_ui_shape_drag_item, 1);
	ak_arr_push(&items, (ak_val){.p = drag_item(0, "", "", "application/x-row", "row 3")});
	return (ak_val){.p = items};
}

static ak_val row_source(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_view *v = ak_ui_new(AK_EL_VIEW);
	bind(v, AK_KEY_VIEW_DRAG_ITEMS, row_items, NULL);
	return (ak_val){.p = v};
}

static ak_val went_app(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_view *col = ak_ui_new(AK_EL_COLUMN);
	ak_closure *cond = fn(read_cell, shown), *then = fn(row_source, NULL);
	add(col, ak_ui_show(cond, then, NULL));
	ak_release(cond), ak_release(then);
	ak_view *target = ak_ui_new(AK_EL_VIEW);
	set(target, AK_KEY_VIEW_DROP_TYPES, (ak_val){.p = strs("application/x-row", NULL)});
	on(target, AK_EV_VIEW_DROP, log_drop, NULL);
	add(col, target);
	return (ak_val){.p = col};
}

// A view whose drag is under way and goes away: a drop that comes after
// still gets its items.
TEST(drop_after_the_source_went) {
	log_buf[0] = 0;
	shown = ak_vm_cell_new((ak_val){.i = 1}, AK_K_BOOL, NULL).p;
	ak_uitest *a = mount(went_app, NULL);
	ak_fake_view *src = nth(a, AK_KIND_VIEW, 1), *target = nth(a, AK_KIND_VIEW, 2);
	REQUIRE(src && target);
	ak_id src_id = src->id;
	dispatch(a, (ak_event){.id = src_id, .kind = AK_EVENT_DRAGGING, .i = AK_DRAG_PHASE_BEGAN});
	write_i(shown, 0);
	ak_ui_flush();
	send_drop(a, target, (ak_drop){.source = src_id}, NULL, 0);
	CHECK_EQ_STR(log_buf, "drop:0,0:local:[0    application/x-row -1 row 3]");
	done(a);
	ak_release(shown);
}

T_MAIN()
