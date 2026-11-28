// Chip, DateRangePicker, Disclosure and a TextField's suggestions
// (widgets.c), and an Image's request (its Headers and Cache before its
// Source: media.c), against the fake host's op streams.
#include "uih.h"

static ak_val set_int_handler(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(n);
	ak_vm_cell_set((void *)c->fn, (ak_val){.i = a[0].i});
	return noval();
}

static ak_val count_handler(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(a), AK_UNUSED(n);
	write_i((void *)c->fn, peek_i((void *)c->fn) + 1);
	return noval();
}

static ak_val set_ref_handler(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(n);
	write_p((void *)c->fn, ak_retain(a[0].p));
	return noval();
}

static ak_arr *strings(const char *const *s, int n) {
	ak_arr *arr = ak_arr_new(AK_K_REF, NULL, (uint32_t)n);
	arr->h.flags = 1;
	for (int i = 0; i < n; i++) ak_arr_push(&arr, (ak_val){.p = ak_str_c(s[i])});
	return arr;
}

// ops_of counts the SetProps of prop p to view id since the last reset.
static int sets_of(ak_fake *f, ak_id id, ak_prop p) {
	int n = 0;
	for (uint32_t i = 0; i < ak_fake_op_count(f); i++) {
		ak_op op = ak_fake_op_at(f, i);
		if (op.code == AK_OP_SET_PROP && op.id == id && op.prop == p) n++;
	}
	return n;
}

// first_set is the index of the first SetProp of p to id (-1: none).
static int first_set(ak_fake *f, ak_id id, ak_prop p) {
	for (uint32_t i = 0; i < ak_fake_op_count(f); i++) {
		ak_op op = ak_fake_op_at(f, i);
		if (op.code == AK_OP_SET_PROP && op.id == id && op.prop == p) return (int)i;
	}
	return -1;
}

// ---- Chips ----

static void *chip_on, *chip_presses, *chip_removes;

static ak_val chips_app(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_view *col = ak_ui_new(AK_EL_COLUMN);
	ak_view *row = ak_ui_new(AK_EL_ROW);
	add(col, row);
	ak_view *filter = ak_ui_new(AK_EL_CHIP);  // bind selected: chip_on
	set_str(filter, AK_KEY_CHIP_TITLE, "Pool");
	bind(filter, AK_KEY_CHIP_SELECTED, read_cell, chip_on);
	on(filter, AK_EV_CHIP_CHANGE, set_int_handler, chip_on);
	add(row, filter);
	ak_view *assist = ak_ui_new(AK_EL_CHIP);
	set_str(assist, AK_KEY_CHIP_TITLE, "Directions");
	set_str(assist, AK_KEY_CHIP_ICON, "map");
	on(assist, AK_EV_CHIP_PRESS, count_handler, chip_presses);
	add(row, assist);
	ak_view *input = ak_ui_new(AK_EL_CHIP);
	set_str(input, AK_KEY_CHIP_TITLE, "Ana");
	on(input, AK_EV_CHIP_REMOVE, count_handler, chip_removes);
	add(row, input);
	return (ak_val){.p = col};
}

