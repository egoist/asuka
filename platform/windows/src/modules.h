// modules.h: the host's own modules (modules.c and the files of each:
// tray.c, dock.c, notify.c, hotkey.c, periodic.c, apps.c, services.c,
// update.c), what std packages call by name: platform/host's call and
// send (the Call command, answered with Result) and platform/host's
// Modules (a native class the host serves itself). A method gets its
// arguments' bytes and answers once, through its reply, on the main
// thread; a long poll keeps its reply until something comes.
#ifndef ASUKA_WIN_MODULES_H
#define ASUKA_WIN_MODULES_H

#include "win.h"
#include "asuka_windows.h"

typedef struct wh_reply wh_reply;

// A module's method: args are the call's bytes (valid during the call).
typedef void (*wh_method)(wh_reply *r, const uint8_t *args, size_t len);

typedef struct {
	const char *name;
	wh_method fn;
} wh_method_def;

// A module: its name and its methods, ended by a {NULL, NULL}.
typedef struct {
	const char *name;
	const wh_method_def *methods;
} wh_module_def;

// Each file's modules.
extern const wh_module_def wh_tray_module, wh_dock_module, wh_notify_module, wh_hotkey_module, wh_periodic_module,
	wh_apps_module, wh_update_module, wh_recent_module, wh_login_module, wh_background_module, wh_contacts_module,
	wh_calendar_module, wh_library_module, wh_compose_module, wh_photo_library_module, wh_push_module,
	wh_widget_module, wh_live_activity_module, wh_icloud_module, wh_view_module, wh_assets_module,
	wh_image_module;

// ---- Answers (main thread; each reply once) ----

void wh_reply_ok(wh_reply *r, const void *p, size_t n);
void wh_reply_nil(wh_reply *r);
void wh_reply_str(wh_reply *r, const char *s);
void wh_reply_u8(wh_reply *r, uint8_t v);
void wh_reply_u32(wh_reply *r, uint32_t v);
// wh_reply_json answers with what w wrote (and frees it).
void wh_reply_json(wh_reply *r, dsk_jw *w);
void wh_reply_fail(wh_reply *r, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
	__attribute__((format(printf, 2, 3)))
#endif
	;
// wh_reply_gone reports whether the call's task went (platform/host's
// cancelling): its answer goes nowhere.
bool wh_reply_gone(const wh_reply *r);

// A long poll's replies waiting, in order.
typedef struct {
	wh_reply **v;
	uint32_t n, cap;
} wh_replies;

void wh_replies_add(wh_replies *q, wh_reply *r);
wh_reply *wh_replies_take(wh_replies *q);  // the first, or NULL
// wh_replies_fail fails every one waiting with msg ("stopped").
void wh_replies_fail(wh_replies *q, const char *msg);

// A queue of what happened, for "next" methods: answered at once if one
// waits, else kept (at most max; 0: no limit).
typedef struct {
	uint8_t **items;
	size_t *lens;
	uint32_t n, cap, max;
	wh_replies waiting;
} wh_queue;

void wh_queue_next(wh_queue *q, wh_reply *r);
void wh_queue_deliver(wh_queue *q, const void *p, size_t n);
void wh_queue_stop(wh_queue *q);

// ---- Work, windows and timers of the modules' ----

// wh_off_main runs work(ctx) on a thread of its own (COM in a
// single-threaded apartment), then done(ctx) on the main thread.
void wh_off_main(void (*work)(void *ctx), void (*done)(void *ctx), void *ctx);

// wh_module_window makes a window of the modules' class whose messages go
// to fn first (handled false: the default's): message-only, or a hidden
// top-level one (what broadcasts reach: TaskbarCreated).
HWND wh_module_window(bool message_only, LRESULT (*fn)(HWND hwnd, UINT m, WPARAM wp, LPARAM lp, bool *handled));

// wh_set_timer calls fn(ctx) on the main thread after ms, again every ms
// while repeat; wh_kill_timer stops it. IDs are never 0.
UINT_PTR wh_set_timer(UINT ms, bool repeat, void (*fn)(void *ctx), void *ctx);
void wh_kill_timer(UINT_PTR id);

// ---- Shared helpers ----

// wh_cache_path is the app's caches joined with name (free it), its
// directory made.
char *wh_cache_path(const char *name);
// wh_known_folder is a known folder's path, UTF-8 (free it), or NULL.
char *wh_known_folder(const GUID *id);
// wh_safe_file_name is s as a file name (or def), without what Windows
// refuses in one, at most 80 bytes (free it).
char *wh_safe_file_name(const char *s, const char *def);
// wh_shell_open opens a file or URL as Explorer does (owner: the window of
// any UI it shows): 0, or ERROR_NO_ASSOCIATION, or another error.
DWORD wh_shell_open(HWND owner, const char *verb, const char *file);
// wh_scheme_has_app reports whether an app is set for URLs of scheme.
bool wh_scheme_has_app(const char *scheme);
// wh_file_exists reports whether a file or directory is at path.
bool wh_file_exists(const char *path);
// wh_win_error is Windows' message for an error (free it).
char *wh_win_error(DWORD e);
// wh_app_user_model_id is the app's AppUserModelID: asuka.toml's id, else
// one of its name.
const char *wh_app_user_model_id(void);
// wh_local_file is a file a call names as a file on this computer
// (asuka_desktop_local_file's: an http(s) URL downloaded to the caches, a
// file: URL's path, a path, an asset): NULL with *err set (free both).
// Blocks: never the main thread.
char *wh_local_file(const char *source, char **err);

// ---- WinRT and COM: asuka_windows.h's (HSTRINGs, factories, slots) ----

// wh_xml_document is an XmlDocument of xml, as IXmlDocument (release it).
void *wh_xml_document(const char *xml);
// wh_xml_escape appends s escaped for XML text or an attribute.
void wh_xml_escape(ak_buf *b, const char *s);

// ---- The taskbar (dock.c), the tray (tray.c), the toasts (notify.c) ----

// wh_taskbar is ITaskbarList3, made once (NULL if none).
void *wh_taskbar(void);
// wh_set_badge_count shows n on the app's taskbar button (0: none).
bool wh_set_badge_count(int n);
// wh_tray_event reports what happened to icon id (0: the Dock menu):
// kind 1 a click, 2 the menu's item numbered index.
void wh_tray_event(uint32_t id, uint8_t kind, uint32_t index);
// wh_icon_from_pixels is an icon of w×h BGRA pixels, straight alpha, top
// row first (destroy it).
HICON wh_icon_from_pixels(const uint8_t *px, int w, int h);
// wh_glyph_coverage draws a glyph of face, size pixels square and
// centered, in gray: how much of each pixel it covers (free it).
uint8_t *wh_glyph_coverage(uint16_t glyph, const wchar_t *face, int size);
// wh_decode_image decodes an image file (WIC) to premultiplied BGRA, its
// longest side at most max (0: as it is), upright: free *px.
bool wh_decode_image_file(const wchar_t *path, int max, uint8_t **px, int *w, int *h);
bool wh_image_file_size(const wchar_t *path, int *w, int *h);
// wh_encode_image writes w×h BGRA pixels (straight alpha) as a PNG or
// JPEG file (WIC).
bool wh_encode_image_file(const wchar_t *path, bool jpeg, const uint8_t *px, int w, int h);
// wh_online reports whether the PC has a network (unknown: yes).
bool wh_online(void);
// wh_keep_system_awake keeps the system from sleeping while on.
void wh_keep_system_awake(bool on);

#endif
