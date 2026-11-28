// widgets.c: Chip, DateRangePicker, Disclosure, and a TextField's
// suggestions (an autocomplete's, a combo box's).
//
// A control the user changes keeps what the host last reported, as
// elements.c's do, and sends the app's value only when it differs: a
// chip's selection, a range, a disclosure's state never round trip.
#include "ui/ui_ext.h"
#include "rt/unicode.h"

static void send_str(ak_view *e, ak_prop p, ak_val v) {
	const ak_str *s = v.p;
	ak_batch_set_string(ak_view_batch(e), e->id, p, s ? s->data : "", s ? s->len : 0);
}

static void keep_text(ak_str **dst, ak_val v) {
	ak_str *s = ak_retain((ak_str *)v.p);
	ak_release(*dst);
	*dst = s;
}

static bool key_set(const ak_view *e, uint16_t key) { return ak_style_get(e, key) != NULL; }

static bool key_bool(const ak_view *e, uint16_t key) {
	const ak_val *v = ak_style_get(e, key);
	return v && v->i;
}

static int64_t key_int(const ak_view *e, uint16_t key) {
	const ak_val *v = ak_style_get(e, key);
	return v ? v->i : 0;
}

// ---- Chip ----
//
// What kind it is follows from what the app gives it: selected (bound or
// set) makes it a filter chip, on remove an input chip; else it's an
// action chip, an assist chip with an icon or a suggestion chip without.

typedef struct {
	bool filter, removable;
	bool selected;  // as the host shows it: last sent or reported
	bool known;
} chip_state;

static void chip_init(ak_view *e) {
	ak_view_measure(e);
	e->impl = ak_alloc(sizeof(chip_state));
}

static void chip_drop(ak_view *e) { ak_free(e->impl); }

// The measure cache tells chips apart by these: a selected filter chip
// shows a checkmark, an input chip its remove button.
static void chip_variant(ak_view *e) {
	chip_state *s = e->impl;
	int32_t v = (s->filter ? 1 : 0) | (s->filter && s->selected ? 2 : 0) | (s->removable ? 4 : 0);
	if (v == e->variant) return;
	e->variant = v;
	ak_view_remeasure(e);
}

static void chip_send_selected(ak_view *e, bool on) {
	chip_state *s = e->impl;
	if (s->known && s->selected == on) return;
	s->selected = on;
	s->known = true;
	ak_batch_set_bool(ak_view_batch(e), e->id, AK_PROP_CHIP_SELECTED, on);
	chip_variant(e);
}

static void chip_set(ak_view *e, uint16_t key, ak_val v, const ak_val *prev) {
	AK_UNUSED(prev);
	switch (key) {
	case AK_KEY_CHIP_TITLE:
		keep_text(&e->text, v);  // the measure cache's key, with the icon
		send_str(e, AK_PROP_CHIP_TITLE, v);
		ak_view_remeasure(e);
		break;
	case AK_KEY_CHIP_ICON:
		keep_text(&e->text2, v);
		send_str(e, AK_PROP_CHIP_ICON, v);
		ak_view_remeasure(e);
		break;
	case AK_KEY_CHIP_SELECTED:
		if (e->finalized && ((chip_state *)e->impl)->filter) chip_send_selected(e, v.i != 0);
		break;
	}
}

static void chip_finalize(ak_view *e) {
	chip_state *s = e->impl;
	ak_batch *b = ak_view_batch(e);
	// Told before it's inserted: a host makes its view for its kind.
	s->filter = key_set(e, AK_KEY_CHIP_SELECTED);
	s->removable = ak_view_handler(e, AK_EV_CHIP_REMOVE) != NULL;
	if (s->filter) {
		ak_batch_set_bool(b, e->id, AK_PROP_CHIP_FILTER, true);
		chip_send_selected(e, key_bool(e, AK_KEY_CHIP_SELECTED));
		ak_view_listen(e, AK_EVENT_TOGGLED);
	}
	if (s->removable) {
		ak_batch_set_bool(b, e->id, AK_PROP_CHIP_REMOVABLE, true);
		ak_view_listen(e, AK_EVENT_REMOVE);
	}
	chip_variant(e);
}

