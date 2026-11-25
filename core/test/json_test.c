#include "t.h"
#include "rt/json.h"

// struct Todo { title: string; done: bool; tags: [string]; due: int? }
static const uint8_t todo_kinds[] = {AK_K_REF, AK_K_BOOL, AK_K_REF, AK_K_REF};
static const char *const todo_names[] = {"title\0s", "done\0b", "tags\0[s", "due\0?i"};
static const ak_shape todo = {.name = "Todo", .nfields = 4, .kinds = todo_kinds, .names = todo_names};

// struct Repo { htmlURL: string; createdAt: int; fullName: string; id: int }
static const uint8_t repo_kinds[] = {AK_K_REF, AK_K_INT, AK_K_REF, AK_K_INT};
static const char *const repo_names[] = {"htmlURL\0s", "createdAt\0i", "fullName\0s", "id\0i"};
static const ak_shape repo = {.name = "Repo", .nfields = 4, .kinds = repo_kinds, .names = repo_names};

static const char *const filter_cases[] = {"all", "unread", "flagged"};

// struct Node { name: string; next: Node? }: a type that nests itself.
static const uint8_t node_kinds[] = {AK_K_REF, AK_K_REF};
static const char *const node_names[] = {"name\0s", "next\0?SNode;"};
static const ak_shape node = {.name = "Node", .nfields = 2, .kinds = node_kinds, .names = node_names};

// enum Shape { circle(radius: float, label: string, tags: [string]), dot, at(where: Node) }
static const uint8_t circle_kinds[] = {AK_K_FLOAT, AK_K_REF, AK_K_REF};
static const char *const circle_names[] = {"radius\0f", "label\0s", "tags\0[s"};
static const uint8_t at_kinds[] = {AK_K_REF};
static const char *const at_names[] = {"where\0SNode;"};
static const ak_case shape_cases[] = {
	{.name = "circle", .nfields = 3, .kinds = circle_kinds, .names = circle_names},
	{.name = "dot"},
	{.name = "at", .nfields = 1, .kinds = at_kinds, .names = at_names},
};
// Not const: its payloadless cases' shared values are made on it.
static ak_shape shape_enum = {.name = "Shape", .ncases = 3, .cases = shape_cases};

// std/json's enum Value { null, bool(bool), int(int), float(float),
// string(string), array([Value]), object([string: Value]) }
static const uint8_t v_bool[] = {AK_K_BOOL}, v_int[] = {AK_K_INT}, v_float[] = {AK_K_FLOAT}, v_ref[] = {AK_K_REF};
static const ak_case value_cases[] = {
	{.name = "null"},
	{.name = "bool", .nfields = 1, .kinds = v_bool},
	{.name = "int", .nfields = 1, .kinds = v_int},
	{.name = "float", .nfields = 1, .kinds = v_float},
	{.name = "string", .nfields = 1, .kinds = v_ref},
	{.name = "array", .nfields = 1, .kinds = v_ref},
	{.name = "object", .nfields = 1, .kinds = v_ref},
};
static ak_shape value_enum = {.name = "Value", .ncases = 7, .cases = value_cases};

