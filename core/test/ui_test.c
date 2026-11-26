// The UI runtime (core/src/ui) on the C fake host: the tree, the flush,
// text fields, media, locales, and the work each does. They build views in C, as compiled code
// does, through ui.h's protocol, with closures of C functions, and assert
// the ops the fake host was sent, its view tree, and the work done.
#include "t.h"
#include "fake/uitest.h"
#include "ui/ui_int.h"
#include "vm/vm_int.h"
#include "fuzz.h"

#include <stdio.h>

// ---- Helpers ----

// The prelude's structs, in types.asuka's field order.
static const ak_shape *const no_shapes[8];
static const uint8_t edges_kinds[] = {AK_K_FLOAT, AK_K_FLOAT, AK_K_FLOAT, AK_K_FLOAT, AK_K_FLOAT, AK_K_FLOAT};
static const ak_shape edges_shape = {"Edges", 6, edges_kinds, no_shapes, NULL, 0, NULL, false, NULL};
static const uint8_t todo_kinds[] = {AK_K_INT, AK_K_REF, AK_K_BOOL};
static const ak_shape todo_shape = {"Todo", 3, todo_kinds, no_shapes, NULL, 0, NULL, false, NULL};
static const uint8_t range_kinds[] = {AK_K_INT, AK_K_INT};
static const ak_shape range_shape = {"TextRange", 2, range_kinds, no_shapes, NULL, 0, NULL, false, NULL};

static void setup(void) {
	ak_signal_bind_thread();
	ak_vm_init();
	ak_ui_builtins_init();
}

static ak_val edges4(float t, float r, float b, float l) {
	ak_struct *s = ak_struct_new(&edges_shape);
	s->f[0].f = t, s->f[1].f = r, s->f[2].f = b, s->f[3].f = l;
	return (ak_val){.p = s};
}

static ak_val edges_se(float start, float end) {
	ak_struct *s = ak_struct_new(&edges_shape);
	s->f[4].f = start, s->f[5].f = end;
	return (ak_val){.p = s};
}

// set sets a key and lets the value go.
static void set(ak_view *v, uint16_t key, ak_val x) {
	ak_ui_set(v, key, x);
	ak_release_val(x, (ak_kind)ak_key_kinds[key]);
}
static void set_str(ak_view *v, uint16_t key, const char *s) { set(v, key, (ak_val){.p = ak_str_c(s)}); }
static void set_i(ak_view *v, uint16_t key, int64_t i) { ak_ui_set(v, key, (ak_val){.i = i}); }
static void set_f(ak_view *v, uint16_t key, double f) { ak_ui_set(v, key, (ak_val){.f = f}); }

// add adds child and lets it go (the parent holds it).
static void add(ak_view *p, ak_view *c) {
	ak_ui_add(p, c);
	ak_release(c);
}

static ak_view *text(const char *s) {
	ak_view *t = ak_ui_new(AK_EL_TEXT);
	set_str(t, AK_KEY_TEXT_TEXT, s);
	return t;
}

static ak_closure *fn(ak_invoke_fn f, const void *data) { return ak_closure_new(f, data, 0, NULL); }

// A closure's capture kinds outlive it: static.
static const uint8_t ref_cap[] = {AK_K_REF};

static void bind(ak_view *v, uint16_t key, ak_invoke_fn f, const void *data) {
	ak_closure *c = fn(f, data);
	ak_ui_bind(v, key, c);
	ak_release(c);
}

static void on(ak_view *v, uint16_t ev, ak_invoke_fn f, const void *data) {
	ak_closure *c = fn(f, data);
	ak_ui_on(v, ev, c);
	ak_release(c);
}

static void *cell_int(int64_t v) { return ak_vm_cell_new((ak_val){.i = v}, AK_K_INT, NULL).p; }
static void *cell_ref(void *p) { return ak_vm_cell_new((ak_val){.p = p}, AK_K_REF, NULL).p; }
static int64_t peek_i(void *cell) { return ak_vm_cell_get(cell, false).i; }
static void write_i(void *cell, int64_t v) { ak_vm_cell_set(cell, (ak_val){.i = v}); }
static void write_p(void *cell, void *p) { ak_vm_cell_set(cell, (ak_val){.p = p}); }  // takes p

static ak_val read_cell(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(a), AK_UNUSED(n);
	return ak_vm_cell_get((void *)c->fn, true);
}

static ak_uitest *mount(ak_invoke_fn build, const void *data) {
	setup();
	ak_closure *c = fn(build, data);
	ak_uitest *a = ak_uitest_mount(c, NULL);
	ak_release(c);
	return a;
}

static const char *cstr(ak_buf *b) {
	ak_buf_u8(b, 0);
	b->len--;
	return (const char *)b->data;
}

// done unmounts the app and checks it leaves nothing: no native view,
// no live view object.
static void done(ak_uitest *a) {
	ak_buf err = {0};
	if (!ak_uitest_unmount(a, &err)) T_FAIL("%s", cstr(&err));
	ak_buf_free(&err);
	ak_release(a);
	ak_ui_stats s;
	ak_ui_get_stats(&s);
	if (s.views != 0 || s.native != 0) T_FAIL("%u views, %u native views outlived the app", s.views, s.native);
}

static ak_fake_view *root(ak_uitest *a) { return ak_fake_root(ak_uitest_host(a)); }

static void check_tree(ak_uitest *a, const char *want) {
	ak_buf b = {0};
	ak_fake_tree(root(a), &b);
	CHECK_EQ_STR(cstr(&b), want);
	ak_buf_free(&b);
}

static void check_text(ak_fake_view *v, const char *want) {
	ak_buf b = {0};
	ak_fake_text(v, &b);
	CHECK_EQ_STR(cstr(&b), want);
	ak_buf_free(&b);
}

static void dump_log(ak_fake *f) {
	ak_buf b = {0};
	ak_fake_log(f, &b);
	fprintf(stderr, "%s\n", cstr(&b));
	ak_buf_free(&b);
}

static ak_fake_view *one(ak_uitest *a, ak_view_kind k, const char *s) { return ak_fake_one(ak_uitest_host(a), k, s); }

static ak_fake_view *nth(ak_uitest *a, ak_view_kind k, uint32_t i) {
	ak_fake_view_vec vs = {0};
	ak_fake_of_kind(ak_uitest_host(a), k, &vs);
	ak_fake_view *v = i < vs.len ? vs.data[i] : NULL;
	ak_vec_free(&vs);
	return v;
}

static uint32_t count_kind(ak_uitest *a, ak_view_kind k) {
	ak_fake_view_vec vs = {0};
	ak_fake_of_kind(ak_uitest_host(a), k, &vs);
	uint32_t n = vs.len;
	ak_vec_free(&vs);
	return n;
}

static void check_frame(const ak_fake_view *v, float x, float y, float w, float h) {
	if (!v) {
		T_FAIL("no view");
		return;
	}
	if (v->frame[0] != x || v->frame[1] != y || v->frame[2] != w || v->frame[3] != h)
		T_FAIL("frame %g,%g %gx%g, want %g,%g %gx%g", v->frame[0], v->frame[1], v->frame[2], v->frame[3], x, y, w, h);
}

// ---- The counter (ui_test.go's TestCounter) ----

static void *count;

static ak_val count_text(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	char buf[32];
	snprintf(buf, sizeof buf, "Count: %lld", (long long)ak_vm_cell_get(count, true).i);
	return (ak_val){.p = ak_str_c(buf)};
}

static ak_val add_one(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	write_i(count, peek_i(count) + 1);
	return (ak_val){0};
}

static ak_val lots(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	return (ak_val){.i = ak_vm_cell_get(count, true).i > 2};
}

static ak_val lot_text(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	return (ak_val){.p = text("That's a lot")};
}

static ak_val counter(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_view *col = ak_ui_new(AK_EL_COLUMN);
	set_f(col, AK_KEY_GAP, 8);
	set(col, AK_KEY_PADDING, edges4(16, 16, 16, 16));
	ak_view *t = ak_ui_new(AK_EL_TEXT);
	bind(t, AK_KEY_TEXT_TEXT, count_text, NULL);
	add(col, t);
	ak_view *b = ak_ui_new(AK_EL_BUTTON);
	set_str(b, AK_KEY_BUTTON_TITLE, "Add");
	on(b, AK_EV_BUTTON_PRESS, add_one, NULL);
	add(col, b);
	ak_closure *cond = fn(lots, NULL), *then = fn(lot_text, NULL);
	add(col, ak_ui_show(cond, then, NULL));
	ak_release(cond);
	ak_release(then);
	return (ak_val){.p = col};
}

TEST(counter) {
	count = cell_int(0);
	ak_uitest *a = mount(counter, NULL);
	ak_fake *f = ak_uitest_host(a);
	CHECK_EQ_INT(f->applies, 2);  // props, then frames
	// The Column is flattened: Text and Button sit in the root view, offset
	// by its padding.
	check_tree(a, "View 0,0 390x844\n  Text 16,16 358x20 \"Count: 0\"\n  Button 16,44 358x44 \"Add\"\n");
	ak_fake_view *btn = one(a, AK_KIND_BUTTON, "Add");
	ak_fake_tap(f, btn);
	check_text(root(a), "Count: 1|Add");
	ak_fake_tap(f, btn);
	ak_fake_tap(f, btn);
	check_text(root(a), "Count: 3|Add|That's a lot");
	done(a);
	ak_release(count);
}

// The same app through the test driver (host/fake/uitest.c): tap and
// expect by text.
TEST(uitest_counter) {
	count = cell_int(0);
	ak_uitest *a = mount(counter, NULL);
	ak_buf err = {0};
	CHECK(ak_uitest_tap(a, AK_STR("Add"), &err));
	CHECK(ak_uitest_expect(a, AK_STR("Count: 1"), &err));
	CHECK(ak_uitest_expect_no(a, AK_STR("That's a lot"), &err));
	CHECK(!ak_uitest_expect(a, AK_STR("Count: 9"), &err));
	CHECK(strstr(cstr(&err), "doesn't show \"Count: 9\"") != NULL);
	CHECK(strstr(cstr(&err), "Text 16,16 358x20 \"Count: 1\"") != NULL);  // what it shows
	ak_buf_reset(&err);
	CHECK(!ak_uitest_tap(a, AK_STR("Remove"), &err));
	ak_buf_free(&err);
	done(a);
	ak_release(count);
}

// ---- One write, one SetProp (TestOneSetOneSetProp, TestEqualWriteSendsNothing) ----

static void *name;

static ak_val two_texts(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_view *col = ak_ui_new(AK_EL_COLUMN);
	ak_view *t = ak_ui_new(AK_EL_TEXT);
	bind(t, AK_KEY_TEXT_TEXT, read_cell, name);
	add(col, t);
	add(col, text("below"));
	return (ak_val){.p = col};
}

TEST(one_set_one_setprop) {
	name = cell_ref(ak_str_c("Ada"));
	ak_uitest *a = mount(two_texts, NULL);
	ak_fake *f = ak_uitest_host(a);
	ak_fake_reset(f);
	write_p(name, ak_str_c("Grace"));
	ak_ui_flush();
	if (ak_fake_count(f, AK_OP_SET_PROP) != 1 || f->measures > 1) {
		T_FAIL("%u SetProps, %d measures", ak_fake_count(f, AK_OP_SET_PROP), f->measures);
		dump_log(f);
	}
	CHECK_EQ_INT(ak_fake_count(f, AK_OP_SET_FRAME), 0);  // the same size: nothing moved
	CHECK_EQ_INT(f->applies, 1);
	// Taller: it and what follows move.
	ak_fake_reset(f);
	char longer[101];
	memset(longer, 'x', 100);
	longer[100] = 0;
	write_p(name, ak_str_c(longer));
	ak_ui_flush();
	CHECK_EQ_INT(ak_fake_count(f, AK_OP_SET_PROP), 1);
	CHECK_EQ_INT(ak_fake_count(f, AK_OP_SET_FRAME), 2);
	done(a);
	ak_release(name);
}

