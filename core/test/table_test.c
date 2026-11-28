// table_test.c: Table, its TableRows and Outline (core/src/ui/table.c) on
// the fake host: what's bound of 100,000 rows, their RowData, the sort
// and the selection the host reports, the views of a row at its columns'
// places, an outline's levels and its children loaded as it expands, the
// phones' lists; the app's undo stack (undo.c) and the cursor.
#include "uih.h"

// TableColumn: id, title, width, minWidth, maxWidth, resizable, sortable,
// align.
static const uint8_t col_kinds[] = {AK_K_REF, AK_K_REF, AK_K_FLOAT, AK_K_FLOAT, AK_K_FLOAT, AK_K_BOOL, AK_K_BOOL, AK_K_INT};
static const ak_shape col_shape = {"TableColumn", 8, col_kinds, no_shapes, NULL, 0, NULL, false, NULL};
static const uint8_t task_kinds[] = {AK_K_INT, AK_K_REF, AK_K_BOOL};
static const ak_shape task_shape = {"Task", 3, task_kinds, no_shapes, NULL, 0, NULL, false, NULL};

static bool str_is(const ak_str *s, const char *c) { return s && s->len == strlen(c) && memcmp(s->data, c, s->len) == 0; }

static ak_val column(const char *id, const char *title, float width, bool sortable) {
	ak_struct *c = ak_struct_new(&col_shape);
	c->f[0].p = ak_str_c(id);
	c->f[1].p = ak_str_c(title);
	c->f[2].f = width;
	c->f[3].f = 0;
	c->f[4].f = 0;
	c->f[5].i = 1;
	c->f[6].i = sortable;
	c->f[7].i = 0;
	return (ak_val){.p = c};
}

static ak_arr *columns(void) {
	ak_arr *a = ak_arr_new(AK_K_REF, &col_shape, 2);
	ak_arr_push(&a, column("name", "Name", 240, true));
	ak_arr_push(&a, column("state", "State", 80, false));
	return a;
}

static ak_arr *tasks(int n) {
	ak_arr *arr = ak_arr_new(AK_K_REF, &task_shape, (uint32_t)n);
	for (int i = 0; i < n; i++) {
		char t[32];
		snprintf(t, sizeof t, "task %d", i);
		ak_struct *s = ak_struct_new(&task_shape);
		s->f[0].i = i;
		s->f[1].p = ak_str_c(t);
		s->f[2].i = i % 2;
		ak_arr_push(&arr, (ak_val){.p = s});
	}
	return arr;
}

static ak_arr *reversed(const ak_arr *a) {
	ak_arr *r = ak_arr_new(AK_K_REF, &task_shape, a->len);
	for (uint32_t i = a->len; i > 0; i--) ak_arr_push(&r, (ak_val){.p = ak_retain(ak_arr_vals(a)[i - 1].p)});
	return r;
}

static ak_arr *strs(const char *const *s, uint32_t n) {
	ak_arr *a = ak_arr_new(AK_K_REF, NULL, n);
	a->h.flags = 1;
	for (uint32_t i = 0; i < n; i++) ak_arr_push(&a, (ak_val){.p = ak_str_c(s[i])});
	return a;
}

static void *task_items;  // a cell of [Task]
static int trows_made;
static bool view_cells;   // the rows are views, not text

static ak_val task_items_of(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	return ak_vm_cell_get(task_items, true);
}

static ak_val task_key(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(n);
	return (ak_val){.i = ((ak_struct *)a[0].p)->f[0].i};
}

static ak_val task_cells(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(a), AK_UNUSED(n);
	ak_val it = ak_vm_acc_get((void *)c->fn);
	ak_struct *s = it.p;
	ak_arr *out = ak_arr_new(AK_K_REF, NULL, 2);
	out->h.flags = 1;
	ak_arr_push(&out, (ak_val){.p = ak_retain(s->f[1].p)});
	ak_arr_push(&out, (ak_val){.p = ak_str_c(s->f[2].i ? "done" : "open")});
	ak_release(it.p);
	return (ak_val){.p = out};
}

static ak_val task_title(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(a), AK_UNUSED(n);
	ak_val it = ak_vm_acc_get((void *)c->fn);
	ak_str *s = ak_retain(((ak_struct *)it.p)->f[1].p);
	ak_release(it.p);
	return (ak_val){.p = s};
}

static ak_closure *of_item(ak_invoke_fn f, void *acc) {
	ak_closure *b = ak_closure_new(f, acc, 1, ref_cap);
	b->caps[0].p = ak_retain(acc);
	return b;
}

static ak_val task_row(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(n);
	trows_made++;
	ak_view *r = ak_ui_new(AK_EL_TABLE_ROW);
	if (view_cells) {
		ak_view *t = ak_ui_new(AK_EL_TEXT);
		ak_closure *b = of_item(task_title, a[0].p);
		ak_ui_bind(t, AK_KEY_TEXT_TEXT, b);
		ak_release(b);
		add(r, t);
		add(r, text("state"));
		return (ak_val){.p = r};
	}
	ak_closure *b = of_item(task_cells, a[0].p);
	ak_ui_bind(r, AK_KEY_TABLE_ROW_CELLS, b);
	ak_release(b);
	return (ak_val){.p = r};
}

