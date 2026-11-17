// a11y.c: screen readers on Windows (Narrator, NVDA), through UI
// Automation.
//
// Each view of the host's own drawing answers WM_GETOBJECT with a
// provider of its own (IRawElementProviderSimple) on top of its window's
// (its host provider: position, focus): its name (Label; a text's text; a
// pressable view's texts, as VoiceOver reads a row), its help text (Hint,
// else Tooltip), its control type (Role: a button, a heading, an image, a
// link, a text, a search field; a pressable view is a button), whether a
// screen reader skips it (Decorative, with everything in it), the Invoke
// pattern for a view that presses, and a ProgressRing's value (the
// RangeValue pattern, read only, as WinUI's ring has it). Announce raises
// a notification. A view's screen reader actions are its context menu
// (Shift+F10, the menu key, a right-click), and an adjustable view takes
// the arrow keys, as a slider does.
//
// RFC 0010's roles and states: each Role is a control type (a checkbox's
// CheckBox, a tab's TabItem, a listbox's List, a dialog's Pane that is a
// dialog...), A11yChecked the Toggle pattern (a radio's checked: its
// SelectionItem's), A11yExpanded ExpandCollapse, A11ySelected
// SelectionItem, A11yValue Value, A11yRange RangeValue (read only, but
// for an adjustable view, which steps toward a value set); A11yControls,
// A11yLabelledBy and A11yDescribedBy are the ControllerFor, LabeledBy and
// DescribedBy relations, the label's text the name where the view has
// none, the description's the help text. While a modal view shows (a
// modal Overlay, an A11yModal view), the rest of its window's views are
// off the control and content views.
//
// A control with no text of its own to be named by (a drop-down list
// combo box, whose text is its selection) is named through Dynamic
// Annotation (IAccPropServices), which UI Automation's proxy for the
// control reads. Tooltips are the common controls' tooltip control: one
// per app, with a tool per window that has a tooltip.
#include "nav.h"

#include <oleacc.h>
#include <uiautomation.h>

#define HEADING_LEVEL_1 80051

static const GUID iid_unknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const GUID iid_simple = {0xd6dd68d1, 0x86fd, 0x4332, {0x86, 0x66, 0x9a, 0xbe, 0xde, 0xa2, 0xd2, 0x4c}};
static const GUID iid_invoke = {0x54fcb24b, 0xe18e, 0x47a2, {0xb4, 0xd3, 0xec, 0xcb, 0xe7, 0x75, 0x99, 0xa2}};
static const GUID iid_range = {0x36dc7aef, 0x33e6, 0x4691, {0xaf, 0xe1, 0x2b, 0xe7, 0x27, 0x4b, 0x3d, 0x33}};
static const GUID iid_toggle = {0x56d00bd0, 0xc4f4, 0x433c, {0xa8, 0x36, 0x1a, 0x52, 0xa5, 0x7e, 0x08, 0x92}};
static const GUID iid_expand = {0xd847d3a5, 0xcab0, 0x4a98, {0x8c, 0x32, 0xec, 0xb4, 0x5c, 0x59, 0xad, 0x24}};
static const GUID iid_select_item = {0x2acad808, 0xb2d4, 0x452d, {0xa4, 0x07, 0x91, 0xff, 0x1a, 0xd1, 0x67, 0xb2}};
static const GUID iid_value = {0xc7935180, 0x6fb3, 0x4201, {0xb1, 0x74, 0x7d, 0xf7, 0x3a, 0xdb, 0xf6, 0x4a}};
static const GUID clsid_acc_prop_services = {0xb5f8350b, 0x0548, 0x48b1, {0xa6, 0xee, 0x88, 0xbd, 0x00, 0xb4, 0xa5, 0xe7}};
static const GUID iid_acc_prop_services = {0x6e26e776, 0x04f0, 0x495d, {0x80, 0xe4, 0x33, 0x30, 0x35, 0x2e, 0x31, 0x69}};
static const GUID propid_acc_name = {0x608d3df8, 0x8128, 0x4aa7, {0xa4, 0x28, 0xf5, 0x5e, 0x49, 0x26, 0x72, 0x91}};

// ---- What a view says ----

// ---- Modal views ----

static uint32_t *modals;  // the A11yModal views, in the order they became modal
static uint32_t nmodals, capmodals;

static bool within(const node *n, const node *a) {
	for (const node *x = n; x; x = x->parent)
		if (x == a) return true;
	return false;
}

// modal_in is the modal view screen readers stay in in top-level window
// top: the A11yModal view that became modal last and shows, unless a
// modal overlay over it is; NULL: none.
static node *modal_in(HWND top) {
	node *v = NULL;
	for (uint32_t i = nmodals; i-- > 0 && !v;) {
		node *m = wh_node(modals[i]);
		if (m && m->hwnd && IsWindowVisible(m->hwnd) && GetAncestor(m->hwnd, GA_ROOT) == top) v = m;
	}
	node *o = wh_modal_overlay(top);
	if (o && (!v || !within(v, o))) return o;
	return v;
}

// outside_modal reports whether a modal view shows in n's window that n
// isn't in: the rest of the window is hidden from screen readers.
static bool outside_modal(const node *n) {
	if (!n->hwnd || (!nmodals && !wh_modal_overlay(GetAncestor(n->hwnd, GA_ROOT)))) return false;
	node *m = modal_in(GetAncestor(n->hwnd, GA_ROOT));
	return m && !within(n, m);
}

static void track_modal(node *n) {
	for (uint32_t i = 0; i < nmodals; i++)
		if (modals[i] == n->id) {
			memmove(modals + i, modals + i + 1, (nmodals - i - 1) * sizeof *modals);
			nmodals--;
			break;
		}
	if (!n->a11y_modal) return;
	if (nmodals == capmodals) modals = wh_realloc(modals, (capmodals = capmodals ? capmodals * 2 : 8) * sizeof *modals);
	modals[nmodals++] = n->id;
}

// wh_a11y_modal_changed tells screen readers a window's views changed
// (a modal view came or went: the rest shows to them, or doesn't).
void wh_a11y_modal_changed(node *n) {
	if (!n->hwnd || !UiaClientsAreListening()) return;
	HWND top = GetAncestor(n->hwnd, GA_ROOT);
	if (top) NotifyWinEvent(EVENT_OBJECT_REORDER, top, OBJID_CLIENT, CHILDID_SELF);
}

// wh_a11y_forget forgets a view going.
void wh_a11y_forget(node *n) {
	if (!n->a11y_modal) return;
	n->a11y_modal = false;
	track_modal(n);
	wh_a11y_modal_changed(n);
}