static void chip_event(ak_view *e, const ak_event *ev) {
	chip_state *s = e->impl;
	switch (ev->kind) {
	case AK_EVENT_PRESS: ak_view_call(e, AK_EV_CHIP_PRESS, NULL, 0); break;
	case AK_EVENT_TOGGLED: {
		if (!s->filter) break;
		s->selected = ev->b;
		s->known = true;
		chip_variant(e);
		ak_val arg = {.i = ev->b};
		ak_view_call(e, AK_EV_CHIP_CHANGE, &arg, 1);
		break;
	}
	case AK_EVENT_REMOVE: ak_view_call(e, AK_EV_CHIP_REMOVE, NULL, 0); break;
	}
}

static const ak_el_class cls_chip = {.name = "Chip", .kind = AK_KIND_CHIP, .flags = AK_ELF_TEXT | AK_ELF_MEASURED | AK_ELF_CACHE, .init = chip_init,
                                     .set = chip_set, .finalize = chip_finalize, .event = chip_event, .drop = chip_drop};

bool ak_chip_filter(const ak_view *e) { return e && e->el == AK_EL_CHIP && e->impl && ((const chip_state *)e->impl)->filter; }

// ---- DateRangePicker ----

typedef struct {
	int64_t start, end;  // as the host shows them
	bool known;
} range_state;

static void range_init(ak_view *e) {
	ak_view_measure(e);
	e->impl = ak_alloc(sizeof(range_state));
}

static void range_drop(ak_view *e) { ak_free(e->impl); }

// range_send sends the app's range unless the host shows it: never an
// end before the start.
static void range_send(ak_view *e) {
	range_state *s = e->impl;
	int64_t start = key_int(e, AK_KEY_DATE_RANGE_PICKER_START), end = key_int(e, AK_KEY_DATE_RANGE_PICKER_END);
	if (start && end && end < start) end = start;
	if (!start) end = 0;  // no start: nothing chosen
	ak_batch *b = ak_view_batch(e);
	bool was = s->known;
	if (!s->known || s->start != start) ak_batch_set_int(b, e->id, AK_PROP_DATE_RANGE_PICKER_START, start);
	if (!s->known || s->end != end) ak_batch_set_int(b, e->id, AK_PROP_DATE_RANGE_PICKER_END, end);
	// Two fields of dates are as wide as the dates' text: measured again.
	if (was && (s->start != start || s->end != end)) ak_view_remeasure(e);
	s->start = start, s->end = end, s->known = true;
}

static void range_set(ak_view *e, uint16_t key, ak_val v, const ak_val *prev) {
	ak_batch *b = ak_view_batch(e);
	switch (key) {
	case AK_KEY_DATE_RANGE_PICKER_START:
	case AK_KEY_DATE_RANGE_PICKER_END:
		// At the flush, once both are the app's: the user's range writes
		// one, then the other.
		if (e->finalized) ak_view_sync(e);
		break;
	case AK_KEY_DATE_RANGE_PICKER_MIN: ak_batch_set_int(b, e->id, AK_PROP_DATE_RANGE_PICKER_MIN, v.i); break;
	case AK_KEY_DATE_RANGE_PICKER_MAX: ak_batch_set_int(b, e->id, AK_PROP_DATE_RANGE_PICKER_MAX, v.i); break;
	case AK_KEY_DATE_RANGE_PICKER_INLINE:
		if (!prev && !v.i) break;
		ak_batch_set_bool(b, e->id, AK_PROP_DATE_RANGE_PICKER_INLINE, v.i != 0);
		e->variant = v.i ? 1 : 0;
		ak_view_remeasure(e);  // a calendar, not two fields
		break;
	}
}

