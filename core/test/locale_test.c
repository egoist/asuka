#define _POSIX_C_SOURCE 200809L  // strtok_r
// The language the app shows and its catalogs (locale.c, i18n.c,
// i18n_tags.c), what screen readers are told in it, and every case of core/test/testdata/catalog_cases.txt (what package i18n's
// Format says), said by the core from the compiled catalog.
#include "uih.h"
#include "ui/i18n.h"
#include "base/thread.h"

#include <stdlib.h>

void ak_locale_reset(void);

static char *read_file(const char *rel, size_t *len) {
	const char *root = getenv("ASUKA_ROOT");
	char path[1024];
	snprintf(path, sizeof path, "%s/%s", root ? root : "..", rel);
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *b = malloc((size_t)n + 1);
	size_t got = fread(b, 1, (size_t)n, f);
	b[got] = 0;
	fclose(f);
	if (len) *len = got;
	return b;
}

static ak_catalog *catalog;
static ak_span keys[64];
static uint32_t nkeys;
static ak_bytes *blob;

// load registers the compiled test catalog (its languages are the app's)
// and reads its keys.
static void load(void) {
	ak_locale_reset();
	ak_i18n_reset();
	ak_ui_set_direction(false);
	ak_zone_set_utc(true);  // the cases' dates are UTC's
	size_t n;
	char *b = read_file("core/test/testdata/catalog.akc", &n);
	if (!b) ak_panic("no catalog.akc");
	blob = ak_bytes_new(b, n);
	free(b);
	catalog = ak_i18n_catalog(blob);
	ak_reader r = {blob->data, blob->len, 4, false};
	ak_read_str(&r);
	nkeys = ak_read_u32(&r);
	for (uint32_t i = 0; i < nkeys && i < AK_LEN(keys); i++) keys[i] = ak_read_str(&r);
}

static void unload(void) {
	ak_release(catalog);
	ak_release(blob);
	catalog = NULL;
	ak_i18n_reset();
	ak_locale_reset();
	ak_ui_set_direction(false);
}

static int64_t key_index(const char *k) {
	for (uint32_t i = 0; i < nkeys; i++)
		if (ak_span_eqc(keys[i], k)) return i;
	ak_panic("no key %s", k);
}

// ---- Every case Go's Format says ----

static const uint8_t tuple_kinds[3][8] = {{AK_K_INT}, {AK_K_FLOAT}, {AK_K_REF}};

TEST(catalog_cases) {
	setup();
	load();
	char *text = read_file("core/test/testdata/catalog_cases.txt", NULL);
	REQUIRE(text);
	int cases = 0, failures = 0;
	for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
		if (line[0] == '#') continue;
		char *fields[4] = {0};
		int nf = 0;
		for (char *p = line; nf < 4; nf++) {
			fields[nf] = p;
			char *tab = strchr(p, '\t');
			if (!tab) {
				nf++;
				break;
			}
			*tab = 0;
			p = tab + 1;
		}
		if (nf < 4) {
			T_FAIL("a short line");
			continue;
		}
		// The arguments: a tuple of their kinds.
		uint8_t kinds[8];
		ak_val vals[8];
		uint16_t nargs = 0;
		for (char *a = fields[2]; *a && nargs < 8;) {
			char *comma = strchr(a, ',');
			if (comma) *comma = 0;
			char k = a[0];
			const char *v = a + 2;
			if (k == 'i') kinds[nargs] = AK_K_INT, vals[nargs].i = strtoll(v, NULL, 10);
			else if (k == 'f') kinds[nargs] = AK_K_FLOAT, vals[nargs].f = strtod(v, NULL);
			else kinds[nargs] = AK_K_REF, vals[nargs].p = ak_str_c(v);
			nargs++;
			if (!comma) break;
			a = comma + 1;
		}
		ak_shape shape = {"Args", nargs, kinds, NULL, NULL, 0, NULL, false, NULL};
		ak_struct *tuple = ak_struct_new(&shape);
		for (uint16_t i = 0; i < nargs; i++) tuple->f[i] = vals[i];
		ak_str *loc = ak_str_c(fields[0]);
		ak_str *got = ak_i18n_say(catalog, key_index(fields[1]), (ak_val){.p = tuple}, loc);
		if (strcmp(got->data, fields[3]) != 0) {
			if (failures++ < 25) T_FAIL("%s %s (%s): %s, want %s", fields[0], fields[1], fields[2], got->data, fields[3]);
		}
		ak_release(got);
		ak_release(loc);
		ak_release(tuple);
		cases++;
	}
	AK_UNUSED(tuple_kinds);
	CHECK(cases > 300);
	free(text);
	unload();
}

// ---- The locale model ----

static ak_val empty_app(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	return (ak_val){.p = ak_ui_new(AK_EL_COLUMN)};
}

static ak_uitest *mount_reading(const char *languages, ak_invoke_fn build) {
	setup();
	ak_environment env = {.width = 390, .height = 844, .scale = 3, .font_scale = 1, .languages = ak_cstr(languages), .platform = AK_PLATFORM_IOS};
	ak_closure *c = fn(build, NULL);
	ak_uitest *a = ak_uitest_mount(c, &env);
	ak_release(c);
	return a;
}

