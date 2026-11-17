// keys.c: a hardware keyboard's keys for the views that declare them (the
// Keys prop, UseKeys' on the root or a Window node: DESIGN.md's Keys).
//
// The message loop hands each key going down (WM_KEYDOWN, WM_SYSKEYDOWN,
// and their repeats) to wh_route_key before anything else sees it
// (TranslateMessage, the menus' accelerators, IsDialogMessage's Tab and
// arrows, the host's own keys: Escape closing a panel, a flyout, a sheet
// or a popover; the focused control's: a field's caret, its Enter). It
// starts at the focused window's node (a field's EDIT is its
// TextField's; a window inside a control, a combo box's EDIT, its
// nearest window the host knows), or at the root of the top-level window
// that has the keyboard with nothing in it focused (the main window's
// root, a Window's, a sheet's, a popover's), and goes out through the
// nodes' parents (key_parent): a list's cell on to its list, a sheet's
// node to the window it's declared in, a popover's to its anchor's
// window, ending at the root of a main window or a Window. The nearest
// node whose keys match takes the key, which is consumed, and the core
// gets Key with its index. The key's letting go is consumed with it, and
// Alt's, if the key took Alt (or DefWindowProc would open the menu bar as
// Alt goes up).
//
// The keys of an input method composing text are its own: those it takes
// come as VK_PROCESSKEY, and none is routed while it composes.
#include "nav.h"

#include <imm.h>
#include <wctype.h>

// The modifiers of a shortcut: MenuItem's Mods, the Keys prop's.
enum {
	MOD_PRIMARY = 1,
	MOD_SHIFT_KEY = 2,
	MOD_ALT_KEY = 4,
	MOD_CONTROL_KEY = 8,
};

// wh_set_keys is the Keys prop: its keys, Control as primary (both are
// Ctrl here, as in the menu bar's accelerators).
void wh_set_keys(node *n, const uint8_t *b, size_t len) {
	free(n->keys);
	n->keys = NULL;
	n->nkeys = 0;
	ak_key_vec v = {0};
	ak_decode_keys(b, len, &v);
	if (v.len) {
		n->keys = wh_alloc(v.len * sizeof *n->keys);
		for (uint32_t i = 0; i < v.len; i++) {
			wh_key *k = &n->keys[n->nkeys++];
			// A name too long for any key's is kept unmatchable.
			if (v.data[i].name.len < sizeof k->name) memcpy(k->name, v.data[i].name.p, v.data[i].name.len);
			else snprintf(k->name, sizeof k->name, "\x01");
			k->mods = v.data[i].mods;
			if (k->mods & MOD_CONTROL_KEY) k->mods = (uint8_t)((k->mods & ~MOD_CONTROL_KEY) | MOD_PRIMARY);
		}
	}
	ak_vec_free(&v);
}

// key_char is the character virtual key vk types without modifiers on
// the thread's keyboard layout, in its low word (a dead key's with the
// high bit set), 0: none. Unlike ToUnicodeEx, it leaves the keyboard's
// state (a dead key waiting) as it is.
static uint32_t key_char(uint32_t vk) { return MapVirtualKeyExW(vk, MAPVK_VK_TO_CHAR, GetKeyboardLayout(0)); }

// composing reports whether an input method is composing text in hwnd
// (marked text: a Japanese or Chinese keyboard's, before it's committed).
static bool composing(HWND hwnd) {
	HIMC c = ImmGetContext(hwnd);
	if (!c) return false;
	LONG n = ImmGetCompositionStringW(c, GCS_COMPSTR, NULL, 0);
	ImmReleaseContext(hwnd, c);
	return n > 0;
}

// key_name writes the Keys prop's name of the virtual key vk going down
// into out (16 bytes): a named key's (enter, up, f5...), else the
// character it types without modifiers, lower case. Letters and digits
// are their virtual keys' (as the menu bar's accelerators have them, so
// Ctrl+D is the D key on any layout), the numeric keypad's digits the
// digits; any other key's character is the keyboard layout's. False: a
// key of no name (a modifier, a key typing nothing).
static bool key_name(uint32_t vk, char out[16]) {
	static const struct {
		uint32_t vk;
		const char *name;
	} named[] = {
		{VK_RETURN, "enter"}, {VK_ESCAPE, "escape"}, {VK_TAB, "tab"}, {VK_SPACE, "space"},
		{VK_DELETE, "delete"}, {VK_BACK, "backspace"}, {VK_UP, "up"}, {VK_DOWN, "down"},
		{VK_LEFT, "left"}, {VK_RIGHT, "right"}, {VK_HOME, "home"}, {VK_END, "end"},
		{VK_PRIOR, "pageup"}, {VK_NEXT, "pagedown"},
	};
	for (size_t i = 0; i < WH_LEN(named); i++)
		if (named[i].vk == vk) {
			snprintf(out, 16, "%s", named[i].name);
			return true;
		}
	if (vk >= VK_F1 && vk <= VK_F12) {
		snprintf(out, 16, "f%u", vk - VK_F1 + 1);
		return true;
	}
	if (vk >= 'A' && vk <= 'Z') {
		out[0] = (char)(vk - 'A' + 'a'), out[1] = 0;
		return true;
	}
	if (vk >= '0' && vk <= '9') {
		out[0] = (char)vk, out[1] = 0;
		return true;
	}
	if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
		out[0] = (char)(vk - VK_NUMPAD0 + '0'), out[1] = 0;
		return true;
	}
	if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN || vk == VK_PROCESSKEY) return false;
	wchar_t c = (wchar_t)(key_char(vk) & 0xFFFF);
	if (c <= L' ' || c == 0x7F || !iswprint(c) || (c >= 0xD800 && c <= 0xDFFF)) return false;
	c = (wchar_t)towlower(c);
	int n = WideCharToMultiByte(CP_UTF8, 0, &c, 1, out, 15, NULL, NULL);
	if (n <= 0) return false;
	out[n] = 0;
	return true;
}

