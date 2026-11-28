// nav.h: navigation's state (nav.c, cmdbar's, the Windows', sheets',
// alerts', popovers', snackbars').
#ifndef ASUKA_WIN_NAV_H
#define ASUKA_WIN_NAV_H

#include "win.h"

typedef struct {
	node *stack;
	node *screen;   // the screen whose toolbar it shows
	HWND *buttons;  // the toolbar's, in order
	uint32_t nbuttons;
	uint32_t fit;   // how many of them fit before the overflow button
	HWND more, primary;
} wh_cmdbar;

struct wh_nav {
	wh_env env;
	bool reported;

	// Screen.
	bool back_gesture;
	char *zoom_from;  // the shared key of the view below it zooms out of ("": none)
	wstr search, search_text;

	// Stack, Tabs: the bar above the screens.
	node *bar;

	// A stack's header.
	node *stack;
	HWND back;
	HWND *buttons;  // the screen's bar buttons
	uint32_t nbuttons;
	node *screen;   // the screen whose title and buttons it shows
	node *field;    // its search field, once a screen had one

	// A tab bar.
	node *tabs;
	int hot;

	// A screen's Toolbar and Primary; a stack's command bar (foot), and
	// that bar's state (cmd).
	actions toolbar, primary;
	node *foot;
	wh_cmdbar *cmd;

	// Sheet, Window: its window; a sheet's owner. Sheet, Alert: the window
	// node it's declared in (its Window prop).
	HWND window, owner;
	uint32_t on;
	bool full;  // a Sheet's FullScreen

	// Sheet: its Detents (kinds and values, in the app's order), the one
	// it's at (1 + its index; 0: the shortest), the tallest that leaves
	// its window usable (1 + its index; 0: none, modal), its corners'
	// radius, and its content's size as the core last sent it (Fit; 0:
	// none yet). A sheet whose one detent is fit opens once it's sized.
	uint8_t *det_kinds;
	float *det_vals;
	uint32_t ndet, detent, undimmed;
	float corner, fit_w, fit_h, shown_h;
	bool no_dismiss, wait_fit;

	// Alert: its task dialog's window while it shows; its text fields
	// (their bytes), and with them the host's own dialog.
	HWND dialog;
	bool tasking;     // show_alert's task dialog runs: it frees the alert's node
	int task_button;  // a button that ends it
	uint8_t *fields;
	uint32_t fields_len;
	void *dlg;

	// Popover: what it points at, and its content's size (points).
	uint32_t anchor;
	float pop_w, pop_h;

	// Window: its content's size when it opens, and its least, in points;
	// its options (Config), and its window as the app controls it.
	float size[2], min_size[2];
	uint8_t *cfg;
	uint32_t cfg_len;
	wh_wctl *ctl;
	bool fitted;
	ak_flyout flyout;
	bool has_flyout;
	bool edited;
};

#define HEADER_HEIGHT 48
#define TAB_BAR_HEIGHT 48
#define CMD_BAR_HEIGHT 48

node *wh_top_screen(node *stack);
// transition.c: a stack's push and pop, animated (WinUI's entrance and
// drill-in); end stops one running in stack (NULL: any).
void wh_transition_begin(node *stack, node *shown, node *screen, bool push);
void wh_transition_end(node *stack);
uint32_t wh_live_screens(node *stack);
void wh_show_screens(node *p);
void wh_update_header(node *stack);
void wh_select_tab(node *tabs, int i);
void wh_close_sheet(node *n);
HWND wh_present_on(uint32_t id);
HWND wh_top_sheet(HWND w);
void wh_update_search(node *b, node *screen);
void wh_layout_search(node *b, int x, bool rtl, int width);
void wh_update_command_bar(node *stack);
void wh_layout_command_bar(node *b);
float wh_cmd_bar_height_of(node *stack);
bool wh_tap_command_bar(node *b, const wchar_t *text);
void wh_forget_command_bar(node *stack);
void wh_badge_size(node *n, const wstr *badge, int *w, int *h);
RECT wh_badge_at(node *n, const wstr *badge, int end, int top, int bottom, bool rtl);
void wh_paint_badge(node *n, HDC dc, gfx g, RECT r, const wstr *badge);
void wh_announce_choice(node *b);

// snackbar.c, popover.c, alertdialog.c
void wh_set_snack_prop(node *n, uint16_t p, const ak_value *v);
void wh_present_snack(node *n);
void wh_destroy_snack(node *n);
void wh_place_snacks(void);
void wh_set_popover_prop(node *n, uint16_t p, const ak_value *v);
void wh_open_popover(node *n);
void wh_close_popover(node *n);
void wh_open_alert_dialog(node *n);
void wh_close_alert_dialog(node *n);
void wh_update_alert_dialog(node *n);
HWND wh_alert_dialog_window(node *n);
bool wh_snack_tap(const wchar_t *text);
bool wh_alert_dialog_tap(const wchar_t *text);
bool wh_pane_tap(const wchar_t *text);
bool wh_combo_tap(const wchar_t *text);

#endif
