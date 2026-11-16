#include "t.h"
#include "rt/json.h"

// struct Todo { title: string; done: bool; tags: [string]; due: int? }
static const uint8_t todo_kinds[] = {AK_K_REF, AK_K_BOOL, AK_K_REF, AK_K_REF};
static const char *const todo_names[] = {"title\0s", "done\0b", "tags\0[s", "due\0?i"};
static const ak_shape todo = {.name = "Todo", .nfields = 4, .kinds = todo_kinds, .names = todo_names};

static const char *const filter_cases[] = {"all", "unread", "flagged"};

static const ak_shape *shape(void *ctx, const char *name, size_t n) {
	AK_UNUSED(ctx);
	if (n == 4 && memcmp(name, "Todo", 4) == 0) return &todo;
	return NULL;
}

static bool cases(void *ctx, const char *name, size_t n, const char *const **out, uint32_t *count) {
	AK_UNUSED(ctx);
	if (n == 6 && memcmp(name, "Filter", 6) == 0) {
		*out = filter_cases;
		*count = 3;
		return true;
	}
	return false;
}

static const ak_json_types types = {shape, cases, NULL};

TEST(decode_structs) {
	const char *src = "[{\"title\": \"milk \\u00e9\", \"done\": true, \"tags\": [\"a\",\"b\"], \"extra\": {\"x\": [1,2]}},\n {\"title\": \"eggs\", \"done\": false, \"due\": 3}]";
	ak_val v = ak_json_decode(src, strlen(src), "[STodo;", &types);
	REQUIRE(!ak_error_pending());
	ak_arr *a = v.p;
	REQUIRE(a->len == 2);
	ak_struct *t0 = a->data[0].p, *t1 = a->data[1].p;
	CHECK_EQ_STR(((ak_str *)t0->f[0].p)->data, "milk é");
	CHECK_EQ_INT(t0->f[1].i, 1);
	CHECK_EQ_INT(((ak_arr *)t0->f[2].p)->len, 2);
	CHECK(t0->f[3].p == NULL);
	CHECK_EQ_INT(((ak_box *)t1->f[3].p)->v.i, 3);
	CHECK_EQ_INT(((ak_arr *)t1->f[2].p)->len, 0);  // missing: empty
	ak_str *out = ak_json_encode(v, "[STodo;", &types, 0);
	CHECK_EQ_STR(out->data, "[{\"title\":\"milk é\",\"done\":true,\"tags\":[\"a\",\"b\"],\"due\":null},{\"title\":\"eggs\",\"done\":false,\"tags\":[],\"due\":3}]");
	ak_release(out);
	ak_release(a);
}

TEST(errors_say_where) {
	const char *src = "{\"title\": \"x\",\n \"done\": 3}";
	ak_val v = ak_json_decode(src, strlen(src), "STodo;", &types);
	CHECK(v.p == NULL);
	void *e = ak_error_take();
	REQUIRE(e != NULL);
	CHECK_EQ_STR(ak_error_message(e)->data, "json: 2:10: expected true or false");
	ak_release(e);
	// A missing required field (no default to start from: a struct).
	src = "{\"title\": \"x\"}";
	v = ak_json_decode(src, strlen(src), "STodo;", &types);
	CHECK(!ak_error_pending());
	ak_release(v.p);
}

TEST(maps_enums_numbers) {
	const char *src = "{\"a\": \"unread\", \"b\": \"flagged\"}";
	ak_val v = ak_json_decode(src, strlen(src), "{eFilter;", &types);
	REQUIRE(!ak_error_pending());
	ak_val got;
	ak_str *k = ak_str_c("b");
	REQUIRE(ak_map_get(v.p, (ak_val){.p = k}, &got));
	CHECK_EQ_INT(got.i, 2);
	ak_str *out = ak_json_encode(v, "{eFilter;", &types, 2);
	CHECK_EQ_STR(out->data, "{\n  \"a\": \"unread\",\n  \"b\": \"flagged\"\n}");
	ak_release(out);
	ak_release(k);
	ak_release(v.p);
	src = "[1.5, 2e3, -0.25]";
	v = ak_json_decode(src, strlen(src), "[f", &types);
	CHECK_EQ_F(((ak_arr *)v.p)->data[1].f, 2000);
	ak_release(v.p);
	src = "1.5";
	v = ak_json_decode(src, strlen(src), "i", &types);
	void *e = ak_error_take();
	CHECK(e != NULL);
	ak_release(e);
}

// Go's encoding/json writes a nil slice or map as null: an empty one.
TEST(null_collections) {
	const char *src = "null";
	ak_val v = ak_json_decode(src, strlen(src), "[f", &types);
	REQUIRE(!ak_error_pending());
	CHECK_EQ_INT(((ak_arr *)v.p)->len, 0);
	ak_release(v.p);
	v = ak_json_decode(src, strlen(src), "{eFilter;", &types);
	REQUIRE(!ak_error_pending());
	CHECK_EQ_INT(((ak_map *)v.p)->len, 0);
	ak_release(v.p);
	v = ak_json_decode(src, strlen(src), "<s", &types);
	REQUIRE(!ak_error_pending());
	CHECK_EQ_INT(((ak_map *)v.p)->len, 0);
	ak_release(v.p);
	src = " nul";
	v = ak_json_decode(src, strlen(src), "[f", &types);
	void *e = ak_error_take();
	CHECK(e != NULL);
	ak_release(e);
}

TEST(bytes_base64) {
	ak_bytes *b = ak_bytes_new("hello!?", 7);
	ak_str *s = ak_json_encode((ak_val){.p = b}, "y", &types, 0);
	CHECK_EQ_STR(s->data, "\"aGVsbG8hPw==\"");
	ak_val back = ak_json_decode(s->data, s->len, "y", &types);
	CHECK(ak_obj_eq(back.p, &b->h));
	ak_release(back.p);
	ak_release(s);
	ak_release(b);
}

T_MAIN()
