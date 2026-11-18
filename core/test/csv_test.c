// std/csv's reader and writer (core/src/std/csv.c), called as the VM
// calls them, on Go's encoding/csv's reader cases (reader_test.go): the
// same rows, and its mistakes as errors where they are.
#include "t.h"
#include "std/std.h"

static ak_lib_fn fn(const char *name) {
	for (const ak_lib_entry *const *t = ak_std_libs; *t; t++)
		for (const ak_lib_entry *e = *t; e->name; e++)
			if (strcmp(e->name, name) == 0) return e->fn;
	fprintf(stderr, "no builtin %s\n", name);
	abort();
}

typedef struct {
	const char *sep, *comment;
	bool trim, lazy, flexible;
} opts;

// show writes rows as [a|b][c], line breaks and CRs escaped, or the
// error's message.
static void show(ak_val v, char *out, size_t n) {
	size_t o = 0;
	out[0] = 0;
	if (ak_error_pending()) {
		void *err = ak_error_take();
		snprintf(out, n, "error %s", ak_error_message(err)->data);
		ak_release(err);
		return;
	}
	ak_arr *rows = v.p;
	for (uint32_t r = 0; r < rows->len; r++) {
		ak_arr *row = rows->data[r].p;
		o += (size_t)snprintf(out + o, n - o, "[");
		for (uint32_t i = 0; i < row->len; i++) {
			ak_str *s = row->data[i].p;
			if (i) o += (size_t)snprintf(out + o, n - o, "|");
			for (uint32_t k = 0; k < s->len && o + 4 < n; k++) {
				char c = s->data[k];
				if (c == '\n') out[o++] = '\\', out[o++] = 'n';
				else if (c == '\r') out[o++] = '\\', out[o++] = 'r';
				else out[o++] = c;
			}
			out[o] = 0;
		}
		o += (size_t)snprintf(out + o, n - o, "]");
	}
}

static void read_case(const char *text, opts op, const char *want, int line) {
	ak_val args[10] = {
		{.p = ak_str_c(text)},
		{.p = ak_str_c(op.sep ? op.sep : ",")},
		{.p = ak_str_c(op.comment ? op.comment : "")},
		{.i = op.trim},
		{.i = op.lazy},
		{.i = op.flexible},
		{.i = 1},
		{.p = ak_str_arr(0)},
		{.i = -1},
		{.p = ak_str_c("[s")},
	};
	ak_val v = fn("csv.read")(args, 10);
	char got[1024];
	show(v, got, sizeof got);
	if (strcmp(got, want) != 0) {
		fprintf(stderr, "csv_test.c:%d: %s:\n got: %s\nwant: %s\n", line, t_current, got, want);
		t_failed++;
	}
	ak_release(v.p);
	for (int i = 0; i < 10; i++)
		if (i < 3 || i == 7 || i == 9) ak_release(args[i].p);
}
#define READ(text, want, ...) read_case(text, (opts){__VA_ARGS__}, want, __LINE__)