// a11y_hidden reports whether n, or a view it's in, is hidden from
// screen readers, or a modal view hides it.
static bool a11y_hidden(const node *n) {
	for (const node *x = n; x; x = x->parent)
		if (x->a11y_hidden) return true;
	return outside_modal(n);
}

// collect_texts gathers the texts in n, as its name: their labels, or
// their texts, ", " between them.
static void collect_texts(const node *n, ak_buf *out) {
	for (uint32_t i = 0; i < n->nkids; i++) {
		const node *c = n->kids[i];
		if (c->hidden || c->a11y_hidden) continue;
		const wstr *s = NULL;
		if (c->a11y_label.n) s = &c->a11y_label;
		else if (c->kind == AK_KIND_TEXT && c->text.n) s = &c->text;
		if (!s) {
			collect_texts(c, out);
			continue;
		}
		if (out->len) ak_buf_put(out, L", ", 2 * sizeof(wchar_t));
		ak_buf_put(out, s->p, (size_t)s->n * sizeof(wchar_t));
	}
}

// badge_number is a badge that's a count ("3"): its number; -1: not one.
static long badge_number(const wchar_t *s) {
	size_t n = wcslen(s);
	if (!n || n > 9) return -1;
	long v = 0;
	for (size_t i = 0; i < n; i++) {
		if (s[i] < L'0' || s[i] > L'9') return -1;
		v = v * 10 + (s[i] - L'0');
	}
	return v;
}

// badge_words is a badge as a screen reader says it: a count as so many
// items ("3 items"), else the badge (a new BSTR).
static BSTR badge_words(const wchar_t *badge) {
	long count = badge_number(badge);
	if (count < 0) return SysAllocString(badge);
	char num[16], out[256];
	snprintf(num, sizeof num, "%ld", count);
	const char *vars[] = {"count", num};
	dsk_word_format(out, sizeof out, H.locale[0] ? H.locale : H.env.locale, "badgeCount", vars, 2);
	wchar_t *w = wh_wide(out, (int)strlen(out));
	BSTR b = SysAllocString(w);
	free(w);
	return b;
}

// badged is a title with its badge, if it has one, for a screen reader:
// "Inbox, 3 items" (a new BSTR).
static BSTR badged(const wchar_t *title, const wchar_t *badge) {
	if (!badge || !*badge) return SysAllocString(title);
	BSTR b = badge_words(badge);
	size_t tl = wcslen(title), bl = SysStringLen(b);
	BSTR out = SysAllocStringLen(NULL, (UINT)(tl + 2 + bl));
	if (out) {
		memcpy(out, title, tl * sizeof(wchar_t));
		memcpy(out + tl, L", ", 2 * sizeof(wchar_t));
		memcpy(out + tl + 2, b, bl * sizeof(wchar_t));
	}
	SysFreeString(b);
	return out;
}

// chosen is a tab bar's selected tab's, or a pane's chosen item's, title
// and badge.
static bool chosen(node *n, const wchar_t **title, const wchar_t **badge) {
	*title = *badge = L"";
	if (n->kind == KIND_TAB_BAR && n->nav && n->nav->tabs) {
		node *tabs = n->nav->tabs;
		if (tabs->selected < 0 || (uint32_t)tabs->selected >= tabs->nkids) return false;
		node *s = tabs->kids[tabs->selected];
		*title = wstr_c(&s->title), *badge = wstr_c(&s->badge);
		return true;
	}
	if (n->kind == KIND_SIDEBAR) return wh_pane_chosen(n, title, badge);
	return false;
}

static void set_bool(VARIANT *v, bool b) {
	v->vt = VT_BOOL;
	v->boolVal = b ? VARIANT_TRUE : VARIANT_FALSE;
}

static void set_i4(VARIANT *v, LONG i) {
	v->vt = VT_I4;
	v->lVal = i;
}

static void set_str(VARIANT *v, const wchar_t *s, int n) {
	v->vt = VT_BSTR;
	v->bstrVal = SysAllocStringLen(s, (UINT)n);
}

// choice_property is a tab bar's or a navigation pane's: the focused
// window is the bar, the arrows moving its choice, so it stands for the
// chosen tab (a tab item) or item (a list item): its title and its badge,
// a count as so many items ("Inbox, 3 items"), the badge its item status
// too, as WinUI's InfoBadge says it.
static void choice_property(node *n, PROPERTYID id, VARIANT *v) {
	const wchar_t *title, *badge;
	chosen(n, &title, &badge);
	switch (id) {
	case UIA_NamePropertyId:
		v->vt = VT_BSTR;
		v->bstrVal = badged(title, badge);
		break;
	case UIA_ItemStatusPropertyId:
		if (*badge) {
			v->vt = VT_BSTR;
			v->bstrVal = badge_words(badge);
		}
		break;
	case UIA_ControlTypePropertyId:
		set_i4(v, n->kind == KIND_TAB_BAR ? UIA_TabItemControlTypeId : UIA_ListItemControlTypeId);
		break;
	case UIA_IsControlElementPropertyId:
	case UIA_IsContentElementPropertyId:
		set_bool(v, !a11y_hidden(n));
		break;
	}
}

// related is the view a relation names (A11yControls...), if it shows.
static node *related(uint32_t id) {
	node *t = id ? wh_node(id) : NULL;
	return t && t->hwnd ? t : NULL;
}

// text_of puts what names t in out: its label, its text, the texts in it.
static void text_of(const node *t, ak_buf *out) {
	if (t->a11y_label.n) ak_buf_put(out, t->a11y_label.p, (size_t)t->a11y_label.n * sizeof(wchar_t));
	else if (t->kind == AK_KIND_TEXT && t->text.n) ak_buf_put(out, t->text.p, (size_t)t->text.n * sizeof(wchar_t));
	else collect_texts(t, out);
}

static void set_buf(VARIANT *v, ak_buf *b) {
	if (b->len) set_str(v, (const wchar_t *)b->data, (int)(b->len / sizeof(wchar_t)));
	ak_buf_free(b);
}

// named_by_texts: a view a screen reader names by the texts in it (a
// pressable row, a checkbox and its text...).
static bool named_by_texts(const node *n) {
	switch (n->a11y_role) {
	case AK_ROLE_NONE: return wh_pressable(n) || n->focusable;
	case AK_ROLE_TEXT: case AK_ROLE_HEADER: case AK_ROLE_IMAGE: case AK_ROLE_SEPARATOR: return false;
	}
	return true;
}