static const ak_shape *shape(void *ctx, const char *name, size_t n) {
	AK_UNUSED(ctx);
	if (n == 5 && memcmp(name, "Value", 5) == 0) return &value_enum;
	if (n == 4 && memcmp(name, "Todo", 4) == 0) return &todo;
	if (n == 4 && memcmp(name, "Node", 4) == 0) return &node;
	if (n == 4 && memcmp(name, "Repo", 4) == 0) return &repo;
	if (n == 5 && memcmp(name, "Shape", 5) == 0) return &shape_enum;
	if (n == 5 && memcmp(name, "Error", 5) == 0) return &ak_shape_error;
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
	ak_struct *t0 = ak_arr_vals(a)[0].p, *t1 = ak_arr_vals(a)[1].p;
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

TEST(snake_case_keys) {
	// A web API's snake_case keys read into the fields they name; a field's
	// own name wins over one, whichever comes first; a key that's a name
	// in another case alone names nothing.
	const char *src = "{\"html_url\": \"u\", \"created_at\": 5, \"fullName\": \"own\", \"full_name\": \"snake\", \"ID\": 9, \"id\": 2}";
	ak_val v = ak_json_decode(src, strlen(src), "SRepo;", &types);
	REQUIRE(!ak_error_pending());
	ak_struct *r = v.p;
	CHECK_EQ_STR(((ak_str *)r->f[0].p)->data, "u");
	CHECK_EQ_INT(r->f[1].i, 5);
	CHECK_EQ_STR(((ak_str *)r->f[2].p)->data, "own");
	CHECK_EQ_INT(r->f[3].i, 2);
	ak_release(r);
	src = "{\"full_name\": \"snake\", \"fullName\": \"own\", \"Id\": 3, \"html-url\": \"k\"}";
	v = ak_json_decode(src, strlen(src), "SRepo;", &types);
	REQUIRE(!ak_error_pending());
	r = v.p;
	CHECK_EQ_STR(((ak_str *)r->f[0].p)->data, "k");
	CHECK_EQ_STR(((ak_str *)r->f[2].p)->data, "own");
	CHECK_EQ_INT(r->f[3].i, 0);
	ak_release(r);
}

TEST(null_for_a_field_is_its_absence) {
	// "title": null reads as a Todo without a title (empty); "due": null,
	// an optional's, is none.
	const char *src = "{\"title\": null, \"done\": true, \"tags\": null, \"due\": null}";
	ak_val v = ak_json_decode(src, strlen(src), "STodo;", &types);
	REQUIRE(!ak_error_pending());
	ak_struct *t = v.p;
	CHECK_EQ_STR(((ak_str *)t->f[0].p)->data, "");
	CHECK_EQ_INT(t->f[1].i, 1);
	CHECK_EQ_INT(((ak_arr *)t->f[2].p)->len, 0);
	CHECK(t->f[3].p == NULL);
	ak_release(t);
	// A snake_case key's null doesn't empty what the field's own name set.
	src = "{\"fullName\": \"own\", \"full_name\": null}";
	v = ak_json_decode(src, strlen(src), "SRepo;", &types);
	REQUIRE(!ak_error_pending());
	CHECK_EQ_STR(((ak_str *)((ak_struct *)v.p)->f[2].p)->data, "own");
	ak_release(v.p);
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
	CHECK_EQ_F(ak_arr_vals(((ak_arr *)v.p))[1].f, 2000);
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

// decode_fails decodes src as desc and wants an error whose message
// contains want, and nothing left behind.
static void decode_fails(const char *src, size_t n, const char *desc, const char *want) {
	int64_t live = ak_obj_live;
	ak_val v = ak_json_decode(src, n, desc, &types);
	void *e = ak_error_take();
	if (!e) {
		T_FAIL("%.40s: no error", src);
		return;
	}
	CHECK(v.p == NULL);
	if (!strstr(ak_error_message(e)->data, want)) T_FAIL("%.40s: %s, want %s", src, ak_error_message(e)->data, want);
	ak_release(e);
	CHECK_EQ_INT(ak_obj_live, live);
}

// An enum's case as an object, {"case": {"field": value, ...}}: a field
// left out starts empty (a string, a collection) or is required (a
// struct); one written twice is the last; the fields are separated by
// commas.
TEST(enum_object_payloads) {
	const char *src = "{\"circle\": {\"radius\": 1}}";
	ak_val v = ak_json_decode(src, strlen(src), "EShape;", &types);
	REQUIRE(!ak_error_pending());
	ak_enum *e = v.p;
	CHECK_EQ_INT(ak_enum_case(e), 0);
	CHECK_EQ_F(e->f[0].f, 1);
	CHECK(e->f[1].p != NULL && ((ak_str *)e->f[1].p)->len == 0);
	CHECK(e->f[2].p != NULL && ((ak_arr *)e->f[2].p)->len == 0);
	ak_release(e);

	int64_t live = ak_obj_live;
	src = "{\"circle\": {\"label\": \"a\", \"label\": \"b\", \"radius\": 2}}";
	v = ak_json_decode(src, strlen(src), "EShape;", &types);
	REQUIRE(!ak_error_pending());
	e = v.p;
	CHECK_EQ_STR(((ak_str *)e->f[1].p)->data, "b");
	ak_release(e);
	CHECK_EQ_INT(ak_obj_live, live);

	src = "{\"circle\": {\"label\": \"a\" \"radius\": 2}}";
	decode_fails(src, strlen(src), "EShape;", "expected ',' or '}'");
	src = "{\"circle\": {\"label\": \"a\",}}";
	decode_fails(src, strlen(src), "EShape;", "expected a string");
	src = "{\"at\": {}}";
	decode_fails(src, strlen(src), "EShape;", "a missing field where");
	src = "\"nope";
	decode_fails(src, strlen(src), "EShape;", "a string that doesn't end");
}

// Nesting deeper than AK_JSON_MAX_DEPTH is an error, not a stack
// overflow: in a value of a type that nests itself, and in one skipped.
TEST(nesting_depth) {
	size_t depth = 1000000;
	char *src = malloc(depth * 10 + 64);
	const char *head = "{\"title\":\"x\",\"extra\":";
	size_t n = strlen(head);
	memcpy(src, head, n);
	for (size_t i = 0; i < depth; i++) src[n++] = '[';
	for (size_t i = 0; i < depth; i++) src[n++] = ']';
	src[n++] = '}';
	decode_fails(src, n, "STodo;", "nested too deeply");
	n = 0;
	for (size_t i = 0; i < depth; i++) {
		memcpy(src + n, "{\"next\":", 8);
		n += 8;
	}
	decode_fails(src, n, "SNode;", "nested too deeply");
	// As deep as it may go is fine: the field's object, then its arrays.
	n = strlen(head);
	memcpy(src, head, n);
	for (int i = 0; i < AK_JSON_MAX_DEPTH - 1; i++) src[n++] = '[';
	for (int i = 0; i < AK_JSON_MAX_DEPTH - 1; i++) src[n++] = ']';
	src[n++] = '}';
	ak_val v = ak_json_decode(src, n, "STodo;", &types);
	CHECK(!ak_error_pending());
	ak_release(v.p);
	free(src);
}

// Encoding takes no C stack per level: a value as deep as an app can
// make it (a long chain of a type that nests itself) is written whole.
TEST(encode_depth) {
	enum { DEPTH = 1000000 };
	ak_struct *head = NULL;
	for (int i = 0; i < DEPTH; i++) {
		ak_struct *n = ak_struct_new(&node);
		n->f[0].p = ak_str_c("n");
		n->f[1].p = head;
		head = n;
	}
	ak_str *out = ak_json_encode((ak_val){.p = head}, "SNode;", &types, 0);
	REQUIRE(out != NULL);
	CHECK_EQ_INT(out->len, (size_t)DEPTH * strlen("{\"name\":\"n\",\"next\":") + strlen("null") + DEPTH);
	CHECK(memcmp(out->data, "{\"name\":\"n\",\"next\":{\"name\":\"n\",", 30) == 0);
	CHECK(memcmp(out->data + out->len - DEPTH - 4, "null}}", 6) == 0 && out->data[out->len - 1] == '}');
	ak_release(out);
	// Freed link by link: releasing the head would free the chain by
	// recursion.
	while (head) {
		ak_struct *next = head->f[1].p;
		head->f[1].p = NULL;
		ak_release(head);
		head = next;
	}
}

// Every kind of container, compact and indented.
TEST(encode_containers) {
	static const struct {
		const char *desc, *src, *compact, *pretty;
	} cases[] = {
		{"[?EShape;", "[{\"circle\": [1.5, \"c\", [\"x\", \"y\"]]}, \"dot\", null, {\"at\": [{\"name\": \"n\", \"next\": {\"name\": \"m\"}}]}]",
		 "[{\"circle\":[1.5,\"c\",[\"x\",\"y\"]]},\"dot\",null,{\"at\":[{\"name\":\"n\",\"next\":{\"name\":\"m\",\"next\":null}}]}]",
		 "[\n  {\"circle\": [1.5,\"c\",[\n      \"x\",\n      \"y\"\n    ]]},\n  \"dot\",\n  null,\n  {\"at\": [{\n      \"name\": \"n\",\n      \"next\": {\n        \"name\": \"m\",\n        \"next\": null\n      }\n    }]}\n]"},
		{"{[?i", "{\"a\": [1, null], \"b\": [], \"c\": null}", "{\"a\":[1,null],\"b\":[],\"c\":[]}", "{\n  \"a\": [\n    1,\n    null\n  ],\n  \"b\": [],\n  \"c\": []\n}"},
		{"<s", "[\"x\", \"y\", \"x\"]", "[\"x\",\"y\"]", "[\n  \"x\",\n  \"y\"\n]"},
		{"[STodo;", "[{\"title\": \"t\", \"done\": true, \"due\": 3}]", "[{\"title\":\"t\",\"done\":true,\"tags\":[],\"due\":3}]",
		 "[\n  {\n    \"title\": \"t\",\n    \"done\": true,\n    \"tags\": [],\n    \"due\": 3\n  }\n]"},
		{"?[?s", "[null, \"a\"]", "[null,\"a\"]", "[\n  null,\n  \"a\"\n]"},
		{"{s", "{}", "{}", "{}"},
	};
	for (size_t i = 0; i < AK_LEN(cases); i++) {
		ak_val v = ak_json_decode(cases[i].src, strlen(cases[i].src), cases[i].desc, &types);
		REQUIRE(!ak_error_pending());
		ak_str *c = ak_json_encode(v, cases[i].desc, &types, 0), *p = ak_json_encode(v, cases[i].desc, &types, 2);
		CHECK_EQ_STR(c->data, cases[i].compact);
		CHECK_EQ_STR(p->data, cases[i].pretty);
		ak_release(c);
		ak_release(p);
		ak_release(v.p);
	}
}

// An integer out of int's range is an error (not clamped, not a float
// cast's undefined behavior); a number is read whole, however long.
TEST(integer_range) {
	decode_fails("1e300", 5, "i", "out of range");
	decode_fails("-1e19", 5, "i", "out of range");
	decode_fails("9223372036854775808", 19, "i", "out of range");
	decode_fails("-9223372036854775809", 20, "i", "out of range");
	decode_fails("9.3e18", 6, "i", "out of range");
	ak_val v = ak_json_decode("-9223372036854775808", 20, "i", &types);
	CHECK(!ak_error_pending());
	CHECK(v.i == INT64_MIN);
	v = ak_json_decode("9.2e18", 6, "i", &types);
	CHECK(!ak_error_pending());
	CHECK_EQ_INT(v.i, 9200000000000000000LL);
	// 0.000…0001e100: more digits than a small buffer holds.
	char longf[128] = "0.";
	for (int i = 0; i < 99; i++) strcat(longf, "0");
	strcat(longf, "1e100");
	v = ak_json_decode(longf, strlen(longf), "f", &types);
	CHECK(!ak_error_pending());
	CHECK_EQ_F(v.f, 1);
	v = ak_json_decode("99999999999999999999", 20, "f", &types);  // past int's, a float's
	CHECK(!ak_error_pending());
	CHECK_EQ_F(v.f, 1e20);
	decode_fails("1e999", 5, "f", "out of range");
	decode_fails("1-2", 3, "f", "a bad number");
	decode_fails("--1", 3, "i", "a bad number");
}

// A surrogate escape without its pair is U+FFFD, and the escape after it
// is still read.
TEST(surrogates) {
	static const struct {
		const char *src, *want;
	} cases[] = {
		{"\"\\ud83d\\ude00\"", "\xf0\x9f\x98\x80"},
		{"\"\\ud83d\\u0041\"", "\xef\xbf\xbd" "A"},
		{"\"\\udc00x\"", "\xef\xbf\xbd" "x"},
		{"\"\\ud83d\"", "\xef\xbf\xbd"},
		{"\"\\ud83d\\ud83d\\ude00\"", "\xef\xbf\xbd\xf0\x9f\x98\x80"},
		{"\"\\ud83d\\n\"", "\xef\xbf\xbd\n"},
	};
	for (size_t i = 0; i < AK_LEN(cases); i++) {
		ak_val v = ak_json_decode(cases[i].src, strlen(cases[i].src), "s", &types);
		REQUIRE(!ak_error_pending());
		CHECK_EQ_STR(((ak_str *)v.p)->data, cases[i].want);
		ak_release(v.p);
	}
}

// A string that fails (a bad escape) leaves nothing behind wherever it
// is: a field, an element, bytes, a key.
TEST(bad_strings_leave_nothing) {
	const char *src = "{\"title\":\"ab\\q\"}";
	decode_fails(src, strlen(src), "STodo;", "a bad escape");
	src = "[\"ab\\q\"]";
	decode_fails(src, strlen(src), "[s", "a bad escape");
	src = "\"ab\\q\"";
	decode_fails(src, strlen(src), "y", "a bad escape");
	src = "{\"a\\q\":1}";
	decode_fails(src, strlen(src), "{i", "a bad escape");
}

// A shape the core defines (Error's) has its fields' descriptors too.
TEST(core_shapes) {
	ak_struct *e = ak_struct_new(&ak_shape_error);
	e->f[0].p = ak_str_c("boom");
	ak_str *s = ak_json_encode((ak_val){.p = e}, "SError;", &types, 0);
	CHECK_EQ_STR(s->data, "{\"message\":\"boom\"}");
	ak_val back = ak_json_decode(s->data, s->len, "SError;", &types);
	REQUIRE(!ak_error_pending());
	CHECK_EQ_STR(((ak_str *)((ak_struct *)back.p)->f[0].p)->data, "boom");
	ak_release(back.p);
	ak_release(s);
	ak_release(e);
}

T_MAIN()

// What print shows of values JSON can't hold: a tuple, its elements in
// parentheses; a map from other than strings, each key as it's written
// (a struct's, however deep, before its value).
TEST(encode_printed_only) {
	// (int, Todo?, string)
	static const uint8_t pair_kinds[] = {AK_K_INT, AK_K_REF, AK_K_REF};
	static const char *const pair_names[] = {"0\0i", "1\0?STodo;", "2\0s"};
	static const ak_shape pair = {.name = "(int,Todo?,string)", .nfields = 3, .kinds = pair_kinds, .names = pair_names};
	ak_struct *t = ak_struct_new(&pair);
	t->f[0].i = 7;
	t->f[2].p = ak_str_new("a\"b", 3);
	ak_str *s = ak_json_encode((ak_val){.p = t}, "(", &types, 0);
	CHECK_EQ_STR(s->data, "(7,null,\"a\\\"b\")");
	ak_release(s);

	// [int: [(int,Todo?,string)]] and [Todo: eFilter;]
	ak_map *m = ak_map_new(AK_K_INT, false, AK_K_REF, NULL);
	ak_arr *a = ak_arr_new(AK_K_REF, &pair, 1);
	ak_arr_vals(a)[a->len++].p = t;
	ak_map_set(&m, (ak_val){.i = -2}, (ak_val){.p = a});
	ak_map_set(&m, (ak_val){.i = 5}, (ak_val){.p = ak_arr_new(AK_K_REF, &pair, 0)});
	s = ak_json_encode((ak_val){.p = m}, "mi[(", &types, 0);
	CHECK_EQ_STR(s->data, "{-2:[(7,null,\"a\\\"b\")],5:[]}");
	ak_release(s);
	s = ak_json_encode((ak_val){.p = m}, "mi[(", &types, 2);
	CHECK_EQ_STR(s->data, "{\n  -2: [\n    (\n      7,\n      null,\n      \"a\\\"b\"\n    )\n  ],\n  5: []\n}");
	ak_release(s);
	ak_release(m);

	const char *src = "{\"title\": \"t\", \"done\": true}";
	ak_val todo_key = ak_json_decode(src, strlen(src), "STodo;", &types);
	REQUIRE(!ak_error_pending());
	ak_map *k = ak_map_new(AK_K_REF, false, AK_K_INT, NULL);
	ak_map_set(&k, todo_key, (ak_val){.i = 2});
	s = ak_json_encode((ak_val){.p = k}, "mSTodo;eFilter;", &types, 0);
	CHECK_EQ_STR(s->data, "{{\"title\":\"t\",\"done\":true,\"tags\":[],\"due\":null}:\"flagged\"}");
	ak_release(s);
	ak_release(k);
	s = ak_json_encode((ak_val){.p = NULL}, "mii", &types, 0);
	CHECK_EQ_STR(s->data, "{}");
	ak_release(s);
}

// std/json's Value: any JSON, as it is. It reads back what it writes;
// integers that fit an int are ints, the rest floats; failures, deep
// nesting included, leave nothing behind.
TEST(json_value) {
	int64_t live = ak_obj_live;
	const char *src = "{\"a\": [1, -2, 2.5, 1e3, 9223372036854775808, true, null, \"s\\n\"], \"b\": {}, \"a\": [], \"c\": {\"d\": [[]]}}";
	ak_val v = ak_json_decode(src, strlen(src), "JValue;", &types);
	REQUIRE(!ak_error_pending());
	ak_str *s = ak_json_encode(v, "JValue;", &types, 0);
	CHECK_EQ_STR(s->data, "{\"a\":[],\"b\":{},\"c\":{\"d\":[[]]}}");
	ak_release(s);
	ak_release(v.p);
	src = "[1, -2, 2.5, 1e3, 9223372036854775808, -9223372036854775808, true, null, \"s\\n\"]";
	v = ak_json_decode(src, strlen(src), "JValue;", &types);
	REQUIRE(!ak_error_pending());
	ak_enum *e = v.p;
	CHECK_EQ_INT(ak_enum_case(e), 5);
	ak_arr *a = e->f[0].p;
	REQUIRE(a->len == 9);
	CHECK_EQ_INT(ak_enum_case(ak_arr_vals(a)[0].p), 2);   // 1: an int
	CHECK_EQ_INT(ak_enum_case(ak_arr_vals(a)[2].p), 3);   // 2.5: a float
	CHECK_EQ_INT(ak_enum_case(ak_arr_vals(a)[3].p), 3);   // 1e3: a float
	CHECK_EQ_INT(ak_enum_case(ak_arr_vals(a)[4].p), 3);   // past an int: a float
	CHECK_EQ_INT(ak_enum_case(ak_arr_vals(a)[5].p), 2);   // int's least
	CHECK(((ak_enum *)ak_arr_vals(a)[5].p)->f[0].i == INT64_MIN);
	s = ak_json_encode(v, "JValue;", &types, 0);
	CHECK_EQ_STR(s->data, "[1,-2,2.5,1000,9223372036854776000,-9223372036854775808,true,null,\"s\\n\"]");
	ak_release(s);
	ak_release(v.p);
	CHECK_EQ_INT(ak_obj_live, live);
	decode_fails("{\"a\": }", 7, "JValue;", "expected a value");
	decode_fails("[1, 2", 5, "JValue;", "expected");
	decode_fails("{\"a\": [1, {\"b\": tru}]}", 22, "JValue;", "expected a value");
	size_t depth = 100000;
	char *deep = malloc(depth * 2);
	for (size_t i = 0; i < depth; i++) deep[i] = '[', deep[depth + i] = ']';
	decode_fails(deep, depth * 2, "JValue;", "nested too deeply");
	free(deep);
}

// same_value: whether two json.Values are the same, case by case: an
// int and a float that are equal differ, and floats compare by their
// bits (-0 isn't 0), objects in their members' order.
static bool same_value(const ak_enum *a, const ak_enum *b) {
	if (!a || !b) return a == b;
	if (ak_enum_case(a) != ak_enum_case(b)) return false;
	switch (ak_enum_case(a)) {
	case 0: return true;
	case 1:
	case 2: return a->f[0].i == b->f[0].i;
	case 3: return memcmp(&a->f[0].f, &b->f[0].f, sizeof(double)) == 0;
	case 4: return ak_str_eq(a->f[0].p, b->f[0].p);
	case 5: {
		const ak_arr *x = a->f[0].p, *y = b->f[0].p;
		if (x->len != y->len) return false;
		for (uint32_t i = 0; i < x->len; i++)
			if (!same_value(ak_arr_get(x, i).p, ak_arr_get(y, i).p)) return false;
		return true;
	}
	case 6: {
		const ak_map *x = a->f[0].p, *y = b->f[0].p;
		if (x->live != y->live) return false;
		uint32_t j = 0;
		for (uint32_t i = 0; i < x->len; i++) {
			if (x->dead[i]) continue;
			while (j < y->len && y->dead[j]) j++;
			if (j >= y->len || !ak_str_eq(x->keys[i].p, y->keys[j].p) || !same_value(x->vals[i].p, y->vals[j].p)) return false;
			j++;
		}
		return true;
	}
	}
	return false;
}

// A type with its own JSON: struct Money { cents: int } whose toJSON
// gives a string ("1.50"), and enum Level { low, high(n: int) } whose
// toJSON gives an int.
static const uint8_t money_kinds[] = {AK_K_INT};
static const char *const money_names[] = {"cents\0i"};
static const ak_shape money = {.name = "Money", .nfields = 1, .kinds = money_kinds, .names = money_names, .hooks = 1u << AK_HOOK_TOJSON, .jdesc = "s"};
static const ak_shape own_list = {.name = "Own", .nfields = 1, .kinds = money_kinds, .names = money_names, .hooks = 1u << AK_HOOK_TOJSON, .jdesc = "[SMoney;"};

static ak_val own_to_json(const ak_shape *s, int hook, ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	if (hook != AK_HOOK_TOJSON) return (ak_val){0};
	int64_t cents = ((ak_struct *)args[0].p)->f[0].i;
	if (s == &money) {
		char buf[32];
		int w = snprintf(buf, sizeof buf, "%lld.%02lld", (long long)(cents / 100), (long long)(cents % 100));
		return (ak_val){.p = ak_str_new(buf, (size_t)w)};
	}
	// Own: [Money(cents), Money(cents * 2)]
	ak_arr *a = ak_arr_new(AK_K_REF, &money, 2);
	for (int i = 1; i <= 2; i++) {
		ak_struct *m = ak_struct_new(&money);
		m->f[0].i = cents * i;
		ak_arr_push(&a, (ak_val){.p = m});
	}
	return (ak_val){.p = a};
}

static const ak_shape *shape_own(void *ctx, const char *name, size_t n) {
	if (n == 5 && memcmp(name, "Money", 5) == 0) return &money;
	if (n == 3 && memcmp(name, "Own", 3) == 0) return &own_list;
	return shape(ctx, name, n);
}

// direct_is_round_trip converts the value src reads as (of desc) to a
// json.Value directly, and as encoding it and decoding that as a Value
// does, and wants the same Value of both.
static void direct_is_round_trip(const char *src, const char *desc, bool nonfinite) {
	ak_json_types t = {shape_own, cases, NULL};
	t.nonfinite = nonfinite;
	ak_val v = ak_json_decode(src, strlen(src), desc, &t);
	if (ak_error_pending()) {
		ak_obj *e = ak_error_take();
		T_FAIL("decoding %s as %s: %s", src, desc, ak_error_message(e)->data);
		ak_release(e);
		return;
	}
	ak_json_types plain = {shape_own, cases, NULL};
	ak_json_types points = plain;
	points.points = true;
	ak_str *text = ak_json_encode(v, desc, &points, 0);
	ak_val ref = ak_json_decode(text->data, text->len, "JValue;", &plain);
	REQUIRE(!ak_error_pending());
	ak_val direct = ak_json_value(v, desc, "JValue;", &plain);
	REQUIRE(!ak_error_pending());
	if (!same_value(ref.p, direct.p)) {
		ak_str *a = ak_json_encode(ref, "JValue;", &points, 0), *b = ak_json_encode(direct, "JValue;", &points, 0);
		T_FAIL("%s as %s: the round trip gives %s, directly %s", src, desc, a->data, b->data);
		ak_release(a);
		ak_release(b);
	}
	ak_release(text);
	ak_release(ref.p);
	ak_release(direct.p);
	ak_release_val(v, *desc == 'f' ? AK_K_FLOAT : *desc == 'b' ? AK_K_BOOL : strchr("inre", *desc) ? AK_K_INT : AK_K_REF);
}

TEST(json_value_direct) {
	ak_val (*saved)(const ak_shape *, int, ak_val *, uint32_t) = ak_shape_call;
	ak_shape_call = own_to_json;
	int64_t live = ak_obj_live;
	static const struct {
		const char *src, *desc;
		bool nonfinite;
	} corpus[] = {
		{"0", "i"}, {"-9223372036854775808", "i"}, {"9223372036854775807", "i"},
		{"-128", "n0"}, {"65535", "n5"}, {"4294967295", "n6"}, {"-2147483648", "n2"},
		{"9223372036854775807", "n7"}, {"9223372036854775808", "n7"}, {"18446744073709551615", "n7"}, {"12345678901234567890", "n7"},
		{"0", "f"}, {"-0.0", "f"}, {"2", "f"}, {"2.5", "f"}, {"1e300", "f"}, {"-1e-300", "f"}, {"5e-324", "f"},
		{"1e21", "f"}, {"123456789012345680000", "f"}, {"0.1", "f"}, {"9007199254740993", "f"}, {"1.7976931348623157e308", "f"},
		{"NaN", "f", true}, {"Infinity", "f", true}, {"-Infinity", "f", true}, {"[1.5, NaN, 3]", "[f", true},
		{"true", "b"}, {"false", "b"},
		{"\"\"", "s"}, {"\"plain\"", "s"}, {"\"é ☃ 😀 \\u0000 \\u001f \\\" \\\\ \\n\\t\"", "s"},
		{"\"a\"", "r"}, {"\"é\"", "r"}, {"\"😀\"", "r"},
		{"\"\"", "y"}, {"\"AA==\"", "y"}, {"\"AAE=\"", "y"}, {"\"AAEC\"", "y"}, {"\"/+/+aGVsbG8=\"", "y"},
		{"null", "?i"}, {"3", "?i"}, {"null", "?s"}, {"\"x\"", "?s"}, {"2", "?f"}, {"null", "??i"}, {"[1, null, 3]", "[?i"},
		{"[]", "[i"}, {"[1, 2, 3]", "[i"}, {"[[1], [], [2, 3]]", "[[i"}, {"[200, 3]", "[n4"}, {"[true, false]", "[b"}, {"[-1, 7]", "[n0"},
		{"{}", "{i"}, {"{\"b\": 1, \"a\": 2}", "{i"}, {"{\"x\": {\"y\": [\"z\"]}}", "{{[s"},
		{"[3, 1, 3, 2]", "<i"}, {"[\"b\", \"a\"]", "<s"},
		{"{\"title\": \"milk\", \"done\": true, \"tags\": [\"a\"], \"due\": 4}", "STodo;"},
		{"[{\"title\": \"a\", \"done\": false}, {\"title\": \"b\", \"done\": true, \"due\": null}]", "[STodo;"},
		{"{\"name\": \"a\", \"next\": {\"name\": \"b\", \"next\": {\"name\": \"c\"}}}", "SNode;"},
		{"\"dot\"", "EShape;"}, {"{\"circle\": [2, \"r\", [\"t\"]]}", "EShape;"}, {"{\"at\": [{\"name\": \"n\"}]}", "EShape;"},
		{"[\"dot\", {\"circle\": [0.5, \"\", []]}]", "[EShape;"},
		{"\"unread\"", "eFilter;"}, {"[\"flagged\", \"all\"]", "[eFilter;"},
		{"null", "JValue;"}, {"[1, 2.5, \"s\", true, null, {\"a\": [{}]}]", "JValue;"}, {"{\"b\": 1, \"a\": {\"c\": 2.0}}", "JValue;"},
		{"{\"k\": [1, \"two\"]}", "{JValue;"}, {"[null, 3]", "[?JValue;"},
		{"{\"cents\": 150}", "SMoney;"}, {"[{\"cents\": 5}, {\"cents\": 1999}]", "[SMoney;"}, {"{\"cents\": 7}", "SOwn;"},
	};
	for (size_t i = 0; i < sizeof corpus / sizeof corpus[0]; i++) direct_is_round_trip(corpus[i].src, corpus[i].desc, corpus[i].nonfinite);
	// Values holding floats the round trip changes: NaN and the
	// infinities null, -0 0. [NaN, {"x": Infinity, "y": [1, -0]}, 2.5]
	{
		ak_json_types t = {shape, cases, NULL}, points = t;
		points.points = true;
		ak_val fs[] = {{.f = NAN}, {.f = INFINITY}, {.f = -0.0}, {.f = 2.5}};
		ak_enum *f[4];
		for (int i = 0; i < 4; i++) f[i] = ak_enum_new(&value_enum, 3), f[i]->f[0] = fs[i];
		ak_enum *one = ak_enum_new(&value_enum, 2);
		one->f[0].i = 1;
		ak_arr *y = ak_arr_new(AK_K_REF, &value_enum, 2);
		ak_arr_push(&y, (ak_val){.p = one});
		ak_arr_push(&y, (ak_val){.p = f[2]});
		ak_enum *ya = ak_enum_new(&value_enum, 5);
		ya->f[0].p = y;
		ak_map *m = ak_map_new(AK_K_REF, true, AK_K_REF, &value_enum);
		ak_map_set(&m, (ak_val){.p = ak_str_c("x")}, (ak_val){.p = f[1]});
		ak_map_set(&m, (ak_val){.p = ak_str_c("y")}, (ak_val){.p = ya});
		ak_enum *mo = ak_enum_new(&value_enum, 6);
		mo->f[0].p = m;
		ak_arr *top = ak_arr_new(AK_K_REF, &value_enum, 3);
		ak_arr_push(&top, (ak_val){.p = f[0]});
		ak_arr_push(&top, (ak_val){.p = mo});
		ak_arr_push(&top, (ak_val){.p = f[3]});
		ak_enum *v = ak_enum_new(&value_enum, 5);
		v->f[0].p = top;
		ak_val direct = ak_json_value((ak_val){.p = v}, "JValue;", "JValue;", &t);
		ak_str *text = ak_json_encode(direct, "JValue;", &points, 0);
		CHECK_EQ_STR(text->data, "[null,{\"x\":null,\"y\":[1,0.0]},2.5]");
		ak_release(text);
		text = ak_json_encode((ak_val){.p = v}, "JValue;", &points, 0);
		ak_val ref = ak_json_decode(text->data, text->len, "JValue;", &t);
		CHECK(same_value(ref.p, direct.p));
		CHECK(((ak_enum *)ak_arr_get(((ak_enum *)direct.p)->f[0].p, 2).p) == f[3]);  // what's unchanged is shared
		ak_release(ref.p);
		ak_release(text);
		ak_release(direct.p);
		ak_release(v);
	}
	// Deep: past what decoding reads back, converted all the same (a
	// list of Nodes 5000 long).
	ak_json_types t = {shape, cases, NULL};
	ak_val deep = {.p = NULL};
	for (int i = 0; i < 5000; i++) {
		ak_struct *n = ak_struct_new(&node);
		n->f[0].p = ak_str_c("n");
		n->f[1] = deep;
		deep.p = n;
	}
	ak_val dv = ak_json_value(deep, "SNode;", "JValue;", &t);
	REQUIRE(!ak_error_pending());
	int depth = 0;
	for (ak_enum *e = dv.p; ak_enum_case(e) == 6; depth++) {
		ak_val next = {0};
		ak_str *k = ak_str_c("next");
		REQUIRE(ak_map_get(e->f[0].p, (ak_val){.p = k}, &next));
		ak_release(k);
		e = next.p;
	}
	CHECK_EQ_INT(depth, 5000);
	ak_release(dv.p);
	ak_release(deep.p);
	CHECK_EQ_INT(ak_obj_live, live);
	ak_shape_call = saved;
}