// key_mods are the Keys prop's modifiers of the keys held: Ctrl is
// primary (Windows has no other control key: a declared Control is Ctrl
// too, as the menu bar has it).
static uint8_t key_mods(void) {
	uint8_t m = 0;
	if (wh_key_down(VK_CONTROL)) m |= MOD_PRIMARY;
	if (wh_key_down(VK_SHIFT)) m |= MOD_SHIFT_KEY;
	if (wh_key_down(VK_MENU)) m |= MOD_ALT_KEY;
	return m;
}

// match is the index of the key in n's keys a key going down is, or -1.
static int match(const node *n, const char *name, uint8_t mods) {
	for (uint32_t i = 0; i < n->nkeys; i++)
		if (n->keys[i].mods == mods && !strcmp(n->keys[i].name, name)) return (int)i;
	return -1;
}

// window_root is the node a top-level window of the app's shows (the
// root, a Window, a sheet, a popover), or NULL.
static node *window_root(HWND w) {
	if (!w) return NULL;
	node *r = wh_root_of(w);
	if (r) return r;
	node *f = wh_node_of(w);
	if (f && f->kind == KIND_POPOVER_WINDOW) return f->parent;
	return NULL;
}

// key_start is the node a key for hwnd (the window with the keyboard's
// focus) starts at: its window's, or its nearest ancestor's the host
// knows, up to its top-level window: a Window's, a sheet's or a
// popover's (whose node's parent is the Window, the sheet, the popover),
// or the main window's root. NULL: not a window of the app's views (a
// dialog's, the system's).
static node *key_start(HWND hwnd) {
	if (!hwnd) return NULL;
	HWND top = GetAncestor(hwnd, GA_ROOT);
	for (HWND w = hwnd; w; w = GetParent(w)) {
		node *n = wh_node_of(w);
		if (n) return n;
		if (w == top) break;  // GetParent is a popup's owner past here
	}
	if (top == H.window) return H.root;
	return NULL;
}

// key_parent is the node a key goes out to from n: its parent in the
// tree; from a node the core leaves without one, where it shows: a
// list's cell its list, a sheet's node the window it's declared in (its
// Window prop: the root, a Window node), a popover's the root of the
// window its anchor is in (its owner: the main window's root, a Window, a
// sheet, a popover), so its way ends at that window's root, with its
// UseKeys'. The root and a Window are ends: a Window's keys are its own.
// A header's search field goes to the screen it searches; another view
// the host made (a stack's header, a list's row) to the view its window
// is in.
static node *key_parent(node *n) {
	if (n == H.root) return NULL;
	if (n->search_of) {
		node *s = n->search_of->nav ? n->search_of->nav->screen : NULL;
		return s ? s : n->search_of;
	}
	if (n->parent) return n->parent;
	if (n->kind == AK_KIND_SHEET && n->nav) {
		node *w = wh_node(n->nav->on);
		if (w && w->kind == AK_KIND_WINDOW && !w->dying) return w;
		return H.root;
	}
	if (n->kind == AK_KIND_POPOVER && n->nav) {
		node *r = window_root(n->nav->owner);
		if (r && r != n) return r;
		return H.root;
	}
	if (n->kind == AK_KIND_OVERLAY) {
		// On to the root it's over (a sheet's: on from there).
		node *r = wh_overlay_root(n);
		return r && r != n ? r : H.root;
	}
	if (n->kind == AK_KIND_WINDOW || !n->hwnd) return NULL;
	// A list's cell (in its row, in the list), a view the host made.
	HWND top = GetAncestor(n->hwnd, GA_ROOT);
	for (HWND w = n->hwnd; w != top && w;) {
		w = GetParent(w);
		node *p = wh_node_of(w);
		if (p && p != n) return p;
	}
	if (top == H.window && top != n->hwnd) return H.root;
	return NULL;
}

// wh_route_key gives a key message to the nearest view whose Keys have
// it, as the message loop gets it; false: the key goes on.
bool wh_route_key(MSG *m) {
	switch (m->message) {
	case WM_KEYDOWN:
	case WM_SYSKEYDOWN:
		break;
	case WM_KEYUP:
	case WM_SYSKEYUP:
		if (m->wParam < WH_LEN(H.key_ups) && H.key_ups[m->wParam]) {
			H.key_ups[m->wParam] = 0;
			return true;
		}
		return false;
	default:
		return false;
	}
	if (m->wParam == VK_PROCESSKEY || m->wParam >= WH_LEN(H.key_ups)) return false;
	// Pressed, not repeated: a letting go missed (it went to another app's
	// window) isn't waited for any longer.
	if (!(m->lParam & (1 << 30))) H.key_ups[m->wParam] = 0;
	if (wh_key_down(VK_LWIN) || wh_key_down(VK_RWIN)) return false;  // the system's shortcuts, none of the Keys prop's
	node *from = key_start(m->hwnd);
	if (!from) return false;
	char name[16];
	if (!key_name((uint32_t)m->wParam, name) || composing(m->hwnd)) return false;
	uint8_t mods = key_mods();
	for (node *n = from; n; n = key_parent(n)) {
		int i = match(n, name, mods);
		if (i < 0) continue;
		H.key_ups[m->wParam] = 1;
		if (mods & MOD_ALT_KEY) H.key_ups[VK_MENU] = 1;
		wh_emit_int(n, AK_EVENT_KEY, i);
		return true;
	}
	return false;
}