// What the handlers heard.
static ak_val heard;      // the last array an event carried (retained)
static int heard_n;
static int64_t heard_i;
static ak_str *heard_s;

static void hear(ak_val v) {
	ak_release(heard.p);
	heard.p = ak_retain(v.p);
	heard_n++;
}

static ak_val on_array(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(n);
	hear(a[0]);
	return (ak_val){0};
}

static ak_val on_int(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(n);
	heard_i = a[0].i;
	heard_n++;
	return (ak_val){0};
}

static ak_val on_str_x(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(n);
	ak_release(heard_s);
	heard_s = ak_retain(a[0].p);
	heard_i = a[1].i;
	heard_n++;
	return (ak_val){0};
}

static void heard_reset(void) {
	ak_release(heard.p);
	heard.p = NULL;
	ak_release(heard_s);
	heard_s = NULL;
	heard_n = 0;
	heard_i = -1;
}

// A Table's selection, bound: the app's cell, written by its handler.
static void *sel_cell;

static ak_val write_sel(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(n);
	hear(a[0]);
	write_p(sel_cell, ak_retain(a[0].p));
	return (ak_val){0};
}

static ak_val read_ref(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(a), AK_UNUSED(n);
	return ak_vm_cell_get((void *)c->fn, true);
}

// A row's context menu: Delete, which says the row it was given.
static const uint8_t button_kinds[] = {AK_K_REF, AK_K_REF, AK_K_INT, AK_K_BOOL, AK_K_BOOL, AK_K_REF};
static const uint8_t sub_kinds[] = {AK_K_REF, AK_K_REF, AK_K_REF};
static const uint8_t sec_kinds[] = {AK_K_REF, AK_K_REF};
static const ak_case action_cases[] = {{"button", 6, button_kinds, NULL, NULL}, {"submenu", 3, sub_kinds, NULL, NULL}, {"section", 2, sec_kinds, NULL, NULL}};
static const ak_shape action_shape = {"Action", 0, NULL, NULL, NULL, 3, action_cases, false, NULL};
static int64_t deleted = -1;
static bool with_menu;

static ak_val delete_row(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(a), AK_UNUSED(n);
	deleted = (int64_t)(intptr_t)c->fn;
	return (ak_val){0};
}

static ak_val row_menu(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(n);
	ak_enum *d = ak_enum_new(&action_shape, 0);
	d->f[0].p = ak_str_c("Delete");
	d->f[1].p = ak_str_empty();
	d->f[2].i = 2;  // destructive
	d->f[5].p = fn(delete_row, (void *)(intptr_t)a[0].i);
	ak_arr *r = ak_arr_new(AK_K_REF, &action_shape, 1);
	ak_arr_push(&r, (ak_val){.p = d});
	return (ak_val){.p = r};
}

static ak_val task_table(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_view *t = ak_ui_new(AK_EL_TABLE);
	set(t, AK_KEY_TABLE_COLUMNS, (ak_val){.p = columns()});
	if (with_menu) {
		ak_closure *m = fn(row_menu, NULL);
		ak_ui_set(t, AK_KEY_TABLE_MENU, (ak_val){.p = m});
		ak_release(m);
	}
	on(t, AK_EV_TABLE_SORT_CHANGE, on_array, NULL);
	on(t, AK_EV_TABLE_OPEN, on_int, NULL);
	on(t, AK_EV_TABLE_COLUMN_MOVE, on_str_x, NULL);
	if (sel_cell) {
		bind(t, AK_KEY_TABLE_SELECTION, read_ref, sel_cell);
		on(t, AK_EV_TABLE_SELECTION_CHANGE, write_sel, NULL);
	} else {
		on(t, AK_EV_TABLE_SELECTION_CHANGE, on_array, NULL);
	}
	ak_closure *it = fn(task_items_of, NULL), *key = fn(task_key, NULL), *row = fn(task_row, NULL);
	ak_ui_lazy_for(t, it, key, row, AK_K_INT);
	ak_release(it), ak_release(key), ak_release(row);
	return (ak_val){.p = t};
}

static ak_uitest *mount_on(ak_invoke_fn build, uint8_t platform) {
	setup();
	ak_closure *c = fn(build, NULL);
	ak_uitest *a = ak_uitest_mount_at(c, NULL, platform, (ak_span){0});
	ak_release(c);
	return a;
}

static ak_span cmd_args(ak_fake *f, ak_cmd cmd) {
	ak_fake_index_vec cmds = {0};
	ak_fake_commands(f, cmd, &cmds);
	ak_span s = {0};
	if (cmds.len) s = ak_fake_op_at(f, cmds.data[cmds.len - 1]).value.s;
	ak_vec_free(&cmds);
	return s;
}

static uint32_t props_sent(ak_fake *f, ak_prop p) {
	uint32_t n = 0;
	for (uint32_t i = 0; i < ak_fake_op_count(f); i++) {
		ak_op op = ak_fake_op_at(f, i);
		if (op.code == AK_OP_SET_PROP && op.prop == p) n++;
	}
	return n;
}

static void runs(ak_buf *b, const uint32_t *r, uint32_t n) {
	ak_buf_u32(b, n);
	for (uint32_t i = 0; i < 2 * n; i++) ak_buf_u32(b, r[i]);
}

