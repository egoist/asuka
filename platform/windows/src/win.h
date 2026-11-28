// win.h: the app's top-level windows as it controls them (winctl.c: the
// main one and each Window's; titlebar.c, placement.c, flyout.c), and what
// the window procedure asks of the rest of the host.
#ifndef ASUKA_WIN_WIN_H
#define ASUKA_WIN_WIN_H

#include "parts.h"

// A hidden title bar's caption buttons' window (titlebar.c).
typedef struct {
	HWND hwnd;
	int shown[3];
	int nshown;
	int hot, pressed;  // indexes; -1: none
	bool tracking, active;
} wh_caption;

typedef struct {
	LONG_PTR style, ex;
	WINDOWPLACEMENT place;
	bool maximized;
} wh_full;

struct wh_wctl {
	HWND hwnd;
	bool main;
	node *n;      // a Window's node; the main window's root, once it's known
	node *frame;  // a Window's top-level window's node (its scale)
	ak_window_config cfg;
	char *key;    // where its placement is kept (NULL: nowhere)
	uint8_t *cfg_bytes;  // the config as it came: cfg's spans point into it
	HWND owner;   // a child's owner, disabled while a modal one is open

	// Its placement (placement.c).
	bool placed, sizing, restore_max;
	RECT normal;

	bool active;
	wh_full *full;
	ak_window_state sent;
	bool reported;

	bool hidden;          // its title bar is hidden
	wh_caption *caption;  // its caption buttons, then
	HWND top_edge;        // the strip along its top that resizes it, then

	// A panel, a flyout, a window sized to its content (flyout.c).
	float fit[2];
	bool fitted, wait_fit, show_pending, show_activate, app_placed, dismiss_pending;

	bool through;  // it lets the mouse through (but over its views that take clicks)
};

#define CAPTION_HEIGHT 32

// winctl.c
wh_wctl *wh_wctl_new(HWND hwnd, bool main);
void wh_wctl_free(wh_wctl *c);
wh_wctl *wh_ctl_of(HWND hwnd);
wh_wctl *wh_ctl_for(node *n);
bool wh_ctl_has(const wh_wctl *c, uint32_t flag);
float wh_ctl_k(const wh_wctl *c);
node *wh_ctl_node(wh_wctl *c);
DWORD wh_ctl_style(const wh_wctl *c);
void wh_ctl_set_config(wh_wctl *c, const uint8_t *args, size_t len);
void wh_configure(wh_wctl *c);
void wh_least(wh_wctl *c, float out[2]);
void wh_outer_size(wh_wctl *c, float w, float h, int *ow, int *oh);
void wh_min_max(wh_wctl *c, MINMAXINFO *mi);
void wh_clamp_size(wh_wctl *c, float *w, float *h);
void wh_size_content(wh_wctl *c, float w, float h);
void wh_move_to(wh_wctl *c, float x, float y);
void wh_center(wh_wctl *c, HWND from);
void wh_show_ctl(wh_wctl *c, bool activate);
void wh_focus_ctl(wh_wctl *c);
void wh_enter_full_screen(wh_wctl *c);
void wh_exit_full_screen(wh_wctl *c);
float wh_safe_top(const wh_wctl *c);
void wh_window_action(node *n, const uint8_t *args, size_t len);
void wh_report_state(wh_wctl *c);
LRESULT wh_ctl_message(wh_wctl *c, UINT m, WPARAM wp, LPARAM lp, bool *handled);
RECT wh_work_area(HWND hwnd);
RECT wh_monitor_work_area(RECT r);
RECT wh_frame_bounds(HWND hwnd);
void wh_show_main(void);

