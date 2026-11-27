// locale.c: the language the app shows.
//
// The model: the app shows one language, which its messages,
// Asuka's own words and the host's views all follow, and the regional
// formats are a setting of their own. The language is the user's
// preferred languages (the environment's languages), or the one the app
// chose (setLocale), matched against the languages the app is written in
// (setLanguages, else its catalogs'), and the first of those when none
// matches: an app written in English alone shows English to a reader of
// Chinese, as UIKit does. It's the process's: every root shows it.
//
// What reads it (locale(), isRTL(), region(), a catalog's message, i18n.c)
// follows it: it's a signal. A change of direction lays the app out again
// (ak_ui_set_direction); the host is told the language with Locale.
#include "ui/ui_ext.h"
#include "ui/i18n.h"

#include <stdlib.h>

static struct {
	bool loaded;
	ak_str *override;  // setLocale's, or ASUKA_LOCALE's; NULL: the user's languages
	bool forced;       // ASUKA_LOCALE chose it: setLocale saves nothing
	ak_str *pending_save;
	bool has_pending_save;
	ak_arr *app_langs;  // setLanguages', or NULL for the catalogs'
	// The environment the locale was worked out from.
	ak_str *env_langs, *env_locale;
	int env_fw, env_hc;
	bool has_env;
	// What the app shows.
	ak_str *locale;
	bool rtl;
	ak_arr *preferred;
	ak_str *region_locale;
	int first_weekday;  // 1 Sunday ... 7 Saturday
	int hour_cycle;
	bool shown;
	ak_node node;
} L;

// ---- Tags ----

static char forced_dir[4];  // ak_ui_force_locale's direction ("rtl", "ltr"), or ""
// A preview's (ak_ui_preview_locale), over the rest while it shows.
static char preview_dir[4];
static ak_str *preview_lang;

// forced_direction is a preview's, asuka dev's --dir (ak_ui_force_locale)
// or ASUKA_DIRECTION's ("rtl" or "ltr"), or NULL.
static const char *forced_direction(void) {
	if (preview_dir[0]) return preview_dir;
	if (forced_dir[0]) return forced_dir;
	const char *d = getenv("ASUKA_DIRECTION");
	return d && (strcmp(d, "rtl") == 0 || strcmp(d, "ltr") == 0) ? d : NULL;
}

static bool rtl_of(const ak_str *locale) {
	const char *d = forced_direction();
	if (d) return d[0] == 'r';
	return ak_i18n_rtl(ak_span_str(locale));
}

// supported is the languages the app is written in, its own first.
static ak_arr *supported(void) {
	if (L.app_langs && L.app_langs->len) return ak_retain(L.app_langs);
	ak_arr *c = ak_i18n_catalog_languages();
	if (c && c->len) return c;
	ak_release(c);
	ak_arr *a = ak_arr_new(AK_K_REF, NULL, 1);
	a->h.flags = 1;
	ak_arr_push(&a, (ak_val){.p = ak_str_c("en")});
	return a;
}

// preferred_of is the environment's languages, normalized, or its
// locale's for a host that reports none.
static ak_arr *preferred_of(void) {
	ak_arr *out = ak_arr_new(AK_K_REF, NULL, 0);
	out->h.flags = 1;
	ak_span list = ak_span_str(L.env_langs);
	ak_str *loc = NULL;
	if (!list.len) {
		loc = ak_i18n_region_locale(ak_span_str(L.env_locale));
		list = ak_span_str(loc);
	}
	uint32_t at = 0;
	while (at < list.len) {
		uint32_t end = at;
		while (end < list.len && list.p[end] != ',') end++;
		ak_str *n = ak_i18n_normalize((ak_span){list.p + at, end - at});
		bool dup = false;
		for (uint32_t i = 0; n && i < out->len && !dup; i++) dup = ak_str_eq(ak_arr_vals(out)[i].p, n);
		if (n && n->len && !dup) ak_arr_push(&out, (ak_val){.p = n});
		else ak_release(n);
		at = end + 1;
	}
	ak_release(loc);
	return out;
}