static void check_runs(ak_span s, const char *want) {
	ak_buf b = {0};
	ak_reader r = {(const uint8_t *)s.p, s.len, 0, false};
	uint32_t n = s.len ? ak_read_u32(&r) : 0;
	for (uint32_t i = 0; i < n && !r.err; i++) {
		uint32_t start = ak_read_u32(&r), len = ak_read_u32(&r);
		ak_buf_printf(&b, "%s%u+%u", i ? " " : "", start, len);
	}
	CHECK_EQ_STR(cstr(&b), want);
	ak_buf_free(&b);
}

static void check_ints(const ak_arr *a, const char *want) {
	ak_buf b = {0};
	for (uint32_t i = 0; a && i < a->len; i++) ak_buf_printf(&b, "%s%lld", i ? " " : "", (long long)ak_arr_vals(a)[i].i);
	CHECK_EQ_STR(cstr(&b), want);
	ak_buf_free(&b);
}

static void check_strs(const ak_arr *a, const char *want) {
	ak_buf b = {0};
	for (uint32_t i = 0; a && i < a->len; i++) {
		const ak_str *s = ak_arr_vals(a)[i].p;
		ak_buf_printf(&b, "%s%.*s", i ? " " : "", (int)s->len, s->data);
	}
	CHECK_EQ_STR(cstr(&b), want);
	ak_buf_free(&b);
}

// ---- Table ----

// 100,000 rows: what shows is bound, each its RowData; a recycled cell
// takes its new row's text in one SetProp, making no view; the columns,
// sort and row height go once.
TEST(table_binds_only_what_shows) {
	ak_arr *all = tasks(100000);
	task_items = cell_ref(ak_retain(all));
	trows_made = 0;
	view_cells = false;
	heard_reset();
	ak_uitest *a = mount_on(task_table, AK_PLATFORM_MACOS);
	ak_fake *f = ak_uitest_host(a);
	ak_fake_view *t = nth(a, AK_KIND_TABLE, 0);
	REQUIRE(t);
	CHECK_EQ_INT(ak_fake_list_count(f, t), 100000);
	CHECK_EQ_INT(t->items.len, 100000);
	CHECK(trows_made > 0 && trows_made < 60);  // what fits a window
	CHECK_EQ_INT(count_kind(a, AK_KIND_TEXT), 0);  // the table draws the text
	CHECK_EQ_INT(ak_fake_float(t, AK_PROP_TABLE_ROW_HEIGHT), 24);
	ak_span cols = ak_fake_string(t, AK_PROP_TABLE_COLUMNS);
	ak_reader r = {(const uint8_t *)cols.p, cols.len, 0, false};
	CHECK_EQ_INT(ak_read_u32(&r), 2);
	ak_span id = ak_read_str(&r);
	CHECK(ak_span_eq(id, AK_STR("name")));
	ak_fake_view *c = ak_fake_new_cell(f);
	ak_fake_bind(f, t, 50000, c);
	check_text(c, "task 50000|open");
	check_frame(c, 0, 0, 390, 24);
	int made = trows_made;
	// Recycled for another row: its text, nothing made.
	ak_fake_reset(f);
	uint64_t allocs = ak_alloc_count;
	ak_fake_bind(f, t, 77777, c);
	uint64_t used = ak_alloc_count - allocs;
	check_text(c, "task 77777|done");
	CHECK_EQ_INT(trows_made, made);
	CHECK_EQ_INT(ak_fake_count(f, AK_OP_CREATE), 0);
	CHECK_EQ_INT(ak_fake_count(f, AK_OP_SET_PROP), 1);
	CHECK(used <= 8);  // the row's own: its cells' array and strings
	// The same text again: nothing sent.
	ak_fake_reset(f);
	ak_fake_bind(f, t, 77777, c);
	CHECK_EQ_INT(ak_fake_count(f, AK_OP_SET_PROP), 0);
	done(a);
	ak_release(task_items), ak_release(all);
	heard_reset();
}

// A click on a sortable column's header sorts by it, ascending, then
// descending; one on a column that doesn't sort does nothing.
TEST(table_sorts_by_its_headers) {
	ak_arr *all = tasks(10);
	task_items = cell_ref(ak_retain(all));
	view_cells = false;
	heard_reset();
	ak_uitest *a = mount_on(task_table, AK_PLATFORM_WINDOWS);
	ak_fake_view *t = nth(a, AK_KIND_TABLE, 0);
	REQUIRE(t);
	CHECK_EQ_INT(ak_fake_float(t, AK_PROP_TABLE_ROW_HEIGHT), 32);
	dispatch(a, (ak_event){.id = t->id, .kind = AK_EVENT_SORT_BY, .i = 0});
	REQUIRE(heard_n == 1 && heard.p);
	const ak_arr *s = heard.p;
	REQUIRE(s->len == 1);
	const ak_struct *k = ak_arr_vals(s)[0].p;
	CHECK(str_is(k->f[0].p, "name") && k->f[1].i == 1);
	ak_span sort = ak_fake_string(t, AK_PROP_TABLE_SORT);
	CHECK_EQ_INT(sort.len, 9);
	CHECK_EQ_INT((uint8_t)sort.p[8], 1);
	dispatch(a, (ak_event){.id = t->id, .kind = AK_EVENT_SORT_BY, .i = 0});
	k = ak_arr_vals((const ak_arr *)heard.p)[0].p;
	CHECK(k->f[1].i == 0);
	CHECK_EQ_INT((uint8_t)ak_fake_string(t, AK_PROP_TABLE_SORT).p[8], 0);
	dispatch(a, (ak_event){.id = t->id, .kind = AK_EVENT_SORT_BY, .i = 1});
	CHECK_EQ_INT(heard_n, 2);
	done(a);
	ak_release(task_items), ak_release(all);
	heard_reset();
}

