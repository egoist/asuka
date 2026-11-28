// daterange.c: DateRangePicker, a range of days, as GNOME's apps choose
// dates: a button showing the range (the user's locale's dates, an en
// dash between) that opens a popover of GTK's calendar, the range's days
// marked on it (gtk_calendar_mark_day); Inline shows the calendar in
// place. The first day chosen starts a range, the second ends it (one
// before the start starts it again, as Material's range picker does),
// which is reported with RangeChanged (the first and last days' local
// midnights) and closes the popover. Min and Max bound what's chosen
// (GtkCalendar can't grey days out: a day past them is taken as the
// bound). While Start is 0 the button shows today, dimmed, and the
// calendar today, nothing marked.
//
// GtkCalendar reports a day chosen (a click, Space or Return on a day)
// with day-selected; going to another month or year changes its date
// without one (where a GTK does send one then, the month's and year's
// signals ahead of it say it isn't a choice).
#include "host.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

typedef struct {
	int64_t start, end, min, max;  // Unix ms (0: none)
	int64_t first;                 // the first day of a range being chosen (its midnight; 0: none)
	bool inline_;
	GtkWidget *button, *label, *cal;
	bool updating, navigating;
	guint nav_idle;
	int64_t reported[2];  // the last range reported
} drange;

static drange *range_of(node *n) { return n && n->kind == AK_KIND_DATE_RANGE_PICKER ? n->ext : NULL; }

static gboolean nav_done(gpointer data) {
	drange *r = data;
	r->navigating = false;
	r->nav_idle = 0;
	return 0;
}

static void free_range(node *n) {
	drange *r = n->ext;
	if (r->nav_idle) g_source_remove(r->nav_idle);
	free(r);
	n->ext = NULL;
}

// midnight is the start of the local day ms is in (Unix ms).
static int64_t midnight(int64_t ms) {
	time_t t = (time_t)(ms >= 0 ? ms / 1000 : (ms - 999) / 1000);
	struct tm tm;
	localtime_r(&t, &tm);
	tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
	tm.tm_isdst = -1;
	return (int64_t)mktime(&tm) * 1000;
}

static int64_t date_ms(GDateTime *d) {
	GDateTime *m = g_date_time_new_local(g_date_time_get_year(d), g_date_time_get_month(d), g_date_time_get_day_of_month(d), 0, 0, 0);
	int64_t ms = m ? g_date_time_to_unix(m) * 1000 : 0;
	if (m) g_date_time_unref(m);
	return ms;
}

static GDateTime *ms_date(int64_t ms) {
	time_t t = (time_t)(ms >= 0 ? ms / 1000 : (ms - 999) / 1000);
	struct tm tm;
	localtime_r(&t, &tm);
	return g_date_time_new_local(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, 12, 0, 0);
}

static int64_t today(void) { return midnight(g_get_real_time() / 1000); }

// mark_range marks the shown month's days in the range (or the first day
// of one being chosen).
static void mark_range(drange *r) {
	if (!r->cal) return;
	gtk_calendar_clear_marks((GtkCalendar *)r->cal);
	int64_t from = r->first ? r->first : r->start ? midnight(r->start) : 0;
	int64_t to = r->first ? r->first : r->end ? midnight(r->end) : from;
	if (!from) return;
	GDateTime *shown = gtk_calendar_get_date((GtkCalendar *)r->cal);
	int y = g_date_time_get_year(shown), m = g_date_time_get_month(shown);
	g_date_time_unref(shown);
	for (int d = 1; d <= 31; d++) {
		GDateTime *day = g_date_time_new_local(y, m, d, 0, 0, 0);
		if (!day) break;
		bool same_month = g_date_time_get_month(day) == m;
		int64_t ms = g_date_time_to_unix(day) * 1000;
		g_date_time_unref(day);
		if (!same_month) break;
		if (ms >= from && ms <= to) gtk_calendar_mark_day((GtkCalendar *)r->cal, (guint)d);
	}
}

