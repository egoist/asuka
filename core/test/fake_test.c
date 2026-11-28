// The C fake host (core/host/fake): it applies encoded batches as a real
// host does, keeps the view tree, measures text at 8 points a character, and
// sends the events a user's taps and typing make.
#include "t.h"
#include "fake/fake.h"

#include <stdio.h>
#include <stdlib.h>

// A pretend core: it keeps the events, list calls and run_posted calls
// the fake host sends it.
typedef struct {
	ak_buf events;  // u32 length, then the event, each
	uint32_t nevents;
	int run_posted;
	int64_t count;
	ak_id bound_list, bound_cell;
	int64_t bound_index;
	ak_id unbound_cell;
} recorder;

static void rec_event(void *ctx, const uint8_t *buf, size_t len) {
	recorder *r = ctx;
	ak_buf_u32(&r->events, (uint32_t)len);
	ak_buf_put(&r->events, buf, len);
	r->nevents++;
}
static int64_t rec_count(void *ctx, ak_id list) {
	AK_UNUSED(list);
	return ((recorder *)ctx)->count;
}
static void rec_bind(void *ctx, ak_id list, int64_t index, ak_id cell) {
	recorder *r = ctx;
	r->bound_list = list, r->bound_index = index, r->bound_cell = cell;
}
static void rec_unbind(void *ctx, ak_id list, ak_id cell) {
	AK_UNUSED(list);
	((recorder *)ctx)->unbound_cell = cell;
}
static void rec_run_posted(void *ctx) { ((recorder *)ctx)->run_posted++; }

// event decodes the recorder's event i.
static ak_event event(recorder *r, uint32_t i) {
	ak_event e = {0};
	size_t off = 0;
	for (uint32_t j = 0; j <= i && off < r->events.len; j++) {
		uint32_t n;
		memcpy(&n, r->events.data + off, 4);
		if (j == i) {
			int err = ak_event_decode(r->events.data + off + 4, n, &e);
			if (err) T_FAIL("event %u: %s", i, ak_err_string(err));
			return e;
		}
		off += 4 + n;
	}
	T_FAIL("no event %u", i);
	return e;
}

// Misuses, kept instead of panicking: a line each.
static void on_fail(void *ctx, const char *msg) {
	ak_buf *b = ctx;
	ak_buf_put(b, msg, strlen(msg));
	ak_buf_u8(b, '\n');
}

static const char *cstr(ak_buf *b) {
	ak_buf_u8(b, 0);
	b->len--;
	return (const char *)b->data;
}

typedef struct {
	ak_fake *f;
	recorder rec;
	ak_buf errors;
	ak_batch b;
} env;

static void setup(env *e) {
	memset(e, 0, sizeof *e);
	e->f = ak_fake_new();
	e->f->core = (ak_fake_core){&e->rec, rec_event, rec_count, rec_bind, rec_unbind, rec_run_posted};
	e->f->fail_fn = on_fail;
	e->f->fail_ctx = &e->errors;
}

static void teardown(env *e) {
	ak_fake_free(e->f);
	ak_buf_free(&e->rec.events);
	ak_buf_free(&e->errors);
	ak_batch_free(&e->b);
}

// flush applies the batch, encoded, as the core sends it.
static void flush(env *e) {
	size_t n;
	const uint8_t *p = ak_batch_encode(&e->b, &n);
	ak_fake_apply(e->f, p, n);
	ak_batch_reset(&e->b);
}

static void set_str(ak_batch *b, ak_id id, ak_prop p, const char *s) { ak_batch_set_string(b, id, p, s, strlen(s)); }

static bool golden(const char *name, ak_buf *b) {
	const char *root = getenv("ASUKA_ROOT");
	char path[1024];
	snprintf(path, sizeof path, "%s/ops/testdata/%s", root ? root : "../..", name);
	FILE *fp = fopen(path, "rb");
	if (!fp) {
		T_FAIL("can't open %s", path);
		return false;
	}
	char tmp[4096];
	size_t n;
	while ((n = fread(tmp, 1, sizeof tmp, fp)) > 0) ak_buf_put(b, tmp, n);
	fclose(fp);
	return true;
}