static ak_span last_cmd(ak_fake *f, ak_cmd c, int *count) {
	ak_fake_index_vec cmds = {0};
	ak_fake_commands(f, c, &cmds);
	*count = (int)cmds.len;
	ak_span s = cmds.len ? ak_fake_op_at(f, cmds.data[cmds.len - 1]).value.s : (ak_span){0};
	ak_vec_free(&cmds);
	return s;
}

static void check_locale(const char *want) {
	ak_str *l = ak_ui_locale();
	CHECK_EQ_STR(l->data, want);
	ak_release(l);
}

// The language shown is what the app has of what the user reads, in
// their order; another script doesn't suit; none, the app's own.
TEST(locale_matches_the_apps_languages) {
	static const char *const cases[][2] = {
		{"de-AT,en", "de"}, {"zh-TW", "zh-Hant"}, {"fr,de", "fr"}, {"ja", "en"}, {"ar-EG,en", "ar"},
		{"en-GB,de", "en-GB"}, {"zh-Hans,en", "en"}, {"pt-BR,ru", "ru"}, {"en-AU", "en"},
	};
	for (size_t i = 0; i < AK_LEN(cases); i++) {
		load();
		ak_uitest *a = mount_reading(cases[i][0], empty_app);
		ak_str *l = ak_ui_locale();
		if (strcmp(l->data, cases[i][1]) != 0) T_FAIL("reading %s: %s, want %s", cases[i][0], l->data, cases[i][1]);
		ak_release(l);
		int n;
		ak_span s = last_cmd(ak_uitest_host(a), AK_CMD_LOCALE, &n);
		if (n != 1 || !ak_span_eq((ak_span){s.p + 1, s.len - 1}, ak_cstr(cases[i][1]))) T_FAIL("reading %s: %d Locale commands", cases[i][0], n);
		done(a);
		unload();
	}
}

// setLanguages' tags are the app's constants, which go with its module on
// a reload (asuka dev): the languages outlive them.
TEST(languages_outlive_the_module) {
	load();
	ak_str *tags[] = {ak_str_c("zh-Hans"), ak_str_c("ja")};
	ak_arr *a = ak_arr_new(AK_K_REF, NULL, 2);
	a->h.flags = 1;
	for (size_t i = 0; i < AK_LEN(tags); i++) {
		ak_make_immortal(tags[i]);  // as a module's strings are
		ak_arr_push(&a, (ak_val){.p = tags[i]});
	}
	ak_ui_set_languages(a);
	REQUIRE(!ak_error_pending());
	ak_release(a);
	for (size_t i = 0; i < AK_LEN(tags); i++) ak_free(tags[i]);  // the module released
	ak_arr *l = ak_ui_languages();
	REQUIRE(l->len == 2);
	CHECK_EQ_STR(((ak_str *)ak_arr_vals(l)[0].p)->data, "zh-Hans");
	CHECK_EQ_STR(((ak_str *)ak_arr_vals(l)[1].p)->data, "ja");
	ak_release(l);
	unload();
}

// ---- Messages follow the language (TestMessagesFollowTheLanguage) ----

static void *count;
static int builds;

static ak_val say0(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(a), AK_UNUSED(n);
	return (ak_val){.p = ak_i18n_say(catalog, key_index((const char *)c->fn), (ak_val){0}, NULL)};
}

static const uint8_t int_kind[] = {AK_K_INT};
static const ak_shape n_shape = {"Args", 1, int_kind, NULL, NULL, 0, NULL, false, NULL};

static ak_val say_files(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_struct *t = ak_struct_new(&n_shape);
	t->f[0].i = ak_vm_cell_get(count, true).i;
	ak_str *s = ak_i18n_say(catalog, key_index("files"), (ak_val){.p = t}, NULL);
	ak_release(t);
	return (ak_val){.p = s};
}

static ak_val messages_app(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	builds++;
	ak_view *col = ak_ui_new(AK_EL_COLUMN);
	ak_view *t1 = ak_ui_new(AK_EL_TEXT);
	bind(t1, AK_KEY_TEXT_TEXT, say0, "hello");
	add(col, t1);
	ak_view *t2 = ak_ui_new(AK_EL_TEXT);
	bind(t2, AK_KEY_TEXT_TEXT, say_files, NULL);
	add(col, t2);
	ak_view *b = ak_ui_new(AK_EL_BUTTON);
	bind(b, AK_KEY_BUTTON_TITLE, say0, "hello");
	add(col, b);
	// A control's label follows too, and leaves its value (accessibility_test.go).
	ak_view *tf = ak_ui_new(AK_EL_TEXT_FIELD);
	set_str(tf, AK_KEY_TEXT_FIELD_TEXT, "Grace");
	bind(tf, AK_KEY_LABEL, say0, "hello");
	add(col, tf);
	return (ak_val){.p = col};
}