TEST(chips) {
	chip_on = cell_int(0);
	chip_presses = cell_int(0);
	chip_removes = cell_int(0);
	ak_uitest *a = mount(chips_app, NULL);
	ak_fake *f = ak_uitest_host(a);
	ak_fake_view *filter = one(a, AK_KIND_CHIP, "Pool"), *assist = one(a, AK_KIND_CHIP, "Directions"), *input = one(a, AK_KIND_CHIP, "Ana");
	// Each kind from what it was given.
	CHECK(ak_fake_bool(filter, AK_PROP_CHIP_FILTER));
	CHECK(!ak_fake_bool(filter, AK_PROP_CHIP_SELECTED) && has_prop(filter, AK_PROP_CHIP_SELECTED));
	CHECK(ak_fake_listens(filter, AK_EVENT_TOGGLED) && !ak_fake_listens(filter, AK_EVENT_PRESS));
	CHECK(!has_prop(assist, AK_PROP_CHIP_FILTER) && !has_prop(assist, AK_PROP_CHIP_REMOVABLE));
	CHECK(ak_span_eqc(ak_fake_string(assist, AK_PROP_CHIP_ICON), "map"));
	CHECK(ak_fake_listens(assist, AK_EVENT_PRESS));
	CHECK(ak_fake_bool(input, AK_PROP_CHIP_REMOVABLE) && ak_fake_listens(input, AK_EVENT_REMOVE));
	// Measured: the icon and the remove button take room; 32 tall, in a row.
	check_frame(filter, 0, filter->frame[1], 4 * 8 + 32, 32);
	check_frame(assist, filter->frame[2], assist->frame[1], 10 * 8 + 32 + 26, 32);
	CHECK_EQ_F(input->frame[2], 3 * 8 + 32 + 26);
	// A tap on the filter chip selects it: the state follows, the host isn't
	// told what it shows already, and its checkmark makes it wider.
	reset(a);
	ak_buf err = {0};
	CHECK(ak_uitest_tap(a, AK_STR("Pool"), &err));
	CHECK_EQ_INT(peek_i(chip_on), 1);
	CHECK(ak_fake_bool(filter, AK_PROP_CHIP_SELECTED));
	CHECK_EQ_INT(sets_of(f, filter->id, AK_PROP_CHIP_SELECTED), 0);
	CHECK_EQ_F(filter->frame[2], 4 * 8 + 32 + 26);
	// The app's change is sent.
	write_i(chip_on, 0);
	ak_ui_flush();
	CHECK(!ak_fake_bool(filter, AK_PROP_CHIP_SELECTED));
	CHECK_EQ_F(filter->frame[2], 4 * 8 + 32);
	// An action chip presses; an input chip's remove button, and Delete
	// while it has the focus, remove it.
	CHECK(ak_uitest_tap(a, AK_STR("Directions"), &err));
	CHECK_EQ_INT(peek_i(chip_presses), 1);
	CHECK(ak_fake_remove_chip(f, input));
	CHECK_EQ_INT(peek_i(chip_removes), 1);
	ak_fake_focus(f, input, true);
	ak_fake_key(f, input, "delete", 0);
	CHECK_EQ_INT(peek_i(chip_removes), 2);
	ak_buf_free(&err);
	done(a);
	ak_release(chip_on), ak_release(chip_presses), ak_release(chip_removes);
}

// ---- DateRangePicker ----

static void *range_start, *range_end, *range_changes;
#define DAY 86400000LL

static ak_val range_app(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_view *col = ak_ui_new(AK_EL_COLUMN);
	ak_view *p = ak_ui_new(AK_EL_DATE_RANGE_PICKER);
	bind(p, AK_KEY_DATE_RANGE_PICKER_START, read_cell, range_start);
	bind(p, AK_KEY_DATE_RANGE_PICKER_END, read_cell, range_end);
	on(p, AK_EV_DATE_RANGE_PICKER_START_CHANGE, set_int_handler, range_start);
	on(p, AK_EV_DATE_RANGE_PICKER_END_CHANGE, set_int_handler, range_end);
	on(p, AK_EV_DATE_RANGE_PICKER_CHANGE, count_handler, range_changes);
	set_i(p, AK_KEY_DATE_RANGE_PICKER_MIN, 10 * DAY);
	add(col, p);
	return (ak_val){.p = col};
}

TEST(date_range_picker) {
	range_start = cell_int(20 * DAY);
	range_end = cell_int(18 * DAY);  // before the start: shown as the start
	range_changes = cell_int(0);
	ak_uitest *a = mount(range_app, NULL);
	ak_fake *f = ak_uitest_host(a);
	ak_fake_view *p = nth(a, AK_KIND_DATE_RANGE_PICKER, 0);
	CHECK_EQ_INT(ak_fake_int(p, AK_PROP_DATE_RANGE_PICKER_START), 20 * DAY);
	CHECK_EQ_INT(ak_fake_int(p, AK_PROP_DATE_RANGE_PICKER_END), 20 * DAY);
	CHECK_EQ_INT(ak_fake_int(p, AK_PROP_DATE_RANGE_PICKER_MIN), 10 * DAY);
	CHECK(ak_fake_listens(p, AK_EVENT_RANGE_CHANGED));
	check_frame(p, 0, p->frame[1], 390, 34);  // stretched across the column, as tall as it measures
	// The user's range writes both, once, and isn't sent back.
	reset(a);
	int measured = f->measures;
	ak_fake_pick_range(f, p, 12 * DAY, 15 * DAY);
	CHECK_EQ_INT(peek_i(range_start), 12 * DAY);
	CHECK_EQ_INT(peek_i(range_end), 15 * DAY);
	CHECK_EQ_INT(peek_i(range_changes), 1);
	ak_ui_flush();
	CHECK_EQ_INT(sets_of(f, p->id, AK_PROP_DATE_RANGE_PICKER_START) + sets_of(f, p->id, AK_PROP_DATE_RANGE_PICKER_END), 0);
	CHECK(f->measures > measured);  // the dates' text is another width
	measured = f->measures;
	ak_ui_flush();
	CHECK_EQ_INT(f->measures, measured);  // once
	// The app's goes.
	write_i(range_end, 16 * DAY);
	ak_ui_flush();
	CHECK_EQ_INT(ak_fake_int(p, AK_PROP_DATE_RANGE_PICKER_END), 16 * DAY);
	CHECK_EQ_INT(sets_of(f, p->id, AK_PROP_DATE_RANGE_PICKER_START), 0);
	done(a);
	ak_release(range_start), ak_release(range_end), ak_release(range_changes);
}