// The golden batch applies as Go's fake applies it: the same ops
// recorded, its two ops no host could apply reported, the tree left.
TEST(golden_batch) {
	env e;
	setup(&e);
	ak_buf bin = {0}, txt = {0}, out = {0};
	REQUIRE(golden("golden.bin", &bin));
	REQUIRE(golden("golden.txt", &txt));
	ak_fake_apply(e.f, bin.data, bin.len);
	CHECK_EQ_INT(e.f->applies, 1);
	CHECK_EQ_INT(ak_fake_op_count(e.f), 22);
	CHECK_EQ_INT(ak_fake_count(e.f, AK_OP_SET_PROP), 12);
	ak_fake_dump(e.f, &out);
	ak_buf_u8(&txt, 0);
	CHECK_EQ_STR(cstr(&out), (const char *)txt.data);
	CHECK_EQ_STR(cstr(&e.errors),
		"SetProp: View has no edges prop 3\n"  // BorderWidth is a float
		"SetProp: View has no bytes prop 8\n"  // A11yLabel a string
		"ListUpdate on 1: args cut short\n");
	ak_buf_reset(&out);
	ak_fake_log(e.f, &out);
	CHECK_EQ_STR(cstr(&out),
		"Create 1 View\n"
		"Create 2 Text\n"
		"SetProp 2 #64=\"héllo, 世界\"\n"  // 2 is gone: no kind to name it by
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
	ak_fake_view *root = ak_fake_root(e.f);
	REQUIRE(root);
	CHECK_EQ_INT(root->id, 1);
	CHECK(ak_fake_view_of(e.f, 2) == NULL);
	CHECK_EQ_INT(e.f->nviews, 1);
	CHECK(!ak_fake_listens(root, AK_EVENT_PRESS));
	CHECK_EQ_INT(ak_fake_int(root, AK_PROP_BACKGROUND), (int64_t)ak_rgba(0x11, 0x22, 0x33, 0xff));
	CHECK(ak_fake_prop_of(root, AK_PROP_BORDER_WIDTH) == NULL);
	ak_buf_reset(&out);
	ak_fake_tree(root, &out);
	CHECK_EQ_STR(cstr(&out), "View 0,0 0x0\n");
	// The recorded ops' bytes outlive the batch.
	memset(bin.data, 0, bin.len);
	ak_op op = ak_fake_op_at(e.f, 2);
	CHECK(ak_span_eqc(op.value.s, "héllo, 世界"));
	ak_fake_reset(e.f);
	CHECK_EQ_INT(ak_fake_op_count(e.f), 0);
	CHECK_EQ_INT(e.f->applies, 0);
	ak_buf_free(&bin);
	ak_buf_free(&txt);
	ak_buf_free(&out);
	teardown(&e);
}

// mount makes a screen: a stack's screen with a text, a button, a text
// field and a toggle.
static void mount(env *e) {
	ak_batch *b = &e->b;
	ak_batch_create(b, 1, AK_KIND_VIEW);
	ak_batch_create(b, 2, AK_KIND_STACK);
	ak_batch_create(b, 3, AK_KIND_SCREEN);
	ak_batch_create(b, 4, AK_KIND_TEXT);
	set_str(b, 4, AK_PROP_TEXT_TEXT, "Hello");
	ak_batch_create(b, 5, AK_KIND_BUTTON);
	set_str(b, 5, AK_PROP_BUTTON_TITLE, "Add");
	ak_batch_create(b, 6, AK_KIND_TEXT_FIELD);
	ak_batch_create(b, 7, AK_KIND_TOGGLE);
	ak_batch_insert(b, 1, 2, 0);
	ak_batch_insert(b, 2, 3, 0);
	ak_batch_insert(b, 3, 4, 0);
	ak_batch_insert(b, 3, 5, 1);
	ak_batch_insert(b, 3, 6, 2);
	ak_batch_insert(b, 3, 7, 3);
	ak_batch_set_root(b, 1);
	ak_batch_listen(b, 5, AK_EVENT_PRESS);
	ak_batch_listen(b, 6, AK_EVENT_TEXT_CHANGED);
	ak_batch_listen(b, 7, AK_EVENT_TOGGLED);
	ak_batch_listen(b, 2, AK_EVENT_POPPED);
	ak_batch_set_frame(b, 1, 0, 0, 390, 844);
	ak_batch_set_frame(b, 2, 0, 0, 390, 844);
	ak_batch_set_frame(b, 3, 0, 0, 390, 844);
	ak_batch_set_frame(b, 4, 16, 100.5f, 40, 20);
	ak_batch_set_frame(b, 5, 16, 120.5f, 48, 44);
	flush(e);
}

TEST(tree) {
	env e;
	setup(&e);
	mount(&e);
	CHECK_EQ_STR(cstr(&e.errors), "");
	ak_buf out = {0};
	ak_fake_view *root = ak_fake_root(e.f);
	REQUIRE(root);
	ak_fake_tree(root, &out);
	CHECK_EQ_STR(cstr(&out),
		"View 0,0 390x844\n"
		"  Stack 0,0 390x844\n"
		"    Screen 0,0 390x844\n"
		"      Text 16,100.5 40x20 \"Hello\"\n"
		"      Button 16,120.5 48x44 \"Add\"\n"
		"      TextField 0,0 0x0\n"
		"      Toggle 0,0 0x0\n");
	ak_buf_reset(&out);
	ak_fake_text(root, &out);
	CHECK_EQ_STR(cstr(&out), "Hello|Add");
	ak_buf_reset(&out);
	ak_fake_log(e.f, &out);
	CHECK(strstr(cstr(&out), "SetProp 4 Text=\"Hello\"\nCreate 5 Button\nSetProp 5 Title=\"Add\"\n") != NULL);

	ak_fake_view *add = ak_fake_one(e.f, AK_KIND_BUTTON, "Add");
	REQUIRE(add);
	CHECK_EQ_INT(add->id, 5);
	CHECK(ak_fake_one(e.f, AK_KIND_BUTTON, "Nope") == NULL);
	CHECK_EQ_STR(cstr(&e.errors), "0 Button views with \"Nope\"\n");
	ak_buf_reset(&e.errors);
	ak_fake_view_vec vs = {0};
	ak_fake_of_kind(e.f, AK_KIND_SCREEN, &vs);
	CHECK_EQ_INT(vs.len, 1);
	ak_vec_free(&vs);

	// Destroying a view takes it out of its parent; its children stay,
	// parentless.
	ak_batch_destroy(&e.b, 3);
	flush(&e);
	CHECK(ak_fake_view_of(e.f, 3) == NULL);
	CHECK_EQ_INT(ak_fake_view_of(e.f, 2)->children.len, 0);
	CHECK(ak_fake_view_of(e.f, 4)->parent == NULL);
	CHECK_EQ_STR(cstr(&e.errors), "");
	ak_buf_free(&out);
	teardown(&e);
}

// Ops no real host could apply are misuses.
TEST(misuse) {
	env e;
	setup(&e);
	mount(&e);
	ak_batch *b = &e.b;
	ak_batch_create(b, 4, AK_KIND_TEXT);
	ak_batch_create(b, AK_HOST_ID_BIT | 9, AK_KIND_TEXT);
	ak_batch_create(b, 9, 999);
	ak_batch_insert(b, 1, 4, 0);
	ak_batch_insert(b, 2, 4, 0);
	ak_batch_remove(b, 1, 5);
	ak_batch_set_bool(b, 4, AK_PROP_TEXT_TEXT, true);
	ak_batch_set_frame(b, 42, 0, 0, 1, 1);
	ak_batch_command(b, 4, AK_CMD_PRESENT, NULL, 0);
	ak_batch_create(b, 10, AK_KIND_SCROLL);
	ak_batch_create(b, 11, AK_KIND_VIEW);
	ak_batch_create(b, 12, AK_KIND_VIEW);
	ak_batch_insert(b, 10, 11, 0);
	ak_batch_insert(b, 10, 12, 1);
	ak_batch_insert(b, 10, 12, 5);
	flush(&e);
	CHECK_EQ_STR(cstr(&e.errors),
		"Create: view 4 exists\n"
		"Create: 2147483657 is a host ID\n"
		"Create: unknown kind 999\n"
		"Insert: 4 already has parent 3\n"
		"Insert: 4 already has parent 3\n"
		"Remove: 5 is not a child of 1\n"
		"SetProp: Text has no bool prop 64\n"
		"SetFrame: no view 42\n"
		"Present on a Text\n"
		"Insert: a Scroll has one child\n"
		"Insert: index 5 of 1 into 10\n");
	ak_buf_reset(&e.errors);
	ak_batch_create(b, 13, AK_KIND_TABS);
	ak_batch_insert(b, 13, 12, 0);
	flush(&e);
	CHECK_EQ_STR(cstr(&e.errors), "Insert: a Tabs holds Screens, not a View\n");
	// A cut batch.
	ak_buf_reset(&e.errors);
	const uint8_t cut[] = {1, 0, 0, 0, 1, 20, 0};
	ak_fake_apply(e.f, cut, sizeof cut);
	CHECK_EQ_STR(cstr(&e.errors), "a bad batch: ops: truncated batch\n");
	teardown(&e);
}

static void measure_all(void *ctx, ak_fake_view *v, float width, float height, float aw, float ah, float *w, float *h) {
	AK_UNUSED(ctx), AK_UNUSED(v), AK_UNUSED(width), AK_UNUSED(height), AK_UNUSED(aw), AK_UNUSED(ah);
	*w = 1, *h = 2;
}

// Views are measured by the fake host's rules.
TEST(measure) {
	env e;
	setup(&e);
	ak_batch *b = &e.b;
	ak_batch_create(b, 1, AK_KIND_TEXT);
	set_str(b, 1, AK_PROP_TEXT_TEXT, "Hello");
	ak_batch_create(b, 2, AK_KIND_TEXT);
	set_str(b, 2, AK_PROP_TEXT_TEXT, "héllo, 世界");
	ak_batch_create(b, 3, AK_KIND_BUTTON);
	set_str(b, 3, AK_PROP_BUTTON_TITLE, "Añadir");
	ak_batch_create(b, 4, AK_KIND_TEXT_FIELD);
	ak_batch_create(b, 5, AK_KIND_TEXT_FIELD);
	ak_batch_set_bool(b, 5, AK_PROP_TEXT_FIELD_MULTILINE, true);
	set_str(b, 5, AK_PROP_TEXT_FIELD_VALUE, "a\nb\nc");
	ak_batch_create(b, 6, AK_KIND_SLIDER);
	ak_batch_create(b, 7, AK_KIND_PROGRESS_RING);
	ak_batch_create(b, 8, AK_KIND_SEGMENTED);
	ak_buf *seg = ak_batch_set_bytes_begin(b, 8, AK_PROP_SEGMENTED_SEGMENTS);
	ak_append_strings(seg, (const ak_span[]){AK_STR("One"), AK_STR("Two"), AK_STR("Three")}, 3);
	ak_batch_set_bytes_end(b);
	ak_batch_create(b, 9, AK_KIND_PICKER);
	set_str(b, 9, AK_PROP_PICKER_LABEL, "Size");
	ak_buf *items = ak_batch_set_bytes_begin(b, 9, AK_PROP_PICKER_ITEMS);
	ak_append_strings(items, (const ak_span[]){AK_STR("Small"), AK_STR("Medium")}, 2);
	ak_batch_set_bytes_end(b);
	ak_batch_create(b, 10, AK_KIND_DATE_PICKER);
	ak_batch_set_enum(b, 10, AK_PROP_DATE_PICKER_MODE, AK_DATE_MODE_DATE_TIME);
	ak_batch_create(b, 11, AK_KIND_PAGE_CONTROL);
	ak_batch_set_int(b, 11, AK_PROP_PAGE_CONTROL_COUNT, 4);
	ak_batch_create(b, 12, AK_KIND_VIEW);
	ak_batch_create(b, 13, AK_KIND_RICH_TEXT);
	ak_buf *rich = ak_batch_set_bytes_begin(b, 13, AK_PROP_RICH_TEXT_CONTENT);
	ak_rich_style plain = {0}, inl = {.flags = AK_RICH_INLINE};
	ak_append_rich(rich, (const ak_rich_run[]){{plain, AK_STR("Hi ")}, {inl, AK_STR("\xef\xbf\xbc")}, {plain, AK_STR("\nsecond line")}}, 3);
	ak_batch_set_bytes_end(b);
	ak_buf *inls = ak_batch_set_bytes_begin(b, 13, AK_PROP_RICH_TEXT_INLINES);
	ak_buf_u32(inls, 1);
	ak_buf_u32(inls, 14);
	ak_buf_f32(inls, 30);
	ak_buf_f32(inls, 32);
	ak_batch_set_bytes_end(b);
	flush(&e);
	CHECK_EQ_STR(cstr(&e.errors), "");

	float w, h;
#define M(id, kw, kh, aw, ah, ew, eh) \
	do { \
		ak_fake_measure(e.f, id, kw, kh, aw, ah, &w, &h); \
		CHECK_EQ_F(w, ew); \
		CHECK_EQ_F(h, eh); \
	} while (0)
	// Text: 8pt a character on 20pt lines, wrapping at the space.
	M(1, AK_NAN, AK_NAN, AK_INF, AK_INF, 40, 20);
	M(1, AK_NAN, AK_NAN, 24, AK_INF, 24, 40);
	M(1, AK_NAN, AK_NAN, 27.9f, AK_INF, 24, 40);
	M(1, AK_NAN, AK_NAN, 3, AK_INF, 8, 100);
	M(1, AK_NAN, AK_NAN, -AK_INF, AK_INF, 8, 100);
	M(1, 16, AK_NAN, AK_INF, AK_INF, 16, 60);
	M(1, AK_NAN, 7, AK_INF, AK_INF, 40, 7);
	M(2, AK_NAN, AK_NAN, AK_INF, AK_INF, 72, 20);  // runes, not bytes
	M(3, AK_NAN, AK_NAN, AK_INF, AK_INF, 72, 44);
	M(4, AK_NAN, AK_NAN, AK_INF, AK_INF, 200, 36);
	M(5, AK_NAN, AK_NAN, AK_INF, AK_INF, 200, 76);
	M(6, AK_NAN, AK_NAN, 300, AK_INF, 300, 30);
	M(6, AK_NAN, AK_NAN, AK_INF, AK_INF, 200, 30);
	M(7, 30, AK_NAN, AK_INF, AK_INF, 30, 30);
	M(7, AK_NAN, 12, AK_INF, AK_INF, 12, 12);
	M(7, AK_NAN, AK_NAN, AK_INF, AK_INF, 20, 20);
	M(8, AK_NAN, AK_NAN, AK_INF, AK_INF, 210, 32);
	M(9, AK_NAN, AK_NAN, AK_INF, AK_INF, 72, 34);  // "Small": unset, Selected is 0
	M(10, AK_NAN, AK_NAN, AK_INF, AK_INF, 200, 34);
	M(11, AK_NAN, AK_NAN, AK_INF, AK_INF, 80, 26);
	M(12, AK_NAN, AK_NAN, AK_INF, AK_INF, 0, 0);
	// "Hi ￼" is 4 characters, its line as tall as the inline
	// component; "second line" 11.
	M(13, AK_NAN, AK_NAN, AK_INF, AK_INF, 88, 52);

	// A picker's chosen item names it, else its label; a line limit caps a
	// multiline field.
	ak_batch_set_int(b, 9, AK_PROP_PICKER_SELECTED, 1);
	ak_batch_set_int(b, 5, AK_PROP_LINE_LIMIT, 2);
	flush(&e);
	M(9, AK_NAN, AK_NAN, AK_INF, AK_INF, 80, 34);  // "Medium"
	ak_batch_set_int(b, 9, AK_PROP_PICKER_SELECTED, -1);
	flush(&e);
	M(9, AK_NAN, AK_NAN, AK_INF, AK_INF, 64, 34);  // "Size"
	M(5, AK_NAN, AK_NAN, AK_INF, AK_INF, 200, 56);
	CHECK_EQ_INT(e.f->measures, 25);

	// A measure function takes over.
	e.f->measure_fn = measure_all;
	M(1, AK_NAN, AK_NAN, AK_INF, AK_INF, 1, 2);
	ak_fake_measure(e.f, 77, AK_NAN, AK_NAN, AK_INF, AK_INF, &w, &h);
	CHECK_EQ_STR(cstr(&e.errors), "Measure: no view 77\n");
#undef M
	teardown(&e);
}

// Events go to the core as a user's taps and typing make them.
TEST(events) {
	env e;
	setup(&e);
	mount(&e);
	ak_fake_view *add = ak_fake_view_of(e.f, 5), *field = ak_fake_view_of(e.f, 6), *toggle = ak_fake_view_of(e.f, 7);
	ak_fake_tap(e.f, add);
	ak_fake_type(e.f, field, "hi");
	ak_fake_flip(e.f, toggle);
	ak_fake_flip(e.f, toggle);
	REQUIRE(e.rec.nevents == 4);
	ak_event ev = event(&e.rec, 0);
	CHECK_EQ_INT(ev.id, 5);
	CHECK_EQ_INT(ev.kind, AK_EVENT_PRESS);
	ev = event(&e.rec, 1);
	CHECK_EQ_INT(ev.id, 6);
	CHECK_EQ_INT(ev.kind, AK_EVENT_TEXT_CHANGED);
	CHECK(ak_span_eqc(ev.s, "hi"));
	ev = event(&e.rec, 2);
	CHECK_EQ_INT(ev.kind, AK_EVENT_TOGGLED);
	CHECK(ev.b);
	CHECK(!event(&e.rec, 3).b);
	CHECK(!ak_fake_bool(toggle, AK_PROP_TOGGLE_ON));
	// The widget changed first: the field shows what was typed.
	ak_fake_view_vec vs = {0};
	ak_fake_find(e.f, AK_KIND_TEXT_FIELD, AK_STR("hi"), &vs);
	CHECK_EQ_INT(vs.len, 1);
	ak_vec_free(&vs);

	// A view that doesn't listen gets nothing.
	ak_fake_tap(e.f, ak_fake_view_of(e.f, 4));
	CHECK_EQ_INT(e.rec.nevents, 4);
	CHECK_EQ_STR(cstr(&e.errors), "view 4 (Text) doesn't listen for Press\n");
	ak_buf_reset(&e.errors);

	// Back pops the top screen, then reports it.
	ak_batch_create(&e.b, 8, AK_KIND_SCREEN);
	ak_batch_insert(&e.b, 2, 8, 1);
	flush(&e);
	ak_fake_view *stack = ak_fake_view_of(e.f, 2);
	ak_fake_back(e.f, stack);
	CHECK_EQ_INT(stack->children.len, 1);
	ev = event(&e.rec, 4);
	CHECK_EQ_INT(ev.id, 2);
	CHECK_EQ_INT(ev.kind, AK_EVENT_POPPED);
	CHECK_EQ_INT(ev.i, 8);
	ak_fake_back(e.f, stack);
	CHECK_EQ_STR(cstr(&e.errors), "Back: nothing to pop\n");

	// The environment, for the root.
	ak_environment env = e.f->env;
	env.width = 844;
	env.height = 390;
	env.dark = true;
	ak_fake_set_environment(e.f, &env);
	ev = event(&e.rec, 5);
	CHECK_EQ_INT(ev.id, 1);
	CHECK_EQ_INT(ev.kind, AK_EVENT_ENVIRONMENT);
	CHECK_EQ_F(ev.env.width, 844);
	CHECK_EQ_F(ev.env.safe_top, 47);
	CHECK(ev.env.dark);
	CHECK(ak_span_eqc(ev.env.locale, "en_US"));
	CHECK(ak_span_eqc(ev.env.languages, "en-US"));

	ak_fake_open_url(e.f, "asuka://x");
	ev = event(&e.rec, 6);
	CHECK_EQ_INT(ev.kind, AK_EVENT_INCOMING_URL);
	CHECK(ak_span_eqc(ev.s, "asuka://x"));
	ak_fake_lifecycle(e.f, AK_LIFECYCLE_BACKGROUND);
	ev = event(&e.rec, 7);
	CHECK_EQ_INT(ev.id, 0);
	CHECK_EQ_INT(ev.i, AK_LIFECYCLE_BACKGROUND);
	teardown(&e);
}

// Keys go to the nearest view out from the focus whose Keys has them.
TEST(keys) {
	env e;
	setup(&e);
	mount(&e);
	ak_buf *k = ak_batch_set_bytes_begin(&e.b, 3, AK_PROP_KEYS);
	ak_append_keys(k, (const ak_key[]){{AK_STR("s"), 1}, {AK_STR("Escape"), 0}}, 2);
	ak_batch_set_bytes_end(&e.b);
	ak_batch_listen(&e.b, 3, AK_EVENT_KEY);
	flush(&e);
	CHECK(ak_fake_key(e.f, ak_fake_view_of(e.f, 6), "S", 1));
	CHECK(!ak_fake_key(e.f, ak_fake_view_of(e.f, 6), "s", 0));
	CHECK(!ak_fake_key(e.f, NULL, "Escape", 0));  // from the root, which has no keys
	CHECK(ak_fake_key(e.f, ak_fake_view_of(e.f, 4), "Escape", 0));
	REQUIRE(e.rec.nevents == 2);
	CHECK_EQ_INT(event(&e.rec, 0).i, 0);
	CHECK_EQ_INT(event(&e.rec, 1).i, 1);
	teardown(&e);
}

// Alerts: their buttons, fields and default action.
TEST(alerts) {
	env e;
	setup(&e);
	mount(&e);
	ak_batch *b = &e.b;
	ak_batch_create(b, 20, AK_KIND_ALERT);
	ak_buf *p = ak_batch_set_bytes_begin(b, 20, AK_PROP_ALERT_BUTTONS);
	ak_append_actions(p, (const ak_action_item[]){
		{.role = AK_ALERT_BUTTON_ROLE_CANCEL, .title = AK_STR("Cancel")},
		{.role = AK_ALERT_BUTTON_ROLE_DEFAULT, .title = AK_STR("Save")},
	}, 2);
	ak_batch_set_bytes_end(b);
	p = ak_batch_set_bytes_begin(b, 20, AK_PROP_ALERT_FIELDS);
	ak_append_alert_fields(p, (const ak_alert_field[]){{.placeholder = AK_STR("Name")}, {.placeholder = AK_STR("Password"), .flags = AK_ALERT_FIELD_SECURE}}, 2);
	ak_batch_set_bytes_end(b);
	ak_batch_listen(b, 20, AK_EVENT_ACTION);
	ak_batch_listen(b, 20, AK_EVENT_FIELD_CHANGED);
	ak_batch_listen(b, 20, AK_EVENT_DISMISSED);
	ak_batch_command(b, 20, AK_CMD_PRESENT, NULL, 0);
	flush(&e);
	CHECK_EQ_STR(cstr(&e.errors), "");
	ak_fake_view *alert = ak_fake_view_of(e.f, 20);
	ak_fake_view_vec vs = {0};
	ak_fake_presented(e.f, &vs);
	CHECK(vs.len == 1 && vs.data[0] == alert);
	ak_vec_free(&vs);
	CHECK_EQ_INT(ak_fake_action_named(alert, AK_PROP_ALERT_BUTTONS, "Save"), 1);

	ak_fake_type_in_alert(e.f, alert, 0, "Ann");
	ak_alert_field_vec fs = {0};
	ak_fake_alert_fields(alert, &fs);
	REQUIRE(fs.len == 2);
	CHECK(ak_span_eqc(fs.data[0].text, "Ann"));
	CHECK(ak_span_eqc(fs.data[1].placeholder, "Password"));
	CHECK_EQ_INT(fs.data[1].flags, AK_ALERT_FIELD_SECURE);
	ak_vec_free(&fs);
	ak_event ev = event(&e.rec, 0);
	uint32_t index;
	ak_span text;
	CHECK(ak_decode_field_changed((const uint8_t *)ev.s.p, ev.s.len, &index, &text));
	CHECK_EQ_INT(index, 0);
	CHECK(ak_span_eqc(text, "Ann"));

	CHECK(ak_fake_return_in_alert(e.f, alert));
	ev = event(&e.rec, 1);
	CHECK_EQ_INT(ev.kind, AK_EVENT_ACTION);
	CHECK_EQ_INT(ev.i, 1);
	ak_fake_dismiss(e.f, alert);
	CHECK_EQ_INT(event(&e.rec, 2).kind, AK_EVENT_DISMISSED);
	teardown(&e);
}

// A Call is answered by the call function, its Result posted.
static bool answer(void *ctx, ak_span module, ak_span method, ak_span args, ak_buf *out) {
	AK_UNUSED(ctx);
	if (!ak_span_eqc(module, "echo")) {
		ak_buf_printf(out, "no such module");
		return false;
	}
	ak_buf_put(out, method.p, method.len);
	ak_buf_u8(out, ':');
	ak_buf_put(out, args.p, args.len);
	return true;
}

static void call_cmd(ak_batch *b, ak_id id, uint32_t call, const char *module, const char *method, const char *args) {
	ak_buf *p = ak_batch_command_begin(b, id, AK_CMD_CALL);
	ak_buf_u32(p, call);
	ak_buf_wstr(p, ak_cstr(module));
	ak_buf_wstr(p, ak_cstr(method));
	ak_buf_put(p, args, strlen(args));
	ak_batch_command_end(b);
}

TEST(calls_and_posts) {
	env e;
	setup(&e);
	mount(&e);
	e.f->call_fn = answer;
	call_cmd(&e.b, 1, 7, "echo", "ping", "xyz");
	call_cmd(&e.b, 1, 8, "nope", "x", "");
	flush(&e);
	CHECK_EQ_INT(e.rec.nevents, 0);
	CHECK_EQ_INT(ak_fake_pending(e.f), 2);
	// The vtable's post, as from another thread.
	asuka_host_callbacks cb = ak_fake_callbacks(e.f);
	cb.post();
	CHECK_EQ_INT(ak_fake_pending(e.f), 3);
	ak_fake_run_posted(e.f);
	CHECK_EQ_INT(ak_fake_pending(e.f), 0);
	CHECK_EQ_INT(e.rec.run_posted, 1);
	REQUIRE(e.rec.nevents == 2);
	ak_event ev = event(&e.rec, 0);
	CHECK_EQ_INT(ev.id, 1);
	CHECK_EQ_INT(ev.kind, AK_EVENT_RESULT);
	CHECK(ak_span_eq(ev.s, AK_STR("\x07\0\0\0\x01ping:xyz")));
	ev = event(&e.rec, 1);
	CHECK(ak_span_eq(ev.s, AK_STR("\x08\0\0\0\0no such module")));

	// The vtable applies and measures.
	ak_batch_set_frame(&e.b, 6, 1, 2, 3, 4);
	size_t n;
	const uint8_t *p = ak_batch_encode(&e.b, &n);
	cb.apply(p, n);
	CHECK_EQ_F(ak_fake_view_of(e.f, 6)->frame[3], 4);
	float w, h;
	cb.measure(5, AK_NAN, AK_NAN, AK_INF, AK_INF, &w, &h);
	CHECK_EQ_F(w, 48);
	teardown(&e);
}

// Lists: their items and sections, cells bound by the core, rows
// selected.
TEST(lists) {
	env e;
	setup(&e);
	mount(&e);
	ak_batch_create(&e.b, 30, AK_KIND_LIST);
	ak_buf *p = ak_batch_command_begin(&e.b, 30, AK_CMD_LIST_UPDATE);
	ak_buf_u32(p, 3);
	ak_buf_u64(p, 10);
	ak_buf_u64(p, 20);
	ak_buf_u64(p, (uint64_t)-1);
	ak_buf_u32(p, 2);
	ak_buf_u32(p, 0);
	ak_buf_u64(p, 100);
	ak_buf_u32(p, 2);
	ak_buf_u64(p, 200);
	ak_batch_command_end(&e.b);
	flush(&e);
	CHECK_EQ_STR(cstr(&e.errors), "");
	ak_fake_view *list = ak_fake_view_of(e.f, 30);
	REQUIRE(list->items.len == 3);
	CHECK_EQ_INT(list->items.data[2], -1);
	REQUIRE(list->sections.len == 2);
	CHECK_EQ_INT(list->sections.data[1].start, 2);
	CHECK_EQ_INT(list->sections.data[1].id, 200);

	ak_fake_view *cell = ak_fake_new_cell(e.f);
	CHECK(cell->id & AK_HOST_ID_BIT);
	CHECK_EQ_INT(cell->kind, AK_FAKE_KIND_CELL);
	ak_buf out = {0};
	ak_fake_kind_name(&out, cell->kind);
	CHECK_EQ_STR(cstr(&out), "Cell");
	e.rec.count = 3;
	CHECK_EQ_INT(ak_fake_list_count(e.f, list), 3);
	ak_fake_bind(e.f, list, 1, cell);
	CHECK_EQ_INT(e.rec.bound_list, 30);
	CHECK_EQ_INT(e.rec.bound_index, 1);
	CHECK_EQ_INT(e.rec.bound_cell, cell->id);
	CHECK(ak_fake_key_out(e.f, cell) == list);
	// The core fills the cell.
	ak_batch_create(&e.b, 31, AK_KIND_TEXT);
	set_str(&e.b, 31, AK_PROP_TEXT_TEXT, "Row");
	ak_batch_insert(&e.b, cell->id, 31, 0);
	ak_batch_listen(&e.b, cell->id, AK_EVENT_ROW_SELECTED);
	flush(&e);
	ak_buf_reset(&out);
	ak_fake_tree(cell, &out);
	CHECK_EQ_STR(cstr(&out), "Cell 0,0 0x0\n  Text 0,0 0x0 \"Row\"\n");
	ak_fake_select_row(e.f, cell, true);
	CHECK(ak_fake_bool(cell, AK_PROP_SELECTED));
	CHECK(event(&e.rec, 0).b);
	ak_fake_unbind(e.f, list, cell);
	CHECK_EQ_INT(e.rec.unbound_cell, cell->id);

	// A first section not at 0 is a misuse.
	p = ak_batch_command_begin(&e.b, 30, AK_CMD_LIST_UPDATE);
	ak_buf_u32(p, 0);
	ak_buf_u32(p, 1);
	ak_buf_u32(p, 1);
	ak_buf_u64(p, 1);
	ak_batch_command_end(&e.b);
	flush(&e);
	CHECK_EQ_STR(cstr(&e.errors), "a list's first section doesn't start at 0\n");
	ak_buf_free(&out);
	teardown(&e);
}

// A native view's values, and its events.
TEST(native) {
	env e;
	setup(&e);
	ak_batch_create(&e.b, 1, AK_KIND_NATIVE);
	set_str(&e.b, 1, AK_PROP_NATIVE_TYPE, "map");
	ak_buf *p = ak_batch_set_bytes_begin(&e.b, 1, AK_PROP_NATIVE_PROP);
	ak_buf_wstr(p, AK_STR("zoom"));
	ak_buf_u8(p, 2);
	ak_buf_u64(p, 42);
	ak_batch_set_bytes_end(&e.b);
	p = ak_batch_set_bytes_begin(&e.b, 1, AK_PROP_NATIVE_PROP);
	ak_buf_wstr(p, AK_STR("title"));
	ak_buf_u8(p, 4);
	ak_buf_wstr(p, AK_STR("Kyoto"));
	ak_batch_set_bytes_end(&e.b);
	p = ak_batch_set_bytes_begin(&e.b, 1, AK_PROP_NATIVE_PROP);
	ak_buf_wstr(p, AK_STR("zoom"));
	ak_buf_u8(p, 2);
	ak_buf_u64(p, 43);
	ak_batch_set_bytes_end(&e.b);
	ak_batch_listen(&e.b, 1, AK_EVENT_NATIVE);
	flush(&e);
	ak_fake_view *v = ak_fake_view_of(e.f, 1);
	CHECK_EQ_INT(v->native.len, 2);
	const ak_fake_native *z = ak_fake_native_value(v, "zoom");
	REQUIRE(z);
	CHECK_EQ_INT(z->i, 43);
	const ak_fake_native *t = ak_fake_native_value(v, "title");
	REQUIRE(t);
	CHECK(ak_span_eqc(t->s, "Kyoto"));
	CHECK(ak_fake_native_value(v, "nope") == NULL);

	ak_fake_emit_native(e.f, v, "moved", (const ak_fake_native[]){{.name = AK_STR("x"), .type = 3, .f = 1.5}, {.name = AK_STR("ok"), .type = 1, .i = 1}}, 2);
	ak_event ev = event(&e.rec, 0);
	CHECK_EQ_INT(ev.kind, AK_EVENT_NATIVE);
	ak_reader r = {(const uint8_t *)ev.s.p, ev.s.len, 0, false};
	CHECK(ak_span_eqc(ak_read_str(&r), "moved"));
	CHECK_EQ_INT(ak_read_u32(&r), 2);
	CHECK(ak_span_eqc(ak_read_str(&r), "x"));
	CHECK_EQ_INT(ak_read_u8(&r), 3);
	CHECK_EQ_F(ak_read_f64(&r), 1.5);
	CHECK(ak_span_eqc(ak_read_str(&r), "ok"));
	CHECK_EQ_INT(ak_read_u8(&r), 1);
	CHECK_EQ_INT(ak_read_u8(&r), 1);
	CHECK(!r.err && r.off == r.len);
	teardown(&e);
}

// Sheets, snackbars and the window keys go to.
TEST(presented) {
	env e;
	setup(&e);
	mount(&e);
	ak_batch *b = &e.b;
	ak_batch_create(b, 40, AK_KIND_SHEET);
	ak_batch_listen(b, 40, AK_EVENT_ENVIRONMENT);
	ak_batch_command(b, 40, AK_CMD_PRESENT, NULL, 0);
	ak_batch_create(b, 41, AK_KIND_SNACKBAR);
	set_str(b, 41, AK_PROP_SNACKBAR_ACTION, "Undo");
	ak_batch_listen(b, 41, AK_EVENT_ACTION);
	ak_batch_listen(b, 41, AK_EVENT_DISMISSED);
	ak_batch_command(b, 41, AK_CMD_PRESENT, NULL, 0);
	flush(&e);
	CHECK_EQ_STR(cstr(&e.errors), "");
	ak_fake_view *sheet = ak_fake_view_of(e.f, 40), *snack = ak_fake_view_of(e.f, 41);
	CHECK(ak_fake_snackbar(e.f) == snack);
	CHECK(ak_fake_key_out(e.f, sheet) == ak_fake_root(e.f));
	CHECK(ak_fake_key_window(e.f) == ak_fake_root(e.f));
	ak_fake_report_sheet(e.f, sheet);
	ak_event ev = event(&e.rec, 0);
	CHECK_EQ_INT(ev.id, 40);
	CHECK_EQ_F(ev.env.height, 844 - 47 - 10);
	CHECK_EQ_F(ev.env.safe_top, 0);
	ak_fake_press_snackbar_action(e.f, snack);
	CHECK_EQ_INT(event(&e.rec, 1).kind, AK_EVENT_ACTION);
	ak_fake_time_out(e.f, snack);
	CHECK_EQ_INT(event(&e.rec, 2).kind, AK_EVENT_DISMISSED);
	// A sheet's FullScreen can't change once it's presented.
	ak_batch_set_bool(b, 40, AK_PROP_SHEET_FULL_SCREEN, true);
	flush(&e);
	CHECK_EQ_STR(cstr(&e.errors), "SetProp: a sheet's FullScreen after its Present: a host presents it as it was\n");
	ak_buf_reset(&e.errors);

	// A Window presented is the one in use.
	ak_batch_create(b, 50, AK_KIND_WINDOW);
	ak_buf *p = ak_batch_set_bytes_begin(b, 50, AK_PROP_WINDOW_CONFIG);
	ak_append_window_config(p, &(ak_window_config){.width = 300, .height = 200, .title = AK_STR("Tools")});
	ak_batch_set_bytes_end(b);
	ak_batch_command(b, 50, AK_CMD_PRESENT, NULL, 0);
	flush(&e);
	CHECK(ak_fake_key_window(e.f) == ak_fake_view_of(e.f, 50));
	ak_fake_set_key_window(e.f, ak_fake_root(e.f));
	CHECK(ak_fake_key_window(e.f) == ak_fake_root(e.f));
	p = ak_batch_command_begin(b, 50, AK_CMD_WINDOW_ACTION);
	ak_append_window_action(p, AK_WINDOW_ACTION_FOCUS, 0, 0);
	ak_batch_command_end(b);
	flush(&e);
	CHECK(ak_fake_key_window(e.f) == ak_fake_view_of(e.f, 50));
	ak_batch_command(b, 4, AK_CMD_WINDOW_ACTION, (const uint8_t[]){2}, 1);
	flush(&e);
	CHECK_EQ_STR(cstr(&e.errors), "WindowAction on a Text\n");
	teardown(&e);
}

T_MAIN()