// show_range puts the range in the button's label and on the calendar
// (its month the start's, or today's).
static void show_range(drange *r) {
	if (r->label) {
		bool none = !r->start;
		int64_t a = none ? today() : r->start, b = none ? a : r->end ? r->end : r->start;
		GDateTime *da = g_date_time_new_from_unix_local(a / 1000), *db = g_date_time_new_from_unix_local(b / 1000);
		char *sa = g_date_time_format(da, "%x"), *sb = g_date_time_format(db, "%x");
		char text[256];
		snprintf(text, sizeof text, "%s \xe2\x80\x93 %s", sa ? sa : "", sb ? sb : "");  // an en dash
		g_free(sa);
		g_free(sb);
		g_date_time_unref(da);
		g_date_time_unref(db);
		gtk_label_set_text((GtkLabel *)r->label, text);
		if (none)
			gtk_widget_add_css_class(r->label, "dim-label");
		else
			gtk_widget_remove_css_class(r->label, "dim-label");
	}
	if (r->cal) {
		GDateTime *d = ms_date(r->first ? r->first : r->start ? r->start : today());
		r->updating = true;
		gtk_calendar_select_day((GtkCalendar *)r->cal, d);
		r->updating = false;
		g_date_time_unref(d);
	}
	mark_range(r);
}

static int64_t clamp_day(drange *r, int64_t ms) {
	if (r->min && ms < midnight(r->min)) ms = midnight(r->min);
	if (r->max && ms > midnight(r->max)) ms = midnight(r->max);
	return ms;
}

static void on_day(GtkCalendar *cal, node *n) {
	drange *r = range_of(n);
	if (!r || r->updating || n->dead) return;
	if (r->navigating) {
		mark_range(r);
		return;
	}
	GDateTime *d = gtk_calendar_get_date(cal);
	int64_t day = date_ms(d);
	g_date_time_unref(d);
	int64_t c = clamp_day(r, day);
	if (!r->first || c < r->first) {
		// The first day: a range starts (one before the start starts again).
		r->first = c;
		if (c != day) show_range(r);
		mark_range(r);
		return;
	}
	int64_t a = r->first;
	r->first = 0;
	r->start = a;
	r->end = c;
	show_range(r);
	r->reported[0] = a;
	r->reported[1] = c;
	lx_emit(n, (ak_event){.kind = AK_EVENT_RANGE_CHANGED, .x = (double)a, .y = (double)c});
	if (r->button) gtk_menu_button_popdown((GtkMenuButton *)r->button);
}

// on_nav: the calendar goes to another month or year: not a choice; its
// marks are the new month's.
static void on_nav(GtkCalendar *cal, node *n) {
	(void)cal;
	drange *r = range_of(n);
	if (!r) return;
	r->navigating = true;
	if (!r->nav_idle) r->nav_idle = g_idle_add_full(0, nav_done, r, NULL);
	mark_range(r);
}

static void on_shown_month(GObject *o, GParamSpec *ps, node *n) {
	(void)o;
	(void)ps;
	drange *r = range_of(n);
	if (r) mark_range(r);
}

// on_popover: opened, the calendar shows the range's start; closed with a
// range half chosen, it's let go.
static void on_popover_shown(GtkWidget *w, node *n) {
	(void)w;
	drange *r = range_of(n);
	if (r) show_range(r);
}

static void on_popover_closed(GtkPopover *p, node *n) {
	(void)p;
	drange *r = range_of(n);
	if (!r || !r->first) return;
	r->first = 0;
	show_range(r);
}

static void range_text(node *n);

static void build_range(node *n, drange *r) {
	r->cal = gtk_calendar_new();
	lx_connect(n, r->cal, "day-selected", G_CALLBACK(on_day));
	static const char *const navs[] = {"next-month", "prev-month", "next-year", "prev-year"};
	for (size_t i = 0; i < AK_LEN(navs); i++) lx_connect(n, r->cal, navs[i], G_CALLBACK(on_nav));
	lx_connect(n, r->cal, "notify::month", G_CALLBACK(on_shown_month));
	lx_connect(n, r->cal, "notify::year", G_CALLBACK(on_shown_month));
	if (r->inline_) {
		r->button = r->label = NULL;
		gtk_widget_set_halign(r->cal, GTK_ALIGN_START);
		gtk_widget_set_valign(r->cal, GTK_ALIGN_START);
		lx_set_inner(n, r->cal);
	} else {
		GtkWidget *pop = gtk_popover_new();
		gtk_popover_set_child((GtkPopover *)pop, r->cal);
		GtkWidget *b = gtk_menu_button_new();
		GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
		gtk_box_append((GtkBox *)box, gtk_image_new_from_icon_name("x-office-calendar-symbolic"));
		r->label = gtk_label_new("");
		gtk_box_append((GtkBox *)box, r->label);
		gtk_menu_button_set_child((GtkMenuButton *)b, box);
		gtk_menu_button_set_always_show_arrow((GtkMenuButton *)b, 1);
		gtk_menu_button_set_popover((GtkMenuButton *)b, pop);
		gtk_widget_set_halign(b, GTK_ALIGN_START);
		gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
		lx_connect(n, pop, "show", G_CALLBACK(on_popover_shown));
		lx_connect(n, pop, "closed", G_CALLBACK(on_popover_closed));
		r->button = b;
		lx_set_inner(n, b);
	}
	show_range(r);
	range_text(n);
	if (n->disabled) gtk_widget_set_sensitive(n->widget, 0);
	lx_update_focusable(n);
}

