// The C encoder against Go's golden files (ops/ops_test.go writes them;
// the Swift, Kotlin and JavaScript decoders read them): every batch,
// event and payload the Go tests make, made again here, must be the same
// bytes.
#include "t.h"
#include "ops/ops.h"

#include <stdio.h>
#include <stdlib.h>

// golden reads $ASUKA_ROOT/ops/testdata/name into b.
static bool golden(const char *name, ak_buf *b) {
	const char *root = getenv("ASUKA_ROOT");
	char path[1024];
	snprintf(path, sizeof path, "%s/ops/testdata/%s", root ? root : "../..", name);
	FILE *f = fopen(path, "rb");
	if (!f) {
		T_FAIL("can't open %s", path);
		return false;
	}
	b->len = 0;
	char tmp[4096];
	size_t n;
	while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) ak_buf_put(b, tmp, n);
	fclose(f);
	return true;
}

static void check_bytes(const char *what, const uint8_t *got, size_t n, const ak_buf *want) {
	if (n != want->len || (n && memcmp(got, want->data, n) != 0)) {
		size_t i = 0;
		while (i < n && i < want->len && got[i] == want->data[i]) i++;
		T_FAIL("%s: %zu bytes, want %zu; first difference at %zu", what, n, want->len, i);
	}
}

static void check_golden(const char *name, const uint8_t *got, size_t n) {
	ak_buf want = {0};
	if (golden(name, &want)) check_bytes(name, got, n, &want);
	ak_buf_free(&want);
}

// cstr returns b's text, NUL-terminated (for comparisons).
static const char *cstr(ak_buf *b) {
	ak_buf_u8(b, 0);
	b->len--;
	return (const char *)b->data;
}

// golden_batch uses every op and value type, as goldenBatch.
static void golden_batch(ak_batch *b) {
	ak_batch_create(b, 1, AK_KIND_VIEW);
	ak_batch_create(b, 2, AK_KIND_TEXT);
	const char *s = "héllo, 世界";
	ak_batch_set_string(b, 2, AK_PROP_TEXT_TEXT, s, strlen(s));
	ak_batch_set_bool(b, 2, AK_PROP_HIDDEN, true);
	ak_batch_set_int(b, 2, AK_PROP_LINE_LIMIT, -3);
	ak_batch_set_float(b, 2, AK_PROP_FONT_SIZE, 17.5);
	ak_batch_set_color(b, 1, AK_PROP_BACKGROUND, ak_rgba(0x11, 0x22, 0x33, 0xff));
	ak_batch_set_color(b, 2, AK_PROP_COLOR, ak_semantic_color(AK_SEMANTIC_COLOR_SECONDARY_LABEL));
	ak_batch_set_enum(b, 2, AK_PROP_TEXT_STYLE, AK_TEXT_STYLE_HEADLINE);
	ak_batch_set_edges(b, 1, AK_PROP_BORDER_WIDTH, 1, 2, 3.25f, 4);
	ak_batch_set_edges(b, 1, AK_PROP_CORNER_RADII, 16, 16, 0, 0.5f);
	ak_batch_set_float(b, 2, AK_PROP_LETTER_SPACING, -0.25);
	ak_batch_set_int(b, 2, AK_PROP_FONT_TRAITS, 3);
	ak_batch_set_bytes(b, 1, AK_PROP_A11Y_LABEL, (const uint8_t[]){0, 1, 0xfe}, 3);
	ak_batch_insert(b, 1, 2, 0);
	ak_batch_set_frame(b, 2, 0.5f, 1, 320, 44.25f);
	ak_batch_listen(b, 1, AK_EVENT_PRESS);
	ak_batch_unlisten(b, 1, AK_EVENT_PRESS);
	ak_batch_command(b, 1, AK_CMD_LIST_UPDATE, (const uint8_t[]){1, 2, 3}, 3);
	ak_batch_remove(b, 1, 2);
	ak_batch_destroy(b, 2);
	ak_batch_set_root(b, 1);
}

TEST(golden_bin) {
	ak_batch b = {0};
	golden_batch(&b);
	CHECK_EQ_INT(ak_batch_len(&b), 22);
	size_t n;
	const uint8_t *p = ak_batch_encode(&b, &n);
	check_golden("golden.bin", p, n);
	ak_batch_free(&b);
}

// The golden file decodes to the ops of golden.txt, and to the text Go's
// Op.String prints.
TEST(golden_txt) {
	ak_buf bin = {0}, txt = {0};
	REQUIRE(golden("golden.bin", &bin));
	REQUIRE(golden("golden.txt", &txt));
	ak_batch_reader br;
	ak_batch_reader_init(&br, bin.data, bin.len);
	ak_buf dump = {0}, str = {0};
	ak_op op;
	int n = 0;
	while (ak_batch_next(&br, &op)) {
		ak_op_dump(&dump, &op);
		ak_buf_u8(&dump, '\n');
		ak_op_format(&str, &op);
		ak_buf_u8(&str, '\n');
		n++;
	}
	CHECK_EQ_INT(br.err, AK_OK);
	CHECK_EQ_INT(n, 22);
	check_bytes("golden.txt", dump.data, dump.len, &txt);
	CHECK_EQ_STR(cstr(&str),
		"Create 1 View\n"
		"Create 2 Text\n"
		"SetProp 2 #64=\"héllo, 世界\"\n"
		"SetProp 2 Hidden=true\n"
		"SetProp 2 LineLimit=-3\n"
		"SetProp 2 FontSize=17.5\n"
		"SetProp 1 Background=#2112233ff\n"
		"SetProp 2 Color=#100000002\n"
		"SetProp 2 TextStyle=5\n"
		"SetProp 1 BorderWidth=[1 2 3.25 4]\n"
		"SetProp 1 CornerRadii=[16 16 0 0.5]\n"
		"SetProp 2 LetterSpacing=-0.25\n"
		"SetProp 2 FontTraits=3\n"
		"SetProp 1 A11yLabel=0001fe\n"
		"Insert 1 2 @0\n"
		"SetFrame 2 0.5,1 320x44.25\n"
		"Listen 1 Press\n"
		"Unlisten 1 Press\n"
		"Command 1 ListUpdate 010203\n"
		"Remove 1 2\n"
		"Destroy 2\n"
		"SetRoot 1\n");
	ak_buf_free(&bin);
	ak_buf_free(&txt);
	ak_buf_free(&dump);
	ak_buf_free(&str);
}