// control_type is a view's UIA control type (0: none of its own).
static LONG control_type(const node *n) {
	switch (n->a11y_role) {
	case AK_ROLE_BUTTON: case AK_ROLE_SWITCH: return UIA_ButtonControlTypeId;  // a switch: WinUI's ToggleSwitch's, a button that toggles
	case AK_ROLE_HEADER: case AK_ROLE_TEXT: return UIA_TextControlTypeId;
	case AK_ROLE_IMAGE: return UIA_ImageControlTypeId;
	case AK_ROLE_LINK: return UIA_HyperlinkControlTypeId;
	case AK_ROLE_SEARCH: return UIA_EditControlTypeId;
	case AK_ROLE_CHECKBOX: return UIA_CheckBoxControlTypeId;
	case AK_ROLE_RADIO: return UIA_RadioButtonControlTypeId;
	case AK_ROLE_RADIO_GROUP: case AK_ROLE_GROUP: return UIA_GroupControlTypeId;
	case AK_ROLE_TAB: return UIA_TabItemControlTypeId;
	case AK_ROLE_TAB_LIST: return UIA_TabControlTypeId;
	case AK_ROLE_TAB_PANEL: case AK_ROLE_DIALOG: case AK_ROLE_ALERT_DIALOG: return UIA_PaneControlTypeId;
	case AK_ROLE_SLIDER: return UIA_SliderControlTypeId;
	case AK_ROLE_SPIN_BUTTON: return UIA_SpinnerControlTypeId;
	case AK_ROLE_PROGRESS_BAR: case AK_ROLE_METER: return UIA_ProgressBarControlTypeId;
	case AK_ROLE_MENU: return UIA_MenuControlTypeId;
	case AK_ROLE_MENU_BAR: return UIA_MenuBarControlTypeId;
	case AK_ROLE_MENU_ITEM: case AK_ROLE_MENU_ITEM_CHECKBOX: case AK_ROLE_MENU_ITEM_RADIO: return UIA_MenuItemControlTypeId;
	case AK_ROLE_LISTBOX: return UIA_ListControlTypeId;
	case AK_ROLE_OPTION: return UIA_ListItemControlTypeId;
	case AK_ROLE_COMBO_BOX: return UIA_ComboBoxControlTypeId;
	case AK_ROLE_TOOLTIP: return UIA_ToolTipControlTypeId;
	case AK_ROLE_SEPARATOR: return UIA_SeparatorControlTypeId;
	case AK_ROLE_TOOLBAR: return UIA_ToolBarControlTypeId;
	case AK_ROLE_STATUS: return UIA_StatusBarControlTypeId;
	}
	if (wh_pressable(n)) return UIA_ButtonControlTypeId;
	if (n->kind == AK_KIND_TEXT) return UIA_TextControlTypeId;
	if (n->kind == AK_KIND_IMAGE || n->kind == AK_KIND_SYMBOL) return UIA_ImageControlTypeId;
	if (n->kind == AK_KIND_PROGRESS_RING) return UIA_ProgressBarControlTypeId;  // as WinUI's ring's peer
	if (n->has_range) return UIA_ProgressBarControlTypeId;
	if (n->a11y_label.n || n->focusable) return UIA_GroupControlTypeId;
	return 0;
}

static IUnknown *element_of(node *t);

// set_elements is a relation to t: one element, or an array of one.
static void set_elements(VARIANT *v, node *t, bool array) {
	IUnknown *u = element_of(t);
	if (!u) return;
	if (!array) {
		v->vt = VT_UNKNOWN;
		v->punkVal = u;  // its reference, the variant's
		return;
	}
	SAFEARRAY *sa = SafeArrayCreateVector(VT_UNKNOWN, 0, 1);
	if (sa) {
		LONG i = 0;
		SafeArrayPutElement(sa, &i, u);  // which takes a reference of its own
		v->vt = VT_UNKNOWN | VT_ARRAY;
		v->parray = sa;
	}
	u->lpVtbl->Release(u);
}

// property is a view's property, or VT_EMPTY for its window's own.
static void property(node *n, PROPERTYID id, VARIANT *v) {
	if (n->kind == KIND_TAB_BAR || n->kind == KIND_SIDEBAR) {
		choice_property(n, id, v);
		return;
	}
	node *t;
	switch (id) {
	case UIA_NamePropertyId: {
		ak_buf b = {0};
		if (n->a11y_label.n) ak_buf_put(&b, n->a11y_label.p, (size_t)n->a11y_label.n * sizeof(wchar_t));
		else if (n->kind == AK_KIND_TEXT && n->text.n) ak_buf_put(&b, n->text.p, (size_t)n->text.n * sizeof(wchar_t));
		else if ((t = related(n->a11y_labelled_by))) text_of(t, &b);  // its label's text
		else if (named_by_texts(n)) collect_texts(n, &b);
		set_buf(v, &b);
		break;
	}
	case UIA_HelpTextPropertyId: {
		ak_buf b = {0};
		if (n->a11y_hint.n) ak_buf_put(&b, n->a11y_hint.p, (size_t)n->a11y_hint.n * sizeof(wchar_t));
		else if ((t = related(n->a11y_described_by))) text_of(t, &b);
		else if (n->tooltip.n) ak_buf_put(&b, n->tooltip.p, (size_t)n->tooltip.n * sizeof(wchar_t));
		set_buf(v, &b);
		break;
	}
	case UIA_ControlTypePropertyId: {
		LONG ct = control_type(n);
		if (ct) set_i4(v, ct);
		break;
	}
	case UIA_HeadingLevelPropertyId:
		if (n->a11y_role == AK_ROLE_HEADER) set_i4(v, HEADING_LEVEL_1);
		break;
	case UIA_IsDialogPropertyId:
		if (n->a11y_role == AK_ROLE_DIALOG || n->a11y_role == AK_ROLE_ALERT_DIALOG) set_bool(v, true);
		break;
	case UIA_LiveSettingPropertyId:
		if (n->a11y_role == AK_ROLE_STATUS) set_i4(v, Polite);  // ARIA's status: a polite live region
		else if (n->a11y_role == AK_ROLE_ALERT_DIALOG) set_i4(v, Assertive);
		break;
	case UIA_LabeledByPropertyId:
		if ((t = related(n->a11y_labelled_by))) set_elements(v, t, false);
		break;
	case UIA_ControllerForPropertyId:
		if ((t = related(n->a11y_controls))) set_elements(v, t, true);
		break;
	case UIA_DescribedByPropertyId:
		if ((t = related(n->a11y_described_by))) set_elements(v, t, true);
		break;
	case UIA_IsControlElementPropertyId:
	case UIA_IsContentElementPropertyId:
		if (a11y_hidden(n)) set_bool(v, false);
		else if (n->kind == AK_KIND_TEXT || n->kind == AK_KIND_PROGRESS_RING || wh_pressable(n) || n->a11y_label.n || n->focusable ||
			(n->a11y_role != AK_ROLE_NONE) || n->has_range || n->a11y_value.n)
			set_bool(v, true);
		break;
	}
}