// The user's selection goes to the app; the app's to the host; and as the
// items move (sorted the other way), it follows them: the host is sent
// their rows after the update, and the app hears them.
TEST(table_selection_follows_the_items) {
	ak_arr *all = tasks(100);
	task_items = cell_ref(ak_retain(all));
	sel_cell = cell_ref(ak_arr_new(AK_K_INT, NULL, 0));
	view_cells = false;
	heard_reset();
	ak_uitest *a = mount_on(task_table, AK_PLATFORM_LINUX);
	ak_fake *f = ak_uitest_host(a);
	ak_fake_view *t = nth(a, AK_KIND_TABLE, 0);
	REQUIRE(t);
	ak_buf b = {0};
	runs(&b, (uint32_t[]){2, 2}, 1);
	ak_fake_reset(f);
	dispatch(a, (ak_event){.id = t->id, .kind = AK_EVENT_ROWS_SELECTED, .s = {(const char *)b.data, (uint32_t)b.len}});
	ak_ui_flush();
	check_ints(heard.p, "2 3");
	ak_arr *bound = ak_vm_cell_get(sel_cell, false).p;  // cell_get's: released below
	check_ints(bound, "2 3");
	ak_release(bound);
	CHECK_EQ_INT(props_sent(f, AK_PROP_TABLE_SELECTION), 0);  // the host shows it already
	ak_buf_free(&b);
	// The app selects: the host is told.
	ak_arr *mine = ak_arr_new(AK_K_INT, NULL, 2);
	ak_arr_push(&mine, (ak_val){.i = 5});
	ak_arr_push(&mine, (ak_val){.i = 7});
	write_p(sel_cell, mine);
	ak_ui_flush();
	check_runs(ak_fake_string(t, AK_PROP_TABLE_SELECTION), "5+1 7+1");
	// Sorted the other way: the selection is the same tasks, at their rows.
	heard_n = 0;
	write_p(task_items, reversed(all));
	ak_ui_flush();
	check_runs(ak_fake_string(t, AK_PROP_TABLE_SELECTION), "92+1 94+1");
	ak_fake_run_posted(f);
	CHECK_EQ_INT(heard_n, 1);
	check_ints(heard.p, "92 94");
	done(a);
	ak_release(task_items), ak_release(all), ak_release(sel_cell);
	sel_cell = NULL;
	heard_reset();
}

// A row of views: a box per column, at its place and width, which follow
// the columns as the user moves and resizes them.
TEST(table_rows_of_views_at_their_columns) {
	ak_arr *all = tasks(20);
	task_items = cell_ref(ak_retain(all));
	view_cells = true;
	heard_reset();
	ak_uitest *a = mount_on(task_table, AK_PLATFORM_WEB);
	ak_fake *f = ak_uitest_host(a);
	ak_fake_view *t = nth(a, AK_KIND_TABLE, 0);
	REQUIRE(t);
	ak_fake_view *c = ak_fake_new_cell(f);
	ak_fake_bind(f, t, 3, c);
	REQUIRE(c->children.len == 2);
	check_frame(c->children.data[0], 0, 0, 240, 32);
	check_frame(c->children.data[1], 240, 0, 80, 32);
	check_text(c, "task 3|state");
	// The host's first layout, then the user's move and resize.
	ak_buf b = {0};
	ak_buf_u32(&b, 2), ak_buf_u32(&b, 0), ak_buf_f32(&b, 240), ak_buf_u32(&b, 1), ak_buf_f32(&b, 80);
	dispatch(a, (ak_event){.id = t->id, .kind = AK_EVENT_COLUMN_LAYOUT, .s = {(const char *)b.data, (uint32_t)b.len}});
	CHECK_EQ_INT(heard_n, 0);
	b.len = 0;
	ak_buf_u32(&b, 2), ak_buf_u32(&b, 1), ak_buf_f32(&b, 100), ak_buf_u32(&b, 0), ak_buf_f32(&b, 200);
	dispatch(a, (ak_event){.id = t->id, .kind = AK_EVENT_COLUMN_LAYOUT, .s = {(const char *)b.data, (uint32_t)b.len}});
	ak_buf_free(&b);
	check_frame(c->children.data[0], 100, 0, 200, 32);
	check_frame(c->children.data[1], 0, 0, 100, 32);
	CHECK_EQ_INT(heard_n, 1);  // a move
	CHECK(heard_s != NULL);
	done(a);
	ak_release(task_items), ak_release(all);
	view_cells = false;
	heard_reset();
}