static void range_finalize(ak_view *e) {
	range_send(e);
	ak_view_listen(e, AK_EVENT_RANGE_CHANGED);
}

static void range_event(ak_view *e, const ak_event *ev) {
	if (ev->kind != AK_EVENT_RANGE_CHANGED) return;
	range_state *s = e->impl;
	int64_t start = (int64_t)ev->x, end = (int64_t)ev->y;
	if (end < start) {
		// The host says it never sends one; the core keeps the rule.
		end = start;
		ak_batch_set_int(ak_view_batch(e), e->id, AK_PROP_DATE_RANGE_PICKER_END, end);
	}
	if (s->start != start || s->end != end) ak_view_remeasure(e);
	s->start = start, s->end = end, s->known = true;
	ak_val a = {.i = start}, z = {.i = end};
	ak_view_call(e, AK_EV_DATE_RANGE_PICKER_START_CHANGE, &a, 1);
	if (e->dead) return;
	ak_view_call(e, AK_EV_DATE_RANGE_PICKER_END_CHANGE, &z, 1);
	if (e->dead) return;
	ak_val both[2] = {a, z};
	ak_view_call(e, AK_EV_DATE_RANGE_PICKER_CHANGE, both, 2);
}

static const ak_el_class cls_range = {.name = "DateRangePicker", .kind = AK_KIND_DATE_RANGE_PICKER, .flags = AK_ELF_MEASURED, .init = range_init,
                                      .set = range_set, .finalize = range_finalize, .event = range_event, .sync = range_send, .drop = range_drop};

// ---- Disclosure ----
//
// A column of its header, the host's (kind Disclosure: the title and the
// platform's indicator), and, while it's expanded, its content: built
// from its block as it opens, under an owner of its own, and disposed as
// it closes. The header's text keys are the element's.

typedef struct {
	ak_view *header;   // the first kid (the element holds it)
	ak_view *content;  // the second, while it shows (the element holds it)
	ak_closure *build;  // the content's builder
	ak_owner *owner;    // the content's, while it shows
	bool expanded;      // the content shows
	bool shown;         // the header shows expanded: last sent or reported
	bool known;
} disclosure_state;

static disclosure_state *disclosure_of(const ak_view *e) { return e && e->el == AK_EL_DISCLOSURE && !e->dead ? e->impl : NULL; }

// The header: measured, as wide as the disclosure.
static void header_init(ak_view *e) {
	ak_view_measure(e);
	ak_view_listen(e, AK_EVENT_TOGGLED);
}

static void header_adjust(ak_view *e, ak_lstyle *ls) {
	AK_UNUSED(e);
	ls->width = ak_lpct(100);
}

static void disclosure_apply(ak_view *e);

static void header_event(ak_view *h, const ak_event *ev) {
	if (ev->kind != AK_EVENT_TOGGLED) return;
	ak_view *e = h->parent;
	disclosure_state *s = disclosure_of(e);
	if (!s) return;
	// The host shows it at once: so does the content, whatever the app
	// writes back (unbound, the disclosure keeps the user's choice).
	s->shown = ev->b;
	s->known = true;
	s->expanded = ev->b;
	disclosure_apply(e);
	ak_val arg = {.i = ev->b};
	ak_view_call(e, AK_EV_DISCLOSURE_CHANGE, &arg, 1);
}

static const ak_el_class cls_header = {.name = "Disclosure", .kind = AK_KIND_DISCLOSURE, .flags = AK_ELF_TEXT | AK_ELF_MEASURED | AK_ELF_CACHE,
                                       .init = header_init, .event = header_event, .adjust = header_adjust};

static void content_close(ak_view *e) {
	disclosure_state *s = e->impl;
	if (s->owner) {
		ak_owner *o = s->owner;
		s->owner = NULL;
		ak_owner_dispose(o);
	}
	if (s->content) {
		ak_view *c = s->content;
		s->content = NULL;
		if (c->parent == e) ak_view_remove(e, c, true);
	}
}