static bool range_create(node *n) {
	if (n->kind != AK_KIND_DATE_RANGE_PICKER) return false;
	drange *r = calloc(1, sizeof *r);
	if (!r) abort();
	n->ext = r;
	n->ext_free = free_range;
	build_range(n, r);
	return true;
}

static bool range_prop(node *n, ak_prop p, const ak_value *v) {
	drange *r = range_of(n);
	if (!r) return false;
	switch (p) {
	case AK_PROP_DATE_RANGE_PICKER_START:
		r->start = v->i;
		if (r->end && r->start > r->end) r->end = r->start;
		r->first = 0;
		show_range(r);
		return true;
	case AK_PROP_DATE_RANGE_PICKER_END:
		r->end = v->i;
		r->first = 0;
		show_range(r);
		return true;
	case AK_PROP_DATE_RANGE_PICKER_MIN:
		r->min = v->i;
		return true;
	case AK_PROP_DATE_RANGE_PICKER_MAX:
		r->max = v->i;
		return true;
	case AK_PROP_DATE_RANGE_PICKER_INLINE:
		if (r->inline_ != (v->i != 0)) {
			r->inline_ = v->i != 0;
			build_range(n, r);
		}
		return true;
	}
	return false;
}

// range_text sets the button's label in the picker's font and color,
// when it sets them.
static void range_text(node *n) {
	drange *r = range_of(n);
	if (!r || !r->label) return;
	if (n->color || n->font_size > 0 || n->text_style || n->font_weight || n->font_traits || n->letter_spacing != 0 || !str_empty(n->font_family) || H.dev_font_scale > 0)
		lx_set_label_attrs(n, r->label);
	else
		gtk_label_set_attributes((GtkLabel *)r->label, NULL);
}

// range_dump writes the range shown: the button's label, the days marked
// on the calendar's month and the first day of one being chosen.
static void range_dump(node *n, int depth, ak_buf *b) {
	drange *r = range_of(n);
	if (!r) return;
	for (int i = 0; i <= depth; i++) ak_buf_put(b, "  ", 2);
	ak_buf_printf(b, "daterange %s", r->inline_ ? "inline" : "button");
	if (r->label) {
		ak_buf_put(b, " label=", 7);
		ak_fmt_quote(b, ak_cstr(gtk_label_get_text((GtkLabel *)r->label)));
	}
	ak_buf_printf(b, " first=%s marked=", r->first ? "set" : "none");
	GDateTime *shown = gtk_calendar_get_date((GtkCalendar *)r->cal);
	int y = g_date_time_get_year(shown), m = g_date_time_get_month(shown);
	g_date_time_unref(shown);
	bool any = false;
	for (int d = 1; d <= 31; d++)
		if (gtk_calendar_get_day_is_marked((GtkCalendar *)r->cal, (guint)d)) {
			ak_buf_printf(b, "%s%d", any ? "," : "", d);
			any = true;
		}
	ak_buf_printf(b, " month=%04d-%02d\n", y, m);
}

// lx_range_calendar is a DateRangePicker's calendar, its popover's
// opened first (automation's day steps).
GtkWidget *lx_range_calendar(node *n, bool open);
GtkWidget *lx_range_calendar(node *n, bool open) {
	drange *r = range_of(n);
	if (!r) return NULL;
	if (open && r->button) gtk_menu_button_popup((GtkMenuButton *)r->button);
	return r->cal;
}

const area lx_area_daterange = {
	.name = "daterange",
	.create = range_create,
	.prop = range_prop,
	.text = range_text,
	.dump = range_dump,
};