// Values print as Go's fmt prints them.
TEST(op_strings) {
	ak_batch b = {0};
	ak_batch_set_float(&b, 1, AK_PROP_OPACITY, 1e6);
	ak_batch_set_float(&b, 1, AK_PROP_OPACITY, 0.1);
	const char s[] = "a\"\\\n\x01­😀\xff";
	ak_batch_set_string(&b, 1, AK_PROP_A11Y_LABEL, s, sizeof s - 1);
	ak_batch_set_frame(&b, 1, 0.1f, 0, 1e7f, 123456.7f);
	ak_batch_create(&b, 3, 999);
	ak_batch_listen(&b, 3, 999);
	ak_batch_command(&b, 3, 999, NULL, 0);
	size_t n;
	const uint8_t *p = ak_batch_encode(&b, &n);
	ak_batch_reader br;
	ak_batch_reader_init(&br, p, n);
	ak_buf out = {0};
	ak_op op;
	while (ak_batch_next(&br, &op)) {
		ak_op_format(&out, &op);
		ak_buf_u8(&out, '\n');
	}
	CHECK_EQ_STR(cstr(&out),
		"SetProp 1 Opacity=1e+06\n"
		"SetProp 1 Opacity=0.1\n"
		"SetProp 1 A11yLabel=\"a\\\"\\\\\\n\\x01\\u00ad😀\\xff\"\n"
		"SetFrame 1 0.1,0 1e+07x123456.7\n"
		"Create 3 Kind(999)\n"
		"Listen 3 Event(999)\n"
		"Command 3 Command(999) \n");
	ak_buf_free(&out);
	ak_batch_free(&b);
}

TEST(go_floats) {
	static const struct {
		double f;
		int bits;
		const char *want;
	} cases[] = {
		{1e6, 64, "1e+06"}, {123456, 64, "123456"}, {1234567, 64, "1.234567e+06"}, {0.0001, 64, "0.0001"},
		{0.00001, 64, "1e-05"}, {17.5, 64, "17.5"}, {0.1f, 32, "0.1"}, {44.25, 32, "44.25"}, {1e21, 64, "1e+21"},
		{100, 64, "100"}, {0, 64, "0"}, {-2.5, 64, "-2.5"}, {0.1, 64, "0.1"}, {1.0 / 3, 64, "0.3333333333333333"},
		{1.0f / 3, 32, "0.33333334"}, {NAN, 64, "NaN"}, {INFINITY, 64, "+Inf"}, {-INFINITY, 32, "-Inf"},
	};
	for (size_t i = 0; i < AK_LEN(cases); i++) {
		ak_buf b = {0};
		ak_fmt_float(&b, cases[i].f, cases[i].bits);
		CHECK_EQ_STR(cstr(&b), cases[i].want);
		ak_buf_free(&b);
	}
}

TEST(round_trip) {
	ak_batch b = {0};
	golden_batch(&b);
	size_t n;
	const uint8_t *p = ak_batch_encode(&b, &n);
	ak_batch_reader br;
	ak_batch_reader_init(&br, p, n);
	ak_op op;
	int count = 0;
	while (ak_batch_next(&br, &op)) count++;
	CHECK_EQ_INT(br.err, AK_OK);
	CHECK_EQ_INT(count, 22);
	// Cut short, it stops with an error.
	ak_batch_reader_init(&br, p, 20);
	while (ak_batch_next(&br, &op)) {
	}
	CHECK_EQ_INT(br.err, AK_ERR_SHORT);
	// Bytes after the last op are an error too.
	ak_buf extra = {0};
	ak_buf_put(&extra, p, n);
	ak_buf_u8(&extra, 0);
	ak_batch_reader_init(&br, extra.data, extra.len);
	while (ak_batch_next(&br, &op)) {
	}
	CHECK_EQ_INT(br.err, AK_ERR_TRAILING);
	ak_buf_free(&extra);
	ak_batch_free(&b);
}

// A batch reused allocates nothing: its buffer stays where it was.
TEST(encode_allocates_nothing) {
	ak_batch b = {0};
	const uint8_t *data = NULL;
	size_t cap = 0;
	for (int round = 0; round < 100; round++) {
		ak_batch_reset(&b);
		for (uint32_t i = 0; i < 50; i++) {
			ak_batch_set_string(&b, i, AK_PROP_TEXT_TEXT, "some text", 9);
			ak_batch_set_frame(&b, i, 1, 2, 3, 4);
			ak_batch_command(&b, i, AK_CMD_FOCUS, (const uint8_t[]){1, 2}, 2);
			ak_buf *w = ak_batch_command_begin(&b, i, AK_CMD_LIST_UPDATE);
			ak_buf_u32(w, 7);
			ak_batch_command_end(&b);
		}
		size_t n;
		const uint8_t *p = ak_batch_encode(&b, &n);
		if (round == 0) {
			data = p;
			cap = b.buf.cap;
		} else if (p != data || b.buf.cap != cap) {
			T_FAIL("round %d moved the buffer", round);
			break;
		}
	}
	ak_batch_free(&b);
}

// The _begin calls write what the plain calls do.
TEST(begin_end) {
	ak_batch a = {0}, b = {0};
	ak_batch_set_bytes(&a, 1, AK_PROP_A11Y_ACTIONS, "xyz", 3);
	ak_batch_command(&a, 2, AK_CMD_FOCUS, "ab", 2);
	ak_buf_put(ak_batch_set_bytes_begin(&b, 1, AK_PROP_A11Y_ACTIONS), "xyz", 3);
	ak_batch_set_bytes_end(&b);
	ak_buf_put(ak_batch_command_begin(&b, 2, AK_CMD_FOCUS), "ab", 2);
	ak_batch_command_end(&b);
	size_t na, nb;
	const uint8_t *pa = ak_batch_encode(&a, &na), *pb = ak_batch_encode(&b, &nb);
	CHECK(na == nb && memcmp(pa, pb, na) == 0);
	ak_batch_free(&a);
	ak_batch_free(&b);
}

// golden_events use every payload, as goldenEvents.
static uint32_t golden_events(ak_event *evs) {
	uint32_t n = 0;
	evs[n++] = (ak_event){.id = 7, .kind = AK_EVENT_PRESS};
	evs[n++] = (ak_event){.id = 7, .kind = AK_EVENT_TEXT_CHANGED, .s = AK_STR("hé")};
	evs[n++] = (ak_event){.id = 8, .kind = AK_EVENT_TOGGLED, .b = true};
	evs[n++] = (ak_event){.id = 9, .kind = AK_EVENT_VALUE_CHANGED, .f = 0.25};
	evs[n++] = (ak_event){.id = 10, .kind = AK_EVENT_SCROLLED, .x = 1.5, .y = -2};
	evs[n++] = (ak_event){.id = 1, .kind = AK_EVENT_LIFECYCLE, .i = AK_LIFECYCLE_BACKGROUND};
	evs[n++] = (ak_event){.id = 1, .kind = AK_EVENT_ENVIRONMENT, .env = {
		.width = 402, .height = 874, .safe_top = 62, .safe_bottom = 34, .keyboard = 336,
		.scale = 3, .font_scale = 1.25f, .dark = true, .reduce_motion = true, .screen_reader = true, .tv = true, .watch = true, .platform = AK_PLATFORM_IOS,
		.width_class = AK_SIZE_CLASS_COMPACT, .height_class = AK_SIZE_CLASS_EXPANDED, .locale = AK_STR("en_US"),
		.time_zone = AK_STR("Asia/Shanghai"), .languages = AK_STR("en-US,zh-Hans"), .first_weekday = 2, .hour_cycle = 24,
		.posture = AK_POSTURE_BOOK, .fold_x = 397, .fold_width = 8, .fold_height = 874, .multiple_windows = true,
	}};
	evs[n++] = (ak_event){.id = 11, .kind = AK_EVENT_PAN, .g = {.state = AK_GESTURE_STATE_CHANGED, .x = 10, .y = 20,
		.translation_x = 5, .translation_y = -3, .velocity_x = 100, .velocity_y = -50, .scale = 1, .rotation = 0.5f}};
	evs[n++] = (ak_event){.id = 12, .kind = AK_EVENT_NATIVE, .s = AK_STR("\x01\x02\x03")};
	evs[n++] = (ak_event){.id = 13, .kind = AK_EVENT_DOUBLE_TAP, .x = 4, .y = 5.5};
	evs[n++] = (ak_event){.id = 14, .kind = AK_EVENT_ZOOMED, .f = 2.5};
	evs[n++] = (ak_event){.id = 15, .kind = AK_EVENT_MOVED, .x = 1, .y = 3};
	return n;
}