static void disclosure_apply(ak_view *e) {
	disclosure_state *s = e->impl;
	if (e->dead) return;
	if (!s->known || s->shown != s->expanded) {
		s->shown = s->expanded;
		s->known = true;
		ak_batch_set_bool(ak_view_batch(e), s->header->id, AK_PROP_DISCLOSURE_EXPANDED, s->expanded);
	}
	if (!s->expanded) {
		content_close(e);
		return;
	}
	if (s->content || !s->build || !e->finalized) return;
	s->owner = ak_root_under(e->owner);
	ak_view *v = ak_build_under(e->r, s->owner, s->build, NULL, 0);
	if (!v) return;
	if (e->dead) {
		ak_release(v);
		return;
	}
	s->content = v;
	ak_view_append(e, v);
	ak_release(v);  // e holds it
}

// It goes with what made it.
static void disclosure_gone(void *ctx) {
	ak_view *e = ctx;
	if (e->impl) content_close(e);
	ak_release(e);
}

static void disclosure_init(ak_view *e) {
	disclosure_state *s = ak_alloc(sizeof *s);
	e->impl = s;
	s->header = ak_view_new(e->r, &cls_header, 0, true);
	ak_view_append(e, s->header);
	ak_release(s->header);  // e holds it
	ak_on_cleanup(disclosure_gone, ak_retain(e));
}

static void disclosure_drop(ak_view *e) {
	disclosure_state *s = e->impl;
	if (!s) return;
	if (s->owner) ak_owner_dispose(s->owner);
	ak_release(s->build);
	ak_free(s);
	e->impl = NULL;
}

static void disclosure_set(ak_view *e, uint16_t key, ak_val v, const ak_val *prev) {
	AK_UNUSED(prev);
	disclosure_state *s = e->impl;
	switch (key) {
	case AK_KEY_DISCLOSURE_TITLE:
		keep_text(&s->header->text, v);  // the measure cache's key, and what automation finds
		send_str(s->header, AK_PROP_DISCLOSURE_TITLE, v);
		ak_view_remeasure(s->header);
		break;
	case AK_KEY_DISCLOSURE_EXPANDED:
		s->expanded = v.i != 0;
		if (e->finalized) disclosure_apply(e);
		break;
	}
}

static void disclosure_finalize(ak_view *e) { disclosure_apply(e); }

static void disclosure_lazy(ak_view *e, ak_closure *build) {
	disclosure_state *s = e->impl;
	ak_retain(build);
	ak_release(s->build);
	s->build = build;
	if (s->content) content_close(e);  // a new builder: built again
	if (e->finalized) disclosure_apply(e);
}

// Its text keys, and what a screen reader or a pointer reads of it, are
// its header's; the rest (padding, background, gap) its own.
static ak_view *disclosure_target(ak_view *e, uint16_t key) {
	switch (key) {
	case AK_KEY_FONT:
	case AK_KEY_COLOR:
	case AK_KEY_TEXT_ALIGN:
	case AK_KEY_LINES:
	case AK_KEY_LETTER_SPACING:
	case AK_KEY_LINE_HEIGHT:
	case AK_KEY_TRUNCATION:
	case AK_KEY_MINIMUM_SCALE:
	case AK_KEY_TEXT_SHADOW:
	case AK_KEY_LABEL:
	case AK_KEY_HINT:
	case AK_KEY_TOOLTIP:
	case AK_KEY_DISABLED:
		return ((disclosure_state *)e->impl)->header;
	}
	return e;
}

static const ak_el_class cls_disclosure = {.name = "Disclosure", .kind = AK_KIND_VIEW, .flags = AK_ELF_FLATTEN | AK_ELF_CHILDREN,
                                           .init = disclosure_init, .set = disclosure_set, .finalize = disclosure_finalize,
                                           .lazy = disclosure_lazy, .style_target = disclosure_target, .drop = disclosure_drop};