// A row's context menu is its cell's (the host's table shows it on a right
// click); a phone's row's, shown on a long press.
TEST(table_rows_have_menus) {
	ak_arr *all = tasks(10);
	task_items = cell_ref(ak_retain(all));
	with_menu = true;
	uint8_t platforms[] = {AK_PLATFORM_MACOS, AK_PLATFORM_IOS};
	for (int p = 0; p < 2; p++) {
		deleted = -1;
		ak_uitest *a = mount_on(task_table, platforms[p]);
		ak_fake *f = ak_uitest_host(a);
		ak_fake_view *t = nth(a, p ? AK_KIND_LIST : AK_KIND_TABLE, 0);
		REQUIRE(t);
		ak_fake_view *c = ak_fake_new_cell(f);
		ak_fake_bind(f, t, 6, c);
		ak_fake_view *on = p ? c->children.data[0] : c;
		CHECK(has_prop(on, AK_PROP_MENU));
		CHECK_EQ_INT(ak_fake_int(on, AK_PROP_MENU_TRIGGER), AK_MENU_TRIGGER_LONG_PRESS);
		CHECK_EQ_INT(ak_fake_action_named(on, AK_PROP_MENU, "Delete"), 0);
		dispatch(a, (ak_event){.id = on->id, .kind = AK_EVENT_ACTION, .i = 0});
		CHECK_EQ_INT(deleted, 6);
		// Recycled for another row: its menu.
		ak_fake_bind(f, t, 8, c);
		dispatch(a, (ak_event){.id = on->id, .kind = AK_EVENT_ACTION, .i = 0});
		CHECK_EQ_INT(deleted, 8);
		done(a);
	}
	with_menu = false;
	ak_release(task_items), ak_release(all);
}

// Open: a double click or Enter on a row.
TEST(table_opens_rows) {
	ak_arr *all = tasks(10);
	task_items = cell_ref(ak_retain(all));
	heard_reset();
	ak_uitest *a = mount_on(task_table, AK_PLATFORM_MACOS);
	ak_fake_view *t = nth(a, AK_KIND_TABLE, 0);
	REQUIRE(t);
	dispatch(a, (ak_event){.id = t->id, .kind = AK_EVENT_OPEN, .i = 4});
	CHECK_EQ_INT(heard_i, 4);
	dispatch(a, (ak_event){.id = t->id, .kind = AK_EVENT_OPEN, .i = 40});  // no such row
	CHECK_EQ_INT(heard_n, 1);
	done(a);
	ak_release(task_items), ak_release(all);
	heard_reset();
}

// texts counts the Text views in v's tree, hidden ones too.
static int texts(const ak_fake_view *v) {
	int n = v->kind == AK_KIND_TEXT;
	for (uint32_t i = 0; i < v->children.len; i++) n += texts(v->children.data[i]);
	return n;
}

// A phone has no table: a List of the rows, each its first cell over the
// rest; a tap selects and opens it.
TEST(table_on_a_phone_is_a_list) {
	ak_arr *all = tasks(10);
	task_items = cell_ref(ak_retain(all));
	view_cells = false;
	heard_reset();
	ak_uitest *a = mount_on(task_table, AK_PLATFORM_IOS);
	ak_fake *f = ak_uitest_host(a);
	CHECK_EQ_INT(count_kind(a, AK_KIND_TABLE), 0);
	ak_fake_view *l = nth(a, AK_KIND_LIST, 0);
	REQUIRE(l);
	CHECK_EQ_INT(ak_fake_list_count(f, l), 10);
	ak_fake_view *c = ak_fake_new_cell(f);
	ak_fake_bind(f, l, 3, c);
	check_text(c, "task 3|done");
	// The cell's row: its first cell's text over the others'.
	REQUIRE(c->children.len == 1);
	ak_fake_view *row = c->children.data[0];
	CHECK_EQ_INT(texts(row), 2);  // the title and the detail line
	ak_fake_tap(f, row);
	CHECK_EQ_INT(heard_i, 3);  // opened
	done(a);
	ak_release(task_items), ak_release(all);
	heard_reset();
}

// ---- Outline ----

// A tree: root "a" (a folder whose children load as it's expanded) and
// "b"; "a/1" and "a/2" once loaded.
static void *loaded;    // a cell: whether a's children came
static void *open_ids;  // a cell of [string]: what's expanded
static int expands;

static ak_val tree_children(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(n);
	const ak_str *id = a[0].p;
	if (str_is(id, "a") && ak_vm_cell_get(loaded, true).i) {
		static const char *const kids[] = {"a/1", "a/2"};
		return (ak_val){.p = strs(kids, 2)};
	}
	return (ak_val){.p = strs(NULL, 0)};
}

static ak_val tree_expandable(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(n);
	return (ak_val){.i = str_is(a[0].p, "a")};
}

static ak_val tree_text(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(n);
	ak_buf b = {0};
	const ak_str *id = a[0].p;
	ak_buf_printf(&b, "Item %.*s", (int)id->len, id->data);
	ak_str *s = ak_str_new((const char *)b.data, b.len);
	ak_buf_free(&b);
	return (ak_val){.p = s};
}

static ak_val on_expand(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(n);
	expands++;
	ak_release(heard_s);
	heard_s = ak_retain(a[0].p);
	heard_i = a[1].i;
	return (ak_val){0};
}

static ak_val on_expanded(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(n);
	write_p(open_ids, ak_retain(a[0].p));
	return (ak_val){0};
}