// titlebar.c
void wh_hide_title_bar(wh_wctl *c);
void wh_forget_chrome(wh_wctl *c);
void wh_layout_chrome(wh_wctl *c);
void wh_layout_all_chrome(void);
LRESULT wh_title_bar_message(wh_wctl *c, UINT m, WPARAM wp, LPARAM lp, bool *handled);
LRESULT wh_chrome_message(wh_wctl *c, HWND hwnd, UINT m, WPARAM wp, LPARAM lp, bool *handled);
void wh_caption_activate(wh_wctl *c, bool active);
int wh_hidden_top(void);

// placement.c
void wh_save_placement(wh_wctl *c);
bool wh_restore_placement(wh_wctl *c);
void wh_placement_message(wh_wctl *c, UINT m, WPARAM wp);

// flyout.c
bool wh_frameless(const wh_wctl *c);
DWORD wh_ex_style(wh_wctl *c);
void wh_configure_frameless(wh_wctl *c);
void wh_set_click_through(wh_wctl *c, bool on);
void wh_apply_click_through(wh_wctl *c, bool on);
// clickregion.c
void wh_click_region_update(void);
void wh_click_region_tick(void);
void wh_set_takes_clicks(node *n, bool on);
void wh_click_region_dump(ak_buf *b, HWND top);
bool wh_click_through(HWND hwnd);
void wh_before_show(wh_wctl *c);
void wh_fit_window(wh_wctl *c, float w, float h);
void wh_place_flyout(wh_wctl *c);
LRESULT wh_light_dismiss_message(wh_wctl *c, UINT m, WPARAM wp, LPARAM lp, bool *handled);
void wh_window_activated(HWND hwnd);
bool wh_escape_dismisses(MSG *m);
void wh_tray_clicked(uint32_t id);
void wh_center_over(wh_wctl *c, HWND over);
bool wh_is_frameless(HWND hwnd);

// appwindow.c: Window nodes' top-level windows, WindowOptions
wh_wctl *wh_nav_ctl(node *n);
bool wh_nav_flyout(node *n, ak_flyout *f);
void wh_nav_min_size(node *n, float out[2]);
void wh_nav_fitted(node *n, float w, float h);
void wh_open_window(node *n);
void wh_close_window(node *n);
void wh_window_dpi_changed(node *frame, int dpi, const RECT *suggested);
LRESULT wh_window_frame_message(node *frame, UINT m, WPARAM wp, LPARAM lp, bool *handled);
void wh_set_window_title(const char *title, size_t len);
void wh_window_options(node *n, const uint8_t *args, size_t len);
HWND wh_parent_window(uint32_t id);
void wh_report_screen(node *n, float w, float h);

// The window procedure's partners.
HICON wh_app_icon(void);
bool wh_menu_command(int id);
LRESULT wh_menu_item_message(HWND hwnd, UINT m, WPARAM wp, LPARAM lp, bool *handled);
LRESULT wh_menu_bar_message(HWND hwnd, UINT m, WPARAM wp, LPARAM lp, bool *handled);
void wh_clear_menu_styles(void);
void wh_build_menu_bar(void);
bool wh_accelerate(HWND win, MSG *m);
void wh_tick(void);
void wh_animation_step(void);
void wh_msg_timer(WPARAM id);
LRESULT wh_copy_data(LPARAM lp, bool *handled);
LRESULT wh_module_message(UINT m, WPARAM wp, LPARAM lp, bool *handled);
LRESULT wh_native_message(node *n, UINT m, WPARAM wp, LPARAM lp, bool *handled);
LRESULT wh_sheet_message(node *n, UINT m, WPARAM wp, LPARAM lp, bool *handled);
LRESULT wh_popover_message(node *n, UINT m, WPARAM wp, LPARAM lp, bool *handled);
LRESULT wh_alert_dialog_message(node *n, UINT m, WPARAM wp, LPARAM lp, bool *handled);
void wh_window_destroyed(HWND hwnd);
void wh_badge_message(UINT m);
void wh_dock_message(UINT m);
LRESULT wh_quit_message(UINT m, WPARAM wp, LPARAM lp, bool *handled);
bool wh_ask_quit(bool ending);
void wh_remove_tray_icons(void);
void wh_intl_env(wh_env *e);
void wh_read_intl(void);
float wh_zoom_up_to(HWND hwnd, HWND top);
void wh_nav_window_dark(node *n, bool dark);
void wh_nav_window_redraw(node *n);
void wh_retheme_rich(node *n);
void wh_flush_routes(void);
void wh_cancel_sheet(node *n);
void wh_cancel_popover(node *n);
void wh_cancel_alert_dialog(node *n);
void wh_fit_covers(HWND hwnd);
void wh_round_covers(HWND hwnd);
bool wh_tray_icon_rect(uint32_t id, RECT *r);
bool wh_snapshotting(void);

