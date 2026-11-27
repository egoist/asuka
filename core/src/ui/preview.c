// preview.c: how asuka dev's player shows a preview (lang/ir's
// preview.go). A preview's module is an app whose root is the preview,
// and its init says how it's shown (ui.preview): light or dark whatever
// the system's setting, a text size, a language and a direction, and a
// frame of a size in the middle of the window. A module that doesn't say
// (the app, again) shows as the app does: the abi's loads begin and end
// each module's init (ak_preview_begin, ak_preview_end), and what no
// init claimed goes.
//
// The color scheme is the ColorScheme command's, over the app's own
// (setColorScheme), which comes back as the preview goes; the text size
// is DevFontScale's (the host's own text size, which it reports in the
// environment); the language and the direction are locale.c's
// (ak_ui_preview_locale), over the app's and asuka dev's --lang; the
// frame is a view of the root's, around the preview's (ak_preview_wrap).
// While a preview shows, the state cells of the app it replaced are kept
// for when the app comes back (abi.c's ak_keep_save, holding).
#include "ui/ui_ext.h"

static struct {
	bool on;       // a preview's module runs
	bool claimed;  // this module's init said how it's shown
	int scheme;    // AK_COLOR_SCHEME_*: the preview's (SYSTEM: the app's)
	float font_scale, width, height;
	ak_str *name;      // the preview's: its views' instance paths start with it
	int app_scheme;    // the app's own (setColorScheme)
	int sent_scheme;   // what the host was last told
	float sent_font;   // likewise (0: its own)
} P;

void ak_preview_host(void) {
	P.sent_scheme = AK_COLOR_SCHEME_SYSTEM;
	P.sent_font = 0;
}

bool ak_preview_active(void) { return P.on; }

static int scheme_now(void) { return P.on && P.scheme != AK_COLOR_SCHEME_SYSTEM ? P.scheme : P.app_scheme; }

// send tells the root's host what changed of the color scheme and the
// text size.
static void send(ak_ui_root *r) {
	if (!r || !r->el || r->disposed) return;
	int scheme = scheme_now();
	if (scheme != P.sent_scheme) {
		P.sent_scheme = scheme;
		uint8_t k = (uint8_t)scheme;
		ak_batch_command(r->batch, r->el->id, AK_CMD_COLOR_SCHEME, &k, 1);
	}
	float font = P.on ? P.font_scale : 0;
	if (font != P.sent_font) {
		P.sent_font = font;
		ak_buf *b = ak_batch_command_begin(r->batch, r->el->id, AK_CMD_DEV_FONT_SCALE);
		ak_buf_f32(b, font);
		ak_batch_command_end(r->batch);
	}
}

void ak_preview_mounted(ak_ui_root *r) { send(r); }

// ak_preview_color_scheme is the app's setColorScheme: sent unless a
// preview's shows over it.
void ak_preview_color_scheme(int scheme) {
	P.app_scheme = scheme;
	send(ak_ui.root);
}

void ak_preview_begin(void) { P.claimed = false; }

static void set(bool on, int scheme, float font_scale, ak_str *lang, ak_str *dir, float width, float height) {
	P.on = on;
	P.scheme = scheme >= 0 && scheme < AK_COLOR_SCHEME_COUNT ? scheme : AK_COLOR_SCHEME_SYSTEM;
	P.font_scale = font_scale > 0 ? font_scale : 0;
	P.width = width > 0 ? width : 0;
	P.height = height > 0 ? height : 0;
	ak_ui_preview_locale(lang, dir);
	send(ak_ui.root);
}

static void set_name(ak_str *name) {
	ak_release(P.name);
	P.name = name && name->len ? ak_str_new(name->data, name->len) : NULL;  // a copy: the module's constant goes with it
}

void ak_preview_end(void) {
	if (P.claimed || !P.on) return;
	set(false, AK_COLOR_SCHEME_SYSTEM, 0, NULL, NULL, 0, 0);
	set_name(NULL);
}

// ak_preview_enter starts the root's instance paths with the preview's
// name, as it builds: its state is its own, not the app's components'
// (which wait for the app, ak_keep_hold).
void ak_preview_enter(void) {
	if (P.on && P.name) ak_ui_enter(P.name);
}

// ui.preview(name, scheme, fontScale, lang, dir, width, height): the
// module is the preview name's (asuka dev's), shown so; name "": as the
// app is (asuka test --previews', after one).
static ak_val b_preview(ak_val *a, uint32_t n) {
	AK_UNUSED(n);
	ak_str *name = a[0].p;
	bool on = name && name->len;
	P.claimed = on;
	set_name(name);
	set(on, (int)a[1].i, (float)a[2].f, a[3].p, a[4].p, (float)a[5].f, (float)a[6].f);
	return (ak_val){0};
}

// ---- The frame ----

static void frame_adjust(ak_view *e, ak_lstyle *ls) {
	AK_UNUSED(e);
	if (P.width > 0) ls->width = ak_lpt(P.width);
	if (P.height > 0) ls->height = ak_lpt(P.height);
	ls->flex_grow = ls->flex_shrink = 0;
	// In the middle of the window: auto margins take the space around it.
	ls->margin = (ak_lrect){AK_LAUTO, AK_LAUTO, AK_LAUTO, AK_LAUTO};
}

static const ak_el_class cls_frame = {.name = "View", .kind = AK_KIND_VIEW, .flags = AK_ELF_CHILDREN, .adjust = frame_adjust};

// ak_preview_wrap is the view the root shows for v, the app's (owned):
// v itself, or, for a preview of a size, a frame of that size around it.
ak_view *ak_preview_wrap(ak_ui_root *r, ak_view *v) {
	if (!P.on || (P.width <= 0 && P.height <= 0) || !v) return v;
	ak_view *f = ak_view_new(r, &cls_frame, 0, true);
	v->fill = true;
	ak_style_relayout(v);
	ak_view_append(f, v);
	ak_release(v);
	ak_style_relayout(f);
	return f;
}

void ak_preview_builtins_init(void);
void ak_preview_builtins_init(void) { ak_builtin_register("ui.preview", b_preview); }