static void *num;

static ak_val parity(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	return (ak_val){.p = ak_str_c(ak_vm_cell_get(num, true).i % 2 ? "1" : "0")};
}

static ak_val parity_text(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_view *t = ak_ui_new(AK_EL_TEXT);
	bind(t, AK_KEY_TEXT_TEXT, parity, NULL);
	return (ak_val){.p = t};
}

TEST(equal_write_sends_nothing) {
	num = cell_int(1);
	ak_uitest *a = mount(parity_text, NULL);
	ak_fake *f = ak_uitest_host(a);
	ak_fake_reset(f);
	write_i(num, 3);  // the text stays "1"
	ak_ui_flush();
	CHECK_EQ_INT(ak_fake_op_count(f), 0);
	CHECK_EQ_INT(f->applies, 0);
	done(a);
	ak_release(num);
}

// ---- Flattening (TestFlattenedContainerMakesNoView, TestStyleFuncMakesContainerReal) ----

static ak_val flat(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_view *col = ak_ui_new(AK_EL_COLUMN);
	set(col, AK_KEY_PADDING, edges4(10, 10, 10, 10));
	ak_view *row = ak_ui_new(AK_EL_ROW);
	set_f(row, AK_KEY_GAP, 4);
	add(row, text("a"));
	add(row, text("b"));
	add(col, row);
	ak_view *red = ak_ui_new(AK_EL_VIEW);
	set_i(red, AK_KEY_BACKGROUND, (int64_t)ak_rgba(255, 0, 0, 255));
	add(red, text("c"));
	add(col, red);
	return (ak_val){.p = col};
}

TEST(flattened_container_makes_no_view) {
	ak_uitest *a = mount(flat, NULL);
	CHECK_EQ_INT(count_kind(a, AK_KIND_VIEW), 2);  // the root and the red view
	check_tree(a, "View 0,0 390x844\n  Text 10,10 8x20 \"a\"\n  Text 22,10 8x20 \"b\"\n  View 10,30 370x20\n    Text 0,0 370x20 \"c\"\n");
	done(a);
}

static void *blue;

static ak_val blue_bg(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	return (ak_val){.i = ak_vm_cell_get(blue, true).i ? (int64_t)ak_rgba(0, 0, 255, 255) : 0};
}

static ak_val becoming_real(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_view *col = ak_ui_new(AK_EL_COLUMN);
	set(col, AK_KEY_PADDING, edges4(10, 10, 10, 10));
	ak_view *inner = ak_ui_new(AK_EL_COLUMN);
	set(inner, AK_KEY_PADDING, edges4(5, 5, 5, 5));
	bind(inner, AK_KEY_BACKGROUND, blue_bg, NULL);
	add(inner, text("x"));
	add(col, inner);
	return (ak_val){.p = col};
}

TEST(bound_look_makes_container_real) {
	blue = cell_int(0);
	ak_uitest *a = mount(becoming_real, NULL);
	check_tree(a, "View 0,0 390x844\n  Text 15,15 360x20 \"x\"\n");
	write_i(blue, 1);
	ak_ui_flush();
	check_tree(a, "View 0,0 390x844\n  View 10,10 370x30\n    Text 5,5 360x20 \"x\"\n");
	done(a);
	ak_release(blue);
}

// ---- For (TestForMovesOneRow) ----

static void *items;
static int created;

static ak_val todo(int64_t id, const char *title) {
	ak_struct *s = ak_struct_new(&todo_shape);
	s->f[0].i = id;
	s->f[1].p = ak_str_c(title);
	return (ak_val){.p = s};
}

static ak_val todo_key(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(n);
	return (ak_val){.i = ((ak_struct *)a[0].p)->f[0].i};
}

static ak_val todo_title(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(a), AK_UNUSED(n);
	ak_val it = ak_vm_acc_get((void *)c->fn);
	ak_str *s = ak_retain(((ak_struct *)it.p)->f[1].p);
	ak_release(it.p);
	return (ak_val){.p = s};
}

static ak_val todo_done(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(a), AK_UNUSED(n);
	ak_val it = ak_vm_acc_get((void *)c->fn);
	bool d = ((ak_struct *)it.p)->f[2].i != 0;
	ak_release(it.p);
	return (ak_val){.p = ak_str_c(d ? "✓" : "")};
}

// A row's Text follows its item's title through the item accessor (its
// closure's data; the accessor lives as long as the row's binding).
static ak_val todo_row(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(n);
	created++;
	ak_view *t = ak_ui_new(AK_EL_TEXT);
	ak_closure *b = ak_closure_new(todo_title, a[0].p, 1, ref_cap);
	b->caps[0].p = ak_retain(a[0].p);  // holds the accessor
	ak_ui_bind(t, AK_KEY_TEXT_TEXT, b);
	ak_release(b);
	return (ak_val){.p = t};
}

static ak_val items_of(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	return ak_vm_cell_get(items, true);
}

static ak_val for_column(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_view *col = ak_ui_new(AK_EL_COLUMN);
	ak_closure *it = fn(items_of, NULL), *key = fn(todo_key, NULL), *row = fn(todo_row, NULL);
	add(col, ak_ui_for(it, key, row, AK_K_INT));
	ak_release(it), ak_release(key), ak_release(row);
	return (ak_val){.p = col};
}

static ak_arr *todos(int n) {
	ak_arr *arr = ak_arr_new(AK_K_REF, &todo_shape, (uint32_t)n);
	for (int i = 0; i < n; i++) {
		char t[32];
		snprintf(t, sizeof t, "item %d", i);
		ak_arr_push(&arr, todo(i, t));
	}
	return arr;
}

static ak_arr *copy(ak_arr *a) { return ak_arr_slice(a, 0, a->len); }

TEST(for_moves_one_row) {
	ak_arr *start = todos(1000);
	items = cell_ref(ak_retain(start));
	created = 0;
	ak_uitest *a = mount(for_column, NULL);
	ak_fake *f = ak_uitest_host(a);
	CHECK_EQ_INT(created, 1000);
	CHECK_EQ_INT(root(a)->children.len, 1000);
	// Move item 10 to the end.
	ak_arr *next = copy(start);
	ak_val moved = ak_arr_remove_at(&next, 10);
	ak_arr_push(&next, moved);
	ak_fake_reset(f);
	write_p(items, ak_retain(next));
	ak_ui_flush();
	if (ak_fake_count(f, AK_OP_REMOVE) != 1 || ak_fake_count(f, AK_OP_INSERT) != 1 || ak_fake_count(f, AK_OP_CREATE) != 0 || ak_fake_count(f, AK_OP_SET_PROP) != 0) {
		T_FAIL("remove %u insert %u create %u setprop %u", ak_fake_count(f, AK_OP_REMOVE), ak_fake_count(f, AK_OP_INSERT),
		       ak_fake_count(f, AK_OP_CREATE), ak_fake_count(f, AK_OP_SET_PROP));
	}
	check_text(root(a)->children.data[999], "item 10");
	check_text(root(a)->children.data[10], "item 11");
	// Change one item: one SetProp, nothing made.
	ak_arr *changed = copy(next);
	ak_arr_set(&changed, 500, todo(((ak_struct *)ak_arr_vals(changed)[500].p)->f[0].i, "changed"));
	ak_fake_reset(f);
	write_p(items, ak_retain(changed));
	ak_ui_flush();
	CHECK_EQ_INT(ak_fake_count(f, AK_OP_SET_PROP), 1);
	CHECK_EQ_INT(created, 1000);
	// Remove one: one Destroy.
	ak_arr *fewer = ak_arr_slice(changed, 1, changed->len);
	ak_fake_reset(f);
	write_p(items, fewer);
	ak_ui_flush();
	CHECK_EQ_INT(ak_fake_count(f, AK_OP_DESTROY), 1);
	CHECK_EQ_INT(root(a)->children.len, 999);
	done(a);
	ak_release(items);
	ak_release(start), ak_release(next), ak_release(changed);
}

// ---- Show (TestShowDisposesItsBranch) ----

static void *on_cell, *ticks;
static int runs;

static ak_val tick_effect(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_vm_cell_get(ticks, true);
	runs++;
	return (ak_val){0};
}

static ak_val ticks_text(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	char b[16];
	snprintf(b, sizeof b, "%lld", (long long)ak_vm_cell_get(ticks, true).i);
	return (ak_val){.p = ak_str_c(b)};
}

static ak_val show_then(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_closure *e = fn(tick_effect, NULL);
	ak_vm_effect(e, false);
	ak_release(e);
	ak_view *row = ak_ui_new(AK_EL_ROW);
	ak_view *t = ak_ui_new(AK_EL_TEXT);
	bind(t, AK_KEY_TEXT_TEXT, ticks_text, NULL);
	add(row, t);
	add(row, text("!"));
	return (ak_val){.p = row};
}

static ak_val show_else(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	return (ak_val){.p = text("off")};
}

static ak_val show_app(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_view *col = ak_ui_new(AK_EL_COLUMN);
	add(col, text("top"));
	ak_closure *cond = fn(read_cell, on_cell), *then = fn(show_then, NULL), *otherwise = fn(show_else, NULL);
	add(col, ak_ui_show(cond, then, otherwise));
	ak_release(cond), ak_release(then), ak_release(otherwise);
	add(col, text("bottom"));
	return (ak_val){.p = col};
}

TEST(show_disposes_its_branch) {
	on_cell = ak_vm_cell_new((ak_val){.i = 1}, AK_K_BOOL, NULL).p;
	ticks = cell_int(0);
	ak_uitest *a = mount(show_app, NULL);
	check_text(root(a), "top|0|!|bottom");
	ak_vm_cell_set(on_cell, (ak_val){.i = 0});
	ak_ui_flush();
	check_text(root(a), "top|off|bottom");
	runs = 0;
	write_i(ticks, 1);
	ak_ui_flush();
	CHECK_EQ_INT(runs, 0);  // an effect of a disposed branch ran
	CHECK_EQ_INT(count_kind(a, AK_KIND_TEXT), 3);
	ak_vm_cell_set(on_cell, (ak_val){.i = 1});
	ak_ui_flush();
	check_text(root(a), "top|1|!|bottom");
	done(a);
	ak_release(on_cell), ak_release(ticks);
}

// ---- Switch and Dynamic ----

static void *which;

static ak_val case_of(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	return (ak_val){.i = ak_vm_cell_get(which, true).i};
}

static ak_val case_view(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(n);
	char b[16];
	snprintf(b, sizeof b, "case %lld", (long long)a[0].i);
	return (ak_val){.p = text(b)};
}

static ak_val dyn_view(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	char b[16];
	snprintf(b, sizeof b, "dyn %lld", (long long)ak_vm_cell_get(which, true).i);
	return (ak_val){.p = text(b)};
}

static ak_val switch_app(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_view *col = ak_ui_new(AK_EL_COLUMN);
	ak_closure *sel = fn(case_of, NULL), *build = fn(case_view, NULL), *dyn = fn(dyn_view, NULL);
	add(col, ak_ui_switch(sel, build));
	add(col, ak_ui_dynamic(dyn));
	ak_release(sel), ak_release(build), ak_release(dyn);
	return (ak_val){.p = col};
}