// ak_toggled_state is what a filter chip or a disclosure's header shows:
// selected, expanded.
bool ak_toggled_state(const ak_view *e) {
	if (!e) return false;
	if (e->el == AK_EL_CHIP && e->impl) return ((const chip_state *)e->impl)->selected;
	disclosure_state *s = disclosure_of(e->kind == AK_KIND_DISCLOSURE ? e->parent : e);
	return s && s->expanded;
}

const ak_el_class *ak_widget_class_of(uint16_t el) {
	switch (el) {
	case AK_EL_CHIP: return &cls_chip;
	case AK_EL_DATE_RANGE_PICKER: return &cls_range;
	case AK_EL_DISCLOSURE: return &cls_disclosure;
	}
	return NULL;
}

// ---- A TextField's suggestions ----
//
// The core filters them, the same on every platform: those that contain
// what's typed, ignoring case (Unicode's case folding). While the text is
// empty, or is one of them (a choice just made), they're all sent, which
// a combo box's drop-down lists. The host is sent the list it shows, and
// reports a choice by its index there: mapped back to the app's.

struct ak_suggest {
	bool filter;      // filterSuggestions
	bool dropdown;
	bool told;        // Suggestions was sent
	AK_VEC(uint32_t) shown;  // the app's index of each one sent
	AK_VEC(uint32_t) folded, needle;  // scratch: a folded item, the folded text
	ak_buf sent;      // the encoded list, as last sent
};

static ak_suggest *suggest_of(ak_view *e, bool make) {
	ak_suggest **slot = ak_field_suggest_slot(e);
	if (!slot) return NULL;
	if (!*slot && make) {
		*slot = ak_alloc(sizeof(ak_suggest));
		memset(*slot, 0, sizeof(ak_suggest));
		(*slot)->filter = true;
	}
	return *slot;
}

void ak_suggest_free(ak_suggest *s) {
	if (!s) return;
	ak_vec_free(&s->shown);
	ak_vec_free(&s->folded);
	ak_vec_free(&s->needle);
	ak_buf_free(&s->sent);
	ak_free(s);
}

static void fold_into(const ak_str *t, void *vec) {
	AK_VEC(uint32_t) *out = vec;
	out->len = 0;
	if (!t) return;
	for (size_t i = 0; i < t->len;) {
		uint32_t cp = ak_utf8_decode(t->data, t->len, &i), m[3];
		int n = ak_case_map(cp, AK_CASE_FOLD, m);
		for (int k = 0; k < n; k++) ak_vec_push(out, m[k]);
	}
}

static bool contains(const uint32_t *h, uint32_t hn, const uint32_t *n, uint32_t nn) {
	if (nn == 0) return true;
	for (uint32_t i = 0; i + nn <= hn; i++)
		if (memcmp(h + i, n, nn * sizeof(uint32_t)) == 0) return true;
	return false;
}

static bool same(const uint32_t *a, uint32_t an, const uint32_t *b, uint32_t bn) { return an == bn && memcmp(a, b, an * sizeof(uint32_t)) == 0; }