// A message said in a binding follows the language, as it follows its
// arguments; a change costs a prop each and no rebuild.
TEST(messages_follow_the_language) {
	load();
	builds = 0;
	count = cell_int(1);
	ak_uitest *a = mount_reading("en", messages_app);
	ak_fake *f = ak_uitest_host(a);
	check_text(root(a), "Hello|1 file|Hello");
	ak_fake_reset(f);
	ak_str *de = ak_str_c("de");
	ak_ui_set_locale(de);
	ak_release(de);
	ak_ui_flush();
	check_text(root(a), "Hallo|1 Datei|Hallo");
	CHECK_EQ_INT(builds, 1);
	CHECK_EQ_INT(ak_fake_count(f, AK_OP_SET_PROP), 4);  // three messages' and the field's label
	ak_fake_view *tf = nth(a, AK_KIND_TEXT_FIELD, 0);
	CHECK(ak_span_eqc(ak_fake_string(tf, AK_PROP_A11Y_LABEL), "Hallo") && ak_span_eqc(ak_fake_string(tf, AK_PROP_TEXT_FIELD_VALUE), "Grace"));
	write_i(count, 3);
	ak_ui_flush();
	check_text(root(a), "Hallo|3 Dateien|Hallo");
	done(a);
	ak_release(count);
	unload();
}

// setLocale asks the host to keep the language, and "" to follow the
// user's again; a tag of no language throws.
TEST(set_locale) {
	load();
	ak_uitest *a = mount_reading("de,en", empty_app);
	ak_fake *f = ak_uitest_host(a);
	check_locale("de");
	ak_str *en = ak_str_c("en"), *none = ak_str_c(""), *bad = ak_str_c("not a language!");
	ak_ui_set_locale(en);
	ak_ui_flush();
	check_locale("en");
	int n;
	ak_span s = last_cmd(f, AK_CMD_SAVE_LOCALE, &n);
	CHECK(n == 1 && ak_span_eqc(s, "en"));
	ak_ui_set_locale(none);
	ak_ui_flush();
	check_locale("de");
	s = last_cmd(f, AK_CMD_SAVE_LOCALE, &n);
	CHECK(n == 2 && s.len == 0);
	ak_ui_set_locale(bad);
	CHECK(ak_error_pending());
	ak_release(ak_error_take());
	ak_release(en), ak_release(none), ak_release(bad);
	done(a);
	unload();
}

static ak_val hello_app(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_view *t = ak_ui_new(AK_EL_TEXT);
	bind(t, AK_KEY_TEXT_TEXT, say0, "hello");
	return (ak_val){.p = t};
}

// The environment's languages change while the app runs.
TEST(locale_follows_the_environment) {
	load();
	ak_uitest *a = mount_reading("en", hello_app);
	ak_fake *f = ak_uitest_host(a);
	ak_environment env = f->env;
	env.languages = AK_STR("de");
	ak_fake_set_environment(f, &env);
	check_text(root(a), "Hallo");
	done(a);
	unload();
}

// The pseudo locales: ar-XB right to left, en-XA left to right, and
// their messages transformed.
TEST(pseudo_locale) {
	load();
	ak_uitest *a = mount_reading("en", hello_app);
	ak_str *xb = ak_str_c("ar-XB"), *xa = ak_str_c("en-XA");
	ak_ui_set_locale(xb);
	ak_ui_flush();
	CHECK(ak_ui_is_rtl());
	ak_fake_view *t = nth(a, AK_KIND_TEXT, 0);
	CHECK(!ak_span_eqc(ak_fake_string(t, AK_PROP_TEXT_TEXT), "Hello"));
	ak_ui_set_locale(xa);
	ak_ui_flush();
	CHECK(!ak_ui_is_rtl());
	CHECK(ak_span_eqc(ak_fake_string(t, AK_PROP_TEXT_TEXT), "[Ĥéļļö one]"));
	ak_release(xb), ak_release(xa);
	done(a);
	unload();
}

static void check_str(ak_str *s, const char *want) {
	if (strcmp(s->data, want) != 0) T_FAIL("%s, want %s", s->data, want);
	ak_release(s);
}

// Every case of relative_cases.txt (what package i18n's FormatRelative
// says), said by the core in the language shown, with the catalog's
// data for it.
TEST(relative_cases) {
	load();
	ak_uitest *a = mount_reading("en", empty_app);
	char *text = read_file("core/test/testdata/relative_cases.txt", NULL);
	REQUIRE(text);
	int cases = 0, failures = 0;
	char *save = NULL;
	for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		if (line[0] == '#') continue;
		char *f[6] = {0};
		int nf = 0;
		for (char *p = line; p && nf < 6; nf++) {
			f[nf] = p;
			char *tab = strchr(p, '\t');
			if (tab) *tab = 0;
			p = tab ? tab + 1 : NULL;
		}
		if (nf < 6) {
			T_FAIL("a short line");
			continue;
		}
		ak_str *loc = ak_str_c(f[0]);
		ak_ui_set_locale(loc);
		ak_str *got = ak_i18n_format_relative(strtoll(f[1], NULL, 10), strtoll(f[2], NULL, 10), atoi(f[3]), atoi(f[4]) != 0);
		if (strcmp(got->data, f[5]) != 0 && failures++ < 25) T_FAIL("%s %s from %s (style %s, numeric %s): %s, want %s", f[0], f[1], f[2], f[3], f[4], got->data, f[5]);
		ak_release(got);
		ak_release(loc);
		cases++;
	}
	CHECK(cases > 1000);
	free(text);
	done(a);
	unload();
}