TEST(switch_and_dynamic) {
	which = cell_int(1);
	ak_uitest *a = mount(switch_app, NULL);
	ak_fake *f = ak_uitest_host(a);
	check_text(root(a), "case 1|dyn 1");
	ak_fake_reset(f);
	write_i(which, 2);
	ak_ui_flush();
	check_text(root(a), "case 2|dyn 2");
	CHECK_EQ_INT(ak_fake_count(f, AK_OP_DESTROY), 2);
	CHECK_EQ_INT(ak_fake_count(f, AK_OP_CREATE), 2);
	done(a);
	ak_release(which);
}

// ---- Text input (TestTextFieldTypingDoesNotRoundTrip, TestTextFieldSelection) ----

static void *field_text, *submitted, *sel_cell;

static ak_val set_text_handler(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(n);
	write_p(field_text, ak_retain(a[0].p));
	return (ak_val){0};
}

static ak_val submit_handler(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	write_p(submitted, ak_vm_cell_get(field_text, false).p);
	write_p(field_text, ak_str_c(""));
	return (ak_val){0};
}

static ak_val field_app(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_view *col = ak_ui_new(AK_EL_COLUMN);
	ak_view *tf = ak_ui_new(AK_EL_TEXT_FIELD);
	bind(tf, AK_KEY_TEXT_FIELD_TEXT, read_cell, field_text);
	on(tf, AK_EV_TEXT_FIELD_CHANGE, set_text_handler, NULL);
	set_str(tf, AK_KEY_TEXT_FIELD_PLACEHOLDER, "New todo");
	on(tf, AK_EV_TEXT_FIELD_SUBMIT, submit_handler, NULL);
	add(col, tf);
	ak_view *t = ak_ui_new(AK_EL_TEXT);
	bind(t, AK_KEY_TEXT_TEXT, read_cell, field_text);
	add(col, t);
	return (ak_val){.p = col};
}

TEST(text_field_typing_does_not_round_trip) {
	field_text = cell_ref(ak_str_c(""));
	submitted = cell_ref(ak_str_c(""));
	ak_uitest *a = mount(field_app, NULL);
	ak_fake *f = ak_uitest_host(a);
	ak_fake_view *tf = nth(a, AK_KIND_TEXT_FIELD, 0);
	ak_fake_reset(f);
	ak_fake_type(f, tf, "milk");
	for (uint32_t i = 0; i < ak_fake_op_count(f); i++) {
		ak_op op = ak_fake_op_at(f, i);
		if (op.id == tf->id && op.code == AK_OP_SET_PROP) T_FAIL("typing echoed back");
	}
	check_text(root(a), "milk");
	ak_fake_submit(f, tf);
	ak_val s = ak_vm_cell_get(submitted, false);
	CHECK_EQ_STR(((ak_str *)s.p)->data, "milk");
	ak_release(s.p);
	CHECK(ak_fake_string(tf, AK_PROP_TEXT_FIELD_VALUE).len == 0);
	// Typing through the driver, into the field its placeholder names.
	ak_buf err = {0};
	CHECK(ak_uitest_type(a, AK_STR("eggs"), AK_STR("New todo"), &err));
	CHECK(ak_uitest_expect(a, AK_STR("eggs"), &err));
	ak_buf_free(&err);
	done(a);
	ak_release(field_text), ak_release(submitted);
}

static ak_val set_sel_handler(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(n);
	write_p(sel_cell, ak_retain(a[0].p));
	return (ak_val){0};
}

static ak_val selection_app(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_view *tf = ak_ui_new(AK_EL_TEXT_FIELD);
	bind(tf, AK_KEY_TEXT_FIELD_TEXT, read_cell, field_text);
	on(tf, AK_EV_TEXT_FIELD_CHANGE, set_text_handler, NULL);
	bind(tf, AK_KEY_TEXT_FIELD_SELECTION, read_cell, sel_cell);
	on(tf, AK_EV_TEXT_FIELD_SELECT, set_sel_handler, NULL);
	return (ak_val){.p = tf};
}

static ak_val range(int64_t s, int64_t e) {
	ak_struct *r = ak_struct_new(&range_shape);
	r->f[0].i = s, r->f[1].i = e;
	return (ak_val){.p = r};
}

static void sel_bytes(uint8_t b[8], uint32_t s, uint32_t e) {
	for (int i = 0; i < 4; i++) b[i] = (uint8_t)(s >> (8 * i)), b[4 + i] = (uint8_t)(e >> (8 * i));
}

TEST(text_field_selection) {
	field_text = cell_ref(ak_str_c("héllo 👋 world"));
	sel_cell = cell_ref(range(0, 0).p);
	ak_uitest *a = mount(selection_app, NULL);
	ak_fake *f = ak_uitest_host(a);
	ak_fake_view *tf = nth(a, AK_KIND_TEXT_FIELD, 0);
	CHECK(ak_fake_listens(tf, AK_EVENT_SELECTION_CHANGED));
	// After the emoji: UTF-16 offset 8 ("héllo " 6, 👋 2), byte 11.
	ak_fake_reset(f);
	uint8_t b[8];
	sel_bytes(b, 8, 8);
	ak_fake_dispatch(f, &(ak_event){.id = tf->id, .kind = AK_EVENT_SELECTION_CHANGED, .s = ak_strn(b, 8)});
	ak_val s = ak_vm_cell_get(sel_cell, false);
	CHECK_EQ_INT(((ak_struct *)s.p)->f[0].i, 11);
	CHECK_EQ_INT(((ak_struct *)s.p)->f[1].i, 11);
	ak_release(s.p);
	CHECK_EQ_INT(ak_fake_count(f, AK_OP_SET_PROP), 0);  // the user's move doesn't go back
	// "world" selected from code: UTF-16 9 to 14.
	write_p(sel_cell, range(12, 17).p);
	ak_ui_flush();
	ak_span got = ak_fake_string(tf, AK_PROP_TEXT_FIELD_SELECTION);
	sel_bytes(b, 9, 14);
	CHECK(got.len == 8 && memcmp(got.p, b, 8) == 0);
	done(a);
	ak_release(field_text), ak_release(sel_cell);
}

// ---- Toggle (TestToggle) and the other controls ----

static void *toggled;

static ak_val set_bool_handler(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(n);
	ak_vm_cell_set((void *)c->fn, (ak_val){.i = a[0].i});
	return (ak_val){0};
}

static ak_val on_text(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	return (ak_val){.p = ak_str_c(ak_vm_cell_get(toggled, true).i ? "true" : "false")};
}

static ak_val toggle_app(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_view *row = ak_ui_new(AK_EL_ROW);
	ak_view *t = ak_ui_new(AK_EL_TOGGLE);
	bind(t, AK_KEY_TOGGLE_ON, read_cell, toggled);
	on(t, AK_EV_TOGGLE_CHANGE, set_bool_handler, toggled);
	add(row, t);
	ak_view *x = ak_ui_new(AK_EL_TEXT);
	bind(x, AK_KEY_TEXT_TEXT, on_text, NULL);
	add(row, x);
	return (ak_val){.p = row};
}

TEST(toggle) {
	toggled = ak_vm_cell_new((ak_val){.i = 0}, AK_K_BOOL, NULL).p;
	ak_uitest *a = mount(toggle_app, NULL);
	ak_fake *f = ak_uitest_host(a);
	ak_fake_view *sw = nth(a, AK_KIND_TOGGLE, 0);
	ak_fake_reset(f);
	ak_fake_flip(f, sw);
	CHECK(peek_i(toggled) == 1);
	check_text(root(a), "true");
	for (uint32_t i = 0; i < ak_fake_op_count(f); i++) {
		ak_op op = ak_fake_op_at(f, i);
		if (op.id == sw->id && op.code == AK_OP_SET_PROP) T_FAIL("toggle echoed back");
	}
	done(a);
	ak_release(toggled);
}

static void *seg_sel, *picked, *slid;

static ak_val set_int_handler(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(n);
	ak_vm_cell_set((void *)c->fn, (ak_val){.i = a[0].i});
	return (ak_val){0};
}

static ak_val set_float_handler(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(n);
	ak_vm_cell_set((void *)c->fn, (ak_val){.f = a[0].f});
	return (ak_val){0};
}

static ak_arr *strings(const char *const *s, int n) {
	ak_arr *arr = ak_arr_new(AK_K_REF, NULL, (uint32_t)n);
	arr->h.flags = 1;
	for (int i = 0; i < n; i++) ak_arr_push(&arr, (ak_val){.p = ak_str_c(s[i])});
	return arr;
}

static ak_val controls_app(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_view *col = ak_ui_new(AK_EL_COLUMN);
	static const char *const segs[] = {"Day", "Week", "Month"};
	ak_view *s = ak_ui_new(AK_EL_SEGMENTED);
	set(s, AK_KEY_SEGMENTED_SEGMENTS, (ak_val){.p = strings(segs, 3)});
	bind(s, AK_KEY_SEGMENTED_SELECTED, read_cell, seg_sel);
	on(s, AK_EV_SEGMENTED_CHANGE, set_int_handler, seg_sel);
	add(col, s);
	static const char *const fruits[] = {"Apple", "Pear"};
	ak_view *p = ak_ui_new(AK_EL_PICKER);
	set(p, AK_KEY_PICKER_ITEMS, (ak_val){.p = strings(fruits, 2)});
	set_str(p, AK_KEY_PICKER_PROMPT, "Fruit");
	on(p, AK_EV_PICKER_CHANGE, set_int_handler, picked);
	add(col, p);
	ak_view *sl = ak_ui_new(AK_EL_SLIDER);
	bind(sl, AK_KEY_SLIDER_VALUE, read_cell, slid);
	on(sl, AK_EV_SLIDER_CHANGE, set_float_handler, slid);
	add(col, sl);
	ak_view *st = ak_ui_new(AK_EL_STEPPER);
	set_f(st, AK_KEY_STEPPER_STEP, 5);
	add(col, st);
	add(col, ak_ui_new(AK_EL_PROGRESS));
	ak_view *ring = ak_ui_new(AK_EL_PROGRESS_RING);
	set(ring, AK_KEY_PROGRESS_RING_VALUE, (ak_val){.p = ak_box_new(AK_K_FLOAT, (ak_val){.f = 1.5})});
	add(col, ring);
	add(col, ak_ui_new(AK_EL_ACTIVITY));
	ak_view *sym = ak_ui_new(AK_EL_SYMBOL);
	set_str(sym, AK_KEY_SYMBOL_NAME, "star");
	add(col, sym);
	return (ak_val){.p = col};
}