// suggest_send works out the list the field shows and sends it if it
// changed.
static void suggest_send(ak_view *e) {
	ak_suggest *s = suggest_of(e, false);
	if (!s || e->dead) return;
	const ak_val *iv = ak_style_get(e, AK_KEY_TEXT_FIELD_SUGGESTIONS);
	const ak_arr *items = iv ? iv->p : NULL;
	uint32_t n = items ? items->len : 0;
	const ak_str *text = ak_field_value(e);
	if (!text) {
		const ak_val *tv = ak_style_get(e, AK_KEY_TEXT_FIELD_TEXT);
		text = tv ? tv->p : NULL;
	}
	fold_into(text, &s->needle);
	bool all = !s->filter || s->needle.len == 0;
	for (uint32_t i = 0; !all && i < n; i++) {
		fold_into(ak_arr_vals(items)[i].p, &s->folded);
		if (same(s->folded.data, s->folded.len, s->needle.data, s->needle.len)) all = true;
	}
	s->shown.len = 0;
	for (uint32_t i = 0; i < n; i++) {
		if (!all) {
			fold_into(ak_arr_vals(items)[i].p, &s->folded);
			if (!contains(s->folded.data, s->folded.len, s->needle.data, s->needle.len)) continue;
		}
		ak_vec_push(&s->shown, i);
	}
	// Encoded as Suggestions is: sent when it's another list.
	ak_buf *scratch = &ak_rext_of(e->r)->scratch;
	ak_buf_reset(scratch);
	ak_buf_u32(scratch, s->shown.len);
	for (uint32_t i = 0; i < s->shown.len; i++) {
		const ak_str *t = ak_arr_vals(items)[s->shown.data[i]].p;
		ak_buf_str(scratch, t ? t->data : "", t ? t->len : 0);
	}
	if (s->told && scratch->len == s->sent.len && memcmp(scratch->data, s->sent.data, scratch->len) == 0) return;
	s->told = true;
	ak_buf_reset(&s->sent);
	ak_buf_put(&s->sent, scratch->data, scratch->len);
	ak_batch_set_bytes(ak_view_batch(e), e->id, AK_PROP_TEXT_FIELD_SUGGESTIONS, s->sent.data, s->sent.len);
}

bool ak_suggest_set(ak_view *e, uint16_t key, ak_val v, const ak_val *prev) {
	switch (key) {
	case AK_KEY_TEXT_FIELD_SUGGESTIONS:
		suggest_of(e, true);
		if (e->finalized) suggest_send(e);  // else at finalize, the text and filter known
		return true;
	case AK_KEY_TEXT_FIELD_FILTER_SUGGESTIONS: {
		ak_suggest *s = suggest_of(e, true);
		s->filter = v.i != 0;
		if (e->finalized) suggest_send(e);
		return true;
	}
	case AK_KEY_TEXT_FIELD_DROPDOWN:
		suggest_of(e, true)->dropdown = v.i != 0;
		if (prev || v.i) ak_batch_set_bool(ak_view_batch(e), e->id, AK_PROP_TEXT_FIELD_DROPDOWN, v.i != 0);
		return true;
	}
	return false;
}

void ak_suggest_finalize(ak_view *e) {
	ak_suggest *s = suggest_of(e, false);
	if (!s || !key_set(e, AK_KEY_TEXT_FIELD_SUGGESTIONS)) return;
	suggest_send(e);  // before it's inserted: the host's view is a suggesting one
	ak_view_listen(e, AK_EVENT_SUGGESTION);
	ak_view_listen(e, AK_EVENT_TEXT_CHANGED);  // what's typed filters them
}

// ak_suggest_text hears the field's text change (typed, or the app's).
void ak_suggest_text(ak_view *e) {
	if (e->finalized) suggest_send(e);
}

bool ak_suggest_event(ak_view *e, const ak_event *ev) {
	if (ev->kind != AK_EVENT_SUGGESTION) return false;
	ak_suggest *s = suggest_of(e, false);
	if (!s || ev->i < 0 || (uint64_t)ev->i >= s->shown.len) return true;
	const ak_val *iv = ak_style_get(e, AK_KEY_TEXT_FIELD_SUGGESTIONS);
	const ak_arr *items = iv ? iv->p : NULL;
	uint32_t i = s->shown.data[ev->i];
	if (!items || i >= items->len) return true;
	// The field shows it: its text now, as if typed, then the choice.
	ak_str *t = ak_retain((ak_str *)ak_arr_vals(items)[i].p);
	if (!t) t = ak_str_new("", 0);
	ak_field_text_reported(e, t);
	ak_val arg = {.p = t};
	ak_view_call(e, AK_EV_TEXT_FIELD_CHANGE, &arg, 1);
	ak_release(t);
	if (e->dead) return true;
	suggest_send(e);
	ak_val idx = {.i = i};
	ak_view_call(e, AK_EV_TEXT_FIELD_SUGGESTION, &idx, 1);
	return true;
}