static bool event_eq(const ak_event *a, const ak_event *b) {
	const ak_environment *x = &a->env, *y = &b->env;
	return a->id == b->id && a->kind == b->kind && a->b == b->b && a->i == b->i && a->f == b->f && a->x == b->x &&
	       a->y == b->y && ak_span_eq(a->s, b->s) && x->width == y->width && x->height == y->height &&
	       x->safe_top == y->safe_top && x->safe_right == y->safe_right && x->safe_bottom == y->safe_bottom &&
	       x->safe_left == y->safe_left && x->keyboard == y->keyboard && x->scale == y->scale &&
	       x->font_scale == y->font_scale && x->dark == y->dark && x->reduce_motion == y->reduce_motion &&
	       x->screen_reader == y->screen_reader && x->tv == y->tv && x->watch == y->watch && x->platform == y->platform && x->width_class == y->width_class &&
	       x->height_class == y->height_class && ak_span_eq(x->locale, y->locale) && ak_span_eq(x->time_zone, y->time_zone) &&
	       ak_span_eq(x->languages, y->languages) && x->first_weekday == y->first_weekday && x->hour_cycle == y->hour_cycle &&
	       x->posture == y->posture && x->fold_x == y->fold_x && x->fold_y == y->fold_y && x->fold_width == y->fold_width &&
	       x->fold_height == y->fold_height && x->multiple_windows == y->multiple_windows && memcmp(&a->g, &b->g, sizeof a->g) == 0;
}

TEST(events_bin) {
	ak_event evs[16];
	uint32_t n = golden_events(evs);
	ak_buf out = {0}, e = {0};
	for (uint32_t i = 0; i < n; i++) {
		e.len = 0;
		ak_event_append(&e, &evs[i]);
		ak_buf_u32(&out, (uint32_t)e.len);
		ak_buf_put(&out, e.data, e.len);
	}
	check_golden("events.bin", out.data, out.len);
	ak_buf_free(&out);
	ak_buf_free(&e);
}

// events.bin decodes to the golden events.
TEST(events_decode) {
	ak_event want[16];
	uint32_t n = golden_events(want);
	ak_buf bin = {0};
	REQUIRE(golden("events.bin", &bin));
	ak_reader r = {bin.data, bin.len, 0, false};
	uint32_t i = 0;
	while (r.off < r.len) {
		uint32_t len;
		const uint8_t *p = ak_read_bytes(&r, &len);
		REQUIRE(!r.err && i < n);
		ak_event got;
		int err = ak_event_decode(p, len, &got);
		CHECK_EQ_INT(err, AK_OK);
		if (!event_eq(&got, &want[i])) T_FAIL("event %u (%s) decoded differently", i, ak_event_name(want[i].kind));
		i++;
	}
	CHECK_EQ_INT(i, n);
	// Cut short, with bytes after it, and of an unknown kind, it fails.
	ak_buf e = {0};
	ak_event_append(&e, &want[6]);
	ak_event got;
	CHECK_EQ_INT(ak_event_decode(e.data, e.len - 1, &got), AK_ERR_SHORT);
	ak_buf_u8(&e, 0);
	CHECK_EQ_INT(ak_event_decode(e.data, e.len, &got), AK_ERR_TRAILING);
	e.len = 0;
	ak_event_append(&e, &(ak_event){.id = 1, .kind = 999});
	CHECK_EQ_INT(ak_event_decode(e.data, e.len, &got), AK_ERR_BAD_EVENT);
	ak_buf_free(&e);
	ak_buf_free(&bin);
}

// Actions.

static const ak_action_item sort_by[] = {
	{.title = AK_STR("Name"), .flags = AK_MENU_CHECKED},
	{.title = AK_STR("Date"), .flags = AK_MENU_DISABLED | AK_MENU_CHECKED},
};
static const ak_action_item untitled[] = {{.title = AK_STR("Untitled section")}};
static const ak_action_item nested[] = {{.title = AK_STR("Archive")}};
static const ak_action_item move_to[] = {
	{.title = AK_STR("Inbox")},
	{.title = AK_STR("Nested"), .items = nested, .count = 1},
};
static const ak_action_item golden_actions[] = {
	{.title = AK_STR("Copy"), .icon = AK_STR("doc.on.doc")},
	{.title = AK_STR("Sort by"), .flags = AK_MENU_INLINE, .items = sort_by, .count = 2},
	{.flags = AK_MENU_INLINE, .items = untitled, .count = 1},
	{.title = AK_STR("Move to"), .icon = AK_STR("folder"), .items = move_to, .count = 2},
	{.title = AK_STR("Löschen…"), .role = AK_ALERT_BUTTON_ROLE_DESTRUCTIVE, .flags = AK_MENU_DISABLED},
	{.title = AK_STR("Cancel"), .role = AK_ALERT_BUTTON_ROLE_CANCEL},
};