TEST(controls) {
	seg_sel = cell_int(1);
	picked = cell_int(-1);
	slid = ak_vm_cell_new((ak_val){.f = 0.25}, AK_K_FLOAT, NULL).p;
	ak_uitest *a = mount(controls_app, NULL);
	ak_fake *f = ak_uitest_host(a);
	ak_fake_view *s = nth(a, AK_KIND_SEGMENTED, 0), *p = nth(a, AK_KIND_PICKER, 0), *sl = nth(a, AK_KIND_SLIDER, 0);
	CHECK_EQ_INT(ak_fake_int(s, AK_PROP_SEGMENTED_SELECTED), 1);
	CHECK_EQ_INT(ak_fake_int(p, AK_PROP_PICKER_SELECTED), -1);  // none chosen: it shows its prompt
	CHECK_EQ_F(ak_fake_float(sl, AK_PROP_SLIDER_MAX), 1);
	CHECK_EQ_F(ak_fake_float(sl, AK_PROP_SLIDER_VALUE), 0.25);
	CHECK_EQ_F(ak_fake_float(nth(a, AK_KIND_STEPPER, 0), AK_PROP_STEPPER_MAX), 100);
	CHECK_EQ_F(ak_fake_float(nth(a, AK_KIND_STEPPER, 0), AK_PROP_STEPPER_STEP), 5);
	CHECK(ak_fake_bool(nth(a, AK_KIND_PROGRESS, 0), AK_PROP_PROGRESS_INDETERMINATE));
	CHECK_EQ_F(ak_fake_float(nth(a, AK_KIND_PROGRESS_RING, 0), AK_PROP_PROGRESS_RING_VALUE), 1);  // clamped
	CHECK(!ak_fake_bool(nth(a, AK_KIND_PROGRESS_RING, 0), AK_PROP_PROGRESS_RING_INDETERMINATE));
	CHECK(ak_fake_bool(nth(a, AK_KIND_ACTIVITY, 0), AK_PROP_ACTIVITY_ANIMATING));
	check_frame(nth(a, AK_KIND_SYMBOL, 0), 0, nth(a, AK_KIND_SYMBOL, 0)->frame[1], 390, 22);
	ak_buf err = {0};
	CHECK(ak_uitest_tap(a, AK_STR("Month"), &err));
	CHECK_EQ_INT(peek_i(seg_sel), 2);
	CHECK(ak_uitest_expect(a, AK_STR("Fruit"), &err));
	ak_fake_reset(f);
	ak_fake_select(f, p, 1);
	CHECK_EQ_INT(peek_i(picked), 1);
	CHECK(ak_uitest_expect(a, AK_STR("Pear"), &err));
	ak_fake_slide(f, sl, 0.75);
	CHECK_EQ_F(ak_vm_cell_get(slid, false).f, 0.75);
	for (uint32_t i = 0; i < ak_fake_op_count(f); i++) {
		ak_op op = ak_fake_op_at(f, i);
		if (op.code == AK_OP_SET_PROP && (op.id == sl->id || op.id == p->id) && op.prop != AK_PROP_CONTENT_INSET) T_FAIL("a control's change echoed back: prop %u", op.prop);
	}
	ak_buf_free(&err);
	done(a);
	ak_release(seg_sel), ak_release(picked), ak_release(slid);
}

// ---- SafeArea (TestSafeAreaAndKeyboard) ----

static ak_val safe_app(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_view *s = ak_ui_new(AK_EL_SAFE_AREA);
	set(s, AK_KEY_PADDING, edges4(4, 4, 4, 4));
	add(s, text("hi"));
	return (ak_val){.p = s};
}

TEST(safe_area_and_keyboard) {
	ak_uitest *a = mount(safe_app, NULL);
	ak_fake *f = ak_uitest_host(a);
	ak_fake_view *t = one(a, AK_KIND_TEXT, "hi");
	check_frame(t, 4, 51, 382, 20);
	ak_environment env = f->env;
	env.keyboard = 300;
	ak_fake_set_environment(f, &env);
	check_frame(t, 4, 51, 382, 20);  // the column ends above the keyboard; the text at its top didn't move
	env.width = 844, env.height = 390;  // rotate
	env.safe_left = env.safe_right = 47, env.safe_top = 0;
	ak_fake_set_environment(f, &env);
	check_frame(t, 51, 4, 742, 20);
	check_frame(root(a), 0, 0, 844, 390);
	done(a);
}

// ---- Lists (TestListBindsOnlyWhatTheHostAsks, TestDisposingAListDestroysItsRows) ----

static int rows_made;

static ak_val list_row(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(n);
	rows_made++;
	ak_view *row = ak_ui_new(AK_EL_ROW);
	set(row, AK_KEY_PADDING, edges4(12, 12, 12, 12));
	set_f(row, AK_KEY_GAP, 8);
	ak_view *t = ak_ui_new(AK_EL_TEXT);
	ak_closure *b = ak_closure_new(todo_title, a[0].p, 1, ref_cap);
	b->caps[0].p = ak_retain(a[0].p);
	ak_ui_bind(t, AK_KEY_TEXT_TEXT, b);
	ak_release(b);
	add(row, t);
	ak_view *d = ak_ui_new(AK_EL_TEXT);
	b = ak_closure_new(todo_done, a[0].p, 1, ref_cap);
	b->caps[0].p = ak_retain(a[0].p);
	ak_ui_bind(d, AK_KEY_TEXT_TEXT, b);
	ak_release(b);
	add(row, d);
	return (ak_val){.p = row};
}

static ak_val list_app(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_view *l = ak_ui_new(AK_EL_LIST);
	ak_closure *it = fn(items_of, NULL), *key = fn(todo_key, NULL), *row = fn(list_row, NULL);
	ak_ui_lazy_for(l, it, key, row, AK_K_INT);
	ak_release(it), ak_release(key), ak_release(row);
	return (ak_val){.p = l};
}

TEST(list_binds_only_what_the_host_asks) {
	ak_arr *all = todos(10000);
	items = cell_ref(ak_retain(all));
	rows_made = 0;
	ak_uitest *a = mount(list_app, NULL);
	ak_fake *f = ak_uitest_host(a);
	ak_fake_view *list = nth(a, AK_KIND_LIST, 0);
	check_frame(list, 0, 0, 390, 844);
	CHECK_EQ_INT(ak_fake_list_count(f, list), 10000);
	int bound_by_driver = rows_made;
	CHECK(bound_by_driver > 0 && bound_by_driver < 40);  // what fits the screen
	ak_fake_view *cells[20];
	for (int i = 0; i < 20; i++) {
		cells[i] = ak_fake_new_cell(f);
		ak_fake_bind(f, list, 5000 + i, cells[i]);
	}
	CHECK_EQ_INT(rows_made, bound_by_driver + 20);
	check_frame(cells[3], 0, 0, 390, 44);
	check_text(cells[3], "item 5003");
	// Recycle cell 0 for item 7000: one SetProp, nothing made.
	ak_fake_reset(f);
	ak_fake_bind(f, list, 7000, cells[0]);
	CHECK_EQ_INT(rows_made, bound_by_driver + 20);
	CHECK_EQ_INT(ak_fake_count(f, AK_OP_SET_PROP), 1);
	CHECK_EQ_INT(ak_fake_count(f, AK_OP_CREATE), 0);
	check_text(cells[0], "item 7000");
	// An item changes: its bound row updates in place.
	ak_arr *next = copy(all);
	ak_struct *t = ak_struct_new(&todo_shape);
	t->f[0].i = 7000;
	t->f[1].p = ak_str_c("item 7000");
	t->f[2].i = 1;
	ak_arr_set(&next, 7000, (ak_val){.p = t});
	ak_fake_reset(f);
	write_p(items, next);
	ak_ui_flush();
	check_text(cells[0], "item 7000|✓");
	CHECK_EQ_INT(ak_fake_count(f, AK_OP_SET_PROP), 1);
	// Unbinding a cell disposes its row.
	ak_fake_unbind(f, list, cells[1]);
	CHECK_EQ_INT(cells[1]->children.len, 0);
	done(a);
	ak_release(items), ak_release(all);
}

// A host that binds a row into an ID that isn't a cell's of its own (0,
// one of the core's views', another list's cell) is wrong: the bind is
// dropped, and the views keep their IDs, rather than two views sharing
// one and the ID table naming a freed one.
TEST(list_bind_into_bad_cell_ids) {
	ak_arr *all = todos(100);
	items = cell_ref(ak_retain(all));
	ak_uitest *a = mount(list_app, NULL);
	ak_fake *f = ak_uitest_host(a);
	ak_fake_view *list = nth(a, AK_KIND_LIST, 0);
	ak_fake_view *text = nth(a, AK_KIND_TEXT, 0);
	REQUIRE(list && text);
	int made = rows_made;
	ak_fake_view bad = {.kind = AK_FAKE_KIND_CELL};
	uint32_t ids[] = {0, list->id, text->id};
	for (size_t i = 0; i < AK_LEN(ids); i++) {
		bad.id = ids[i];
		ak_fake_bind(f, list, 50, &bad);
	}
	CHECK_EQ_INT(rows_made, made);
	CHECK(ak_view_of(list->id) != NULL && ak_view_of(list->id)->el == AK_EL_LIST);
	// A cell bound twice is recycled, as it should be.
	ak_fake_view *c = ak_fake_new_cell(f);
	ak_fake_bind(f, list, 60, c);
	ak_fake_bind(f, list, 61, c);
	CHECK_EQ_INT(rows_made, made + 1);
	check_text(c, "item 61");
	done(a);
	ak_release(items), ak_release(all);
}

static void *list_shown;

static ak_val list_branch(ak_closure *c, ak_val *a, uint32_t n) { return list_app(c, a, n); }

static ak_val shown_list_app(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_closure *cond = fn(read_cell, list_shown), *then = fn(list_branch, NULL);
	ak_view *v = ak_ui_show(cond, then, NULL);
	ak_release(cond), ak_release(then);
	return (ak_val){.p = v};
}

// A List going (a Show off) destroys its bound rows' views, which the
// hosts keep by ID until they're destroyed.
TEST(disposing_a_list_destroys_its_rows) {
	ak_arr *all = todos(10);
	items = cell_ref(all);
	list_shown = ak_vm_cell_new((ak_val){.i = 1}, AK_K_BOOL, NULL).p;
	ak_uitest *a = mount(shown_list_app, NULL);
	ak_fake *f = ak_uitest_host(a);
	CHECK_EQ_INT(count_kind(a, AK_KIND_TEXT), 20);  // the driver bound the 10 rows: a title and a mark each
	ak_vm_cell_set(list_shown, (ak_val){.i = 0});
	ak_ui_flush();
	ak_uitest_settle(a);
	CHECK_EQ_INT(count_kind(a, AK_KIND_TEXT), 0);
	CHECK_EQ_INT(count_kind(a, AK_KIND_LIST), 0);
	AK_UNUSED(f);
	done(a);
	ak_release(items), ak_release(list_shown);
}

// endReached is posted once per length, as the last rows bind.
static int ends;

static ak_val end_handler(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ends++;
	return (ak_val){0};
}

static ak_val ending_list_app(ak_closure *c, ak_val *a, uint32_t n) {
	ak_val v = list_app(c, a, n);
	on(v.p, AK_EV_LIST_END_REACHED, end_handler, NULL);
	return v;
}

TEST(list_end_reached) {
	items = cell_ref(todos(5));
	ends = 0;
	ak_uitest *a = mount(ending_list_app, NULL);
	CHECK_EQ_INT(ends, 1);  // the driver bound them all, the last near the end
	ak_ui_flush();
	ak_uitest_settle(a);
	CHECK_EQ_INT(ends, 1);
	done(a);
	ak_release(items);
}

// ---- Steady state and memory (TestSteadyStateFlushAllocatesNothing, TestDisposeDestroysEverything) ----

static ak_str *labels[2];
static void *sel;

static ak_val label_of(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	return (ak_val){.p = ak_retain(labels[ak_vm_cell_get(sel, true).i])};
}

static ak_val steady_app(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_view *col = ak_ui_new(AK_EL_COLUMN);
	set(col, AK_KEY_PADDING, edges4(8, 8, 8, 8));
	ak_view *row = ak_ui_new(AK_EL_ROW);
	set_f(row, AK_KEY_GAP, 4);
	ak_view *t = ak_ui_new(AK_EL_TEXT);
	bind(t, AK_KEY_TEXT_TEXT, label_of, NULL);
	add(row, t);
	add(row, text("static"));
	add(col, row);
	ak_view *t2 = ak_ui_new(AK_EL_TEXT);
	bind(t2, AK_KEY_TEXT_TEXT, label_of, NULL);
	add(col, t2);
	return (ak_val){.p = col};
}