static void load(void) {
	if (L.loaded) return;
	L.loaded = true;
	ak_signal_init(&L.node);
	const char *tag = getenv("ASUKA_LOCALE");
	ak_str *n = tag && *tag ? ak_i18n_normalize(ak_cstr(tag)) : NULL;
	if (n && n->len) {
		L.override = n;
		L.forced = true;
	} else {
		ak_release(n);
	}
}

static bool sent;  // update sent Locale (ak_locale_mounted's)

static void send_locale(ak_ui_root *r) {
	if (!r || !r->el || r->disposed) return;
	sent = true;
	ak_buf *b = ak_batch_command_begin(r->batch, r->el->id, AK_CMD_LOCALE);
	ak_buf_u8(b, L.rtl ? 1 : 0);
	ak_buf_put(b, L.locale->data, L.locale->len);
	ak_batch_command_end(r->batch);
}

// update works out what the app shows and, if it changed, what follows
// it does.
static void update(void) {
	load();
	ak_arr *pref = preferred_of();
	ak_arr *sup = supported();
	ak_str *loc = ak_i18n_resolve(preview_lang ? preview_lang : L.override, pref, sup);
	ak_release(sup);
	bool rtl = rtl_of(loc);
	// The regional formats: the environment's, what it doesn't say from
	// CLDR's week and time data for the locale's region (or its language's
	// likely one).
	ak_str *rl = ak_i18n_region_locale(ak_span_str(L.env_locale));
	if (!rl->len && pref->len) {
		ak_release(rl);
		rl = ak_retain(ak_arr_vals(pref)[0].p);
	}
	int fw = L.env_fw >= 1 && L.env_fw <= 7 ? L.env_fw : ak_i18n_first_weekday(ak_span_str(rl));
	int hc = L.env_hc == 12 || L.env_hc == 24 ? L.env_hc : ak_i18n_hour_cycle(ak_span_str(rl));
	bool same_pref = L.preferred && L.preferred->len == pref->len;
	for (uint32_t i = 0; same_pref && i < pref->len; i++) same_pref = ak_str_eq(ak_arr_vals(L.preferred)[i].p, ak_arr_vals(pref)[i].p);
	bool lang_changed = !L.shown || !ak_str_eq(L.locale, loc) || L.rtl != rtl;
	bool dir_changed = !L.shown || L.rtl != rtl;
	bool changed = lang_changed || !same_pref || !ak_str_eq(L.region_locale, rl) || L.first_weekday != fw || L.hour_cycle != hc;
	ak_release(L.locale), L.locale = loc;
	ak_release(L.preferred), L.preferred = pref;
	ak_release(L.region_locale), L.region_locale = rl;
	L.rtl = rtl, L.first_weekday = fw, L.hour_cycle = hc;
	L.shown = true;
	if (!changed) return;
	if (lang_changed) send_locale(ak_ui.root);
	if (dir_changed && ak_ui.rtl != rtl) ak_ui_set_direction(rtl);
	ak_signal_write(&L.node);
}

static bool env_same(const ak_environment *e) {
	return L.has_env && ak_span_eq(e->languages, ak_span_str(L.env_langs)) && ak_span_eq(e->locale, ak_span_str(L.env_locale)) &&
	       e->first_weekday == L.env_fw && e->hour_cycle == L.env_hc;
}

static void take_env(const ak_environment *e) {
	ak_release(L.env_langs);
	ak_release(L.env_locale);
	L.env_langs = ak_str_span(e->languages);
	L.env_locale = ak_str_span(e->locale);
	L.env_fw = e->first_weekday;
	L.env_hc = e->hour_cycle;
	L.has_env = true;
}