// ---- Disclosure ----

static int built;
static void *dis_open;

static ak_val dis_content(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	built++;
	ak_view *col = ak_ui_new(AK_EL_COLUMN);
	add(col, text("Inside"));
	return (ak_val){.p = col};
}

static ak_val dis_app(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(a), AK_UNUSED(n);
	bool bound = c->fn != NULL;
	ak_view *col = ak_ui_new(AK_EL_COLUMN);
	ak_view *d = ak_ui_new(AK_EL_DISCLOSURE);
	set_str(d, AK_KEY_DISCLOSURE_TITLE, "Advanced");
	if (bound) {
		bind(d, AK_KEY_DISCLOSURE_EXPANDED, read_cell, dis_open);
		on(d, AK_EV_DISCLOSURE_CHANGE, set_int_handler, dis_open);
	}
	ak_closure *b = fn(dis_content, NULL);
	ak_ui_lazy(d, b);
	ak_release(b);
	add(col, d);
	add(col, text("After"));
	return (ak_val){.p = col};
}

TEST(disclosure_unbound) {
	built = 0;
	ak_uitest *a = mount(dis_app, NULL);
	ak_fake_view *h = one(a, AK_KIND_DISCLOSURE, "Advanced");
	CHECK(!ak_fake_bool(h, AK_PROP_DISCLOSURE_EXPANDED));
	CHECK(ak_fake_listens(h, AK_EVENT_TOGGLED));
	CHECK_EQ_INT(built, 0);  // closed: its content isn't built
	check_frame(h, 0, h->frame[1], 390, 44);  // as wide as the disclosure
	ak_buf err = {0};
	// Opened by the user, it keeps the choice: the content shows under the
	// header, before what follows the disclosure.
	CHECK(ak_uitest_tap(a, AK_STR("Advanced"), &err));
	CHECK_EQ_INT(built, 1);
	ak_fake_view *in = one(a, AK_KIND_TEXT, "Inside"), *after = one(a, AK_KIND_TEXT, "After");
	CHECK(in && after && in->frame[1] < after->frame[1]);
	CHECK(ak_uitest_tap(a, AK_STR("Advanced"), &err));
	CHECK_EQ_INT(count_kind(a, AK_KIND_TEXT), 1);  // its content's views are gone
	CHECK(ak_uitest_tap(a, AK_STR("Advanced"), &err));
	CHECK_EQ_INT(built, 2);  // built again as it opens
	ak_buf_free(&err);
	done(a);
}

TEST(disclosure_bound) {
	built = 0;
	dis_open = cell_int(1);
	ak_uitest *a = mount(dis_app, (void *)1);
	ak_fake *f = ak_uitest_host(a);
	ak_fake_view *h = one(a, AK_KIND_DISCLOSURE, "Advanced");
	CHECK(ak_fake_bool(h, AK_PROP_DISCLOSURE_EXPANDED));
	CHECK_EQ_INT(built, 1);
	// The user's closing writes the state, and isn't sent back.
	reset(a);
	ak_fake_tap(f, h);
	ak_ui_flush();
	CHECK_EQ_INT(peek_i(dis_open), 0);
	CHECK_EQ_INT(sets_of(f, h->id, AK_PROP_DISCLOSURE_EXPANDED), 0);
	CHECK_EQ_INT(count_kind(a, AK_KIND_TEXT), 1);
	// The app's opening opens it.
	write_i(dis_open, 1);
	ak_ui_flush();
	CHECK(ak_fake_bool(h, AK_PROP_DISCLOSURE_EXPANDED));
	CHECK(one(a, AK_KIND_TEXT, "Inside") != NULL);
	CHECK_EQ_INT(built, 2);
	done(a);
	ak_release(dis_open);
}