// The host's own work (the fake host copies what it's sent) isn't the
// core's: what it allocates is left out of the count.
static ak_fake *steady_host;

static void steady_apply(void *ctx, const uint8_t *buf, size_t len) {
	AK_UNUSED(ctx);
	uint64_t n = ak_alloc_count;
	ak_fake_apply(steady_host, buf, len);
	ak_alloc_count = n;
}

static void steady_measure(void *ctx, uint32_t id, float w, float h, float aw, float ah, float *ow, float *oh) {
	AK_UNUSED(ctx);
	uint64_t n = ak_alloc_count;
	ak_fake_measure(steady_host, id, w, h, aw, ah, ow, oh);
	ak_alloc_count = n;
}

TEST(steady_state_flush_allocates_nothing) {
	setup();
	labels[0] = ak_str_c("one");
	labels[1] = ak_str_c("two");
	sel = cell_int(0);
	steady_host = ak_fake_new();
	ak_ui_init(&(ak_host){NULL, steady_apply, steady_measure});
	ak_ui_set_environment(&steady_host->env);
	ak_closure *c = fn(steady_app, NULL);
	ak_ui_mount(c);
	ak_release(c);
	// Warm up: the batches, the layout pass and the measure cache grow
	// to their size.
	for (int i = 1; i <= 4; i++) {
		ak_fake_reset(steady_host);
		write_i(sel, i % 2);
		ak_ui_flush();
	}
	uint64_t allocs = 0;
	for (int i = 0; i < 200; i++) {
		ak_fake_reset(steady_host);
		uint64_t before = ak_alloc_count;
		write_i(sel, i % 2);
		ak_ui_flush();
		allocs += ak_alloc_count - before;
	}
	if (allocs != 0) T_FAIL("%llu allocations in 200 updates", (unsigned long long)allocs);
	CHECK_EQ_INT(ak_fake_count(steady_host, AK_OP_SET_PROP), 2);  // the work was done: two texts changed
	ak_ui_unmount();
	ak_fake_free(steady_host);
	ak_release(sel), ak_release(labels[0]), ak_release(labels[1]);
	ak_ui_stats st;
	ak_ui_get_stats(&st);
	CHECK_EQ_INT(st.views, 0);
}

// An effect that registers a cleanup each run (cleanup { } in its body,
// rfcs/0016): the cleanup runs before the effect's next run and as its
// owner goes, and the core allocates nothing to register and run it in
// the steady state. (The closure is the program's: what cleanup { }
// lowers to makes one per run, as any closure an effect's body makes.)
static void *cl_cell;
static ak_closure *cl_undo;
static int cl_runs, cl_undone;

static ak_val cl_undo_fn(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	cl_undone++;
	return (ak_val){0};
}

static ak_val cl_effect_fn(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	(void)ak_vm_cell_get(cl_cell, true);
	cl_runs++;
	ak_vm_cleanup(cl_undo);
	return (ak_val){0};
}

static void cl_build(void *ctx, ak_owner *root) {
	AK_UNUSED(ctx), AK_UNUSED(root);
	ak_closure *e = fn(cl_effect_fn, NULL);
	ak_vm_effect(e, false);
	ak_release(e);
}

TEST(effect_cleanup_runs_before_each_run_and_allocates_nothing) {
	setup();
	cl_runs = cl_undone = 0;
	cl_cell = cell_int(0);
	cl_undo = fn(cl_undo_fn, NULL);
	ak_owner *r = ak_root(cl_build, NULL);
	CHECK_EQ_INT(cl_runs, 1);
	CHECK_EQ_INT(cl_undone, 0);
	write_i(cl_cell, 1);
	CHECK_EQ_INT(cl_runs, 2);
	CHECK_EQ_INT(cl_undone, 1);  // the first run's, before the second
	uint64_t before = ak_alloc_count;
	for (int i = 2; i < 1002; i++) write_i(cl_cell, i);
	if (ak_alloc_count != before) T_FAIL("%llu allocations in 1000 runs", (unsigned long long)(ak_alloc_count - before));
	CHECK_EQ_INT(cl_undone, 1001);
	ak_owner_dispose(r);
	CHECK_EQ_INT(cl_undone, 1002);  // the last run's, as it goes
	write_i(cl_cell, 0);
	CHECK_EQ_INT(cl_runs, 1002);
	ak_release(cl_undo);
	ak_release(cl_cell);
}

TEST(mount_unmount_keeps_nothing) {
	for (int round = 0; round < 3; round++) {
		count = cell_int(0);
		items = cell_ref(todos(100));
		ak_uitest *a = mount(counter, NULL);
		ak_buf err = {0};
		CHECK(ak_uitest_tap(a, AK_STR("Add"), &err));
		ak_buf_free(&err);
		done(a);  // no native view, no view object left
		ak_release(count);
		a = mount(for_column, NULL);
		done(a);
		ak_release(items);
	}
}

// ---- The measure cache (TestIdenticalContentMeasuresOnce) ----

static int nrows;

static ak_val rows_app(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_view *col = ak_ui_new(AK_EL_COLUMN);
	for (int i = 0; i < nrows; i++) {
		ak_view *row = ak_ui_new(AK_EL_ROW);
		add(row, text("same label"));
		ak_view *b = ak_ui_new(AK_EL_BUTTON);
		set_str(b, AK_KEY_BUTTON_TITLE, "Go");
		add(row, b);
		add(col, row);
	}
	return (ak_val){.p = col};
}

static int measures_of(int n) {
	nrows = n;
	ak_uitest *a = mount(rows_app, NULL);
	int m = ak_uitest_host(a)->measures;
	done(a);
	return m;
}

TEST(identical_content_measures_once) {
	// Identical rows share measurements: 200 rows ask the host no more
	// than 50 do (both overflow the screen, so they lay out alike).
	int a = measures_of(50), b = measures_of(200);
	if (a != b || a > 12) T_FAIL("50 rows: %d measures, 200 rows: %d", a, b);
}

// ---- Applying (TestFlushWhileApplying) ----

static ak_fake *hooked;
static void (*during)(void);

static void hook_apply(void *ctx, const uint8_t *buf, size_t len) {
	AK_UNUSED(ctx);
	ak_fake_apply(hooked, buf, len);
	if (during) {
		void (*d)(void) = during;
		during = NULL;
		d();
	}
}

static void hook_measure(void *ctx, uint32_t id, float w, float h, float aw, float ah, float *ow, float *oh) {
	AK_UNUSED(ctx);
	ak_fake_measure(hooked, id, w, h, aw, ah, ow, oh);
}

static void nested(void) {
	write_i(num, 2);
	ak_ui_flush();
}

static ak_val num_text(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	char b[16];
	snprintf(b, sizeof b, "%lld", (long long)ak_vm_cell_get(num, true).i);
	ak_view *t = ak_ui_new(AK_EL_TEXT);
	set_str(t, AK_KEY_TEXT_TEXT, b);
	return (ak_val){.p = t};
}

static ak_val num_label(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	char b[16];
	snprintf(b, sizeof b, "%lld", (long long)ak_vm_cell_get(num, true).i);
	return (ak_val){.p = ak_str_c(b)};
}

static ak_val num_app(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_view *t = ak_ui_new(AK_EL_TEXT);
	bind(t, AK_KEY_TEXT_TEXT, num_label, NULL);
	return (ak_val){.p = t};
}

TEST(flush_while_applying) {
	setup();
	num = cell_int(0);
	hooked = ak_fake_new();
	ak_fake_core core = {NULL, NULL, NULL, NULL, NULL, NULL};
	hooked->core = core;
	ak_ui_init(&(ak_host){NULL, hook_apply, hook_measure});
	ak_ui_set_environment(&hooked->env);
	ak_closure *c = fn(num_app, NULL);
	ak_ui_mount(c);
	ak_release(c);
	ak_fake_reset(hooked);
	during = nested;
	write_i(num, 1);
	ak_ui_flush();
	// The nested flush applies only its own op, not the batch being applied.
	CHECK_EQ_INT(ak_fake_count(hooked, AK_OP_SET_PROP), 2);
	CHECK(ak_fake_one(hooked, AK_KIND_TEXT, "2") != NULL);
	ak_ui_unmount();
	ak_fake_free(hooked);
	ak_release(num);
	AK_UNUSED(num_text);
}

// ---- Frames and insets (TestSnappingNeverShrinksMeasuredContent, TestPaddedButtonGetsItsInsetBeforeItsMeasured) ----

static void measure_wide(void *ctx, ak_fake_view *v, float w, float h, float aw, float ah, float *ow, float *oh) {
	AK_UNUSED(ctx), AK_UNUSED(v), AK_UNUSED(w), AK_UNUSED(h), AK_UNUSED(aw), AK_UNUSED(ah);
	*ow = 113.90476f, *oh = 48;
}

static ak_val note_app(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_view *row = ak_ui_new(AK_EL_ROW);
	set(row, AK_KEY_PADDING, edges4(20, 20, 20, 20));
	ak_view *b = ak_ui_new(AK_EL_BUTTON);
	set_str(b, AK_KEY_BUTTON_TITLE, "Add a note");
	add(row, b);
	return (ak_val){.p = row};
}

TEST(snapping_never_shrinks_measured_content) {
	setup();
	ak_closure *c = fn(note_app, NULL);
	// A Pixel's screen, at 2.625 pixels a point.
	ak_environment e = {.width = 412, .height = 915, .scale = 2.625f, .font_scale = 1, .platform = AK_PLATFORM_ANDROID};
	ak_uitest *a = ak_uitest_mount(c, &e);
	// Content 113.90476 wide (299px) at x 20: rounding both edges to
	// pixels would make it 298px.
	ak_fake *f = ak_uitest_host(a);
	f->measure_fn = measure_wide;
	ak_ui_remeasure();
	const ak_fake_view *v = one(a, AK_KIND_BUTTON, "Add a note");
	double px = round((double)v->frame[2] * 2.625);
	if (px < 299) T_FAIL("frame width %g is %gpx", v->frame[2], px);
	done(a);
	ak_release(c);
}

static void *pad;
static float seen_padded[4], seen_plain[4], seen_bordered[4], seen_zero[4];

static void measure_seen(void *ctx, ak_fake_view *v, float w, float h, float aw, float ah, float *ow, float *oh) {
	AK_UNUSED(ctx), AK_UNUSED(w), AK_UNUSED(h), AK_UNUSED(aw), AK_UNUSED(ah);
	ak_span t = ak_fake_string(v, AK_PROP_BUTTON_TITLE);
	const ak_value *in = ak_fake_prop_of(v, AK_PROP_CONTENT_INSET);
	float e[4] = {0};
	if (in) memcpy(e, in->edges, sizeof e);
	if (ak_span_eqc(t, "Padded")) memcpy(seen_padded, e, sizeof e);
	if (ak_span_eqc(t, "Plain")) memcpy(seen_plain, e, sizeof e);
	if (ak_span_eqc(t, "Bordered")) memcpy(seen_bordered, e, sizeof e);
	if (ak_span_eqc(t, "Zero")) memcpy(seen_zero, e, sizeof e);
	*ow = 60, *oh = 20;
}

static ak_val pad_edges(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	float p = (float)ak_vm_cell_get(pad, true).f;
	return edges4(p, 22, p, 22);
}

