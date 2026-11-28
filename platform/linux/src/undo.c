// undo.c: undo and redo, as GNOME's apps have them. The app's MenuBar
// items of the undo and redo roles are the main menu's Undo and Redo:
// "Undo Rename" (the name UndoState gives, in the host's words), enabled
// while the app has something to undo, or a text being edited has the
// focus, Ctrl+Z and Ctrl+Shift+Z unless the item has a shortcut of its
// own. Choosing one undoes the focused text's typing (GtkText's and
// GtkTextView's own history, their text.undo and text.redo actions), else
// the app's last change (the item's Action, which the core takes as its
// undo). Without such items, Ctrl+Z and Ctrl+Shift+Z where no text has the
// focus send Undo to the root (keys.c asks lx_undo_key): a text keeps its
// own.
#include "host.h"

#include <stdlib.h>

static struct {
	uint8_t flags;  // 1 something to undo, 2 something to redo
	char *names[2];  // what: Rename (NULL: no name)
	bool known;
	GSimpleAction *items[2];  // the menu's items' actions (NULL: none)
	bool disabled[2];         // what the core says of them (the stack's)
} U;

// lx_text_focus is the text being edited that has the focus (a GtkText, an
// entry's; a GtkTextView), or NULL.
GtkWidget *lx_text_focus(void);
GtkWidget *lx_text_focus(void) {
	GtkWindow *w = H.app ? gtk_application_get_active_window(H.app) : NULL;
	if (!w && H.main) w = H.main->window;
	GtkWidget *f = w ? gtk_window_get_focus(w) : NULL;
	if (!f || !gtk_widget_is_sensitive(f)) return NULL;
	if (gtk_text_get_type && AK_IS(f, gtk_text_get_type)) return gtk_editable_get_editable((GtkEditable *)f) ? f : NULL;
	if (AK_IS(f, gtk_text_view_get_type)) return gtk_text_view_get_editable((GtkTextView *)f) ? f : NULL;
	return NULL;
}

// text_can is whether the focused text can undo (redo): a GtkTextView's
// buffer says; a GtkText keeps its history to itself (its action is
// tried).
static bool text_can(GtkWidget *t, bool redo) {
	if (AK_IS(t, gtk_text_view_get_type)) {
		GtkTextBuffer *b = gtk_text_view_get_buffer((GtkTextView *)t);
		return b && (redo ? gtk_text_buffer_get_can_redo(b) : gtk_text_buffer_get_can_undo(b));
	}
	return true;
}

static void update_items(void) {
	GtkWidget *t = lx_text_focus();
	for (int i = 0; i < 2; i++)
		if (U.items[i]) g_simple_action_set_enabled(U.items[i], t ? text_can(t, i == 1) : U.known ? (U.flags & (1 << i)) != 0 : !U.disabled[i]);
}

// lx_undo_title is an Undo (redo) item's title, in the host's words:
// "Undo", "Undo Rename".
const char *lx_undo_title(bool redo);
const char *lx_undo_title(bool redo) {
	const char *name = U.names[redo];
	if (str_empty(name)) return lx_word(redo ? "redo" : "undo");
	static char out[2][512];
	const char *vars[] = {"action", name};
	dsk_word_format(out[redo], sizeof out[0], S(H.locale), redo ? "redoAction" : "undoAction", vars, 2);
	return out[redo];
}

// lx_undo_item takes the menu's Undo (redo) item's action as the menu is
// made (NULL: the menu has none now), disabled as the core says.
void lx_undo_item(bool redo, GSimpleAction *a, bool disabled);
void lx_undo_item(bool redo, GSimpleAction *a, bool disabled) {
	U.items[redo] = a;
	U.disabled[redo] = disabled;
	if (a) update_items();
}

// lx_undo_enabled is whether the menu's Undo (redo) item can be chosen.
bool lx_undo_enabled(bool redo);
bool lx_undo_enabled(bool redo) {
	GtkWidget *t = lx_text_focus();
	return U.items[redo] && (t ? text_can(t, redo) : U.known ? (U.flags & (1 << redo)) != 0 : !U.disabled[redo]);
}

void lx_menu_rebuild(void);

// lx_undo_state takes UndoState: what's undoable, and the names, which
// the menu's items say.
void lx_undo_state(ak_span args);
void lx_undo_state(ak_span args) {
	ak_reader r = {(const uint8_t *)args.p, args.len, 0, false};
	uint8_t flags = ak_read_u8(&r);
	ak_span names[2];
	names[0] = ak_read_str(&r);
	names[1] = ak_read_str(&r);
	if (r.err) return;
	bool renamed = false;
	for (int i = 0; i < 2; i++)
		if (!U.names[i] || strlen(U.names[i]) != names[i].len || memcmp(U.names[i], names[i].p, names[i].len)) {
			str_set(&U.names[i], names[i]);
			renamed = true;
		}
	U.flags = flags;
	U.known = true;
	if (renamed && (U.items[0] || U.items[1])) lx_menu_rebuild();  // their titles
	update_items();
}

// lx_undo_focus_changed: the focus moved (a text's own undo, or the app's).
void lx_undo_focus_changed(void);
void lx_undo_focus_changed(void) { update_items(); }

// lx_undo_choose undoes (redoes): the focused text's typing, else the
// app's change (Action index on the root, the item's; -1: Undo).
void lx_undo_choose(bool redo, int64_t index);
void lx_undo_choose(bool redo, int64_t index) {
	GtkWidget *t = lx_text_focus();
	if (t) {
		gtk_widget_activate_action(t, redo ? "text.redo" : "text.undo", NULL);
		return;
	}
	if (!H.root) return;
	if (index >= 0)
		lx_dispatch(&(ak_event){.id = H.root->id, .kind = AK_EVENT_ACTION, .i = index});
	else
		lx_emit(H.root, (ak_event){.kind = AK_EVENT_UNDO, .i = redo ? 1 : 0});
}

// lx_undo_key takes Ctrl+Z (undo) or Ctrl+Shift+Z (redo) where no text has
// the focus and the menu has no item for it: Undo to the root, if it
// listens. False: not those, or not the app's (a text's own).
bool lx_undo_key(ak_span name, uint8_t mods);
bool lx_undo_key(ak_span name, uint8_t mods) {
	if (name.len != 1 || name.p[0] != 'z' || (mods != 1 && mods != 3)) return false;
	bool redo = mods == 3;
	if (U.items[redo] || lx_text_focus() || !listening(H.root, AK_EVENT_UNDO)) return false;
	lx_emit(H.root, (ak_event){.kind = AK_EVENT_UNDO, .i = redo ? 1 : 0});
	return true;
}