static ak_val tree_app(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_view *o = ak_ui_new(AK_EL_OUTLINE);
	static const char *const roots[] = {"a", "b"};
	set(o, AK_KEY_OUTLINE_ROOTS, (ak_val){.p = strs(roots, 2)});
	ak_closure *k = fn(tree_children, NULL);
	ak_ui_set(o, AK_KEY_OUTLINE_CHILDREN_OF, (ak_val){.p = k});
	ak_release(k);
	k = fn(tree_expandable, NULL);
	ak_ui_set(o, AK_KEY_OUTLINE_EXPANDABLE, (ak_val){.p = k});
	ak_release(k);
	k = fn(tree_text, NULL);
	ak_ui_set(o, AK_KEY_OUTLINE_TEXT, (ak_val){.p = k});
	ak_release(k);
	bind(o, AK_KEY_OUTLINE_EXPANDED, read_ref, open_ids);
	on(o, AK_EV_OUTLINE_EXPAND, on_expand, NULL);
	on(o, AK_EV_OUTLINE_EXPANDED_CHANGE, on_expanded, NULL);
	on(o, AK_EV_OUTLINE_SELECTION_CHANGE, on_array, NULL);
	return (ak_val){.p = o};
}

static void check_levels(ak_fake_view *o, const char *want) {
	ak_span s = ak_fake_string(o, AK_PROP_OUTLINE_LEVELS);
	ak_reader r = {(const uint8_t *)s.p, s.len, 0, false};
	uint32_t n = s.len ? ak_read_u32(&r) : 0;
	ak_buf b = {0};
	for (uint32_t i = 0; i < n && !r.err; i++) {
		uint16_t d = ak_read_u16(&r);
		uint8_t fl = ak_read_u8(&r);
		ak_buf_printf(&b, "%s%u%s%s", i ? " " : "", d, fl & 1 ? "+" : "", fl & 2 ? "o" : "");
	}
	CHECK_EQ_STR(cstr(&b), want);
	ak_buf_free(&b);
}

// The roots show; expanding "a" asks the app (which loads its children),
// and its children show once they've come, a level deeper.
TEST(outline_loads_children_as_it_expands) {
	loaded = cell_int(0);
	open_ids = cell_ref(strs(NULL, 0));
	expands = 0;
	heard_reset();
	ak_uitest *a = mount_on(tree_app, AK_PLATFORM_MACOS);
	ak_fake *f = ak_uitest_host(a);
	ak_fake_view *o = nth(a, AK_KIND_OUTLINE, 0);
	REQUIRE(o);
	CHECK_EQ_INT(ak_fake_list_count(f, o), 2);
	check_levels(o, "0+ 0");
	ak_fake_view *c = ak_fake_new_cell(f);
	ak_fake_bind(f, o, 0, c);
	check_text(c, "Item a");
	dispatch(a, (ak_event){.id = o->id, .kind = AK_EVENT_EXPAND, .x = 0, .y = 1});
	ak_ui_flush();
	CHECK_EQ_INT(expands, 1);
	CHECK(heard_s && str_is(heard_s, "a") && heard_i == 1);
	check_levels(o, "0+o 0");  // expanded, its children not here yet
	CHECK_EQ_INT(ak_fake_list_count(f, o), 2);
	write_i(loaded, 1);
	ak_ui_flush();
	CHECK_EQ_INT(ak_fake_list_count(f, o), 4);
	check_levels(o, "0+o 1 1 0");
	CHECK_EQ_INT(o->items.len, 4);
	ak_fake_view *c2 = ak_fake_new_cell(f);
	ak_fake_bind(f, o, 2, c2);
	check_text(c2, "Item a/2");
	// The user selects a child: the app hears its ID.
	ak_buf b = {0};
	runs(&b, (uint32_t[]){2, 1}, 1);
	dispatch(a, (ak_event){.id = o->id, .kind = AK_EVENT_ROWS_SELECTED, .s = {(const char *)b.data, (uint32_t)b.len}});
	ak_buf_free(&b);
	check_strs(heard.p, "a/2");
	// Collapsed: the children go.
	dispatch(a, (ak_event){.id = o->id, .kind = AK_EVENT_EXPAND, .x = 0, .y = 0});
	ak_ui_flush();
	CHECK_EQ_INT(ak_fake_list_count(f, o), 2);
	check_levels(o, "0+ 0");
	done(a);
	ak_release(loaded), ak_release(open_ids);
	heard_reset();
}