static ak_val buttons_app(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_view *col = ak_ui_new(AK_EL_COLUMN);
	set_i(col, AK_KEY_ALIGN, 1);  // start
	ak_view *b = ak_ui_new(AK_EL_BUTTON);
	set_str(b, AK_KEY_BUTTON_TITLE, "Padded");
	bind(b, AK_KEY_PADDING, pad_edges, NULL);
	add(col, b);
	b = ak_ui_new(AK_EL_BUTTON);
	set_str(b, AK_KEY_BUTTON_TITLE, "Plain");
	add(col, b);
	b = ak_ui_new(AK_EL_BUTTON);
	set_str(b, AK_KEY_BUTTON_TITLE, "Bordered");
	set_f(b, AK_KEY_BORDER, 2);
	add(col, b);
	b = ak_ui_new(AK_EL_BUTTON);
	set_str(b, AK_KEY_BUTTON_TITLE, "Zero");
	set(b, AK_KEY_PADDING, edges4(0, 0, 0, 0));
	set_f(b, AK_KEY_BORDER, 1);
	add(col, b);
	return (ak_val){.p = col};
}

static ak_fake *seen_host;

static void apply_seen(void *ctx, const uint8_t *buf, size_t len) {
	AK_UNUSED(ctx);
	ak_fake_apply(seen_host, buf, len);
}

static void measure_seen_cb(void *ctx, uint32_t id, float w, float h, float aw, float ah, float *ow, float *oh) {
	AK_UNUSED(ctx);
	ak_fake_measure(seen_host, id, w, h, aw, ah, ow, oh);
}

// A button's padding replaces the platform's own, so the host has to
// know of it before it measures the button: its ContentInset comes with
// the style, not after the layout it's measured for.
TEST(padded_button_gets_its_inset_before_its_measured) {
	setup();
	pad = ak_vm_cell_new((ak_val){.f = 10}, AK_K_FLOAT, NULL).p;
	seen_host = ak_fake_new();
	seen_host->measure_fn = measure_seen;
	ak_ui_init(&(ak_host){NULL, apply_seen, measure_seen_cb});
	ak_ui_set_environment(&seen_host->env);
	ak_closure *c = fn(buttons_app, NULL);
	ak_ui_mount(c);
	ak_release(c);
	CHECK(seen_padded[0] == 10 && seen_padded[1] == 22 && seen_padded[2] == 10 && seen_padded[3] == 22);
	CHECK(seen_plain[0] == 0 && seen_plain[1] == 0);
	// A border alone keeps the platform's padding, as CSS's does.
	CHECK(seen_bordered[0] == 0 && seen_bordered[1] == 0);
	// A padding of 0 is a padding: it replaces the platform's, and the
	// inset is the border.
	CHECK(seen_zero[0] == 1 && seen_zero[1] == 1 && seen_zero[2] == 1 && seen_zero[3] == 1);
	CHECK_EQ_F(ak_fake_one(seen_host, AK_KIND_BUTTON, "Bordered")->frame[3], 24);  // its measure and its border
	CHECK_EQ_F(ak_fake_one(seen_host, AK_KIND_BUTTON, "Padded")->frame[3], 40);    // its content and its padding
	ak_vm_cell_set(pad, (ak_val){.f = 4});
	ak_ui_flush();
	ak_fake_view *v = ak_fake_one(seen_host, AK_KIND_BUTTON, "Padded");
	const ak_value *in = ak_fake_prop_of(v, AK_PROP_CONTENT_INSET);
	CHECK(in && in->edges[0] == 4 && in->edges[1] == 22);
	CHECK_EQ_F(v->frame[3], 28);
	ak_ui_unmount();
	ak_fake_free(seen_host);
	ak_release(pad);
}

// ---- Images (TestImageTakesItsNaturalSize) ----

static float loaded[2];

static ak_val on_load(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(n);
	loaded[0] = (float)a[0].f, loaded[1] = (float)a[1].f;
	return (ak_val){0};
}

static ak_val images_app(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_view *col = ak_ui_new(AK_EL_COLUMN);
	set_i(col, AK_KEY_ALIGN, 1);
	ak_view *i = ak_ui_new(AK_EL_IMAGE);
	set_str(i, AK_KEY_IMAGE_SOURCE, "https://example.com/a.png");
	on(i, AK_EV_IMAGE_LOADED, on_load, NULL);
	add(col, i);
	i = ak_ui_new(AK_EL_IMAGE);
	set_str(i, AK_KEY_IMAGE_SOURCE, "b.png");
	set_i(i, AK_KEY_WIDTH, ak_length_pt(100));
	add(col, i);
	return (ak_val){.p = col};
}

TEST(image_takes_its_natural_size) {
	ak_uitest *a = mount(images_app, NULL);
	ak_fake *f = ak_uitest_host(a);
	ak_fake_view *x = nth(a, AK_KIND_IMAGE, 0), *y = nth(a, AK_KIND_IMAGE, 1);
	CHECK(ak_span_eqc(ak_fake_string(x, AK_PROP_IMAGE_SOURCE), "https://example.com/a.png"));
	CHECK_EQ_F(x->frame[2], 0);
	CHECK_EQ_F(y->frame[2], 100);
	CHECK_EQ_F(y->frame[3], 0);
	// Natural sizes arrive: no wider than the root (390), aspect kept.
	ak_fake_dispatch(f, &(ak_event){.id = x->id, .kind = AK_EVENT_LOADED, .x = 800, .y = 400});
	ak_fake_dispatch(f, &(ak_event){.id = y->id, .kind = AK_EVENT_LOADED, .x = 200, .y = 100});
	check_frame(x, 0, 0, 390, 195);
	check_frame(y, 0, 195, 100, 50);
	CHECK(loaded[0] == 800 && loaded[1] == 400);
	done(a);
}

// ---- Scroll ----

static ak_val scroll_app(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_view *s = ak_ui_new(AK_EL_SCROLL);
	set_i(s, AK_KEY_HEIGHT, ak_length_pt(100));
	set(s, AK_KEY_PADDING, edges4(10, 10, 10, 10));  // its content's
	set_f(s, AK_KEY_GAP, 5);
	for (int i = 0; i < 20; i++) add(s, text("line"));
	return (ak_val){.p = s};
}

TEST(scroll_lays_out_its_content) {
	ak_uitest *a = mount(scroll_app, NULL);
	ak_fake_view *s = nth(a, AK_KIND_SCROLL, 0);
	check_frame(s, 0, 0, 390, 100);
	REQUIRE(s->children.len == 1);
	ak_fake_view *content = s->children.data[0];
	// 20 lines of 20, 19 gaps of 5, padded by 10: it scrolls.
	check_frame(content, 0, 0, 390, 20 * 20 + 19 * 5 + 20);
	check_frame(content->children.data[1], 10, 35, 370, 20);
	done(a);
}

// ---- Right to left (TestRightToLeftLayout) ----

static ak_view *labelled(const char *label) {
	ak_view *v = ak_ui_new(AK_EL_VIEW);
	set_str(v, AK_KEY_LABEL, label);
	return v;
}

static ak_val rtl_app(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_view *col = ak_ui_new(AK_EL_COLUMN);
	ak_view *row = ak_ui_new(AK_EL_ROW);
	set_i(row, AK_KEY_HEIGHT, ak_length_pt(20));
	ak_view *first = labelled("first");
	set_i(first, AK_KEY_WIDTH, ak_length_pt(30));
	add(row, first);
	ak_view *second = labelled("second");
	set_i(second, AK_KEY_WIDTH, ak_length_pt(40));
	add(row, second);
	add(col, row);
	ak_view *logical = labelled("logical");
	set_i(logical, AK_KEY_HEIGHT, ak_length_pt(10));
	set(logical, AK_KEY_MARGIN, edges_se(5, 7));
	add(col, logical);
	ak_view *physical = labelled("physical");
	set_i(physical, AK_KEY_HEIGHT, ak_length_pt(10));
	set(physical, AK_KEY_MARGIN, edges4(0, 7, 0, 5));
	add(col, physical);
	ak_view *box = ak_ui_new(AK_EL_VIEW);
	set_i(box, AK_KEY_HEIGHT, ak_length_pt(30));
	ak_view *inset = labelled("inset");
	set_i(inset, AK_KEY_ABSOLUTE, 1);
	set_i(inset, AK_KEY_START, ak_length_pt(3));
	set_i(inset, AK_KEY_WIDTH, ak_length_pt(9));
	set_i(inset, AK_KEY_HEIGHT, ak_length_pt(9));
	add(box, inset);
	add(col, box);
	// A subtree in a direction of its own, whatever the app's.
	ak_view *ltr = ak_ui_new(AK_EL_ROW);
	set_i(ltr, AK_KEY_LAYOUT_DIRECTION, AK_LAYOUT_DIRECTION_LEFT_TO_RIGHT);
	ak_view *pinned = labelled("pinned");
	set_i(pinned, AK_KEY_WIDTH, ak_length_pt(10));
	add(ltr, pinned);
	add(col, ltr);
	return (ak_val){.p = col};
}

static float x_of(ak_uitest *a, const char *label) {
	ak_fake_view_vec vs = {0};
	ak_fake_of_kind(ak_uitest_host(a), AK_KIND_VIEW, &vs);
	float x = -1;
	for (uint32_t i = 0; i < vs.len; i++)
		if (ak_span_eqc(ak_fake_string(vs.data[i], AK_PROP_A11Y_LABEL), label)) x = vs.data[i]->frame[0];
	ak_vec_free(&vs);
	if (x < 0) T_FAIL("no view labelled %s", label);
	return x;
}

TEST(right_to_left_layout) {
	ak_uitest *a = mount(rtl_app, NULL);
	CHECK_EQ_F(x_of(a, "first"), 0);
	CHECK_EQ_F(x_of(a, "second"), 30);
	CHECK_EQ_F(x_of(a, "logical"), 5);
	CHECK_EQ_F(x_of(a, "physical"), 5);
	CHECK_EQ_F(x_of(a, "inset"), 3);
	CHECK_EQ_F(x_of(a, "pinned"), 0);
	ak_ui_set_direction(true);
	CHECK_EQ_F(x_of(a, "first"), 390 - 30);
	CHECK_EQ_F(x_of(a, "second"), 390 - 70);
	CHECK_EQ_F(x_of(a, "logical"), 7);
	CHECK_EQ_F(x_of(a, "physical"), 5);
	CHECK_EQ_F(x_of(a, "inset"), 390 - 12);
	CHECK_EQ_F(x_of(a, "pinned"), 0);  // left to right whatever the app's
	ak_fake_view *pinned = NULL;
	ak_fake_view_vec vs = {0};
	ak_fake_of_kind(ak_uitest_host(a), AK_KIND_VIEW, &vs);
	for (uint32_t i = 0; i < vs.len; i++)
		if (ak_span_eqc(ak_fake_string(vs.data[i], AK_PROP_A11Y_LABEL), "pinned")) pinned = vs.data[i];
	ak_vec_free(&vs);
	CHECK(pinned && ak_fake_int(pinned, AK_PROP_LAYOUT_DIRECTION) == AK_LAYOUT_DIRECTION_LEFT_TO_RIGHT);
	ak_ui_set_direction(false);
	CHECK_EQ_F(x_of(a, "first"), 0);
	CHECK_EQ_F(x_of(a, "logical"), 5);
	done(a);
}

// ---- The environment ----

static ak_val env_text(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_val e = ak_ui_environment_value();
	const ak_struct *s = e.p;
	char b[64];
	snprintf(b, sizeof b, "%gx%g %s%s", s->f[0].f, s->f[1].f, s->f[9].i ? "dark" : "light", s->f[20].i ? " tv" : "");
	ak_release(e.p);
	return (ak_val){.p = ak_str_c(b)};
}