TEST(actions_bin) {
	ak_buf b = {0};
	ak_append_actions(&b, golden_actions, AK_LEN(golden_actions));
	check_golden("actions.bin", b.data, b.len);
	ak_action_vec items = {0};
	CHECK(ak_decode_actions(b.data, b.len, &items));
	ak_buf dump = {0}, want = {0};
	for (uint32_t i = 0; i < items.len; i++) {
		ak_action *it = &items.data[i];
		CHECK_EQ_INT(it->index, i);
		ak_buf_printf(&dump, "%u\t%u\t%u\t%u\t%.*s\t%.*s\n", it->index, it->depth, it->role, it->flags, (int)it->title.len,
			it->title.p ? it->title.p : "", (int)it->icon.len, it->icon.p ? it->icon.p : "");
	}
	if (golden("actions.txt", &want)) check_bytes("actions.txt", dump.data, dump.len, &want);
	CHECK_EQ_INT(ak_count_actions(golden_actions, AK_LEN(golden_actions)), items.len);
	CHECK_EQ_INT(items.data[1].count, 2);
	CHECK_EQ_INT(items.len, 12);
	CHECK_EQ_INT(items.data[6].count, 2);
	CHECK_EQ_INT(items.data[9].parent, 8);
	CHECK_EQ_INT(items.data[9].depth, 2);
	CHECK_EQ_INT(ak_action_named(items.data, items.len, AK_STR("Archive")), 9);
	CHECK_EQ_INT(ak_action_named(items.data, items.len, AK_STR("Nope")), -1);
	// A cut menu ends where it's cut; nothing is nothing.
	items.len = 0;
	CHECK(!ak_decode_actions(b.data, b.len - 3, &items));
	CHECK_EQ_INT(items.len, 11);
	items.len = 0;
	CHECK(ak_decode_actions(NULL, 0, &items));
	CHECK_EQ_INT(items.len, 0);
	ak_vec_free(&items);
	ak_buf_free(&b);
	ak_buf_free(&dump);
	ak_buf_free(&want);
}

TEST(menu_round_trip) {
	static const ak_menu_item recent[] = {
		{.title = AK_STR("a.txt")},
		{.flags = AK_MENU_SEPARATOR},
		{.title = AK_STR("Clear"), .flags = AK_MENU_DESTRUCTIVE},
	};
	static const ak_menu_item file[] = {
		{.title = AK_STR("New"), .key = AK_STR("n"), .mods = 1},
		{.title = AK_STR("Open Recent"), .items = recent, .count = 3},
		{.flags = AK_MENU_SEPARATOR},
	};
	static const ak_menu_item app[] = {{.role = AK_MENU_ROLE_QUIT}, {.title = AK_STR("Off"), .flags = AK_MENU_DISABLED | AK_MENU_CHECKED}};
	static const ak_menu_bar_menu bar[] = {{AK_STR("File"), file, 3}, {AK_STR("App"), app, 2}};
	ak_buf b = {0};
	ak_append_menu_bar(&b, bar, 2);
	ak_menu_title_vec menus = {0};
	ak_menu_entry_vec items = {0};
	CHECK(ak_decode_menu_bar(b.data, b.len, &menus, &items));
	REQUIRE(menus.len == 2 && items.len == 8);
	CHECK(ak_span_eqc(menus.data[0].title, "File") && menus.data[0].first == 0 && menus.data[0].total == 6);
	CHECK(ak_span_eqc(menus.data[1].title, "App") && menus.data[1].first == 6 && menus.data[1].total == 2);
	for (uint32_t i = 0; i < items.len; i++) CHECK_EQ_INT(items.data[i].index, i);
	CHECK(ak_span_eqc(items.data[0].key, "n") && items.data[0].mods == 1);
	CHECK(items.data[1].flags == AK_MENU_SUBMENU && items.data[1].count == 3);
	CHECK(items.data[3].flags == AK_MENU_SEPARATOR && items.data[3].depth == 1 && items.data[3].parent == 1);
	CHECK(items.data[5].depth == 0 && items.data[5].menu == 0);
	CHECK(items.data[6].role == AK_MENU_ROLE_QUIT && items.data[6].menu == 1);
	CHECK(items.data[7].flags == (AK_MENU_DISABLED | AK_MENU_CHECKED));
	// A cut menu ends where it's cut.
	b.len = 0;
	ak_append_menu(&b, file, 3);
	items.len = 0;
	CHECK(!ak_decode_menu(b.data, b.len - 3, &items));
	CHECK_EQ_INT(items.len, 5);
	CHECK_EQ_INT(items.data[1].count, 3);
	ak_vec_free(&menus);
	ak_vec_free(&items);
	ak_buf_free(&b);
}

// Sidebars.

static const ak_sidebar_item side0[] = {
	{AK_STR("Inbox"), AK_STR("tray"), AK_STR("3")},
	{AK_STR("Drafts"), AK_STR("doc"), {0}},
};
static const ak_sidebar_item side1[] = {
	{AK_STR("Archiv"), {0}, AK_STR("Neu")},
	{AK_STR("Gelöscht"), AK_STR("trash"), AK_STR("1234")},
};
static const ak_sidebar_section golden_sidebar[] = {
	{{0}, side0, 2},
	{AK_STR("Mailboxes"), side1, 2},
	{AK_STR("Empty"), NULL, 0},
};

#define S(x) (int)(x).len, (x).p ? (x).p : ""

TEST(sidebar_bin) {
	ak_buf b = {0};
	ak_append_sidebar(&b, golden_sidebar, 3);
	check_golden("sidebar.bin", b.data, b.len);
	ak_sidebar_heading_vec secs = {0};
	ak_sidebar_item_vec items = {0};
	CHECK(ak_decode_sidebar(b.data, b.len, &secs, &items));
	ak_buf dump = {0}, want = {0};
	for (uint32_t i = 0; i < secs.len; i++) {
		ak_buf_printf(&dump, "#\t%.*s\n", S(secs.data[i].title));
		for (uint32_t j = 0; j < secs.data[i].count; j++) {
			ak_sidebar_item *it = &items.data[secs.data[i].first + j];
			ak_buf_printf(&dump, "%u\t%.*s\t%.*s\t%.*s\n", i, S(it->title), S(it->icon), S(it->badge));
		}
	}
	if (golden("sidebar.txt", &want)) check_bytes("sidebar.txt", dump.data, dump.len, &want);
	// Cut short, it ends where it's cut, with whole items.
	ak_sidebar_heading_vec cut = {0};
	ak_sidebar_item_vec cut_items = {0};
	ak_decode_sidebar(b.data, b.len - 18, &cut, &cut_items);
	CHECK(cut.len == 2 && cut.data[1].count == 1 && ak_span_eqc(cut_items.data[cut.data[1].first].badge, "Neu"));
	CHECK(!ak_same_sidebar_shape(secs.data, secs.len, cut.data, cut.len));
	CHECK(ak_same_sidebar_shape(secs.data, secs.len, secs.data, secs.len));
	static const struct {
		const char *s;
		int32_t n;
	} nums[] = {{"0", 0}, {"3", 3}, {"1234", 1234}, {"999999999", 999999999}};
	for (size_t i = 0; i < AK_LEN(nums); i++) {
		int32_t n;
		CHECK(ak_badge_number(ak_cstr(nums[i].s), &n) && n == nums[i].n);
	}
	static const char *const words[] = {"", "New", "99+", "-1", "1.5", "٣", "1234567890"};
	for (size_t i = 0; i < AK_LEN(words); i++) {
		int32_t n;
		CHECK(!ak_badge_number(ak_cstr(words[i]), &n));
	}
	ak_vec_free(&secs);
	ak_vec_free(&items);
	ak_vec_free(&cut);
	ak_vec_free(&cut_items);
	ak_buf_free(&b);
	ak_buf_free(&dump);
	ak_buf_free(&want);
}