// The unit of a relative time, on the device's calendar.
TEST(relative_units) {
	ak_zone_set_utc(true);
	int64_t now = 1773576000000000000;  // 2026-03-15 12:00 UTC
	int64_t h = 3600000000000, n;
	CHECK_EQ_INT(ak_i18n_relative_unit(now, now, &n), 0);
	CHECK_EQ_INT(n, 0);
	CHECK_EQ_INT(ak_i18n_relative_unit(now - 13 * h, now, &n), 2);  // yesterday 23:00: hours
	CHECK_EQ_INT(n, -13);
	CHECK_EQ_INT(ak_i18n_relative_unit(now + 47 * h, now, &n), 3);
	CHECK_EQ_INT(n, 1);
	CHECK_EQ_INT(ak_i18n_relative_unit(now - 20 * 24 * h, now, &n), 4);
	CHECK_EQ_INT(n, -2);
	CHECK_EQ_INT(ak_i18n_relative_unit(now - 400 * 24 * h, now, &n), 6);
	CHECK_EQ_INT(n, -1);
	// No catalog: English.
	ak_locale_reset();
	ak_i18n_reset();
	ak_str *s = ak_i18n_format_relative(now - 3 * h, now, 0, false);
	CHECK_EQ_STR(s->data, "3 hours ago");
	ak_release(s);
	s = ak_i18n_format_relative(now - 24 * h, now, 0, false);
	CHECK_EQ_STR(s->data, "yesterday");
	ak_release(s);
	s = ak_i18n_format_relative(now + 60000000000, now, 0, true);
	CHECK_EQ_STR(s->data, "in 1 minute");
	ak_release(s);
}

// Numbers and dates outside messages (std/i18n): in the language shown,
// with the catalog's data for it, in the user's hour cycle.
TEST(formatting) {
	load();
	ak_uitest *a = mount_reading("de-DE", empty_app);
	check_str(ak_i18n_format_number(1234567.891, 0, 0, 3), "1.234.567,891");
	check_str(ak_i18n_format_number(0.256, 1, 0, 0), "26\u00a0%");
	check_str(ak_i18n_format_number(2.5, 0, 2, 2), "2,50");
	check_str(ak_i18n_format_int(-1234567, 0), "-1.234.567");
	check_str(ak_i18n_format_int(1234567, 4), "1234567");
	int64_t t = 1767618429000000000;  // 2026-01-05 13:07:09 UTC
	check_str(ak_i18n_format_date(t, 2, 0), "05.01.2026");
	check_str(ak_i18n_format_date(t, 4, 1), "Montag, 5. Januar 2026 um 13:07");
	check_str(ak_i18n_format_date(t, 0, 2), "13:07:09");
	// Another reader's hour cycle: their regional formats'.
	ak_fake *f = ak_uitest_host(a);
	ak_environment env = f->env;
	env.hour_cycle = 12;
	ak_fake_set_environment(f, &env);
	check_str(ak_i18n_format_date(t, 0, 1), "01:07 PM");  // as Go writes de-u-hc-h12
	ak_str *hi = ak_str_c("hi"), *ar = ak_str_c("ar"), *xx = ak_str_c("zz-Qaaa");
	ak_ui_set_locale(hi);
	check_str(ak_i18n_format_number(1234567, 0, 0, 3), "12,34,567");
	ak_ui_set_locale(ar);
	check_str(ak_i18n_format_date(t, 1, 0), "5\u200f/1\u200f/2026");
	// Languages by their own names.
	ak_str *de = ak_str_c("de"), *zh = ak_str_c("zh-Hant");
	check_str(ak_i18n_autonym(de), "Deutsch");
	check_str(ak_i18n_autonym(zh), "繁體中文");
	check_str(ak_i18n_autonym(ar), "العربية");
	check_str(ak_i18n_autonym(xx), "zz-Qaaa");
	ak_release(hi), ak_release(ar), ak_release(xx), ak_release(de), ak_release(zh);
	done(a);
	unload();
	// Without catalogs: Latin digits, English's patterns, ISO dates.
	ak_locale_reset();
	ak_i18n_reset();
	check_str(ak_i18n_format_number(1234.5, 0, 0, 3), "1,234.5");
	check_str(ak_i18n_format_date(t, 2, 0), "2026-01-05 13:07");
	// Any double, however large or small, in its shortest digits and the
	// zeros after or ahead of them (Go's FormatFloat(v, 'f', -1, 64)).
	char want[400];
	snprintf(want, sizeof want, "1%0300d", 0);
	check_str(ak_i18n_format_number(1e300, 4, 0, 0), want);
	snprintf(want, sizeof want, "-1%063d", 0);
	check_str(ak_i18n_format_number(-1e63, 4, 0, 0), want);
	check_str(ak_i18n_format_number(1.2345678901234567e25, 4, 0, 0), "12345678901234566000000000");
	check_str(ak_i18n_format_number(1.7976931348623157e308, 0, 0, 0), "179,769,313,486,231,570,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000,000");
	check_str(ak_i18n_format_number(1e-25, 0, 0, 30), "0.0000000000000000000000001");
	check_str(ak_i18n_format_number(0.1 + 0.2, 0, 0, 20), "0.30000000000000004");
	check_str(ak_i18n_format_number(-0.5, 0, 0, 3), "-0.5");
}