static ak_val env_app(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_view *t = ak_ui_new(AK_EL_TEXT);
	bind(t, AK_KEY_TEXT_TEXT, env_text, NULL);
	return (ak_val){.p = t};
}

TEST(environment_follows_the_host) {
	ak_uitest *a = mount(env_app, NULL);
	ak_fake *f = ak_uitest_host(a);
	ak_buf err = {0};
	CHECK(ak_uitest_expect(a, AK_STR("390x844 light"), &err));
	ak_environment env = f->env;
	env.dark = true;
	env.width = 844, env.height = 390;
	ak_fake_set_environment(f, &env);
	CHECK(ak_uitest_expect(a, AK_STR("844x390 dark"), &err));
	ak_environment got;
	ak_ui_environment(&got);
	CHECK_EQ_INT(got.width_class, AK_SIZE_CLASS_EXPANDED);  // Material's, from 844 points
	// A TV (Apple TV, Android TV): environment().tv.
	env.tv = true;
	env.width = 1920, env.height = 1080;
	ak_fake_set_environment(f, &env);
	CHECK(ak_uitest_expect(a, AK_STR("1920x1080 dark tv"), &err));
	ak_buf_free(&err);
	done(a);
}

// ---- Errors, contexts, paths ----

static int caught;

static void count_errors(void *ctx, void *err) {
	AK_UNUSED(ctx), AK_UNUSED(err);
	caught++;
}

static ak_val throwing(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_throw_msg("boom");
	return (ak_val){0};
}

static ak_val throwing_app(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_view *b = ak_ui_new(AK_EL_BUTTON);
	set_str(b, AK_KEY_BUTTON_TITLE, "Boom");
	on(b, AK_EV_BUTTON_PRESS, throwing, NULL);
	return (ak_val){.p = b};
}

// An error a handler throws goes to the catcher of what made the view
// (the default handler with none), and the app goes on.
TEST(handler_errors_go_to_the_catcher) {
	caught = 0;
	ak_signal_set_error_handler(count_errors, NULL);
	ak_uitest *a = mount(throwing_app, NULL);
	ak_buf err = {0};
	CHECK(ak_uitest_tap(a, AK_STR("Boom"), &err));
	CHECK_EQ_INT(caught, 1);
	CHECK(!ak_error_pending());
	ak_buf_free(&err);
	done(a);
	ak_signal_set_error_handler(NULL, NULL);
}

static ak_str *paths[4];
static int npaths;

static ak_val record_path(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_str *k = ak_str_c("Row.x");
	if (npaths < 4) paths[npaths++] = ak_ui_path_key(k);
	ak_release(k);
	return (ak_val){0};
}

static ak_val path_row(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_str *name = ak_str_c("Row");
	ak_ui_enter(name);
	record_path(NULL, NULL, 0);
	ak_ui_leave();
	ak_release(name);
	return (ak_val){.p = text("row")};
}

static ak_val str_items(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	static const char *const keys[] = {"milk", "eggs"};
	return (ak_val){.p = strings(keys, 2)};
}

static ak_val str_key(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(n);
	return (ak_val){.p = ak_retain(a[0].p)};
}

static ak_val path_app(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_str *app = ak_str_c("App"), *card = ak_str_c("Card");
	ak_ui_enter(app);
	ak_view *col = ak_ui_new(AK_EL_COLUMN);
	for (int i = 0; i < 2; i++) {
		ak_ui_enter(card);
		if (i == 1) record_path(NULL, NULL, 0);
		ak_ui_leave();
	}
	ak_closure *it = fn(str_items, NULL), *key = fn(str_key, NULL), *row = fn(path_row, NULL);
	add(col, ak_ui_for(it, key, row, AK_K_REF));
	ak_release(it), ak_release(key), ak_release(row);
	ak_ui_leave();
	ak_release(app), ak_release(card);
	return (ak_val){.p = col};
}

TEST(instance_paths) {
	npaths = 0;
	ak_uitest *a = mount(path_app, NULL);
	REQUIRE(npaths == 3);
	CHECK_EQ_STR(paths[0]->data, "App#0/Card#1/Row.x");
	CHECK_EQ_STR(paths[1]->data, "App#0/[milk]/Row#0/Row.x");
	CHECK_EQ_STR(paths[2]->data, "App#0/[eggs]/Row#0/Row.x");
	for (int i = 0; i < npaths; i++) ak_release(paths[i]);
	done(a);
}

// A component's name is its module's constant (immortal, freed with the
// module); the path, which a reload keeps, must not hold it.
static ak_str *const_name;

static ak_val const_app(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_ui_enter(const_name);
	ak_view *t = text("app");
	ak_ui_leave();
	return (ak_val){.p = t};
}

TEST(instance_paths_outlive_module) {
	const_name = ak_str_c("App");
	ak_make_immortal(const_name);
	ak_uitest *a = mount(const_app, NULL);
	done(a);
	ak_free(const_name);  // the module released (ak_module_release)
	ak_path_reset();      // the next mount's
}

static ak_val use_theme(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_str *k = ak_str_c("Theme");
	ak_val v = ak_ui_use(k);
	ak_release(k);
	ak_view *t = text(v.p ? ((ak_str *)v.p)->data : "none");
	ak_release(v.p);
	return (ak_val){.p = t};
}

static ak_val provide_app(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_str *k = ak_str_c("Theme"), *v = ak_str_c("dark theme");
	ak_ui_provide(k, (ak_val){.p = v});
	ak_release(k), ak_release(v);
	ak_view *col = ak_ui_new(AK_EL_COLUMN);
	ak_closure *cond = fn(read_cell, on_cell), *then = fn(use_theme, NULL);
	add(col, ak_ui_show(cond, then, NULL));  // a branch sees what's provided around it
	ak_release(cond), ak_release(then);
	return (ak_val){.p = col};
}

TEST(provide_and_use) {
	on_cell = ak_vm_cell_new((ak_val){.i = 1}, AK_K_BOOL, NULL).p;
	ak_uitest *a = mount(provide_app, NULL);
	ak_buf err = {0};
	CHECK(ak_uitest_expect(a, AK_STR("dark theme"), &err));
	ak_buf_free(&err);
	done(a);
	ak_release(on_cell);
}

// ---- Unset keys (TestNilPropsAreZeroValues), layout, autofocus ----

static float laid[4];

static ak_val on_layout(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(n);
	const ak_struct *f = a[0].p;
	for (int i = 0; i < 4; i++) laid[i] = (float)f->f[i].f;
	return (ak_val){0};
}

static ak_val bare_app(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_view *col = ak_ui_new(AK_EL_COLUMN);
	set(col, AK_KEY_PADDING, edges4(0, 0, 0, 12));
	add(col, ak_ui_new(AK_EL_BUTTON));
	add(col, ak_ui_new(AK_EL_TEXT_FIELD));
	add(col, ak_ui_new(AK_EL_TOGGLE));
	ak_view *s = ak_ui_new(AK_EL_SCROLL);
	add(s, ak_ui_new(AK_EL_TEXT));
	add(col, s);
	ak_view *box = ak_ui_new(AK_EL_VIEW);
	set_i(box, AK_KEY_HEIGHT, ak_length_pt(30));
	on(box, AK_EV_VIEW_LAYOUT, on_layout, NULL);
	add(col, box);
	ak_view *tf = ak_ui_new(AK_EL_TEXT_FIELD);
	set_i(tf, AK_KEY_TEXT_FIELD_AUTOFOCUS, 1);
	add(col, tf);
	return (ak_val){.p = col};
}

TEST(unset_keys_send_nothing) {
	ak_uitest *a = mount(bare_app, NULL);
	ak_fake *f = ak_uitest_host(a);
	// No prop is sent for a key left unset: the host's default is the zero
	// value.
	static const ak_view_kind kinds[] = {AK_KIND_BUTTON, AK_KIND_TEXT_FIELD, AK_KIND_TOGGLE, AK_KIND_SCROLL, AK_KIND_TEXT};
	for (size_t k = 0; k < AK_LEN(kinds); k++) {
		ak_fake_view *v = nth(a, kinds[k], 0);
		for (uint32_t i = 0; v && i < v->props.len; i++)
			if (v->props.data[i].prop >= AK_FIRST_KIND_PROP) T_FAIL("%s has prop %u", ak_kind_name(kinds[k]), v->props.data[i].prop);
	}
	// The layout handler hears its frame, once laid out.
	CHECK(laid[0] == 12 && laid[2] == 378 && laid[3] == 30);
	// The autofocused field asked for the keyboard once placed: after its
	// frame, in the frames' batch.
	ak_fake_index_vec cmds = {0};
	ak_fake_commands(f, AK_CMD_FOCUS, &cmds);
	REQUIRE(cmds.len == 1);
	ak_op focus = ak_fake_op_at(f, cmds.data[0]);
	CHECK(focus.id == nth(a, AK_KIND_TEXT_FIELD, 1)->id);
	ak_vec_free(&cmds);
	done(a);
}

// ---- A component's caller's style, over the component's own ----

static void *os_inner, *os_outer;

static ak_val os_inner_w(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	return (ak_val){.i = ak_length_pt((float)ak_vm_cell_get(os_inner, true).i)};
}

static ak_val os_outer_h(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	return (ak_val){.i = ak_length_pt((float)ak_vm_cell_get(os_outer, true).i)};
}

// The component's element binds its width and height; its caller sets
// the width and binds the height (ui.set_outer, ui.bind_outer).
static ak_val outer_style(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_view *t = text("card");
	bind(t, AK_KEY_WIDTH, os_inner_w, NULL);
	bind(t, AK_KEY_HEIGHT, os_inner_w, NULL);
	ak_ui_set_outer(t, AK_KEY_WIDTH, (ak_val){.i = ak_length_pt(120)});
	ak_closure *h = fn(os_outer_h, NULL);
	ak_ui_bind_outer(t, AK_KEY_HEIGHT, h);
	ak_release(h);
	return (ak_val){.p = t};
}

TEST(outer_style) {
	os_inner = cell_int(50);
	os_outer = cell_int(30);
	ak_uitest *a = mount(outer_style, NULL);
	ak_fake_view *v = nth(a, AK_KIND_TEXT, 0);
	REQUIRE(v);
	check_frame(v, 0, 0, 120, 30);
	// The component's own binding runs again: the caller's stay.
	write_i(os_inner, 70);
	ak_uitest_settle(a);
	check_frame(v, 0, 0, 120, 30);
	// The caller's binding follows its own.
	write_i(os_outer, 44);
	ak_uitest_settle(a);
	check_frame(v, 0, 0, 120, 44);
	done(a);
	ak_release(os_inner), ak_release(os_outer);
}

// ---- The VM's builtins ----

TEST(builtins_are_registered) {
	setup();
	static const char *const names[] = {"ui.new", "ui.set", "ui.bind", "ui.set_outer", "ui.bind_outer", "ui.on", "ui.add", "ui.slot", "ui.ref", "ui.show", "ui.for", "ui.switch",
	                                    "ui.dynamic", "ui.lazy_for", "ui.fragment", "ui.use", "ui.provide", "ui.task", "ui.focus", "ui.blur",
	                                    "ui.scroll_to", "ui.environment", "ui.enter", "ui.leave", "ui.native", "ui.native_set",
	                                    "ui.native_bind", "ui.native_on", "ui.load", "load.state", "load.loading", "load.value", "load.error", "load.reload"};
	for (size_t i = 0; i < AK_LEN(names); i++)
		if (!ak_builtin_find(names[i])) T_FAIL("no builtin %s", names[i]);
}