static bool invokes(const node *n) { return n->hwnd && !n->disabled && node_listening(n, AK_EVENT_PRESS); }

// range is a view's range for screen readers (A11yRange: min, max, now),
// or a ProgressRing's value, as a percentage (read only, as WinUI's ring
// has it); an indeterminate ring has none.
static bool range(const node *n, double out[3]) {
	if (n->has_range) {
		memcpy(out, n->range, 3 * sizeof *out);
		return true;
	}
	if (n->kind != AK_KIND_PROGRESS_RING || n->indeterminate) return false;
	out[0] = 0, out[1] = 100, out[2] = n->value * 100;
	return true;
}

static bool checkable_role(int32_t r) { return r == AK_ROLE_CHECKBOX || r == AK_ROLE_SWITCH || r == AK_ROLE_MENU_ITEM_CHECKBOX; }
static bool radio_role(int32_t r) { return r == AK_ROLE_RADIO || r == AK_ROLE_MENU_ITEM_RADIO; }

// toggles: the Toggle pattern (a checkbox, a switch, a checkable item; a
// view with a checked state but a radio's).
static bool toggles(const node *n) { return checkable_role(n->a11y_role) || (n->a11y_checked != AK_A11Y_CHECKED_NONE && !radio_role(n->a11y_role)); }
static bool expands(const node *n) { return n->a11y_expanded != AK_A11Y_EXPANDED_NONE; }
static bool selects(const node *n) {
	switch (n->a11y_role) {
	case AK_ROLE_TAB: case AK_ROLE_OPTION: case AK_ROLE_RADIO: case AK_ROLE_MENU_ITEM_RADIO: return true;
	}
	return n->a11y_selected;
}
static bool selected(const node *n) { return n->a11y_selected || (radio_role(n->a11y_role) && n->a11y_checked == AK_A11Y_CHECKED_CHECKED); }
static bool has_value(const node *n) { return n->a11y_value.n > 0; }

// ---- The providers: COM objects of the host's ----

// A view's provider holds the view's ID (the view may go while a screen
// reader holds it: it then says nothing); its patterns hold the provider.
struct wh_uia {
	IRawElementProviderSimple iface;
	LONG refs;
	uint32_t id;
	HWND hwnd;  // NULL once its window is gone
};

typedef struct {
	IInvokeProvider iface;
	LONG refs;
	struct wh_uia *p;
} invoker;

typedef struct {
	IRangeValueProvider iface;
	LONG refs;
	struct wh_uia *p;
} ranger;

static node *provider_node(struct wh_uia *p) {
	if (!p->hwnd) return NULL;
	node *n = wh_node(p->id);
	return n && n->hwnd == p->hwnd ? n : NULL;
}

static HRESULT STDMETHODCALLTYPE p_query(IRawElementProviderSimple *t, REFIID riid, void **out) {
	if (!out) return E_POINTER;
	*out = NULL;
	if (!IsEqualIID(riid, &iid_unknown) && !IsEqualIID(riid, &iid_simple)) return E_NOINTERFACE;
	*out = t;
	t->lpVtbl->AddRef(t);
	return S_OK;
}

static ULONG STDMETHODCALLTYPE p_add_ref(IRawElementProviderSimple *t) { return (ULONG)InterlockedIncrement(&((struct wh_uia *)t)->refs); }

static ULONG STDMETHODCALLTYPE p_release(IRawElementProviderSimple *t) {
	LONG r = InterlockedDecrement(&((struct wh_uia *)t)->refs);
	if (r == 0) free(t);
	return (ULONG)r;
}

static HRESULT STDMETHODCALLTYPE p_options(IRawElementProviderSimple *t, enum ProviderOptions *out) {
	(void)t;
	*out = ProviderOptions_ServerSideProvider;
	return S_OK;
}

static IInvokeProvider *new_invoker(struct wh_uia *p);
static IRangeValueProvider *new_ranger(struct wh_uia *p);
static IToggleProvider *new_toggler(struct wh_uia *p);
static IExpandCollapseProvider *new_expander(struct wh_uia *p);
static ISelectionItemProvider *new_selector(struct wh_uia *p);
static IValueProvider *new_valuer(struct wh_uia *p);