// The regional formats, with the rg keyword's region, and the locale to
// format in.
TEST(region) {
	load();
	setup();
	ak_environment env = {.width = 390, .height = 844, .scale = 3, .font_scale = 1, .languages = AK_STR("en"), .locale = AK_STR("en_DE@rg=gbzzzz")};
	ak_closure *c = fn(empty_app, NULL);
	ak_uitest *a = ak_uitest_mount(c, &env);
	ak_release(c);
	ak_val r = ak_ui_region();
	const ak_struct *s = r.p;
	CHECK_EQ_STR(((ak_str *)s->f[0].p)->data, "en-GB");
	CHECK_EQ_INT(s->f[1].i, 2);  // Monday
	CHECK_EQ_INT(s->f[2].i, 24);
	ak_release(r.p);
	ak_str *fl = ak_ui_format_locale();
	CHECK_EQ_STR(fl->data, "en-GB-u-fw-mon-hc-h23");
	ak_release(fl);
	done(a);
	unload();
	// What CLDR says where the system doesn't.
	CHECK_EQ_INT(ak_i18n_first_weekday(AK_STR("en")), 1);  // the United States: Sunday
	CHECK_EQ_INT(ak_i18n_first_weekday(AK_STR("de")), 2);
	CHECK_EQ_INT(ak_i18n_hour_cycle(AK_STR("en")), 12);
	CHECK_EQ_INT(ak_i18n_hour_cycle(AK_STR("de")), 24);
	CHECK_EQ_INT(ak_i18n_first_weekday(AK_STR("ar-EG")), 7);  // Saturday
}

// ---- Right to left (TestRightToLeftLayout) ----

static ak_view *labelled(const char *label, float w, float h) {
	ak_view *v = ak_ui_new(AK_EL_VIEW);
	set_str(v, AK_KEY_LABEL, label);
	if (w) set_i(v, AK_KEY_WIDTH, ak_length_pt(w));
	if (h) set_i(v, AK_KEY_HEIGHT, ak_length_pt(h));
	return v;
}

static ak_val rtl_app(ak_closure *c, ak_val *a, uint32_t n) {
	AK_UNUSED(c), AK_UNUSED(a), AK_UNUSED(n);
	ak_view *col = ak_ui_new(AK_EL_COLUMN);
	ak_view *row = ak_ui_new(AK_EL_ROW);
	set_i(row, AK_KEY_HEIGHT, ak_length_pt(20));
	add(row, labelled("first", 30, 0));
	add(row, labelled("second", 40, 0));
	add(col, row);
	ak_view *logical = labelled("logical", 0, 10);
	set(logical, AK_KEY_MARGIN, edges_se(5, 7));
	add(col, logical);
	ak_view *physical = labelled("physical", 0, 10);
	set(physical, AK_KEY_MARGIN, edges4(0, 7, 0, 5));
	add(col, physical);
	return (ak_val){.p = col};
}

static float x_of(ak_uitest *a, const char *label) {
	ak_fake_view_vec vs = {0};
	ak_fake_of_kind(ak_uitest_host(a), AK_KIND_VIEW, &vs);
	float x = -1;
	for (uint32_t i = 0; i < vs.len; i++)
		if (ak_span_eqc(ak_fake_string(vs.data[i], AK_PROP_A11Y_LABEL), label)) x = vs.data[i]->frame[0];
	ak_vec_free(&vs);
	return x;
}

// A row runs from the right in a right-to-left language, start and end
// mirror, left and right don't; the host is told with Locale.
TEST(right_to_left_layout) {
	load();
	ak_uitest *a = mount_reading("en", rtl_app);
	CHECK(x_of(a, "first") == 0 && x_of(a, "second") == 30 && x_of(a, "logical") == 5 && x_of(a, "physical") == 5);
	ak_str *ar = ak_str_c("ar");
	ak_ui_set_locale(ar);
	ak_release(ar);
	ak_ui_flush();
	CHECK(ak_ui_is_rtl());
	CHECK(x_of(a, "first") == 390 - 30 && x_of(a, "second") == 390 - 70 && x_of(a, "logical") == 7 && x_of(a, "physical") == 5);
	int n;
	ak_span s = last_cmd(ak_uitest_host(a), AK_CMD_LOCALE, &n);
	CHECK(s.len == 3 && s.p[0] == 1 && s.p[1] == 'a' && s.p[2] == 'r');
	ak_str *en = ak_str_c("en");
	ak_ui_set_locale(en);
	ak_release(en);
	ak_ui_flush();
	CHECK(x_of(a, "first") == 0 && x_of(a, "logical") == 5);
	done(a);
	unload();
}

// ---- Tags ----