// Alerts.

static const ak_alert_field golden_fields[] = {
	{.placeholder = AK_STR("Name"), .text = AK_STR("Untitled"), .content_type = AK_STR("name")},
	{.placeholder = AK_STR("Passwort"), .flags = AK_ALERT_FIELD_SECURE, .content_type = AK_STR("password")},
	{.placeholder = AK_STR("Email"), .text = AK_STR("a@b.c"), .keyboard = AK_KEYBOARD_EMAIL},
	{.flags = 0},
};

TEST(alert_fields_bin) {
	ak_buf b = {0};
	ak_append_alert_fields(&b, golden_fields, 4);
	check_golden("alertfields.bin", b.data, b.len);
	ak_alert_field_vec fs = {0};
	CHECK(ak_decode_alert_fields(b.data, b.len, &fs));
	ak_buf dump = {0}, want = {0};
	for (uint32_t i = 0; i < fs.len; i++) {
		ak_alert_field *f = &fs.data[i];
		ak_buf_printf(&dump, "%.*s\t%.*s\t%u\t%u\t%.*s\n", S(f->placeholder), S(f->text), f->flags, f->keyboard, S(f->content_type));
	}
	if (golden("alertfields.txt", &want)) check_bytes("alertfields.txt", dump.data, dump.len, &want);
	fs.len = 0;
	ak_decode_alert_fields(b.data, b.len - 3, &fs);
	CHECK_EQ_INT(fs.len, 3);
	fs.len = 0;
	ak_decode_alert_fields(NULL, 0, &fs);
	CHECK_EQ_INT(fs.len, 0);

	b.len = 0;
	ak_append_field_changed(&b, 2, AK_STR("Grüße"));
	check_golden("fieldchanged.bin", b.data, b.len);
	ak_buf fc = {0};
	REQUIRE(golden("fieldchanged.bin", &fc));
	uint32_t i;
	ak_span s;
	CHECK(ak_decode_field_changed(fc.data, fc.len, &i, &s) && i == 2 && ak_span_eqc(s, "Grüße"));
	CHECK(!ak_decode_field_changed((const uint8_t[]){2, 0, 0, 0, 9, 0, 0, 0, 'x'}, 9, &i, &s));

	static const ak_action_item buttons[] = {
		{.role = AK_ALERT_BUTTON_ROLE_CANCEL}, {.role = AK_ALERT_BUTTON_ROLE_DESTRUCTIVE}, {0}, {0}};
	b.len = 0;
	ak_append_actions(&b, buttons, 4);
	ak_action_vec items = {0};
	ak_decode_actions(b.data, b.len, &items);
	CHECK_EQ_INT(ak_default_alert_action(items.data, items.len), 2);
	CHECK_EQ_INT(ak_default_alert_action(items.data, 2), -1);
	ak_vec_free(&items);
	ak_vec_free(&fs);
	ak_buf_free(&fc);
	ak_buf_free(&b);
	ak_buf_free(&dump);
	ak_buf_free(&want);
}

TEST(color) {
	ak_color c = ak_rgba(1, 2, 3, 4);
	uint8_t r, g, b, a;
	CHECK(ak_color_channels(c, &r, &g, &b, &a) && r == 1 && g == 2 && b == 3 && a == 4);
	ak_color s = ak_semantic_color(AK_SEMANTIC_COLOR_ACCENT);
	uint16_t v;
	CHECK(ak_color_semantic(s, &v) && v == AK_SEMANTIC_COLOR_ACCENT);
	CHECK(!ak_color_semantic(0, &v));
	ak_color h = ak_color_opacity(ak_color_opacity(s, 0.5f), 0.5f);
	CHECK(ak_color_semantic(h, &v) && v == AK_SEMANTIC_COLOR_ACCENT);
	CHECK_EQ_INT(ak_color_alpha(h), 64);
	CHECK(ak_color_opacity(s, 1) == s && ak_color_alpha(s) == 255);
	CHECK(ak_color_channels(ak_color_opacity(c, 0.5f), &r, &g, &b, &a) && a == 2);
	CHECK(ak_color_opacity(0, 0.5f) == 0);
}

TEST(schema_names) {
	const ak_prop_info *p = ak_prop_of(AK_KIND_BUTTON, AK_PROP_BUTTON_TITLE);
	CHECK(p && strcmp(p->name, "Title") == 0);
	CHECK_EQ_STR(ak_prop_of(0, AK_PROP_BACKGROUND)->name, "Background");
	p = ak_prop_of(AK_KIND_TEXT_FIELD, AK_PROP_TEXT_FIELD_PLACEHOLDER);
	CHECK(p && p->type == AK_T_STRING);
	CHECK(ak_prop_of(AK_KIND_VIEW, 200) == NULL);
	CHECK(ak_prop_of(AK_KIND_COUNT, AK_FIRST_KIND_PROP) == NULL);
	CHECK_EQ_STR(ak_kind_name(AK_KIND_LIST), "List");
	CHECK_EQ_STR(ak_event_name(AK_EVENT_TOGGLED), "Toggled");
	CHECK_EQ_STR(ak_cmd_name(AK_CMD_LIST_UPDATE), "ListUpdate");
	CHECK(ak_kind_name(0) == NULL && ak_kind_name(AK_KIND_COUNT) == NULL);
	CHECK_EQ_INT(ak_event_payload(AK_EVENT_ENVIRONMENT), AK_P_ENVIRONMENT);
	CHECK_EQ_INT(ak_event_payload(999), 255);
	p = ak_prop_of(0, AK_PROP_TEXT_STYLE);
	CHECK(p && p->type == AK_T_ENUM && p->enum_id == AK_ENUM_TEXT_STYLE);
	CHECK_EQ_STR(ak_enum_value_name(p->enum_id, AK_TEXT_STYLE_HEADLINE), "Headline");
	CHECK(ak_enum_value_name(AK_ENUM_TEXT_STYLE, AK_TEXT_STYLE_COUNT) == NULL);
	CHECK_EQ_STR(ak_op_name(AK_OP_SET_PROP), "SetProp");
}

TEST(size_classes) {
	static const struct {
		float dp;
		uint8_t w, h;
	} cases[] = {
		{0, AK_SIZE_CLASS_COMPACT, AK_SIZE_CLASS_COMPACT},
		{479, AK_SIZE_CLASS_COMPACT, AK_SIZE_CLASS_COMPACT},
		{480, AK_SIZE_CLASS_COMPACT, AK_SIZE_CLASS_MEDIUM},
		{599, AK_SIZE_CLASS_COMPACT, AK_SIZE_CLASS_MEDIUM},
		{600, AK_SIZE_CLASS_MEDIUM, AK_SIZE_CLASS_MEDIUM},
		{839, AK_SIZE_CLASS_MEDIUM, AK_SIZE_CLASS_MEDIUM},
		{840, AK_SIZE_CLASS_EXPANDED, AK_SIZE_CLASS_MEDIUM},
		{900, AK_SIZE_CLASS_EXPANDED, AK_SIZE_CLASS_EXPANDED},
	};
	for (size_t i = 0; i < AK_LEN(cases); i++) {
		CHECK_EQ_INT(ak_width_class_of(cases[i].dp), cases[i].w);
		CHECK_EQ_INT(ak_height_class_of(cases[i].dp), cases[i].h);
	}
	CHECK_EQ_STR(ak_platform_name(AK_PLATFORM_IOS), "iOS");
	CHECK_EQ_STR(ak_size_class_name(AK_SIZE_CLASS_EXPANDED), "expanded");
}