static HRESULT STDMETHODCALLTYPE p_pattern(IRawElementProviderSimple *t, PATTERNID id, IUnknown **out) {
	*out = NULL;
	struct wh_uia *p = (struct wh_uia *)t;
	node *n = provider_node(p);
	if (!n) return S_OK;
	double v[3];
	if (id == UIA_InvokePatternId && invokes(n)) *out = (IUnknown *)new_invoker(p);
	else if (id == UIA_RangeValuePatternId && range(n, v)) *out = (IUnknown *)new_ranger(p);
	else if (id == UIA_TogglePatternId && toggles(n)) *out = (IUnknown *)new_toggler(p);
	else if (id == UIA_ExpandCollapsePatternId && expands(n)) *out = (IUnknown *)new_expander(p);
	else if (id == UIA_SelectionItemPatternId && selects(n)) *out = (IUnknown *)new_selector(p);
	else if (id == UIA_ValuePatternId && has_value(n)) *out = (IUnknown *)new_valuer(p);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE p_property(IRawElementProviderSimple *t, PROPERTYID id, VARIANT *out) {
	VariantInit(out);  // VT_EMPTY: the window's own
	node *n = provider_node((struct wh_uia *)t);
	if (n) property(n, id, out);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE p_host(IRawElementProviderSimple *t, IRawElementProviderSimple **out) {
	*out = NULL;
	struct wh_uia *p = (struct wh_uia *)t;
	if (p->hwnd && IsWindow(p->hwnd)) UiaHostProviderFromHwnd(p->hwnd, out);
	return S_OK;
}

static IRawElementProviderSimpleVtbl provider_vtbl = {p_query, p_add_ref, p_release, p_options, p_pattern, p_property, p_host};

// Invoke: the view's press.

static HRESULT STDMETHODCALLTYPE i_query(IInvokeProvider *t, REFIID riid, void **out) {
	if (!out) return E_POINTER;
	*out = NULL;
	if (!IsEqualIID(riid, &iid_unknown) && !IsEqualIID(riid, &iid_invoke)) return E_NOINTERFACE;
	*out = t;
	t->lpVtbl->AddRef(t);
	return S_OK;
}

static ULONG STDMETHODCALLTYPE i_add_ref(IInvokeProvider *t) { return (ULONG)InterlockedIncrement(&((invoker *)t)->refs); }

static ULONG STDMETHODCALLTYPE i_release(IInvokeProvider *t) {
	invoker *x = (invoker *)t;
	LONG r = InterlockedDecrement(&x->refs);
	if (r == 0) {
		p_release(&x->p->iface);
		free(x);
	}
	return (ULONG)r;
}

static HRESULT STDMETHODCALLTYPE i_invoke(IInvokeProvider *t) {
	node *n = provider_node(((invoker *)t)->p);
	if (!n || !invokes(n)) return UIA_E_ELEMENTNOTENABLED;
	wh_press(n, n->hwnd);
	return S_OK;
}

static IInvokeProviderVtbl invoke_vtbl = {i_query, i_add_ref, i_release, i_invoke};

static IInvokeProvider *new_invoker(struct wh_uia *p) {
	invoker *x = wh_alloc(sizeof *x);
	x->iface.lpVtbl = &invoke_vtbl;
	x->refs = 1;
	x->p = p;
	p_add_ref(&p->iface);
	return &x->iface;
}

// RangeValue, read only: setting it refuses.

static HRESULT STDMETHODCALLTYPE r_query(IRangeValueProvider *t, REFIID riid, void **out) {
	if (!out) return E_POINTER;
	*out = NULL;
	if (!IsEqualIID(riid, &iid_unknown) && !IsEqualIID(riid, &iid_range)) return E_NOINTERFACE;
	*out = t;
	t->lpVtbl->AddRef(t);
	return S_OK;
}

static ULONG STDMETHODCALLTYPE r_add_ref(IRangeValueProvider *t) { return (ULONG)InterlockedIncrement(&((ranger *)t)->refs); }

static ULONG STDMETHODCALLTYPE r_release(IRangeValueProvider *t) {
	ranger *x = (ranger *)t;
	LONG r = InterlockedDecrement(&x->refs);
	if (r == 0) {
		p_release(&x->p->iface);
		free(x);
	}
	return (ULONG)r;
}

// r_range is the range's min, max and now (0s: none any more).
static void r_range(IRangeValueProvider *t, double out[3]) {
	node *n = provider_node(((ranger *)t)->p);
	if (!n || !range(n, out)) out[0] = out[1] = out[2] = 0;
}

static double r_now(IRangeValueProvider *t) {
	double v[3];
	r_range(t, v);
	return v[2];
}

// r_set steps an adjustable view toward v (its Adjust, as an arrow key
// does); others are read only.
static HRESULT STDMETHODCALLTYPE r_set(IRangeValueProvider *t, double v) {
	node *n = provider_node(((ranger *)t)->p);
	if (!n || !n->adjustable || !n->has_range || n->disabled) return UIA_E_ELEMENTNOTENABLED;
	if (v > n->range[2]) wh_emit_int(n, AK_EVENT_ADJUST, 1);
	else if (v < n->range[2]) wh_emit_int(n, AK_EVENT_ADJUST, -1);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE r_value(IRangeValueProvider *t, double *out) {
	*out = r_now(t);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE r_read_only(IRangeValueProvider *t, BOOL *out) {
	node *n = provider_node(((ranger *)t)->p);
	*out = !(n && n->adjustable && n->has_range && !n->disabled);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE r_max(IRangeValueProvider *t, double *out) {
	double v[3];
	r_range(t, v);
	*out = v[1];
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE r_min(IRangeValueProvider *t, double *out) {
	double v[3];
	r_range(t, v);
	*out = v[0];
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE r_large(IRangeValueProvider *t, double *out) {
	double v[3];
	r_range(t, v);
	*out = (v[1] - v[0]) / 10;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE r_small(IRangeValueProvider *t, double *out) {
	double v[3];
	r_range(t, v);
	*out = (v[1] - v[0]) / 100;
	return S_OK;
}

static IRangeValueProviderVtbl range_vtbl = {r_query, r_add_ref, r_release, r_set, r_value, r_read_only, r_max, r_min, r_large, r_small};

static IRangeValueProvider *new_ranger(struct wh_uia *p) {
	ranger *x = wh_alloc(sizeof *x);
	x->iface.lpVtbl = &range_vtbl;
	x->refs = 1;
	x->p = p;
	p_add_ref(&p->iface);
	return &x->iface;
}

// The patterns of RFC 0010's states: each a COM object holding the
// view's provider.
#define PATTERN(name, Iface, iid)                                                                                       \
	typedef struct {                                                                                                     \
		Iface iface;                                                                                                     \
		LONG refs;                                                                                                       \
		struct wh_uia *p;                                                                                                \
	} name;                                                                                                              \
	static HRESULT STDMETHODCALLTYPE name##_query(Iface *t, REFIID riid, void **out) {                                   \
		if (!out) return E_POINTER;                                                                                      \
		*out = NULL;                                                                                                     \
		if (!IsEqualIID(riid, &iid_unknown) && !IsEqualIID(riid, &iid)) return E_NOINTERFACE;                            \
		*out = t;                                                                                                        \
		t->lpVtbl->AddRef(t);                                                                                            \
		return S_OK;                                                                                                     \
	}                                                                                                                    \
	static ULONG STDMETHODCALLTYPE name##_add_ref(Iface *t) { return (ULONG)InterlockedIncrement(&((name *)t)->refs); } \
	static ULONG STDMETHODCALLTYPE name##_release(Iface *t) {                                                            \
		name *x = (name *)t;                                                                                             \
		LONG r = InterlockedDecrement(&x->refs);                                                                         \
		if (r == 0) {                                                                                                    \
			p_release(&x->p->iface);                                                                                     \
			free(x);                                                                                                     \
		}                                                                                                                \
		return (ULONG)r;                                                                                                 \
	}                                                                                                                    \
	static node *name##_node(Iface *t) { return provider_node(((name *)t)->p); }                                         \
	static Iface *name##_new(struct wh_uia *p, Iface##Vtbl *vtbl) {                                                      \
		name *x = wh_alloc(sizeof *x);                                                                                   \
		x->iface.lpVtbl = vtbl;                                                                                          \
		x->refs = 1;                                                                                                     \
		x->p = p;                                                                                                        \
		p_add_ref(&p->iface);                                                                                            \
		return &x->iface;                                                                                                \
	}

static void press_later(void *ctx) {
	node *n = wh_node((uint32_t)(uintptr_t)ctx);
	if (n && invokes(n)) wh_press(n, n->hwnd);
}

// act presses n for a pattern's action (Toggle, Expand, Select): the
// app's view changes its state. The press comes once the screen reader's
// call has returned, not inside it.
static HRESULT act(node *n) {
	if (!n || !invokes(n)) return UIA_E_ELEMENTNOTENABLED;
	wh_post(press_later, (void *)(uintptr_t)n->id);
	return S_OK;
}

// Toggle: A11yChecked; toggling presses it.
PATTERN(toggler, IToggleProvider, iid_toggle)

static HRESULT STDMETHODCALLTYPE t_toggle(IToggleProvider *t) { return act(toggler_node(t)); }

static HRESULT STDMETHODCALLTYPE t_state(IToggleProvider *t, enum ToggleState *out) {
	node *n = toggler_node(t);
	*out = ToggleState_Off;
	if (n && n->a11y_checked == AK_A11Y_CHECKED_CHECKED) *out = ToggleState_On;
	else if (n && n->a11y_checked == AK_A11Y_CHECKED_MIXED) *out = ToggleState_Indeterminate;
	return S_OK;
}

static IToggleProviderVtbl toggle_vtbl = {toggler_query, toggler_add_ref, toggler_release, t_toggle, t_state};
static IToggleProvider *new_toggler(struct wh_uia *p) { return toggler_new(p, &toggle_vtbl); }

// ExpandCollapse: A11yExpanded; expanding a collapsed one (collapsing an
// expanded one) presses it.
PATTERN(expander, IExpandCollapseProvider, iid_expand)

static HRESULT STDMETHODCALLTYPE e_expand(IExpandCollapseProvider *t) {
	node *n = expander_node(t);
	if (n && n->a11y_expanded == AK_A11Y_EXPANDED_EXPANDED) return S_OK;
	return act(n);
}

static HRESULT STDMETHODCALLTYPE e_collapse(IExpandCollapseProvider *t) {
	node *n = expander_node(t);
	if (n && n->a11y_expanded == AK_A11Y_EXPANDED_COLLAPSED) return S_OK;
	return act(n);
}

static HRESULT STDMETHODCALLTYPE e_state(IExpandCollapseProvider *t, enum ExpandCollapseState *out) {
	node *n = expander_node(t);
	*out = n && n->a11y_expanded == AK_A11Y_EXPANDED_EXPANDED ? ExpandCollapseState_Expanded : ExpandCollapseState_Collapsed;
	return S_OK;
}

static IExpandCollapseProviderVtbl expand_vtbl = {expander_query, expander_add_ref, expander_release, e_expand, e_collapse, e_state};
static IExpandCollapseProvider *new_expander(struct wh_uia *p) { return expander_new(p, &expand_vtbl); }

// SelectionItem: A11ySelected (a radio's: checked); selecting one that
// isn't presses it. Its container: the tab list, listbox, radio group or
// menu it's in.
PATTERN(selector, ISelectionItemProvider, iid_select_item)

static HRESULT STDMETHODCALLTYPE s_select(ISelectionItemProvider *t) {
	node *n = selector_node(t);
	if (n && selected(n)) return S_OK;
	return act(n);
}

static HRESULT STDMETHODCALLTYPE s_remove(ISelectionItemProvider *t) {
	node *n = selector_node(t);
	if (!n || !selected(n)) return S_OK;
	return UIA_E_INVALIDOPERATION;  // one of a group stays chosen
}

static HRESULT STDMETHODCALLTYPE s_is(ISelectionItemProvider *t, BOOL *out) {
	node *n = selector_node(t);
	*out = n && selected(n);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE s_container(ISelectionItemProvider *t, IRawElementProviderSimple **out) {
	*out = NULL;
	node *n = selector_node(t);
	for (node *x = n ? n->parent : NULL; x; x = x->parent) {
		int32_t r = x->a11y_role;
		if (r == AK_ROLE_TAB_LIST || r == AK_ROLE_LISTBOX || r == AK_ROLE_RADIO_GROUP || r == AK_ROLE_MENU || r == AK_ROLE_MENU_BAR) {
			*out = (IRawElementProviderSimple *)element_of(x);
			break;
		}
	}
	return S_OK;
}

static ISelectionItemProviderVtbl select_vtbl = {selector_query, selector_add_ref, selector_release, s_select, s_select, s_remove, s_is,
	s_container};
static ISelectionItemProvider *new_selector(struct wh_uia *p) { return selector_new(p, &select_vtbl); }

// Value: A11yValue, read only.
PATTERN(valuer, IValueProvider, iid_value)

static HRESULT STDMETHODCALLTYPE v_set(IValueProvider *t, LPCWSTR v) {
	(void)t, (void)v;
	return UIA_E_ELEMENTNOTENABLED;
}

static HRESULT STDMETHODCALLTYPE v_value(IValueProvider *t, BSTR *out) {
	node *n = valuer_node(t);
	*out = SysAllocString(n ? wstr_c(&n->a11y_value) : L"");
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE v_read_only(IValueProvider *t, BOOL *out) {
	(void)t;
	*out = TRUE;
	return S_OK;
}

static IValueProviderVtbl value_vtbl = {valuer_query, valuer_add_ref, valuer_release, v_set, v_value, v_read_only};
static IValueProvider *new_valuer(struct wh_uia *p) { return valuer_new(p, &value_vtbl); }

// provider_of is n's provider, made the first time it's asked for.
static struct wh_uia *provider_of(node *n, HWND hwnd) {
	if (!n->uia) {
		struct wh_uia *p = wh_alloc(sizeof *p);
		p->iface.lpVtbl = &provider_vtbl;
		p->refs = 1;
		p->id = n->id, p->hwnd = hwnd;
		n->uia = p;
	}
	return n->uia;
}

// element_of is t as an element a relation names (a reference of its
// own): its provider, or a control's window's.
static IUnknown *element_of(node *t) {
	if (!t || !t->hwnd) return NULL;
	if (t->ctl || t->kind == AK_KIND_NATIVE) {
		IRawElementProviderSimple *h = NULL;
		if (FAILED(UiaHostProviderFromHwnd(t->ctl ? t->ctl : t->hwnd, &h))) return NULL;
		return (IUnknown *)h;
	}
	struct wh_uia *p = provider_of(t, t->hwnd);
	p_add_ref(&p->iface);
	return (IUnknown *)&p->iface;
}

// wh_get_object answers WM_GETOBJECT for UI Automation: n's provider.
LRESULT wh_get_object(node *n, HWND hwnd, WPARAM wp, LPARAM lp, bool *handled) {
	*handled = false;
	if ((LONG)lp != UiaRootObjectId || n->ctl || n->kind == AK_KIND_NATIVE) return 0;  // a control's window, or the app's: theirs
	*handled = true;
	return UiaReturnRawElementProvider(hwnd, wp, lp, &provider_of(n, hwnd)->iface);
}

// wh_drop_provider lets go of a view's provider as its window goes.
void wh_drop_provider(node *n, HWND hwnd) {
	if (!n->uia) return;
	UiaReturnRawElementProvider(hwnd, 0, 0, NULL);
	n->uia->hwnd = NULL;
	p_release(&n->uia->iface);
	n->uia = NULL;
}

// wh_uia_range_changed tells screen readers a ProgressRing's value
// changed, so they read its new value as it fills.
void wh_uia_range_changed(node *n, double old, double now) {
	if (!n->uia || !UiaClientsAreListening()) return;
	VARIANT o, v;
	VariantInit(&o);
	VariantInit(&v);
	o.vt = v.vt = VT_R8;
	o.dblVal = old, v.dblVal = now;
	UiaRaiseAutomationPropertyChangedEvent(&n->uia->iface, UIA_RangeValueValuePropertyId, o, v);
}

// ---- Props ----

static LONG toggle_state(int32_t c) {
	return c == AK_A11Y_CHECKED_CHECKED ? ToggleState_On : c == AK_A11Y_CHECKED_MIXED ? ToggleState_Indeterminate : ToggleState_Off;
}

// raise_i4 and raise_bool tell screen readers a state changed.
static void raise_i4(node *n, PROPERTYID id, LONG old, LONG now) {
	if (!n->uia || !UiaClientsAreListening()) return;
	VARIANT o, v;
	VariantInit(&o);
	VariantInit(&v);
	set_i4(&o, old);
	set_i4(&v, now);
	UiaRaiseAutomationPropertyChangedEvent(&n->uia->iface, id, o, v);
}

static void raise_bool(node *n, PROPERTYID id, bool old, bool now) {
	if (!n->uia || !UiaClientsAreListening()) return;
	VARIANT o, v;
	VariantInit(&o);
	VariantInit(&v);
	set_bool(&o, old);
	set_bool(&v, now);
	UiaRaiseAutomationPropertyChangedEvent(&n->uia->iface, id, o, v);
}

// focusable makes a view with actions or adjustable a tab stop: the
// keyboard reaches its context menu and its arrow keys.
static void focusable(node *n) { wh_update_tab_stop(n); }

// wh_set_a11y sets a screen reader prop (Label is views.c's).
void wh_set_a11y(node *n, uint16_t p, const ak_value *v) {
	switch (p) {
	case AK_PROP_A11Y_HINT:
		wstr_set(&n->a11y_hint, v->s);
		break;
	case AK_PROP_A11Y_ROLE:
		n->a11y_role = (int32_t)v->i;
		break;
	case AK_PROP_A11Y_HIDDEN:
		n->a11y_hidden = v->i != 0;
		break;
	case AK_PROP_A11Y_ACTIONS:
		wh_free_strings(n->a11y_actions, n->na11y_actions);
		n->na11y_actions = wh_decode_strings(&n->a11y_actions, (const uint8_t *)v->s.p, v->s.len);
		focusable(n);
		break;
	case AK_PROP_A11Y_ADJUSTABLE:
		n->adjustable = v->i != 0;
		focusable(n);
		break;
	case AK_PROP_A11Y_CHECKED: {
		int32_t was = n->a11y_checked;
		n->a11y_checked = (int32_t)v->i;
		if (was != n->a11y_checked) {
			raise_i4(n, UIA_ToggleToggleStatePropertyId, toggle_state(was), toggle_state(n->a11y_checked));
			if (radio_role(n->a11y_role)) raise_bool(n, UIA_SelectionItemIsSelectedPropertyId, was == AK_A11Y_CHECKED_CHECKED, selected(n));
		}
		break;
	}
	case AK_PROP_A11Y_EXPANDED: {
		int32_t was = n->a11y_expanded;
		n->a11y_expanded = (int32_t)v->i;
		if (was != n->a11y_expanded)
			raise_i4(n, UIA_ExpandCollapseExpandCollapseStatePropertyId, was == AK_A11Y_EXPANDED_EXPANDED, n->a11y_expanded == AK_A11Y_EXPANDED_EXPANDED);
		break;
	}
	case AK_PROP_A11Y_SELECTED: {
		bool was = n->a11y_selected;
		n->a11y_selected = v->i != 0;
		if (was != n->a11y_selected) raise_bool(n, UIA_SelectionItemIsSelectedPropertyId, was, n->a11y_selected);
		break;
	}
	case AK_PROP_A11Y_VALUE:
		wstr_set(&n->a11y_value, v->s);
		if (n->uia && UiaClientsAreListening()) {
			VARIANT o, now;
			VariantInit(&o);
			VariantInit(&now);
			set_str(&now, wstr_c(&n->a11y_value), n->a11y_value.n);
			UiaRaiseAutomationPropertyChangedEvent(&n->uia->iface, UIA_ValueValuePropertyId, o, now);
			VariantClear(&now);
		}
		break;
	case AK_PROP_A11Y_RANGE: {
		double was = n->range[2];
		n->has_range = v->s.len >= 24;
		if (n->has_range) memcpy(n->range, v->s.p, sizeof n->range);  // f64 min, max, now: little endian, as Windows
		if (n->has_range && was != n->range[2]) wh_uia_range_changed(n, was, n->range[2]);
		break;
	}
	case AK_PROP_A11Y_MODAL:
		n->a11y_modal = v->i != 0;
		track_modal(n);
		wh_a11y_modal_changed(n);
		break;
	case AK_PROP_A11Y_CONTROLS:
		n->a11y_controls = (uint32_t)v->i;
		break;
	case AK_PROP_A11Y_LABELLED_BY:
		n->a11y_labelled_by = (uint32_t)v->i;
		break;
	case AK_PROP_A11Y_DESCRIBED_BY:
		n->a11y_described_by = (uint32_t)v->i;
		break;
	}
	if (n->hwnd && UiaClientsAreListening()) NotifyWinEvent(EVENT_OBJECT_NAMECHANGE, n->hwnd, OBJID_CLIENT, CHILDID_SELF);
}

// wh_adjust_key adjusts an adjustable view with the arrow keys: up and
// the reading direction's forward arrow by one step, the others back.
bool wh_adjust_key(node *n, WPARAM vk) {
	if (!n->adjustable) return false;
	int64_t by;
	switch (vk) {
	case VK_UP: by = 1; break;
	case VK_DOWN: by = -1; break;
	case VK_RIGHT:
	case VK_LEFT:
		by = (vk == VK_LEFT) != wh_is_rtl(n) ? -1 : 1;
		break;
	default:
		return false;
	}
	wh_emit_int(n, AK_EVENT_ADJUST, by);
	return true;
}

typedef struct {
	uint32_t id;
	bool from_keys;
} actions_job;

static void actions_now(void *ctx) {
	actions_job *j = ctx;
	node *n = wh_node(j->id);
	if (n && n->hwnd && n->na11y_actions) {
		actions a = {wh_alloc(n->na11y_actions * sizeof *a.v), n->na11y_actions};
		for (uint32_t i = 0; i < a.n; i++) a.v[i] = (action){.title = n->a11y_actions[i], .index = i};
		bool rtl = wh_is_rtl(n);
		POINT p;
		GetCursorPos(&p);
		if (j->from_keys) {
			RECT r;
			GetWindowRect(n->hwnd, &r);
			p = (POINT){rtl ? r.right : r.left, r.bottom};
		}
		int64_t got = wh_track_actions(&a, rtl, p, GetAncestor(n->hwnd, GA_ROOT));
		free(a.v);
		if (got >= 0 && (n = wh_node(j->id))) wh_emit_int(n, AK_EVENT_A11Y_ACTION, got);
	}
	free(j);
}

// wh_actions_menu shows a view's screen reader actions as a popup menu
// (its context menu), at the mouse or, from the keyboard (lp -1), at the
// view; the action taken is A11yAction.
bool wh_actions_menu(node *n, HWND hwnd, LPARAM lp) {
	(void)hwnd;
	if (!n->na11y_actions) return false;
	actions_job *j = wh_alloc(sizeof *j);
	j->id = n->id;
	j->from_keys = (LONG)lp == -1;
	wh_after_core(actions_now, j);
	return true;
}

// ---- Announcements ----

typedef HRESULT(WINAPI *notify_fn)(IRawElementProviderSimple *, enum NotificationKind, enum NotificationProcessing, BSTR, BSTR);

// announce has screen readers say text, through hwnd's provider (Windows
// 10 1709 and later).
static void announce(HWND hwnd, const wchar_t *text) {
	static notify_fn notify;
	static bool looked;
	if (!looked) {
		looked = true;
		HMODULE m = GetModuleHandleW(L"uiautomationcore.dll");
		if (!m) m = LoadLibraryExW(L"uiautomationcore.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
		if (m) notify = (notify_fn)(void (*)(void))GetProcAddress(m, "UiaRaiseNotificationEvent");
	}
	if (!notify) return;
	IRawElementProviderSimple *host = NULL;
	if (FAILED(UiaHostProviderFromHwnd(hwnd, &host)) || !host) return;
	BSTR s = SysAllocString(text), id = SysAllocString(L"asuka.announce");
	notify(host, NotificationKind_Other, NotificationProcessing_ImportantAll, s, id);
	SysFreeString(s);
	SysFreeString(id);
	host->lpVtbl->Release(host);
}

// wh_announce has screen readers say text (the Announce command).
void wh_announce(const char *s, size_t len) {
	if (!len || !H.window) return;
	wchar_t *w = wh_wide(s, (int)len);
	announce(wh_window_in_use(), w);
	free(w);
}

// wh_announce_choice says the tab or item the keyboard chose.
void wh_announce_choice(node *b) {
	const wchar_t *title, *badge;
	if (!chosen(b, &title, &badge) || !*title || !H.window) return;
	BSTR s = badged(title, badge);
	if (s) announce(wh_window_in_use(), s);
	SysFreeString(s);
}

// ---- A control's accessible name ----

static IAccPropServices *acc_props;  // made once

// wh_set_accessible_name names hwnd (its client object) for screen
// readers; empty: its own name again.
void wh_set_accessible_name(HWND hwnd, const wchar_t *name) {
	if (!hwnd) return;
	if (!name || !*name) {
		wh_clear_accessible_name(hwnd);
		return;
	}
	if (!acc_props && FAILED(CoCreateInstance(&clsid_acc_prop_services, NULL, CLSCTX_INPROC_SERVER, &iid_acc_prop_services, (void **)&acc_props)))
		acc_props = NULL;
	if (!acc_props) return;
	IAccPropServices_SetHwndPropStr(acc_props, hwnd, (DWORD)OBJID_CLIENT, CHILDID_SELF, propid_acc_name, name);
}

// wh_clear_accessible_name drops the name wh_set_accessible_name gave
// hwnd, as Dynamic Annotation asks before the window goes.
void wh_clear_accessible_name(HWND hwnd) {
	if (!acc_props || !hwnd) return;
	MSAAPROPID props[1] = {propid_acc_name};
	IAccPropServices_ClearHwndProps(acc_props, hwnd, (DWORD)OBJID_CLIENT, CHILDID_SELF, props, 1);
}

// ---- Tooltips ----

#define TOOLTIP_MAX_WIDTH_PX 400

// wh_tooltip_on gives the window hwnd the tooltip text, replacing the one
// it had; NULL or empty: none. The tooltip control watches each tool's
// mouse itself (TTF_SUBCLASS), and shows the text where the pointer
// rests, themed as the system's.
void wh_tooltip_on(HWND hwnd, const wchar_t *tip) {
	if (!hwnd) return;
	bool has = tip && *tip;
	if (!H.tips) {
		if (!has) return;
		H.tips = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, L"", WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0, 0, 0, H.window, NULL,
			GetModuleHandleW(NULL), NULL);
		if (!H.tips) return;
		SendMessageW(H.tips, TTM_SETMAXTIPWIDTH, 0, TOOLTIP_MAX_WIDTH_PX);
	}
	TTTOOLINFOW ti = {.cbSize = sizeof ti, .uFlags = TTF_IDISHWND | TTF_SUBCLASS, .hwnd = hwnd, .uId = (UINT_PTR)hwnd,
		.hinst = GetModuleHandleW(NULL)};
	SendMessageW(H.tips, TTM_DELTOOLW, 0, (LPARAM)&ti);
	if (!has) return;
	ti.lpszText = (wchar_t *)tip;  // the control copies it
	SendMessageW(H.tips, TTM_ADDTOOLW, 0, (LPARAM)&ti);
}

// wh_set_tooltip is a view's Tooltip.
void wh_set_tooltip(node *n, const wchar_t *tip) { wh_tooltip_on(n->hwnd, tip); }