TEST(tags) {
	struct {
		const char *in, *want;
	} norm[] = {{"en_US", "en-US"}, {"zh-hant-tw", "zh-Hant-TW"}, {"iw", "he"}, {"de_DE.UTF-8@euro", "de-DE"}, {"C", NULL}, {"", NULL}, {"und", NULL}};
	for (size_t i = 0; i < AK_LEN(norm); i++) {
		ak_str *n = ak_i18n_normalize(ak_cstr(norm[i].in));
		CHECK_EQ_STR(n ? n->data : NULL, norm[i].want);
		ak_release(n);
	}
	CHECK(ak_i18n_rtl(AK_STR("ar")) && ak_i18n_rtl(AK_STR("he-IL")) && ak_i18n_rtl(AK_STR("ar-XB")) && ak_i18n_rtl(AK_STR("pa-Arab")));
	CHECK(!ak_i18n_rtl(AK_STR("en")) && !ak_i18n_rtl(AK_STR("pa")) && !ak_i18n_rtl(AK_STR("az-Latn")));
	ak_str *rl = ak_i18n_region_locale(AK_STR("en_US@rg=gbzzzz"));
	CHECK_EQ_STR(rl->data, "en-GB");
	ak_release(rl);
	ak_operands o = ak_operands_dec("1.50", 4);
	CHECK(o.i == 1 && o.v == 2 && o.w == 1 && o.f == 50 && o.t == 5);
	ak_operands one = ak_operands_int(1), two = ak_operands_int(2);
	CHECK(ak_plural(AK_STR("en"), &one, false) == AK_PLURAL_ONE && ak_plural(AK_STR("ja"), &one, false) == AK_PLURAL_OTHER);
	CHECK(ak_plural(AK_STR("en-US"), &two, true) == AK_PLURAL_TWO);
	CHECK(ak_plural(AK_STR("ar"), &two, false) == AK_PLURAL_TWO);
}

// ---- Plural rules: every sample CLDR gives of every language's
// categories (i18n/testdata/plural_samples.txt, which package i18n's
// rules are tested against too), cardinal and ordinal ----

static const char *const plural_names[] = {"zero", "one", "two", "few", "many", "other"};

static int plural_index(const char *name) {
	for (int i = 0; i < 6; i++)
		if (strcmp(plural_names[i], name) == 0) return i;
	return -1;
}

// one_sample calls f for a sample, or each of a range's: 2~16 every
// integer, 0.0~1.5 every tenth.
static void one_sample(char *item, void (*f)(const char *n, void *ctx), void *ctx) {
	while (*item == ' ') item++;
	char *end = item + strlen(item);
	while (end > item && end[-1] == ' ') *--end = 0;
	if (!*item || strncmp(item, "\xE2\x80\xA6", 3) == 0) return;  // …
	char *tilde = strchr(item, '~');
	if (!tilde) {
		f(item, ctx);
		return;
	}
	*tilde = 0;
	const char *lo = item, *hi = tilde + 1;
	const char *dot = strchr(lo, '.');
	int frac = dot ? (int)strlen(dot + 1) : 0;
	char digits[32];
	size_t n = 0;
	for (const char *p = lo; *p && n + 1 < sizeof digits; p++)
		if (*p != '.') digits[n++] = *p;
	digits[n] = 0;
	long long a = strtoll(digits, NULL, 10);
	n = 0;
	for (const char *p = hi; *p && n + 1 < sizeof digits; p++)
		if (*p != '.') digits[n++] = *p;
	digits[n] = 0;
	long long z = strtoll(digits, NULL, 10);
	for (long long v = a; v <= z; v++) {
		char out[48];
		if (frac) {
			long long scale = 1;
			for (int i = 0; i < frac; i++) scale *= 10;
			snprintf(out, sizeof out, "%lld.%0*lld", v / scale, frac, v % scale);
		} else {
			snprintf(out, sizeof out, "%lld", v);
		}
		f(out, ctx);
	}
}

// each_sample calls f for each number of samples ("@integer 0, 2~16, …
// @decimal 0.0~1.5"), as plural_test.go's expandSamples writes them out.
static void each_sample(char *samples, void (*f)(const char *n, void *ctx), void *ctx) {
	char *save_g = NULL;
	for (char *group = strtok_r(samples, "@", &save_g); group; group = strtok_r(NULL, "@", &save_g)) {
		char *list = strchr(group, ' ');  // after "integer" or "decimal"
		if (!list) continue;
		char *save = NULL;
		for (char *item = strtok_r(list, ",", &save); item; item = strtok_r(NULL, ",", &save)) one_sample(item, f, ctx);
	}
}

typedef struct {
	char *langs;
	int want;
	bool ordinal;
	int checked, failures;
} sample_ctx;

static void check_sample(const char *num, void *p) {
	sample_ctx *c = p;
	ak_operands o = ak_operands_dec(num, strlen(num));
	char langs[2048];
	snprintf(langs, sizeof langs, "%s", c->langs);
	char *save = NULL;
	for (char *lang = strtok_r(langs, ",", &save); lang; lang = strtok_r(NULL, ",", &save)) {
		int got = ak_plural(ak_cstr(lang), &o, c->ordinal);
		if (got != c->want && c->failures++ < 20)
			T_FAIL("%s %s %s: %s, want %s", lang, c->ordinal ? "ordinal" : "cardinal", num, plural_names[got], plural_names[c->want]);
		c->checked++;
	}
}