// On a phone: a List of indented rows, a disclosure on those that expand.
TEST(outline_on_a_phone_is_indented_rows) {
	loaded = cell_int(1);
	static const char *const opened[] = {"a"};
	open_ids = cell_ref(strs(opened, 1));
	heard_reset();
	ak_uitest *a = mount_on(tree_app, AK_PLATFORM_ANDROID);
	ak_fake *f = ak_uitest_host(a);
	CHECK_EQ_INT(count_kind(a, AK_KIND_OUTLINE), 0);
	ak_fake_view *l = nth(a, AK_KIND_LIST, 0);
	REQUIRE(l);
	CHECK_EQ_INT(ak_fake_list_count(f, l), 4);
	ak_fake_view *t = ak_fake_one(f, AK_KIND_TEXT, "Item a/1");
	REQUIRE(t);
	ak_fake_view *row = t;
	while (row && row->parent && row->parent->kind != AK_FAKE_KIND_CELL) row = row->parent;
	REQUIRE(row);
	ak_fake_view *t0 = ak_fake_one(f, AK_KIND_TEXT, "Item a");
	REQUIRE(t0);
	float in[4], in0[4];
	ak_fake_abs_frame(f, t, in);
	ak_fake_abs_frame(f, t0, in0);
	CHECK(in[0] - in0[0] >= 15);  // a level deeper
	// Its text alone: no detail line under it (which would take a line
	// hidden), and centered in the row as the disclosure is.
	check_text(row, "Item a/1");
	CHECK_EQ_INT(texts(row), 1);
	float rf[4];
	ak_fake_abs_frame(f, row, rf);
	CHECK(fabsf((in[1] + in[3] / 2) - (rf[1] + rf[3] / 2)) <= 1);
	// A tap on a row that expands collapses it.
	ak_fake_view *row0 = t0;
	while (row0 && row0->parent && row0->parent->kind != AK_FAKE_KIND_CELL) row0 = row0->parent;
	ak_fake_tap(f, row0);
	ak_ui_flush();
	ak_uitest_settle(a);
	CHECK_EQ_INT(ak_fake_list_count(f, l), 2);
	done(a);
	ak_release(loaded), ak_release(open_ids);
	heard_reset();
}

// A sidebar's outline beside a table, in a row: the outline as wide as
// it says, the table taking the rest (a list grows along its parent's
// main axis only).
static ak_val sidebar_app(ak_closure *c, ak_val *a, uint32_t n) {
	ak_view *row = ak_ui_new(AK_EL_ROW);
	ak_view *o = tree_app(c, a, n).p;
	set_i(o, AK_KEY_WIDTH, ak_length_pt(220));
	add(row, o);
	add(row, task_table(c, a, n).p);
	return (ak_val){.p = row};
}

TEST(lists_grow_along_their_parents_axis) {
	loaded = cell_int(0);
	open_ids = cell_ref(strs(NULL, 0));
	ak_arr *all = tasks(10);
	task_items = cell_ref(ak_retain(all));
	ak_uitest *a = mount_on(sidebar_app, AK_PLATFORM_MACOS);
	ak_fake_view *o = nth(a, AK_KIND_OUTLINE, 0), *t = nth(a, AK_KIND_TABLE, 0);
	REQUIRE(o && t);
	CHECK_EQ_INT(o->frame[2], 220);
	CHECK_EQ_INT(t->frame[0], 220);
	CHECK_EQ_INT(t->frame[2], root(a)->frame[2] - 220);
	CHECK_EQ_INT(o->frame[3], root(a)->frame[3]);  // stretched down the row
	done(a);
	ak_release(loaded), ak_release(open_ids), ak_release(task_items), ak_release(all);
	heard_reset();
}

// ---- Undo ----

static void *counter;  // a cell: what the changes change
static int undo_reads;

static ak_val dec(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	write_i(counter, peek_i(counter) - (int64_t)(intptr_t)c->fn);
	return (ak_val){0};
}

static ak_val inc(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	write_i(counter, peek_i(counter) + (int64_t)(intptr_t)c->fn);
	return (ak_val){0};
}

static void change(const char *name, int64_t by) {
	write_i(counter, peek_i(counter) + by);
	ak_closure *u = fn(dec, (void *)(intptr_t)by), *r = fn(inc, (void *)(intptr_t)by);
	ak_str *s = ak_str_c(name);
	ak_undo_register(s, u, r);
	ak_release(s), ak_release(u), ak_release(r);
}

static ak_val group_body(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	change("", 10);
	change("", 100);
	return (ak_val){0};
}

static ak_val can_undo_text(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	undo_reads++;
	return (ak_val){.p = ak_str_c(ak_undo_can(false) ? "can undo" : "nothing")};
}

static ak_val undo_app(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_view *t = ak_ui_new(AK_EL_TEXT);
	bind(t, AK_KEY_TEXT_TEXT, can_undo_text, NULL);
	return (ak_val){.p = t};
}

static void check_undo_state(ak_fake *f, uint8_t flags, const char *undo, const char *redo) {
	ak_span s = cmd_args(f, AK_CMD_UNDO_STATE);
	ak_reader r = {(const uint8_t *)s.p, s.len, 0, false};
	CHECK_EQ_INT(ak_read_u8(&r), flags);
	ak_span u = ak_read_str(&r), d = ak_read_str(&r);
	CHECK(ak_span_eq(u, (ak_span){undo, (uint32_t)strlen(undo)}));
	CHECK(ak_span_eq(d, (ak_span){redo, (uint32_t)strlen(redo)}));
}