// ---- A TextField's suggestions ----

static void *sug_text, *sug_chosen;
static const char *const cities[] = {"Paris", "Prague", "Rome", "PARMA"};

static ak_val sug_app(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(a), AK_UNUSED(n);
	ak_view *col = ak_ui_new(AK_EL_COLUMN);
	ak_view *t = ak_ui_new(AK_EL_TEXT_FIELD);
	bind(t, AK_KEY_TEXT_FIELD_TEXT, read_cell, sug_text);
	on(t, AK_EV_TEXT_FIELD_CHANGE, set_ref_handler, sug_text);
	on(t, AK_EV_TEXT_FIELD_SUGGESTION, set_int_handler, sug_chosen);
	set(t, AK_KEY_TEXT_FIELD_SUGGESTIONS, (ak_val){.p = strings(cities, 4)});
	if (c->fn) set_i(t, AK_KEY_TEXT_FIELD_FILTER_SUGGESTIONS, 0);
	set_i(t, AK_KEY_TEXT_FIELD_DROPDOWN, 1);
	add(col, t);
	return (ak_val){.p = col};
}

static void check_suggestions(ak_fake_view *v, const char *want) {
	ak_str_vec list = {0};
	ak_fake_strings(v, AK_PROP_TEXT_FIELD_SUGGESTIONS, &list);
	char got[256] = "";
	for (uint32_t i = 0; i < list.len; i++) {
		size_t l = strlen(got);
		snprintf(got + l, sizeof got - l, "%s%.*s", i ? "," : "", (int)list.data[i].len, list.data[i].p);
	}
	ak_vec_free(&list);
	CHECK_EQ_STR(got, want);
}

TEST(suggestions) {
	sug_text = cell_ref(ak_str_c(""));
	sug_chosen = cell_int(-1);
	ak_uitest *a = mount(sug_app, NULL);
	ak_fake *f = ak_uitest_host(a);
	ak_fake_view *t = nth(a, AK_KIND_TEXT_FIELD, 0);
	// Nothing typed: all of them, told before it's inserted.
	check_suggestions(t, "Paris,Prague,Rome,PARMA");
	CHECK(ak_fake_bool(t, AK_PROP_TEXT_FIELD_DROPDOWN));
	CHECK(ak_fake_listens(t, AK_EVENT_SUGGESTION));
	// Typing filters them, ignoring case.
	ak_fake_type(f, t, "pAr");
	ak_ui_flush();
	check_suggestions(t, "Paris,PARMA");
	// A choice, by its index in what the host shows: the text is its, the
	// handler gets its index among the app's, and the field isn't sent
	// its own text back; one of them typed in full lists them all again.
	reset(a);
	CHECK(ak_fake_choose_suggestion(f, t, 1));
	ak_ui_flush();
	ak_val got = ak_vm_cell_get(sug_text, false);  // retained
	CHECK(ak_span_eqc(ak_span_str(got.p), "PARMA"));
	ak_release(got.p);
	CHECK_EQ_INT(peek_i(sug_chosen), 3);
	CHECK_EQ_INT(sets_of(f, t->id, AK_PROP_TEXT_FIELD_VALUE), 0);
	check_suggestions(t, "Paris,Prague,Rome,PARMA");
	// The app's text filters them too; an unchanged list isn't sent again.
	write_p(sug_text, ak_str_c("ro"));
	ak_ui_flush();
	check_suggestions(t, "Rome");
	reset(a);
	write_p(sug_text, ak_str_c("rom"));
	ak_ui_flush();
	CHECK_EQ_INT(sets_of(f, t->id, AK_PROP_TEXT_FIELD_SUGGESTIONS), 0);
	done(a);
	ak_release(sug_text), ak_release(sug_chosen);
}