TEST(plural_samples) {
	char *text = read_file("i18n/testdata/plural_samples.txt", NULL);
	REQUIRE(text);
	int checked = 0, failures = 0;
	char *save = NULL;
	for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		if (line[0] == '#' || !line[0]) continue;
		// kind, languages, category, samples
		char *kind = line, *langs = strchr(kind, ' ');
		if (!langs) continue;
		*langs++ = 0;
		char *cat = strchr(langs, ' ');
		if (!cat) continue;
		*cat++ = 0;
		char *samples = strchr(cat, ' ');
		if (!samples) continue;
		*samples++ = 0;
		sample_ctx c = {langs, plural_index(cat), strcmp(kind, "ordinal") == 0, 0, 0};
		REQUIRE(c.want >= 0);
		each_sample(samples, check_sample, &c);
		checked += c.checked, failures += c.failures;
	}
	free(text);
	CHECK(failures == 0);
	if (checked < 10000) T_FAIL("checked %d samples only", checked);
	// A region's or a script's rules: its own when CLDR has them, else its
	// language's (as ICU looks them up).
	ak_operands zero = ak_operands_int(0), one5 = ak_operands_dec("1.5", 3), many = ak_operands_int(1000000);
	CHECK(ak_plural(AK_STR("pt-PT"), &zero, false) == AK_PLURAL_OTHER);  // pt-PT's: i = 1 and v = 0
	CHECK(ak_plural(AK_STR("pt-BR"), &zero, false) == AK_PLURAL_ONE);    // pt's: i = 0..1
	CHECK(ak_plural(AK_STR("fr-CA"), &one5, false) == AK_PLURAL_ONE);
	CHECK(ak_plural(AK_STR("fr"), &many, false) == AK_PLURAL_MANY);
	CHECK(ak_plural(AK_STR("sr-Latn-RS"), &zero, false) == AK_PLURAL_OTHER);
	CHECK(ak_plural(AK_STR("zh-Hant-TW"), &zero, false) == AK_PLURAL_OTHER);
	CHECK(ak_plural(AK_STR("xx-unknown"), &zero, false) == AK_PLURAL_OTHER);
	CHECK(ak_plural(AK_STR("en-u-nu-latn"), &one5, false) == AK_PLURAL_OTHER);
}

// ---- Catalogs and numbers that ask too much ----

// A catalog's counts beyond its bytes are an error, not memory read or
// freed past what was allocated.
TEST(catalog_counts_beyond_its_bytes) {
	static const uint8_t langs[] = {'A', 'K', 'C', '2', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 0xff, 0xff};
	static const uint8_t keys[] = {'A', 'K', 'C', '2', 0, 0, 0, 0, 0xff, 0xff, 0xff, 0x0f, 0, 0, 0, 0, 0, 0, 0, 0};
	const uint8_t *cases[] = {langs, keys};
	for (int i = 0; i < 2; i++) {
		ak_bytes *b = ak_bytes_new(cases[i], sizeof langs);
		CHECK(ak_i18n_catalog(b) == NULL);
		void *e = ak_error_take();
		CHECK(e != NULL);
		ak_release(e);
		ak_release(b);
	}
}

static void put_str(ak_buf *b, const char *s) { ak_buf_str(b, s, strlen(s)); }

// A catalog of one language, "zz", whose patterns have min_int integer
// digits at least.
static ak_bytes *catalog_min_int(uint8_t min_int) {
	ak_buf b = {0};
	ak_buf_put(&b, "AKC4", 4);
	put_str(&b, "zz");
	ak_buf_u32(&b, 0);  // keys
	ak_buf_u32(&b, 0);  // skeletons
	ak_buf_u32(&b, 1);  // languages
	put_str(&b, "zz");
	ak_buf_u8(&b, 0);
	put_str(&b, "Zz");
	for (int d = 0; d < 10; d++) ak_buf_str(&b, &"0123456789"[d], 1);
	put_str(&b, ".");
	put_str(&b, ",");
	ak_buf_u8(&b, 1);
	for (int pat = 0; pat < 4; pat++) {  // decimal, percent, currency, a currency's of letters
		put_str(&b, pat >= 2 ? "\302\244" : ""), put_str(&b, ""), put_str(&b, "-"), put_str(&b, "");
		ak_buf_u8(&b, 3), ak_buf_u8(&b, 3), ak_buf_u8(&b, min_int), ak_buf_u8(&b, 3);
	}
	put_str(&b, " "), put_str(&b, ""), put_str(&b, "");  // currency spacing, separators
	ak_buf_u32(&b, 0);  // currencies
	ak_buf_u8(&b, 0);   // no dates
	ak_buf_u32(&b, 0);  // messages
	ak_bytes *out = ak_bytes_new(b.data, b.len);
	ak_buf_free(&b);
	return out;
}