void ak_locale_mounted(ak_ui_root *r) {
	load();
	take_env(&r->env);
	sent = false;
	update();
	if (!sent) send_locale(r);  // the host hears it as the root mounts, whatever changed
	if (L.has_pending_save) {
		ak_batch_command(r->batch, r->el->id, AK_CMD_SAVE_LOCALE, L.pending_save ? L.pending_save->data : "", L.pending_save ? L.pending_save->len : 0);
		ak_release(L.pending_save);
		L.pending_save = NULL;
		L.has_pending_save = false;
	}
}

void ak_locale_environment(ak_ui_root *r) {
	if (env_same(&r->env)) return;
	take_env(&r->env);
	update();
}

bool ak_locale_rtl(void) {
	if (!L.shown) update();
	return L.rtl;
}

// ---- To compiled code ----

static void track(void) {
	load();
	if (!L.shown) update();
	ak_signal_read(&L.node);
}

// ak_ui_locale is the language the app shows, a BCP 47 tag of those it's
// written in ("zh-Hans"). Reading it follows it.
ak_str *ak_ui_locale(void) {
	track();
	return ak_retain(L.locale);
}

// ak_ui_set_locale makes the app show tag's language (of those it's
// written in), or with "" the user's languages again. It's kept for the
// next launches (the host saves it), unless ASUKA_LOCALE chose one for
// this run. It throws on a tag that names no language.
void ak_ui_set_locale(ak_str *tag) {
	load();
	ak_str *n = NULL;
	if (tag && tag->len) {
		n = ak_i18n_normalize(ak_span_str(tag));
		if (!n || !n->len) {
			ak_release(n);
			ak_throw_msg("setLocale: %.*s is not a language tag", (int)tag->len, tag->data);
			return;
		}
	}
	if (!L.forced) {
		ak_ui_root *r = ak_ui.root;
		if (r && r->el) {
			ak_batch_command(r->batch, r->el->id, AK_CMD_SAVE_LOCALE, n ? n->data : "", n ? n->len : 0);
		} else {
			ak_release(L.pending_save);
			L.pending_save = ak_retain(n);
			L.has_pending_save = true;
		}
	}
	ak_release(L.override);
	L.override = n;
	L.forced = false;
	update();
}

// ak_ui_force_locale shows lang (a tag; "": the user's languages) laid
// out in dir ("rtl", "ltr"; "": the language's) whatever the user's or
// setLocale's, for this run: asuka dev's --lang and --dir, which the
// module's init says. Nothing is saved.
void ak_ui_force_locale(ak_str *lang, ak_str *dir) {
	load();
	forced_dir[0] = 0;
	if (dir && (ak_span_eqc(ak_span_str(dir), "rtl") || ak_span_eqc(ak_span_str(dir), "ltr"))) memcpy(forced_dir, dir->data, 4);
	if (lang && lang->len) {
		ak_str *n = ak_i18n_normalize(ak_span_str(lang));
		if (n && n->len) {
			ak_release(L.override);
			L.override = n;
			L.forced = true;
		} else {
			ak_release(n);
		}
	}
	if (L.shown) update();
}

void ak_ui_preview_locale(ak_str *lang, ak_str *dir) {
	load();
	char d[4] = {0};
	if (dir && (ak_span_eqc(ak_span_str(dir), "rtl") || ak_span_eqc(ak_span_str(dir), "ltr"))) memcpy(d, dir->data, 3);
	ak_str *n = lang && lang->len ? ak_i18n_normalize(ak_span_str(lang)) : NULL;
	if (n && !n->len) {
		ak_release(n);
		n = NULL;
	}
	bool same = memcmp(d, preview_dir, 4) == 0 && (n ? preview_lang && ak_str_eq(n, preview_lang) : !preview_lang);
	if (same) {
		ak_release(n);
		return;
	}
	memcpy(preview_dir, d, 4);
	ak_release(preview_lang);
	preview_lang = n;
	if (L.shown) update();
}