TEST(csv_go_reader_cases) {
	READ("a,b,c\n", "[a|b|c]");
	READ("a,b\r\nc,d\r\n", "[a|b][c|d]");
	READ("a,b\rc,d\r\n", "[a|b\\rc|d]");
	READ("#field1,field2,field3\n\"aaa\",\"bb\nb\",\"ccc\"\n\"a,a\",\"b\"\"bb\",\"ccc\"\nzzz,yyy,xxx\n", "[#field1|field2|field3][aaa|bb\\nb|ccc][a,a|b\"bb|ccc][zzz|yyy|xxx]");
	READ("a,b,c", "[a|b|c]");
	READ("a;b;c\n", "[a|b|c]", .sep = ";");
	READ("\"two\nline\",\"one line\",\"three\nline\nfield\"", "[two\\nline|one line|three\\nline\\nfield]");
	READ("a,b,c\n\nd,e,f\n\n", "[a|b|c][d|e|f]");
	READ(" a,  b,   c\n", "[a|b|c]", .trim = true);
	READ(" a,  b,   c\n", "[ a|  b|   c]");
	READ("#1,2,3\na,b,c\n#comment", "[a|b|c]", .comment = "#");
	READ("#1,2,3\na,b,c", "[#1|2|3][a|b|c]");
	READ("a \"word\",\"1\"2\",a\",\"b", "[a \"word\"|1\"2|a\"|b]", .lazy = true);
	READ("a \"word\",\"1\"2\",a\"", "[a \"word\"|1\"2|a\"]", .lazy = true);
	READ("a\"\"b,c", "[a\"\"b|c]", .lazy = true);
	READ("a\"\"b,c", "error csv: 1:2: a bare \" in a field that isn't quoted (quote the field, and double the quote: \"a\"\"b\")");
	READ(" \"a\",\" b\",c", "[a| b|c]", .trim = true);
	READ("a \"word\",\"b\"", "error csv: 1:3: a bare \" in a field that isn't quoted (quote the field, and double the quote: \"a\"\"b\")");
	READ("\"a word\",b\"", "error csv: 1:11: a bare \" in a field that isn't quoted (quote the field, and double the quote: \"a\"\"b\")");
	READ("\"a \"word\",\"b\"", "error csv: 1:4: a \" in a quoted field must be doubled (\"a\"\"b\")");
	READ("a,b,c\nd,e", "error csv: 2:1: a row of 2 fields, not 3 as the first");
	READ("a,b,c\nd,e", "[a|b|c][d|e]", .flexible = true);
	READ("a,b,c,", "[a|b|c|]");
	READ("a,b,c,\n", "[a|b|c|]");
	READ("a,b,c, ", "[a|b|c|]", .trim = true);
	READ("a,b,c, \n", "[a|b|c| ]");
	READ("x,y,z,w\nx,y,z,\nx,y,,\nx,,,\n,,,\n\"x\",\"y\",\"z\",\"w\"\n\"x\",\"y\",\"z\",\"\"\n\"x\",\"y\",\"\",\"\"\n\"x\",\"\",\"\",\"\"\n\"\",\"\",\"\",\"\"\n",
		"[x|y|z|w][x|y|z|][x|y||][x|||][|||][x|y|z|w][x|y|z|][x|y||][x|||][|||]");
	READ("a,b,\nc,d,e", "[a|b|][c|d|e]", .trim = true);
	READ("a,\"b\nc\"d,e", "error csv: 2:2: a \" in a quoted field must be doubled (\"a\"\"b\")");
	READ("A,\"Hello\r\nHi\",B\r\n", "[A|Hello\\nHi|B]");
	READ("x09\x41\xb4\x1c,aktau", "[x09A\xb4\x1c|aktau]");
	READ("field1,field2\r", "[field1|field2]");
	READ("\"field\"\r", "[field]");
	READ("\"field\"\r\r", "error csv: 1:7: a \" in a quoted field must be doubled (\"a\"\"b\")");
	READ("field\rfield\r", "[field\\rfield]");
	READ("field\r\rfield\r\r", "[field\\r\\rfield\\r]");
	READ("field\r\r\nfield\r\r\n", "[field\\r][field\\r]");
	READ("field\r\r\n\rfield\r\r\n\r", "[field\\r][\\rfield\\r]");
	READ("field\r\r\r\rfield\r\r\r\r", "[field\\r\\r\\r\\rfield\\r\\r\\r]");
	READ("field1,field2\r\r\n\r\rfield1,field2\r\r\n\r\r,", "[field1|field2\\r][\\r\\rfield1|field2\\r][\\r\\r|]");
	READ("a£b,c£ \td,e\n€ comment\n", "[a|b,c|d,e]", .sep = "£", .comment = "€", .trim = true);
	READ("a€\"  b,\"€ c\nλ comment\n", "[a|  b,| c]", .sep = "€", .comment = "λ");
	READ("\"abθcd\"λefθgh", "[abθcd|efθgh]", .sep = "λ", .comment = "€");
	READ("λ\nλ\nθ\nλ\n", "[λ][λ][λ]", .comment = "θ");
	READ("\"\n\n\n\n\"", "[\\n\\n\\n\\n]");
	READ("\r\n\r\n\r\n\r\n", "");
	READ("\"foo\"bar\"\r\n", "error csv: 1:5: a \" in a quoted field must be doubled (\"a\"\"b\")");
	READ("\"foo\"bar\"\r\n", "[foo\"bar]", .lazy = true);
	READ("\"foo\"\"bar\"\r\n", "[foo\"bar]");
	READ("\"\"\"\"\"\"\"\"", "[\"\"\"]");
	READ("\"\"\"\"\"\"\"", "error csv: 1:1: a quoted field that doesn't end");
	READ("\"\"\"\"\"\"\"", "[\"\"\"]", .lazy = true);
	READ("a", "error csv: a separator is one character, not a quote or a line break: \"\n\"", .sep = "\n");
	READ("a", "error csv: a separator is one character, not a quote or a line break: \",,\"", .sep = ",,");
	READ("a", "error csv: a comment is one character, not a quote, a line break or the separator: \",\"", .comment = ",");
	// A byte order mark at the start is dropped.
	READ("\xef\xbb\xbf" "a,b\n", "[a|b]");
}