// main.c, automation.c
bool wh_pre_translate(MSG *m);
void wh_automation(void);
void wh_automation_timer(void);
void wh_snapshot(const char *path);
char *wh_dump(size_t *len);
bool wh_on_screen(node *n);
bool wh_write_png(const char *path, int w, int h, const uint8_t *bgra, int stride);
void wh_hover_at(HWND top, POINT screen);
void wh_hover_moves(HWND top, POINT screen);
bool wh_accelerator(const char *key, uint8_t mods, ACCEL *out);
bool wh_open_bar_menu(const wchar_t *title);
bool wh_choose_menu_item(const wchar_t *title);
bool wh_tap_nav(const wchar_t *text);
// wh_tap_overlay is automation's tap on what the host makes outside the
// views: a snackbar's buttons, an alert dialog's, a sidebar's items, a
// Wheel's or Picker's items (snackbar.c; nav.h has each part's).
bool wh_tap_overlay(const wchar_t *text);
void wh_list_dump(node *n, ak_buf *b, int depth, void (*walk)(ak_buf *, node *, int));
HWND wh_nav_window(node *n);
void wh_web_close_all(void);

// instance.c: one instance, the links the app opens with, quitting
const wchar_t *wh_instance_title(void);
bool wh_take_instance(int argc, wchar_t **argv);
void wh_take_login_arg(int *argc, wchar_t **argv);
void wh_after_launch(int argc, wchar_t **argv);
void wh_finish_quit(void);
void wh_incoming_url(const char *u, size_t n);
void wh_send_pending_urls(void);
void wh_to_front(void);
void wh_quit(bool relaunch);
bool wh_dock_item_launch(int argc, wchar_t **argv);
void wh_start_toasts(void);
void wh_take_toast_launch(int *argc, wchar_t **argv);  // notify.c

// update.c: at launch
void wh_relaunch_wait(void);
void wh_update_clean(void);

// crash.c
void wh_crash_start(void);

// locale.c
void wh_word_app(wchar_t *buf, size_t cap, const char *key);
void wh_say_words(void);
void wh_mirror_window(HWND win, node *content);
DWORD wh_top_level_style(void);
void wh_nav_say_words(node *n);
// anim.c, commands.c
void wh_begin_animation(const uint8_t *args, size_t len);
void wh_end_animation(void);
void wh_listen_location(node *n);
void wh_open_url(const char *s, size_t n);
void wh_module_timer(WPARAM id);
void wh_share_text(const char *s, size_t n);
// menu.c
int64_t wh_track_menu(const uint8_t *menu, size_t len, POINT p, HWND owner);
int64_t wh_track_actions(const actions *a, bool rtl, POINT p, HWND owner);
void wh_set_menu_bar(const uint8_t *b, size_t len);
void wh_set_window_menu(wh_wctl *c);
void wh_read_saved_locale(void);
void wh_set_locale(const uint8_t *args, size_t len);
void wh_save_locale(const char *tag, size_t len);

// native.c: the core's native_* callbacks, the registry's
void wh_native_host_callbacks(asuka_host_callbacks *cb);

// modules.c: the host's own modules
void wh_call(node *root, const uint8_t *args, size_t len);

// notify.c
void wh_wait_for_toast(void);

#endif
