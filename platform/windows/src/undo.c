// undo.c: the app's undo stack (the core's) on Windows. UndoState says
// what there is to undo and redo and their names; the MenuBar's items of
// the roles undo and redo say "Undo Rename" in the host's words, are
// enabled while there's something to undo (or the text field the
// keyboard is in can undo its own typing), and take Ctrl+Z, and Ctrl+Y
// and Ctrl+Shift+Z for redo, where they have no shortcut of their own.
// Chosen, they undo the field's typing if one has the keyboard (EM_UNDO,
// a rich edit's EM_REDO), else the app's (the item's Action, which the
// core runs as an undo). The shortcuts work without any such item too:
// pressed where no field has the keyboard, they're the root's Undo
// event. A field keeps its own Ctrl+Z: the host never takes it from one.
#include "win.h"

#include <richedit.h>

static struct {
	uint8_t flags;  // 1 something to undo, 2 something to redo
	wchar_t *names[2];
} U;

void wh_undo_state(const uint8_t *args, size_t len) {
	if (len < 1) return;
	U.flags = args[0];
	size_t o = 1;
	for (int i = 0; i < 2; i++) {
		free(U.names[i]);
		U.names[i] = NULL;
		if (o + 4 > len) continue;
		uint32_t l = wh_u32(args + o);
		o += 4;
		if (o + l > len) break;
		U.names[i] = l ? wh_wide((const char *)args + o, (int)l) : NULL;
		o += l;
	}
}

// wh_undo_field is the text field the keyboard is in (an EDIT, a rich
// edit), or NULL.
HWND wh_undo_field(void) {
	HWND f = GetFocus();
	if (!f) return NULL;
	wchar_t cls[64];
	if (!GetClassNameW(f, cls, (int)WH_LEN(cls))) return NULL;
	if (_wcsicmp(cls, L"Edit") && _wcsnicmp(cls, L"RichEdit", 8)) return NULL;
	if (GetWindowLongPtrW(f, GWL_STYLE) & ES_READONLY) return NULL;  // a RichText's: nothing typed in it
	return f;
}

static bool rich(HWND f) {
	wchar_t cls[64];
	return GetClassNameW(f, cls, (int)WH_LEN(cls)) && !_wcsnicmp(cls, L"RichEdit", 8);
}

// wh_undo_enabled reports whether Undo (or Redo) does something: the
// field's, if one has the keyboard, else the app's.
bool wh_undo_enabled(bool redo) {
	HWND f = wh_undo_field();
	if (f) {
		if (redo) return rich(f) && SendMessageW(f, EM_CANREDO, 0, 0);
		return SendMessageW(f, EM_CANUNDO, 0, 0) != 0;
	}
	return (U.flags & (redo ? 2 : 1)) != 0;
}

// wh_undo_title is what an Undo or Redo item says: "Undo Rename", the
// app's last change's name, or "Undo" for one without, or a field's.
const wchar_t *wh_undo_title(bool redo, wchar_t *buf, size_t cap) {
	const wchar_t *name = wh_undo_field() ? NULL : U.names[redo];
	if (!name || !*name) {
		wcsncpy(buf, wh_word(redo ? "redo" : "undo"), cap - 1);
		buf[cap - 1] = 0;
		return buf;
	}
	char *u = wh_utf8(name, -1, NULL);
	const char *vars[] = {"action", u};
	wh_word_vars(buf, cap, redo ? "redoAction" : "undoAction", vars, 2);
	free(u);
	return buf;
}

// wh_undo_chosen is an Undo or Redo item chosen (index: its number): the
// field's typing undone, or the app's change (the item's Action).
void wh_undo_chosen(bool redo, int64_t index) {
	HWND f = wh_undo_field();
	if (f) {
		if (redo) {
			if (rich(f)) SendMessageW(f, EM_REDO, 0, 0);
		} else {
			SendMessageW(f, EM_UNDO, 0, 0);
		}
		return;
	}
	if (!(U.flags & (redo ? 2 : 1))) return;
	node *r = wh_root_of(wh_window_in_use());
	if (!r) r = H.root;
	if (r && index >= 0) wh_emit_int(r, AK_EVENT_ACTION, index);
}

// wh_undo_key takes Ctrl+Z, Ctrl+Shift+Z and Ctrl+Y where no field has
// the keyboard and no menu item took them: the root's Undo event, if it
// listens for it.
bool wh_undo_key(MSG *m) {
	if (m->message != WM_KEYDOWN || (m->wParam != 'Z' && m->wParam != 'Y')) return false;
	if (!wh_key_down(VK_CONTROL) || wh_key_down(VK_MENU) || wh_key_down(VK_LWIN) || wh_key_down(VK_RWIN)) return false;
	bool shift = wh_key_down(VK_SHIFT);
	if (m->wParam == 'Y' && shift) return false;
	if (wh_undo_field()) return false;  // the field's own
	HWND top = GetAncestor(m->hwnd, GA_ROOT);
	node *r = wh_root_of(top);
	if (!r || !node_listening(r, AK_EVENT_UNDO)) r = H.root;
	if (!r || !node_listening(r, AK_EVENT_UNDO)) return false;
	wh_emit_int(r, AK_EVENT_UNDO, m->wParam == 'Y' || shift ? 1 : 0);
	return true;
}