TEST(location) {
	ak_location l = {AK_LOCATION_ACTION_BACK, -2, AK_STR("/chat/7?tab=info"), AK_STR("{\"e\":[]}"), AK_STR("main")};
	ak_buf b = {0};
	ak_append_location(&b, &l);
	ak_location got;
	CHECK(ak_decode_location(b.data, b.len, &got));
	CHECK(got.action == l.action && got.delta == l.delta && ak_span_eq(got.href, l.href) && ak_span_eq(got.state, l.state) &&
	      ak_span_eq(got.window, l.window));
	CHECK(!ak_decode_location(b.data, 7, &got));
	b.len = 0;
	ak_append_location_event(&b, AK_STR("/a"), AK_STR("s"));
	ak_span h, s;
	CHECK(ak_decode_location_event(b.data, b.len, &h, &s) && ak_span_eqc(h, "/a") && ak_span_eqc(s, "s"));
	ak_buf_free(&b);
}

// The error overlay's DevError command, as ops/ops_test.go's
// TestDevErrorRoundTrip has its bytes.
TEST(dev_error) {
	ak_dev_error e = {AK_DEV_ERROR_RELOAD, AK_STR("T"), AK_STR("m"), AK_STR(""), AK_STR("d")};
	ak_buf b = {0};
	ak_append_dev_error(&b, &e);
	static const uint8_t want[] = {1, 1, 0, 0, 0, 'T', 1, 0, 0, 0, 'm', 0, 0, 0, 0, 1, 0, 0, 0, 'd'};
	CHECK(b.len == sizeof want && memcmp(b.data, want, sizeof want) == 0);
	ak_dev_error got;
	CHECK(ak_decode_dev_error(b.data, b.len, &got));
	CHECK(got.flags == e.flags && ak_span_eq(got.title, e.title) && ak_span_eq(got.message, e.message) && got.source.len == 0 &&
	      ak_span_eq(got.details, e.details));
	CHECK(!ak_decode_dev_error(b.data, b.len - 1, &got));
	ak_buf_free(&b);
}

TEST(keys) {
	static const ak_key keys[] = {{AK_STR("s"), 1}, {AK_STR("enter"), 0}, {AK_STR("ä"), 2}, {AK_STR("ж"), 0}};
	ak_buf b = {0};
	ak_append_keys(&b, keys, 4);
	ak_key_vec got = {0};
	CHECK(ak_decode_keys(b.data, b.len, &got));
	REQUIRE(got.len == 4);
	CHECK(ak_span_eqc(got.data[2].name, "ä") && got.data[2].mods == 2);
	CHECK_EQ_INT(ak_match_key(got.data, got.len, AK_STR("S"), 1), 0);
	CHECK_EQ_INT(ak_match_key(got.data, got.len, AK_STR("s"), 0), -1);
	CHECK_EQ_INT(ak_match_key(got.data, got.len, AK_STR("enter"), 0), 1);
	CHECK_EQ_INT(ak_match_key(got.data, got.len, AK_STR("Ä"), 2), 2);
	CHECK_EQ_INT(ak_match_key(got.data, got.len, AK_STR("Ж"), 0), 3);
	CHECK_EQ_INT(ak_match_key(got.data, got.len, AK_STR("Enter"), 0), -1);
	got.len = 0;
	CHECK(!ak_decode_keys((const uint8_t[]){0xff, 0, 0, 0}, 4, &got));
	CHECK_EQ_INT(got.len, 0);
	ak_vec_free(&got);
	ak_buf_free(&b);
}

static bool item_eq(const ak_item *a, const ak_item *b) {
	return a->kind == b->kind && ak_span_eq(a->text, b->text) && ak_span_eq(a->path, b->path) && ak_span_eq(a->name, b->name) &&
	       ak_span_eq(a->mime, b->mime) && a->size == b->size && a->value == b->value;
}

TEST(items) {
	static const ak_item items[] = {
		{.kind = AK_SHARE_KIND_IMAGE, .path = AK_STR("/a/b.png"), .name = AK_STR("b.png"), .mime = AK_STR("image/png"), .size = 12},
		{.kind = AK_SHARE_KIND_TEXT, .text = AK_STR("hi"), .mime = AK_STR("text/plain"), .size = -1},
	};
	ak_buf b = {0};
	ak_append_items(&b, items, 2);
	ak_item_vec got = {0};
	CHECK(ak_decode_items(b.data, b.len, &got));
	CHECK(got.len == 2 && item_eq(&got.data[0], &items[0]) && item_eq(&got.data[1], &items[1]));

	static const ak_item drag[] = {
		{.kind = AK_SHARE_KIND_LINK, .text = AK_STR("https://asuka.dev"), .mime = AK_STR("text/uri-list"), .size = -1, .value = true},
		{.kind = AK_SHARE_KIND_FILE, .path = AK_STR("/x.pdf"), .name = AK_STR("x.pdf"), .mime = AK_STR("application/pdf"), .size = -1},
	};
	b.len = got.len = 0;
	ak_append_drag_items(&b, drag, 2);
	CHECK(ak_decode_drag_items(b.data, b.len, &got));
	CHECK(got.len == 2 && item_eq(&got.data[0], &drag[0]) && item_eq(&got.data[1], &drag[1]));

	ak_drop d = {1.5f, 2, 7, true}, gd;
	b.len = got.len = 0;
	ak_append_drop(&b, &d, items, 2);
	CHECK(ak_decode_drop(b.data, b.len, &gd, &got));
	CHECK(gd.x == 1.5f && gd.y == 2 && gd.source == 7 && gd.copied && got.len == 2 && item_eq(&got.data[1], &items[1]));
	CHECK(!ak_decode_drop((const uint8_t[]){1, 2, 3}, 3, &gd, &got));

	static const ak_span strs[] = {AK_STR("image/*"), {0}, AK_STR("text/plain")};
	ak_str_vec gs = {0};
	b.len = 0;
	ak_append_strings(&b, strs, 3);
	CHECK(ak_decode_strings(b.data, b.len, &gs));
	CHECK(gs.len == 3 && ak_span_eq(gs.data[0], strs[0]) && gs.data[1].len == 0 && ak_span_eq(gs.data[2], strs[2]));

	// A cut list ends where it's cut; a corrupt length reads nothing.
	static const ak_item two[] = {{.text = AK_STR("one"), .size = -1}, {.text = AK_STR("two"), .size = -1}};
	b.len = got.len = 0;
	ak_append_items(&b, two, 2);
	ak_decode_items(b.data, b.len - 3, &got);
	CHECK(got.len == 1 && ak_span_eqc(got.data[0].text, "one"));
	got.len = 0;
	CHECK(!ak_decode_items((const uint8_t[]){1, 0, 0, 0, 0, 0xff, 0xff, 0xff, 0x7f}, 9, &got));
	CHECK_EQ_INT(got.len, 0);
	ak_vec_free(&got);
	ak_vec_free(&gs);
	ak_buf_free(&b);
}