static void write_case(const char *rows[][4], int n, const char *sep, bool crlf, const char *want, int line) {
	ak_arr *all = ak_arr_new(AK_K_REF, NULL, 0);
	for (int r = 0; r < n; r++) {
		ak_arr *row = ak_str_arr(0);
		for (int i = 0; i < 4 && rows[r][i]; i++) ak_str_arr_push(&row, rows[r][i], strlen(rows[r][i]));
		ak_arr_push(&all, (ak_val){.p = row});
	}
	ak_val args[5] = {{.p = all}, {.p = ak_str_c(sep)}, {.i = 1}, {.i = crlf}, {.p = ak_str_c("[s")}};
	ak_val v = fn("csv.write")(args, 5);
	ak_str *s = v.p;
	if (!s || strcmp(s->data, want) != 0) {
		fprintf(stderr, "csv_test.c:%d: %s:\n got: %s\nwant: %s\n", line, t_current, s ? s->data : "(error)", want);
		t_failed++;
	}
	ak_release(s);
	ak_release(args[0].p);
	ak_release(args[1].p);
	ak_release(args[4].p);
}

// Go's encoding/csv's writer cases: quoted only when a field must be.
TEST(csv_go_writer_cases) {
	write_case((const char *[][4]){{"abc"}}, 1, ",", false, "abc\n", __LINE__);
	write_case((const char *[][4]){{"abc"}}, 1, ",", true, "abc\r\n", __LINE__);
	write_case((const char *[][4]){{"\"abc\""}}, 1, ",", false, "\"\"\"abc\"\"\"\n", __LINE__);
	write_case((const char *[][4]){{"a\"b"}}, 1, ",", false, "\"a\"\"b\"\n", __LINE__);
	write_case((const char *[][4]){{" abc"}}, 1, ",", false, "\" abc\"\n", __LINE__);
	write_case((const char *[][4]){{"abc,def"}}, 1, ",", false, "\"abc,def\"\n", __LINE__);
	write_case((const char *[][4]){{"abc", "def"}}, 1, ",", false, "abc,def\n", __LINE__);
	write_case((const char *[][4]){{"abc"}, {"def"}}, 2, ",", false, "abc\ndef\n", __LINE__);
	write_case((const char *[][4]){{"abc\ndef"}}, 1, ",", false, "\"abc\ndef\"\n", __LINE__);
	write_case((const char *[][4]){{"abc\ndef"}}, 1, ",", true, "\"abc\r\ndef\"\r\n", __LINE__);
	write_case((const char *[][4]){{"abc\rdef"}}, 1, ",", true, "\"abc\rdef\"\r\n", __LINE__);
	write_case((const char *[][4]){{"abc\r\ndef"}}, 1, ",", true, "\"abc\r\ndef\"\r\n", __LINE__);
	write_case((const char *[][4]){{""}}, 1, ",", false, "\n", __LINE__);
	write_case((const char *[][4]){{"", ""}}, 1, ",", false, ",\n", __LINE__);
	write_case((const char *[][4]){{"", "", ""}}, 1, ",", false, ",,\n", __LINE__);
	write_case((const char *[][4]){{"a", "b", ""}}, 1, ",", false, "a,b,\n", __LINE__);
	write_case((const char *[][4]){{"\\."}}, 1, ",", false, "\"\\.\"\n", __LINE__);
	write_case((const char *[][4]){{"x09\x41\xb4\x1c", "aktau"}}, 1, ",", false, "x09\x41\xb4\x1c,aktau\n", __LINE__);
	write_case((const char *[][4]){{"a", "a,", "b"}}, 1, "|", false, "a|a,|b\n", __LINE__);
	write_case((const char *[][4]){{"a", "a|b", "c"}}, 1, "|", false, "a|\"a|b\"|c\n", __LINE__);
	write_case((const char *[][4]){{"a", "a€b", "\u00a0c"}}, 1, "€", false, "a€\"a€b\"€\"\u00a0c\"\n", __LINE__);
}

// Reading a large file holds its rows and one record's text: what it
// allocates is in proportion to its rows, not more.
TEST(csv_no_runaway_allocation) {
	ak_buf b = {0};
	for (int i = 0; i < 20000; i++) ak_buf_printf(&b, "%d,\"field %d, quoted\",x\n", i, i);
	ak_val args[10] = {
		{.p = ak_str_new((const char *)b.data, b.len)}, {.p = ak_str_c(",")}, {.p = ak_str_c("")}, {.i = 0}, {.i = 0}, {.i = 0}, {.i = 1}, {.p = ak_str_arr(0)}, {.i = -1}, {.p = ak_str_c("[s")},
	};
	uint64_t before = ak_alloc_count;
	ak_val v = fn("csv.read")(args, 10);
	uint64_t made = ak_alloc_count - before;
	REQUIRE(!ak_error_pending());
	CHECK_EQ_INT(((ak_arr *)v.p)->len, 20000);
	// Each row: its array and three strings, and the arrays' growth.
	CHECK(made < 20000 * 5 + 200);
	ak_release(v.p);
	for (int i = 0; i < 10; i++)
		if (i < 3 || i == 7 || i == 9) ak_release(args[i].p);
	ak_buf_free(&b);
}

T_MAIN()