// Fraction digits: at most 100, as Intl.NumberFormat takes; integer
// digits: as many as the catalog's pattern asks (a u8).
TEST(number_digits_asked_for) {
	ak_locale_reset();
	ak_i18n_reset();
	ak_str *s = ak_i18n_format_number(1.5, 0, 200, 300);
	CHECK_EQ_INT(s->len, 2 + 100);
	CHECK(strncmp(s->data, "1.5000", 6) == 0 && s->data[s->len - 1] == '0');
	ak_release(s);
	s = ak_i18n_format_number(0.5, 1, 150, 150);  // a percentage's
	CHECK_EQ_INT(s->len, 3 + 100 + 1);
	ak_release(s);
	s = ak_i18n_format_number(99.99999, 0, 0, 2);
	CHECK_EQ_STR(s->data, "100");
	ak_release(s);
	ak_bytes *blob = catalog_min_int(255);
	ak_catalog *c = ak_i18n_catalog(blob);
	REQUIRE(c != NULL);
	ak_str *zz = ak_str_c("zz");
	ak_ui_set_locale(zz);
	s = ak_i18n_format_number(12345678901234567890.0, 4, 100, 100);
	CHECK_EQ_INT(s->len, 255 + 1 + 100);
	CHECK(s->data[0] == '0' && s->data[255] == '.');
	ak_release(s);
	s = ak_i18n_format_int(INT64_MIN, 4);
	CHECK_EQ_INT(s->len, 1 + 255);
	ak_release(s);
	ak_release(zz);
	ak_release(c);
	ak_release(blob);
	ak_i18n_reset();
	ak_locale_reset();
}

// Amounts of a currency without catalogs: English's pattern and symbols,
// the currency's fraction digits; a symbol of letters spaced from the
// number; a number of any length, exact; what isn't one, an error.
TEST(format_decimal) {
	ak_locale_reset();
	ak_i18n_reset();
	static const char *const cases[][3] = {
	    {"1234.5", "USD", "$1,234.50"},
	    {"-1234.5", "EUR", "-\342\202\2541,234.50"},
	    {"1234.5", "JPY", "\302\2451,235"},
	    {"1.2345", "KWD", "KWD\302\2401.235"},
	    {"12", "CHF", "CHF\302\24012.00"},
	    {"-0.125", "CHF", "-CHF\302\2400.13"},
	    {"5", "XYZ", "XYZ\302\2405.00"},
	    {"1234.5678", "", "1,234.568"},
	    {"123456789012345678901234567890.25", "USD", "$123,456,789,012,345,678,901,234,567,890.25"},
	};
	for (size_t i = 0; i < AK_LEN(cases); i++) {
		ak_str *d = ak_str_c(cases[i][0]), *c = ak_str_c(cases[i][1]);
		ak_str *s = ak_i18n_format_decimal(d, 0, 0, 0, c);
		if (!s || strcmp(s->data, cases[i][2]) != 0) T_FAIL("%s %s: %s, want %s", cases[i][0], cases[i][1], s ? s->data : "(error)", cases[i][2]);
		ak_release(s), ak_release(d), ak_release(c);
	}
	// Fraction digits asked for; a number longer than a frame's.
	ak_str *d = ak_str_c("19.999"), *c = ak_str_c("EUR");
	ak_str *s = ak_i18n_format_decimal(d, 16, 3, 3, c);
	CHECK_EQ_STR(s->data, "\342\202\25419.999");
	ak_release(s), ak_release(d);
	char big[1200];
	memset(big, '9', 1000);
	strcpy(big + 1000, ".999");  // rounded up: a carry through every digit
	d = ak_str_c(big);
	s = ak_i18n_format_decimal(d, 4, 0, 0, c);
	REQUIRE(s != NULL);
	CHECK_EQ_INT(s->len, 3 + 1001 + 3);  // €, a carry and 1000 zeros, .00
	CHECK(strncmp(s->data, "\342\202\2541000", 7) == 0);
	ak_release(s), ak_release(d);
	static const char *const bad[] = {"", "-", "1.", ".5", "1e5", "1.2.3", "12a", "--1"};
	for (size_t i = 0; i < AK_LEN(bad); i++) {
		d = ak_str_c(bad[i]);
		s = ak_i18n_format_decimal(d, 0, 0, 0, c);
		void *e = ak_error_take();
		if (s || !e) T_FAIL("%s: not an error", bad[i]);
		ak_release(e), ak_release(s), ak_release(d);
	}
	ak_release(c);
	ak_i18n_reset();
	ak_locale_reset();
}

// A plural's offset taken from the smallest integer: the float's
// difference, as Go's Format says it, not an overflow.
TEST(plural_offset_of_the_smallest) {
	load();
	ak_shape shape = {"Args", 2, (const uint8_t[]){AK_K_INT, AK_K_REF}, NULL, NULL, 0, NULL, false, NULL};
	ak_struct *tuple = ak_struct_new(&shape);
	tuple->f[0].i = INT64_MIN;
	tuple->f[1].p = ak_str_c("Ada");
	ak_str *en = ak_str_c("en");
	ak_str *got = ak_i18n_say(catalog, key_index("guests"), (ak_val){.p = tuple}, en);
	CHECK(strstr(got->data, "Ada") != NULL);
	ak_release(got);
	ak_release(en);
	ak_release(tuple);
	unload();
}

// An exponent of any length: the operands of compact notation clamp it.
TEST(operands_long_exponent) {
	ak_operands o = ak_operands_dec("1e99999999999999999999", 22);
	CHECK_EQ_INT(o.c, 100);
	o = ak_operands_dec("1.5c3", 5);
	CHECK_EQ_INT(o.i, 1500);
	CHECK_EQ_INT(o.c, 3);
}

T_MAIN()