TEST(match_mime) {
	static const struct {
		const char *patterns[2];
		const char *mime;
		bool want;
	} cases[] = {
		{{"*/*"}, "application/x-card", true},
		{{"*"}, "image/png", true},
		{{"image/*"}, "image/png", true},
		{{"image/*"}, "IMAGE/HEIC", true},
		{{"image/*"}, "imagery/png", false},
		{{"image/*"}, "image", false},
		{{"text/plain"}, "text/plain; charset=utf-8", true},
		{{"Text/Plain"}, "text/plain", true},
		{{"text/plain"}, "text/uri-list", false},
		{{"video/*", "text/uri-list"}, "text/uri-list", true},
		{{NULL}, "text/plain", false},
	};
	for (size_t i = 0; i < AK_LEN(cases); i++) {
		ak_span ps[2];
		uint32_t n = 0;
		for (int j = 0; j < 2 && cases[i].patterns[j]; j++) ps[n++] = ak_cstr(cases[i].patterns[j]);
		if (ak_match_mime(ps, n, ak_cstr(cases[i].mime)) != cases[i].want) T_FAIL("case %zu: %s", i, cases[i].mime);
	}
}

TEST(rich) {
	ak_rich_style plain = {0}, bold = {.weight = AK_FONT_WEIGHT_BOLD, .link = AK_STR("https://a.b")};
	static ak_rich_run runs[4];
	runs[0] = (ak_rich_run){plain, AK_STR("Hello ")};
	runs[1] = (ak_rich_run){plain, AK_STR("world\nsecond ")};
	runs[2] = (ak_rich_run){bold, AK_STR("line")};
	runs[3] = (ak_rich_run){plain, AK_STR("\n")};
	ak_buf b = {0};
	ak_append_rich(&b, runs, 4);
	// Two styles; three paragraphs: one merged run, two runs, none.
	ak_reader r = {b.data, b.len, 0, false};
	CHECK_EQ_INT(ak_read_u32(&r), 2);
	ak_rich_run_vec got = {0};
	CHECK(ak_decode_rich(b.data, b.len, &got));
	REQUIRE(got.len == 5);
	CHECK(ak_span_eqc(got.data[0].text, "Hello world"));
	CHECK(ak_span_eqc(got.data[1].text, "\n"));
	CHECK(ak_span_eqc(got.data[2].text, "second ") && ak_rich_style_eq(&got.data[2].style, &plain));
	CHECK(ak_span_eqc(got.data[3].text, "line") && ak_rich_style_eq(&got.data[3].style, &bold));
	CHECK(ak_span_eqc(got.data[4].text, "\n") && ak_rich_style_eq(&got.data[4].style, &bold));
	// Encoding what it decoded gives the same bytes: canonical.
	ak_buf again = {0};
	ak_append_rich(&again, got.data, got.len);
	CHECK(again.len == b.len && memcmp(again.data, b.data, b.len) == 0);
	// A style cut short is nothing.
	got.len = 0;
	CHECK(!ak_decode_rich(b.data, 20, &got));
	CHECK_EQ_INT(got.len, 0);
	ak_vec_free(&got);
	ak_buf_free(&b);
	ak_buf_free(&again);
}

TEST(window_config) {
	ak_window_config c = {.width = 800, .height = 600, .min_width = 320, .max_height = 900, .x = 10, .y = -20,
		.flags = AK_WINDOW_FIXED_SIZE | AK_WINDOW_MODAL, .title_bar = AK_TITLE_BAR_HIDDEN, .background = ak_rgba(1, 2, 3, 255),
		.parent = 7, .state_key = AK_STR("editor"), .title = AK_STR("Notes — 2")};
	ak_buf b = {0};
	ak_append_window_config(&b, &c);
	ak_window_config g = ak_decode_window_config(b.data, b.len);
	CHECK(g.width == 800 && g.height == 600 && g.min_width == 320 && g.min_height == 0 && g.max_width == 0 &&
	      g.max_height == 900 && g.x == 10 && g.y == -20 && g.flags == c.flags && g.title_bar == c.title_bar &&
	      g.background == c.background && g.parent == 7 && ak_span_eq(g.state_key, c.state_key) && ak_span_eq(g.title, c.title));
	for (size_t n = 0; n < b.len; n++) ak_decode_window_config(b.data, n);  // cut short: no crash
	ak_buf old = {0};
	ak_buf_f32(&old, 640);
	ak_buf_f32(&old, 480);
	ak_buf_f32(&old, 200);
	ak_buf_f32(&old, 100);
	ak_buf_put(&old, "Old", 3);
	g = ak_decode_window_config(old.data, old.len);
	CHECK(g.width == 640 && g.height == 480 && g.min_width == 200 && g.min_height == 100 && ak_span_eqc(g.title, "Old") && g.flags == 0);
	ak_buf_free(&b);
	ak_buf_free(&old);
}

TEST(window_state_action_flyout) {
	ak_window_state s = {1, 2, 300, 200, AK_WINDOW_STATE_VISIBLE | AK_WINDOW_STATE_FOCUSED};
	ak_buf b = {0};
	ak_append_window_state(&b, &s);
	ak_window_state gs = ak_decode_window_state(b.data, b.len);
	CHECK(memcmp(&gs, &s, sizeof s) == 0 || (gs.x == 1 && gs.y == 2 && gs.width == 300 && gs.height == 200 && gs.flags == s.flags));
	uint8_t a;
	float x, y;
	b.len = 0;
	ak_append_window_action(&b, AK_WINDOW_ACTION_SIZE, 640, 480);
	CHECK(ak_decode_window_action(b.data, b.len, &a, &x, &y) && a == AK_WINDOW_ACTION_SIZE && x == 640 && y == 480);
	b.len = 0;
	ak_append_window_action(&b, AK_WINDOW_ACTION_FOCUS, 0, 0);
	CHECK(b.len == 1 && ak_decode_window_action(b.data, b.len, &a, &x, &y) && a == AK_WINDOW_ACTION_FOCUS);
	CHECK(!ak_decode_window_action((const uint8_t[]){AK_WINDOW_ACTION_POSITION, 1}, 2, &a, &x, &y));
	b.len = 0;
	ak_append_window_action(&b, AK_WINDOW_ACTION_FIT, 280, 120);
	CHECK(ak_decode_window_action(b.data, b.len, &a, &x, &y) && a == AK_WINDOW_ACTION_FIT && x == 280 && y == 120);

	ak_flyout f = {3, 10, 20, 30, 40, AK_PLACEMENT_TOP_END, 4}, gf;
	b.len = 0;
	ak_append_flyout(&b, &f);
	CHECK(ak_decode_flyout(b.data, b.len, &gf) && gf.tray == 3 && gf.x == 10 && gf.height == 40 && gf.placement == AK_PLACEMENT_TOP_END && gf.gap == 4);
	for (size_t n = 0; n < b.len; n++)
		if (ak_decode_flyout(b.data, n, &gf)) T_FAIL("a flyout cut to %zu bytes decoded", n);
	ak_buf_free(&b);
}