// Changes undo last first and redo again; a group is one change; the
// host's Undo undoes; what reads canUndo follows it; the host is told
// what there is to undo.
TEST(undo_stack) {
	counter = cell_int(0);
	undo_reads = 0;
	ak_uitest *a = mount_on(undo_app, AK_PLATFORM_MACOS);
	ak_fake *f = ak_uitest_host(a);
	CHECK(ak_fake_one(f, AK_KIND_TEXT, "nothing") != NULL);
	ak_batch_begin();
	change("Add", 1);
	change("Double", 2);
	ak_closure *g = fn(group_body, NULL);
	ak_str *name = ak_str_c("Paste");
	ak_undo_group(name, g);
	ak_release(g), ak_release(name);
	ak_batch_end();
	ak_ui_flush();
	CHECK_EQ_INT(peek_i(counter), 113);
	CHECK(ak_fake_one(f, AK_KIND_TEXT, "can undo") != NULL);
	check_undo_state(f, 1, "Paste", "");
	CHECK(ak_fake_listens(root(a), AK_EVENT_UNDO));
	// The host's Undo: the group, at once.
	dispatch(a, (ak_event){.id = root(a)->id, .kind = AK_EVENT_UNDO, .i = 0});
	CHECK_EQ_INT(peek_i(counter), 3);
	check_undo_state(f, 3, "Double", "Paste");
	CHECK(ak_undo_undo());
	CHECK(ak_undo_undo());
	CHECK(!ak_undo_undo());
	CHECK_EQ_INT(peek_i(counter), 0);
	ak_ui_flush();
	CHECK(ak_fake_one(f, AK_KIND_TEXT, "nothing") != NULL);
	dispatch(a, (ak_event){.id = root(a)->id, .kind = AK_EVENT_UNDO, .i = 1});  // redo
	CHECK_EQ_INT(peek_i(counter), 1);
	check_undo_state(f, 3, "Add", "Double");
	// A new change: nothing to redo.
	ak_batch_begin();
	change("Triple", 3);
	ak_batch_end();
	ak_ui_flush();
	check_undo_state(f, 1, "Triple", "");
	CHECK(!ak_undo_redo());
	ak_undo_clear();
	CHECK(!ak_undo_can(false));
	done(a);
	ak_release(counter);
}

// A MenuBar's undo and redo items: the platform's (roles 4 and 5 on the
// wire), enabled while there's something to undo or redo; choosing one
// undoes.
static ak_val undo_bar(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_view *items = ak_ui_new(AK_EL_MENU);
	set_str(items, AK_KEY_MENU_TITLE, "Edit");
	ak_view *u = ak_ui_new(AK_EL_MENU_ITEM);
	set_i(u, AK_KEY_MENU_ITEM_ROLE, 5);
	add(items, u);
	ak_view *r = ak_ui_new(AK_EL_MENU_ITEM);
	set_i(r, AK_KEY_MENU_ITEM_ROLE, 6);
	add(items, r);
	return (ak_val){.p = items};
}

static ak_val undo_bar_app(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_view *col = ak_ui_new(AK_EL_COLUMN);
	ak_view *bar = ak_ui_new(AK_EL_MENU_BAR);
	ak_closure *b = fn(undo_bar, NULL);
	ak_ui_lazy(bar, b);
	ak_release(b);
	add(col, bar);
	add(col, text("doc"));
	return (ak_val){.p = col};
}

// The items' flags and roles, as the MenuBar command has them: u32
// menus, per menu its title and items (u8 flags, title, key, u8 mods, u8
// role).
static void check_undo_items(ak_fake *f, const char *want) {
	ak_span s = cmd_args(f, AK_CMD_MENU_BAR);
	ak_reader r = {(const uint8_t *)s.p, s.len, 0, false};
	ak_buf b = {0};
	uint32_t menus = ak_read_u32(&r);
	for (uint32_t m = 0; m < menus && !r.err; m++) {
		ak_read_str(&r);
		uint32_t n = ak_read_u32(&r);
		for (uint32_t i = 0; i < n && !r.err; i++) {
			uint8_t flags = ak_read_u8(&r);
			ak_read_str(&r), ak_read_str(&r), ak_read_u8(&r);
			uint8_t role = ak_read_u8(&r);
			ak_buf_printf(&b, "%s%u%s", b.len ? " " : "", role, flags & AK_MENU_DISABLED ? "-" : "+");
		}
	}
	CHECK_EQ_STR(cstr(&b), want);
	ak_buf_free(&b);
}

TEST(undo_menu_items) {
	counter = cell_int(0);
	ak_uitest *a = mount_on(undo_bar_app, AK_PLATFORM_WINDOWS);
	ak_fake *f = ak_uitest_host(a);
	check_undo_items(f, "4- 5-");
	ak_batch_begin();
	change("Add", 1);
	ak_batch_end();
	ak_ui_flush();
	check_undo_items(f, "4+ 5-");
	// Chosen: the menu bar's item 0 (its Edit menu's first), the app's undo.
	dispatch(a, (ak_event){.id = root(a)->id, .kind = AK_EVENT_ACTION, .i = 0});
	CHECK_EQ_INT(peek_i(counter), 0);
	ak_ui_flush();
	check_undo_items(f, "4- 5+");
	done(a);
	ak_release(counter);
}

// ---- The cursor ----

static ak_val cursor_app(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_view *v = ak_ui_new(AK_EL_VIEW);
	set_i(v, AK_KEY_CURSOR, AK_CURSOR_POINTER);
	add(v, text("link"));
	return (ak_val){.p = v};
}

// A container with a cursor has a view of its own, which says it.
TEST(cursor_makes_a_view) {
	ak_uitest *a = mount_on(cursor_app, AK_PLATFORM_MACOS);
	ak_fake_view *t = ak_fake_one(ak_uitest_host(a), AK_KIND_TEXT, "link");
	REQUIRE(t && t->parent);
	CHECK_EQ_INT(ak_fake_int(t->parent, AK_PROP_CURSOR), AK_CURSOR_POINTER);
	done(a);
}

T_MAIN()