// ak_ui_set_languages sets the languages the app is written in, its own
// first (for an app that translates itself, without catalogs).
void ak_ui_set_languages(ak_arr *tags) {
	for (uint32_t i = 0; tags && i < tags->len; i++) {
		ak_str *n = ak_i18n_normalize(ak_span_str(ak_arr_vals(tags)[i].p));
		bool ok = n && n->len;
		ak_release(n);
		if (!ok) {
			const ak_str *t = ak_arr_vals(tags)[i].p;
			ak_throw_msg("setLanguages: %.*s is not a language tag", t ? (int)t->len : 0, t ? t->data : "");
			return;
		}
	}
	// Copies: the tags are the app's constants, freed with its module when
	// asuka dev reloads it, and the languages stay.
	ak_arr *own = ak_arr_new(AK_K_REF, NULL, tags ? tags->len : 0);
	own->h.flags = 1;
	for (uint32_t i = 0; tags && i < tags->len; i++) {
		const ak_str *t = ak_arr_vals(tags)[i].p;
		ak_arr_push(&own, (ak_val){.p = ak_str_new(t->data, t->len)});
	}
	ak_release(L.app_langs);
	L.app_langs = own;
	update();
}

// ak_ui_languages is the languages the app is written in, its own first.
ak_arr *ak_ui_languages(void) { return supported(); }

// ak_ui_preferred_languages is the languages the user reads, most
// preferred first. Reading it follows them.
ak_arr *ak_ui_preferred_languages(void) {
	track();
	return ak_retain(L.preferred);
}

// ak_ui_is_rtl reports whether the app is laid out right to left.
bool ak_ui_is_rtl(void) {
	track();
	return L.rtl;
}

static const ak_shape *const no_shapes[4];
static const uint8_t region_kinds[] = {AK_K_REF, AK_K_INT, AK_K_INT};
static const char *const region_names[] = {"locale", "firstWeekday", "hourCycle"};
const ak_shape ak_ui_shape_region = {"Region", 3, region_kinds, no_shapes, region_names, 0, NULL, false, NULL};

// ak_ui_region is the user's regional formats (Region).
ak_val ak_ui_region(void) {
	track();
	ak_struct *s = ak_struct_new(&ak_ui_shape_region);
	s->f[0].p = ak_retain(L.region_locale);
	s->f[1].i = L.first_weekday;
	s->f[2].i = L.hour_cycle;
	return (ak_val){.p = s};
}

// ak_ui_format_locale is the locale to format numbers and dates in: the
// language shown, written the way the user set their formats
// (en-GB-u-fw-mon-hc-h23 for English read with British formats).
ak_str *ak_ui_format_locale(void) {
	track();
	return ak_i18n_format_locale(ak_span_str(L.locale), ak_span_str(L.region_locale), L.first_weekday, L.hour_cycle);
}

// ak_locale_now is the language shown, borrowed (i18n.c's messages).
ak_str *ak_locale_now(void) {
	track();
	return L.locale;
}

int ak_locale_hour_cycle(void) {
	track();
	return L.hour_cycle;
}

int ak_locale_first_weekday(void) {
	track();
	return L.first_weekday;
}

// ak_locale_changed tells the locale the catalogs' languages changed.
void ak_locale_changed(void) {
	if (L.shown) update();
}

void ak_locale_forget_choice(void) {
	load();
	if (!L.forced) {
		ak_release(L.override);
		L.override = NULL;
	}
	ak_release(L.pending_save);
	L.pending_save = NULL;
	L.has_pending_save = false;
}

// ak_locale_reset forgets the process's locale (tests).
void ak_locale_reset(void) {
	ak_release(L.override);
	ak_release(L.pending_save);
	ak_release(L.app_langs);
	ak_release(L.env_langs);
	ak_release(L.env_locale);
	ak_release(L.locale);
	ak_release(L.preferred);
	ak_release(L.region_locale);
	ak_signal_drop(&L.node);
	memset(&L, 0, sizeof L);
	forced_dir[0] = 0;
}