TEST(suggestions_unfiltered) {
	sug_text = cell_ref(ak_str_c("zzz"));
	sug_chosen = cell_int(-1);
	ak_uitest *a = mount(sug_app, (void *)1);
	ak_fake *f = ak_uitest_host(a);
	ak_fake_view *t = nth(a, AK_KIND_TEXT_FIELD, 0);
	check_suggestions(t, "Paris,Prague,Rome,PARMA");  // the app filtered them
	CHECK(ak_fake_choose_suggestion(f, t, 2));
	CHECK_EQ_INT(peek_i(sug_chosen), 2);
	done(a);
	ak_release(sug_text), ak_release(sug_chosen);
}

// ---- An Image's request ----

static void *img_src;

static ak_val img_app(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_view *col = ak_ui_new(AK_EL_COLUMN);
	ak_view *i = ak_ui_new(AK_EL_IMAGE);
	bind(i, AK_KEY_IMAGE_SOURCE, read_cell, img_src);
	ak_map *h = ak_map_new(AK_K_REF, true, AK_K_REF, NULL);
	ak_map_set(&h, (ak_val){.p = ak_str_c("Authorization")}, (ak_val){.p = ak_str_c("Bearer t")});
	set(i, AK_KEY_IMAGE_HEADERS, (ak_val){.p = h});
	set_i(i, AK_KEY_IMAGE_CACHE, AK_IMAGE_CACHE_MEMORY);
	add(col, i);
	ak_view *plain = ak_ui_new(AK_EL_IMAGE);
	set_str(plain, AK_KEY_IMAGE_SOURCE, "logo.png");
	add(col, plain);
	return (ak_val){.p = col};
}

TEST(image_request) {
	img_src = cell_ref(ak_str_c("https://example.com/a.png"));
	setup();
	ak_closure *c = fn(img_app, NULL);
	ak_uitest *a = ak_uitest_mount(c, NULL);
	ak_release(c);
	ak_fake *f = ak_uitest_host(a);
	ak_fake_view *i = nth(a, AK_KIND_IMAGE, 0);
	CHECK(ak_span_eqc(ak_fake_string(i, AK_PROP_IMAGE_SOURCE), "https://example.com/a.png"));
	CHECK_EQ_INT(ak_fake_int(i, AK_PROP_IMAGE_CACHE), AK_IMAGE_CACHE_MEMORY);
	ak_str_vec hs = {0};
	ak_fake_strings(i, AK_PROP_IMAGE_HEADERS, &hs);
	CHECK(hs.len == 2 && ak_span_eqc(hs.data[0], "Authorization") && ak_span_eqc(hs.data[1], "Bearer t"));
	ak_vec_free(&hs);
	CHECK(ak_span_eqc(ak_fake_string(nth(a, AK_KIND_IMAGE, 1), AK_PROP_IMAGE_SOURCE), "logo.png"));
	CHECK(!has_prop(nth(a, AK_KIND_IMAGE, 1), AK_PROP_IMAGE_HEADERS));
	// A new source goes after the request, at the flush.
	reset(a);
	write_p(img_src, ak_str_c("https://example.com/b.png"));
	ak_ui_flush();
	CHECK_EQ_INT(sets_of(f, i->id, AK_PROP_IMAGE_SOURCE), 1);
	CHECK(ak_span_eqc(ak_fake_string(i, AK_PROP_IMAGE_SOURCE), "https://example.com/b.png"));
	done(a);
	ak_release(img_src);
}

// The first batch: Headers and Cache before Source.

TEST(image_request_order) {
	img_src = cell_ref(ak_str_c("https://example.com/a.png"));
	setup();
	ak_closure *c = fn(img_app, NULL);
	ak_uitest *a = ak_uitest_mount(c, NULL);
	ak_release(c);
	ak_fake *f = ak_uitest_host(a);
	ak_fake_view *i = nth(a, AK_KIND_IMAGE, 0);
	int hp = first_set(f, i->id, AK_PROP_IMAGE_HEADERS), cp = first_set(f, i->id, AK_PROP_IMAGE_CACHE), sp = first_set(f, i->id, AK_PROP_IMAGE_SOURCE);
	CHECK(hp >= 0 && cp >= 0 && sp >= 0);
	CHECK(hp < sp && cp < sp);
	done(a);
	ak_release(img_src);
}

T_MAIN()