// A root mounted before its window has a size (a desktop's, whose size
// comes as it shows) lays out again once it has one: its texts measured
// again at their new width, not the width-0 measures kept.
static ak_val counter_text_app(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_view *col = ak_ui_new(AK_EL_COLUMN);
	add(col, text("Count: 0"));
	return (ak_val){.p = col};
}

// What the Windows host answers: nothing at no width, a line and a
// button's height at any other.
static void windows_measure(void *ctx, ak_fake_view *v, float width, float height, float aw, float ah, float *ow, float *oh) {
	AK_UNUSED(ctx), AK_UNUSED(height), AK_UNUSED(aw), AK_UNUSED(ah);
	bool button = v->kind == AK_KIND_BUTTON;
	if (width == 0) {
		*ow = -0.0f, *oh = button ? 32 : -0.0f;
		return;
	}
	*ow = width == width ? width : (button ? 49 : 53);
	*oh = button ? 32 : 19;
}

static ak_val counter_app2(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_view *col = ak_ui_new(AK_EL_COLUMN);
	set_f(col, AK_KEY_GAP, 8);
	set(col, AK_KEY_PADDING, edges4(16, 16, 16, 16));
	add(col, text("Count: 0"));
	ak_view *b = ak_ui_new(AK_EL_BUTTON);
	set_str(b, AK_KEY_BUTTON_TITLE, "Add");
	add(col, b);
	return (ak_val){.p = col};
}

TEST(remeasures_once_the_root_has_a_size_windows) {
	setup();
	ak_uitest_set_measure(windows_measure, NULL);
	ak_closure *c = fn(counter_app2, NULL);
	ak_environment env = {.scale = 1, .font_scale = 1, .platform = AK_PLATFORM_WINDOWS};
	ak_uitest *a = ak_uitest_mount(c, &env);
	ak_release(c);
	ak_uitest_set_measure(NULL, NULL);
	ak_fake *f = ak_uitest_host(a);
	env.width = 800, env.height = 600;
	ak_fake_set_environment(f, &env);
	ak_ui_flush();
	ak_fake_view *t = one(a, AK_KIND_TEXT, "Count: 0");
	CHECK_EQ_F(t->frame[2], 768);
	CHECK_EQ_F(t->frame[3], 19);
	CHECK_EQ_F(one(a, AK_KIND_BUTTON, "Add")->frame[3], 32);
	done(a);
}

TEST(remeasures_once_the_root_has_a_size) {
	setup();
	ak_closure *c = fn(counter_text_app, NULL);
	ak_environment env = {.scale = 1, .font_scale = 1, .platform = AK_PLATFORM_WINDOWS};
	ak_uitest *a = ak_uitest_mount(c, &env);
	ak_release(c);
	ak_fake *f = ak_uitest_host(a);
	env.width = 800, env.height = 600;
	ak_fake_set_environment(f, &env);
	ak_ui_flush();
	ak_fake_view *t = one(a, AK_KIND_TEXT, "Count: 0");
	CHECK_EQ_F(t->frame[2], 800);
	CHECK_EQ_F(t->frame[3], 20);
	done(a);
}

// ---- A Grid's repeat() counts, whatever the app passes ----

static const uint8_t track_f[] = {AK_K_FLOAT}, track_rep[] = {AK_K_INT, AK_K_REF}, track_list[] = {AK_K_REF}, track_mm[] = {AK_K_REF, AK_K_REF};
static const ak_case track_cases[] = {
	{"auto", 0, NULL, NULL, NULL}, {"points", 1, track_f, NULL, NULL}, {"percent", 1, track_f, NULL, NULL}, {"fr", 1, track_f, NULL, NULL},
	{"minContent", 0, NULL, NULL, NULL}, {"maxContent", 0, NULL, NULL, NULL}, {"minmax", 2, track_mm, NULL, NULL},
	{"repeat", 2, track_rep, NULL, NULL}, {"autoFill", 1, track_list, NULL, NULL}, {"autoFit", 1, track_list, NULL, NULL},
};
static const ak_shape track_shape = {"Track", 0, NULL, NULL, NULL, 10, track_cases, false, NULL};
static const uint8_t grid_kinds[] = {AK_K_REF, AK_K_REF, AK_K_REF, AK_K_REF, AK_K_BOOL, AK_K_BOOL};
static const ak_shape grid_shape = {"Grid", 6, grid_kinds, NULL, NULL, 0, NULL, false, NULL};

static int64_t repeat_count;

// A Grid of repeat(repeat_count, [points(10)]) columns, two texts in it.
static ak_val repeat_grid_app(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_enum *pt = ak_enum_new(&track_shape, 1);
	pt->f[0].f = 10;
	ak_arr *sizes = ak_arr_new(AK_K_REF, NULL, 1);
	ak_arr_push(&sizes, (ak_val){.p = pt});
	ak_enum *rep = ak_enum_new(&track_shape, 7);
	rep->f[0].i = repeat_count;
	rep->f[1].p = sizes;
	ak_arr *cols = ak_arr_new(AK_K_REF, NULL, 1);
	ak_arr_push(&cols, (ak_val){.p = rep});
	ak_struct *g = ak_struct_new(&grid_shape);
	g->f[0].p = cols;
	g->f[1].p = ak_arr_new(AK_K_REF, NULL, 0);
	g->f[2].p = ak_arr_new(AK_K_REF, NULL, 0);
	g->f[3].p = ak_arr_new(AK_K_REF, NULL, 0);
	ak_view *v = ak_ui_new(AK_EL_COLUMN);
	set(v, AK_KEY_GRID, (ak_val){.p = g});
	add(v, text("a"));
	add(v, text("b"));
	return (ak_val){.p = v};
}

// A count beyond 32 bits isn't cut to its low bits (2^32 + 1 was one
// column), a negative one isn't auto-fill's: more than a grid's lines
// are as many as it has, fewer than one are one.
TEST(grid_repeat_counts) {
	static const struct {
		int64_t count;
		float second_x;  // the second text's: beside the first, or below
	} cases[] = {{(INT64_C(1) << 32) + 1, 10}, {-1, 0}, {0, 0}, {2, 10}, {INT64_MAX, 10}};
	for (size_t i = 0; i < AK_LEN(cases); i++) {
		repeat_count = cases[i].count;
		ak_uitest *a = mount(repeat_grid_app, NULL);
		ak_fake_view *b = one(a, AK_KIND_TEXT, "b");
		REQUIRE(b);
		if (b->frame[0] != cases[i].second_x) T_FAIL("repeat(%lld): x %g, want %g", (long long)cases[i].count, b->frame[0], cases[i].second_x);
		done(a);
	}
}

// The host.read* builtins: at any offset past the end, 0 (an offset near
// the int's largest overflowed when the read's length was added).
TEST(host_reads_past_the_end) {
	ak_bytes *b = ak_bytes_new((const uint8_t *)"\x01\x02\x03\x04\x05\x06\x07\x08\x09", 9);
	static const char *const readers[] = {"host.readU8", "host.readU32", "host.readU64", "host.readF64"};
	const int64_t offsets[] = {INT64_MAX, INT64_MAX - 3, INT64_MAX - 7, 9, -1};
	for (size_t i = 0; i < AK_LEN(readers); i++) {
		ak_builtin_fn f = ak_builtin_find(readers[i]);
		REQUIRE(f);
		for (size_t k = 0; k < AK_LEN(offsets); k++) {
			ak_val args[2] = {{.p = b}, {.i = offsets[k]}};
			ak_val v = f(args, 2);
			if (v.i != 0) T_FAIL("%s at %lld: %lld", readers[i], (long long)offsets[k], (long long)v.i);
		}
		ak_val args[2] = {{.p = b}, {.i = 1}};
		CHECK(f(args, 2).i != 0);
	}
	ak_builtin_fn str = ak_builtin_find("host.readStr");
	REQUIRE(str);
	ak_val args[2] = {{.p = b}, {.i = INT64_MAX - 3}};
	ak_str *s = str(args, 2).p;
	CHECK_EQ_INT(s->len, 0);
	ak_release(s);
	ak_release(b);
}

// A frame's time from a host that isn't one, or is beyond an int64's
// nanoseconds, is a time all the same.
void ak_anim_frame(ak_id id, double seconds);

TEST(frame_times_out_of_range) {
	ak_uitest *a = mount(scroll_app, NULL);
	double times[] = {NAN, INFINITY, -INFINITY, 1e300, -1e300, 1.5};
	for (size_t i = 0; i < AK_LEN(times); i++) ak_anim_frame(0, times[i]);
	done(a);
}

// ---- Events from a host that sends anything (fuzz.h) ----

TEST(events_from_a_host_that_sends_anything) {
	ak_invoke_fn apps[] = {show_app, switch_app, field_app, selection_app, toggle_app, controls_app, list_app, shown_list_app, scroll_app};
	int per_app = fuzz_seed(0x2545f4914f6cdd1d, 3000);
	for (size_t i = 0; i < AK_LEN(apps); i++) {
		on_cell = ak_vm_cell_new((ak_val){.i = 1}, AK_K_BOOL, NULL).p;
		ticks = cell_int(0);
		which = cell_int(1);
		field_text = cell_ref(ak_str_c("héllo 👋 world"));
		submitted = cell_ref(ak_str_c(""));
		sel_cell = cell_ref(range(0, 0).p);
		toggled = ak_vm_cell_new((ak_val){.i = 0}, AK_K_BOOL, NULL).p;
		seg_sel = cell_int(1);
		picked = cell_int(-1);
		slid = ak_vm_cell_new((ak_val){.f = 0.25}, AK_K_FLOAT, NULL).p;
		items = cell_ref(todos(50));
		list_shown = ak_vm_cell_new((ak_val){.i = 1}, AK_K_BOOL, NULL).p;
		ak_uitest *a = mount(apps[i], NULL);
		fuzz_events(a, per_app);
		done(a);
		void *cells[] = {on_cell, ticks, which, field_text, submitted, sel_cell, toggled, seg_sel, picked, slid, items, list_shown};
		for (size_t k = 0; k < AK_LEN(cells); k++) ak_release(cells[k]);
	}
}

// asuka dev's automation's scroll: the List a page further, to the row
// after the last the host bound; at its end, an error.
TEST(automation_scroll) {
	ak_arr *all = todos(100);
	items = cell_ref(ak_retain(all));
	ak_uitest *a = mount(list_app, NULL);
	ak_fake *f = ak_uitest_host(a);
	ak_fake_reset(f);
	ak_buf out = {0};
	ak_ui_automate(ak_cstr("see item 0\nnotsee item 90\nscroll"), &out);
	ak_buf_u8(&out, 0);
	CHECK_EQ_STR((const char *)out.data, "ok\nok\nok\n");
	ak_fake_index_vec sent = {0};
	ak_fake_commands(f, AK_CMD_SCROLL_TO, &sent);
	CHECK_EQ_INT(sent.len, 1);
	if (sent.len == 1) {
		ak_op op = ak_fake_op_at(f, sent.data[0]);
		uint32_t to;
		memcpy(&to, op.value.s.p, 4);
		CHECK(to > 5 && to < 100);  // past the rows that fit the screen
	}
	ak_vec_free(&sent);
	// The host binds the last rows: at its end.
	ak_fake_view *cell = ak_fake_new_cell(f);
	ak_fake_bind(f, nth(a, AK_KIND_LIST, 0), 99, cell);
	ak_buf_reset(&out);
	ak_ui_automate(ak_cstr("scroll\nsee item 99"), &out);
	ak_buf_u8(&out, 0);
	CHECK_EQ_STR((const char *)out.data, "error: the list is at its end\nok\n");
	ak_buf_free(&out);
	done(a);
	ak_release(items), ak_release(all);
}

T_MAIN()
