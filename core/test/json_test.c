#include "t.h"
#include "rt/json.h"

// struct Todo { title: string; done: bool; tags: [string]; due: int? }
static const uint8_t todo_kinds[] = {AK_K_REF, AK_K_BOOL, AK_K_REF, AK_K_REF};
static const char *const todo_names[] = {"title\0s", "done\0b", "tags\0[s", "due\0?i"};
static const ak_shape todo = {.name = "Todo", .nfields = 4, .kinds = todo_kinds, .names = todo_names};

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
	a->data[a->len++].p = t;
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
	CHECK_EQ_INT(ak_enum_case(a->data[0].p), 2);   // 1: an int
	CHECK_EQ_INT(ak_enum_case(a->data[2].p), 3);   // 2.5: a float
	CHECK_EQ_INT(ak_enum_case(a->data[3].p), 3);   // 1e3: a float
	CHECK_EQ_INT(ak_enum_case(a->data[4].p), 3);   // past an int: a float
	CHECK_EQ_INT(ak_enum_case(a->data[5].p), 2);   // int's least
	CHECK(((ak_enum *)a->data[5].p)->f[0].i == INT64_MIN);
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