// Every decoder of bytes from a host (a batch, an event, the payloads)
// on bytes cut short, mutated and random: they read only what's there
// (each gets a copy exactly its size, so ASan sees a byte past it) and
// say what they couldn't read, never more.
static uint64_t fuzz_state = 88172645463325252u;
static uint32_t fuzz_rand(void) {
	fuzz_state ^= fuzz_state << 13;
	fuzz_state ^= fuzz_state >> 7;
	fuzz_state ^= fuzz_state << 17;
	return (uint32_t)fuzz_state;
}

static void fuzz_decode(const uint8_t *b, size_t n) {
	ak_buf out = {0};
	ak_batch_reader br;
	ak_op op;
	ak_batch_reader_init(&br, b, n);
	while (ak_batch_next(&br, &op)) {
		ak_op_format(&out, &op);
		ak_op_dump(&out, &op);
		ak_op_format_kind(&out, &op, (ak_view_kind)(fuzz_rand() % 64));
	}
	ak_event e;
	ak_event_decode(b, n, &e);
	ak_action_vec av = {0};
	ak_decode_actions(b, n, &av);
	ak_vec_free(&av);
	ak_menu_entry_vec mv = {0};
	ak_decode_menu(b, n, &mv);
	ak_vec_free(&mv);
	ak_menu_title_vec mt = {0};
	ak_decode_menu_bar(b, n, &mt, &mv);
	ak_vec_free(&mt);
	ak_vec_free(&mv);
	ak_alert_field_vec af = {0};
	ak_decode_alert_fields(b, n, &af);
	ak_vec_free(&af);
	uint32_t index;
	ak_span sp, sp2;
	ak_decode_field_changed(b, n, &index, &sp);
	ak_key_vec kv = {0};
	ak_decode_keys(b, n, &kv);
	for (uint32_t i = 0; i < kv.len; i++) {
		char t[4];
		ak_key_name(kv.data[i].name, t);
	}
	ak_vec_free(&kv);
	ak_item_vec iv = {0};
	ak_decode_items(b, n, &iv);
	for (uint32_t i = 0; i < iv.len; i++) ak_match_mime(&iv.data[i].text, 1, iv.data[i].mime);
	ak_vec_free(&iv);
	ak_decode_drag_items(b, n, &iv);
	ak_vec_free(&iv);
	ak_drop d;
	ak_decode_drop(b, n, &d, &iv);
	ak_vec_free(&iv);
	ak_str_vec sv = {0};
	ak_decode_strings(b, n, &sv);
	for (uint32_t i = 0; i < sv.len; i++) ak_fmt_quote(&out, sv.data[i]);
	ak_vec_free(&sv);
	ak_rich_run_vec rv = {0};
	ak_decode_rich(b, n, &rv);
	ak_vec_free(&rv);
	ak_sidebar_heading_vec sh = {0};
	ak_sidebar_item_vec si = {0};
	ak_decode_sidebar(b, n, &sh, &si);
	ak_vec_free(&sh);
	ak_vec_free(&si);
	ak_window_config wc = ak_decode_window_config(b, n);
	ak_fmt_quote(&out, wc.title);
	ak_fmt_quote(&out, wc.state_key);
	ak_decode_window_state(b, n);
	uint8_t a;
	float x, y;
	ak_decode_window_action(b, n, &a, &x, &y);
	ak_flyout f;
	ak_decode_flyout(b, n, &f);
	ak_location l;
	ak_decode_location(b, n, &l);
	ak_decode_location_event(b, n, &sp, &sp2);
	ak_buf_free(&out);
}

// fuzz_one decodes a copy of b exactly n bytes long.
static void fuzz_one(const uint8_t *b, size_t n) {
	uint8_t *c = malloc(n ? n : 1);
	if (n) memcpy(c, b, n);
	fuzz_decode(c, n);
	free(c);
}

TEST(fuzz_decoders) {
	// Seeds: what the encoders write (the golden files), cut short at
	// every length, then mutated.
	static const char *const seeds[] = {"golden.bin", "events.bin", "actions.bin", "alertfields.bin", "fieldchanged.bin", "sidebar.bin"};
	for (size_t s = 0; s < AK_LEN(seeds); s++) {
		ak_buf b = {0};
		if (!golden(seeds[s], &b)) continue;
		for (size_t n = 0; n <= b.len; n++) fuzz_one(b.data, n);
		for (int it = 0; it < 3000; it++) {
			uint8_t *m = malloc(b.len ? b.len : 1);
			memcpy(m, b.data, b.len);
			for (uint32_t k = 1 + fuzz_rand() % 4; b.len && k > 0; k--) {
				size_t at = fuzz_rand() % b.len;
				switch (fuzz_rand() % 3) {
				case 0: m[at] = (uint8_t)fuzz_rand(); break;
				case 1: m[at] ^= (uint8_t)(1u << (fuzz_rand() % 8)); break;
				default: m[at] = (uint8_t)(fuzz_rand() % 3 == 0 ? 0xff : fuzz_rand() % 4); break;
				}
			}
			fuzz_one(m, b.len ? fuzz_rand() % (b.len + 1) : 0);
			free(m);
		}
		ak_buf_free(&b);
	}
	// Random bytes, small numbers more often, so lengths and counts often
	// fit; a batch's header often a plausible count and op.
	uint8_t r[96];
	for (int it = 0; it < 30000; it++) {
		size_t n = fuzz_rand() % sizeof r;
		for (size_t i = 0; i < n; i++) {
			uint32_t k = fuzz_rand() % 8;
			r[i] = k < 4 ? (uint8_t)(fuzz_rand() % 4) : k < 6 ? (uint8_t)(fuzz_rand() % 16) : (uint8_t)fuzz_rand();
		}
		if (n >= 5 && (fuzz_rand() & 1)) {
			r[0] = (uint8_t)(fuzz_rand() % 4);
			r[1] = r[2] = r[3] = 0;
			r[4] = (uint8_t)(1 + fuzz_rand() % 10);
		}
		fuzz_one(r, n);
	}
}

T_MAIN()
