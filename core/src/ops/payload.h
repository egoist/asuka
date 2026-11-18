// payload.h: the encodings some props, commands and events carry in
// their bytes, as ops/actions.go, menu.go, alert.go, keys.go, item.go,
// rich.go, sidebar.go, window.go and location.go write and read them.
// Included by ops/ops.h.
//
// Encoders append to an ak_buf (a batch's, from ak_batch_set_bytes_begin
// or ak_batch_command_begin, writes in place). Decoders append what they
// read to a vector the caller owns and frees (ak_vec_free); the spans in
// what they read point into the bytes decoded. Like Go's, they keep what
// they could read of bytes cut short, whole items only, and say so by
// returning false.
#ifndef AK_OPS_PAYLOAD_H
#define AK_OPS_PAYLOAD_H

// Actions (ops/actions.go): a view's Menu, a List cell's swipe actions, a
// Screen's bar buttons and an Alert's Buttons.
//
// A menu is u32 count, then its items. An item is u8 AlertButtonRole, a
// u32-length title, a u32-length icon (empty: none) and u8 menu flags;
// with AK_MENU_SUBMENU, its items follow, a menu: a submenu, or with
// AK_MENU_INLINE a section shown in place. Items are numbered depth
// first, a submenu or a section before its own items; an Action event
// carries the chosen item's number.

// Menu flags (ops.MenuFlags).
enum {
	AK_MENU_SEPARATOR = 1 << 0,
	AK_MENU_DISABLED = 1 << 1,
	AK_MENU_CHECKED = 1 << 2,
	AK_MENU_DESTRUCTIVE = 1 << 3,
	AK_MENU_SUBMENU = 1 << 4,  // the item opens a submenu, which follows it
	AK_MENU_INLINE = 1 << 5,   // with AK_MENU_SUBMENU: a section shown in place (action menus)
};

// Menu roles (ops.MenuRole): what an item does that the host does itself.
enum {
	AK_MENU_ROLE_NONE,
	AK_MENU_ROLE_QUIT,
	AK_MENU_ROLE_CLOSE,
	AK_MENU_ROLE_OPEN_RECENT,
};

// A Screen's Action events say which of its menus the item is in: its bar
// buttons' carry the item's number, its Toolbar's
// AK_SCREEN_TOOLBAR_ACTIONS + the number, its Primary's
// AK_SCREEN_PRIMARY_ACTION + the number.
#define AK_SCREEN_TOOLBAR_ACTIONS (INT64_C(1) << 32)
#define AK_SCREEN_PRIMARY_ACTION (INT64_C(2) << 32)

// An item of an action menu, to encode: items are its submenu's or
// section's.
typedef struct ak_action_item {
	uint8_t role;  // AK_ALERT_BUTTON_ROLE_*
	ak_span title, icon;
	uint8_t flags;
	const struct ak_action_item *items;
	uint32_t count;
} ak_action_item;

// An item of an action menu, decoded: in a flat list, depth first, so an
// item's index is its place in the list; its own items follow it.
typedef struct {
	uint32_t index;
	uint32_t depth;   // 0: the menu's own
	uint32_t parent;  // its submenu's item's index; UINT32_MAX at depth 0
	uint32_t count;   // its own items (not theirs)
	uint8_t role;
	ak_span title, icon;
	uint8_t flags;
} ak_action;

typedef AK_VEC(ak_action) ak_action_vec;

// ak_append_actions appends a menu. An item with items, or with
// AK_MENU_INLINE, gets AK_MENU_SUBMENU.
void ak_append_actions(ak_buf *b, const ak_action_item *items, uint32_t n);
bool ak_decode_actions(const uint8_t *b, size_t n, ak_action_vec *out);
// ak_count_actions is how many numbers a menu's items take.
uint32_t ak_count_actions(const ak_action_item *items, uint32_t n);
// ak_action_named is the index of the first item titled title, or -1.
int64_t ak_action_named(const ak_action *items, uint32_t n, ak_span title);

// Menus (ops/menu.go): the app's menu bar and tray and Dock menus.
//
// A menu is u32 count, then its items. An item is u8 menu flags, a
// u32-length title, a u32-length key (a character or a named key; empty:
// no shortcut), u8 modifiers (1 primary, 2 shift, 4 alt, 8 control) and
// u8 menu role; with AK_MENU_SUBMENU its submenu follows, a menu. A menu
// bar is u32 count, then per menu a u32-length title and a menu. Items
// are numbered depth first across a menu bar's menus.

typedef struct ak_menu_item {
	uint8_t flags;
	ak_span title, key;
	uint8_t mods, role;
	const struct ak_menu_item *items;
	uint32_t count;
} ak_menu_item;

typedef struct {
	ak_span title;
	const ak_menu_item *items;
	uint32_t count;
} ak_menu_bar_menu;

// A menu's item, decoded, depth first as ak_action.
typedef struct {
	uint32_t index;
	uint32_t depth, parent, count;
	uint32_t menu;  // in a menu bar: its menu's index
	uint8_t flags;
	ak_span title, key;
	uint8_t mods, role;
} ak_menu_entry;

// A menu bar's menu, decoded: its items are entries first..first+total-1.
typedef struct {
	ak_span title;
	uint32_t first, total;
} ak_menu_title;

typedef AK_VEC(ak_menu_entry) ak_menu_entry_vec;
typedef AK_VEC(ak_menu_title) ak_menu_title_vec;

// ak_append_menu appends a menu: an item with items gets AK_MENU_SUBMENU.
void ak_append_menu(ak_buf *b, const ak_menu_item *items, uint32_t n);
void ak_append_menu_bar(ak_buf *b, const ak_menu_bar_menu *menus, uint32_t n);
bool ak_decode_menu(const uint8_t *b, size_t n, ak_menu_entry_vec *out);
bool ak_decode_menu_bar(const uint8_t *b, size_t n, ak_menu_title_vec *menus, ak_menu_entry_vec *items);

// An alert's fields (ops/alert.go): u32 count, then per field a
// u32-length placeholder, text, u8 flags, u8 Keyboard and a u32-length
// content type. FieldChanged's payload is u32 the field's index, then its
// text, u32-length.

enum { AK_ALERT_FIELD_SECURE = 1 };

typedef struct {
	ak_span placeholder, text;
	uint8_t flags;     // AK_ALERT_FIELD_*
	uint8_t keyboard;  // AK_KEYBOARD_*
	ak_span content_type;
} ak_alert_field;

typedef AK_VEC(ak_alert_field) ak_alert_field_vec;

void ak_append_alert_fields(ak_buf *b, const ak_alert_field *fs, uint32_t n);
bool ak_decode_alert_fields(const uint8_t *b, size_t n, ak_alert_field_vec *out);
void ak_append_field_changed(ak_buf *b, uint32_t index, ak_span text);
bool ak_decode_field_changed(const uint8_t *b, size_t n, uint32_t *index, ak_span *text);
// ak_default_alert_action is the number of an alert's default button,
// the first of its buttons (items, decoded) whose role is Default; -1 if
// there's none.
int64_t ak_default_alert_action(const ak_action *items, uint32_t n);

// Keys (ops/keys.go): a view's Keys prop: u32 count, per key a u32-length
// name (a lower case character or a named key) and u8 modifiers.

typedef struct {
	ak_span name;
	uint8_t mods;
} ak_key;

typedef AK_VEC(ak_key) ak_key_vec;

void ak_append_keys(ak_buf *b, const ak_key *keys, uint32_t n);
bool ak_decode_keys(const uint8_t *b, size_t n, ak_key_vec *out);
// ak_key_name is a key's name as the Keys prop has it: a single
// character lower case (in buf, which takes 4 bytes), a named key as it
// is.
ak_span ak_key_name(ak_span name, char buf[4]);
// ak_match_key is the index of the key in keys that a key going down is,
// or -1.
int ak_match_key(const ak_key *keys, uint32_t n, ak_span name, uint8_t mods);

// Things shared or dragged (ops/item.go): a Shared or Drop event's items,
// a view's DragItems.

typedef struct {
	uint8_t kind;  // AK_SHARE_KIND_*
	ak_span text;   // a text's text, a link's URL
	ak_span path;   // a file's
	ak_span name;   // the file's name where it came from
	ak_span mime;
	int64_t size;  // the file's size in bytes, -1 if unknown
	bool value;    // DragItems: it carries an app's value
} ak_item;

typedef AK_VEC(ak_item) ak_item_vec;
typedef AK_VEC(ak_span) ak_str_vec;

// Items: u32 count, per item u8 ShareKind, string text, path, name, MIME
// type, i64 size.
void ak_append_items(ak_buf *b, const ak_item *items, uint32_t n);
bool ak_decode_items(const uint8_t *b, size_t n, ak_item_vec *out);
// Drag items: the same, with u8 flags (1: value) for the size.
void ak_append_drag_items(ak_buf *b, const ak_item *items, uint32_t n);
bool ak_decode_drag_items(const uint8_t *b, size_t n, ak_item_vec *out);

// A Drop event's payload: f32 x, y, u32 source, u8 flags (1: copied),
// then the items.
typedef struct {
	float x, y;
	ak_id source;  // the view dragged from, in the app; 0: another app's
	bool copied;   // the files are the app's own copies
} ak_drop;

void ak_append_drop(ak_buf *b, const ak_drop *d, const ak_item *items, uint32_t n);
bool ak_decode_drop(const uint8_t *b, size_t n, ak_drop *d, ak_item_vec *items);

// A list of strings: u32 count, then each u32-length (DropAccept's).
void ak_append_strings(ak_buf *b, const ak_span *s, uint32_t n);
bool ak_decode_strings(const uint8_t *b, size_t n, ak_str_vec *out);
// ak_match_mime reports whether a MIME type matches any of the patterns:
// an exact type, a family (image/*) or anything (*/*, *), regardless of
// case and parameters.
bool ak_match_mime(const ak_span *patterns, uint32_t n, ak_span mime);

// Styled text (ops/rich.go): RichText's Content, a Rich TextField's Rich.
//
// u32 style count, per style u8 TextStyle, f32 size, u8 FontWeight, u8
// flags, u64 color, u64 background, a u32-length link; then u32
// paragraph count, per paragraph u32 run count, per run u32 style and a
// u32-length text.

enum {
	AK_RICH_ITALIC = 1,
	AK_RICH_MONO = 2,
	AK_RICH_UNDERLINE = 4,
	AK_RICH_STRIKE = 8,
	AK_RICH_INLINE = 16,
};

typedef struct {
	uint8_t text_style;  // AK_TEXT_STYLE_*
	float size;
	uint8_t weight;  // AK_FONT_WEIGHT_*
	uint8_t flags;   // AK_RICH_*
	ak_color color, background;
	ak_span link;
} ak_rich_style;

typedef struct {
	ak_rich_style style;
	ak_span text;  // a "\n" ends a paragraph
} ak_rich_run;

typedef AK_VEC(ak_rich_run) ak_rich_run_vec;

bool ak_rich_style_eq(const ak_rich_style *a, const ak_rich_style *b);
// ak_append_rich appends runs canonically: styles in the order runs first
// use them, a paragraph per line, runs of one style merged.
void ak_append_rich(ak_buf *b, const ak_rich_run *runs, uint32_t n);
// ak_decode_rich reads a run per encoded run, each paragraph but the
// first starting with a "\n" run of the last style.
bool ak_decode_rich(const uint8_t *b, size_t n, ak_rich_run_vec *out);

// A SplitView's sidebar (ops/sidebar.go): u32 section count, per section
// a u32-length title and u32 item count, per item a u32-length title,
// icon and badge.

typedef struct {
	ak_span title, icon, badge;
} ak_sidebar_item;

typedef struct {
	ak_span title;
	const ak_sidebar_item *items;
	uint32_t count;
} ak_sidebar_section;

// A section, decoded: its items are first..first+count-1.
typedef struct {
	ak_span title;
	uint32_t first, count;
} ak_sidebar_heading;

typedef AK_VEC(ak_sidebar_heading) ak_sidebar_heading_vec;
typedef AK_VEC(ak_sidebar_item) ak_sidebar_item_vec;

void ak_append_sidebar(ak_buf *b, const ak_sidebar_section *s, uint32_t n);
bool ak_decode_sidebar(const uint8_t *b, size_t n, ak_sidebar_heading_vec *sections, ak_sidebar_item_vec *items);
// ak_same_sidebar_shape reports whether two decoded sidebars have as many
// sections, each with as many items.
bool ak_same_sidebar_shape(const ak_sidebar_heading *a, uint32_t na, const ak_sidebar_heading *b, uint32_t nb);
// ak_badge_number reports the count a badge says, if it's only ASCII
// digits, at most nine.
bool ak_badge_number(ak_span s, int32_t *n);

// Windows (ops/window.go).

// Window flags (ops.WindowFlags).
enum {
	AK_WINDOW_FIXED_SIZE = 1 << 0,
	AK_WINDOW_NO_MINIMIZE = 1 << 1,
	AK_WINDOW_NO_MAXIMIZE = 1 << 2,
	AK_WINDOW_NO_CLOSE = 1 << 3,
	AK_WINDOW_ALWAYS_ON_TOP = 1 << 4,
	AK_WINDOW_HIDDEN = 1 << 5,
	AK_WINDOW_SKIP_TASKBAR = 1 << 6,
	AK_WINDOW_CENTERED = 1 << 7,
	AK_WINDOW_POSITIONED = 1 << 8,
	AK_WINDOW_TRANSPARENT = 1 << 9,
	AK_WINDOW_MAXIMIZED = 1 << 10,
	AK_WINDOW_FULLSCREEN = 1 << 11,
	AK_WINDOW_CHILD = 1 << 12,
	AK_WINDOW_MODAL = 1 << 13,
	AK_WINDOW_PANEL = 1 << 14,
	AK_WINDOW_FLYOUT = 1 << 15,
	AK_WINDOW_FIT_CONTENT = 1 << 16,
	AK_WINDOW_CLICK_THROUGH = 1 << 17,
	AK_WINDOW_MENU_BAR = 1 << 18,
};

// A WindowState event's flags (ops.WindowStateFlags).
enum {
	AK_WINDOW_STATE_VISIBLE = 1 << 0,
	AK_WINDOW_STATE_FOCUSED = 1 << 1,
	AK_WINDOW_STATE_MINIMIZED = 1 << 2,
	AK_WINDOW_STATE_MAXIMIZED = 1 << 3,
	AK_WINDOW_STATE_FULLSCREEN = 1 << 4,
	AK_WINDOW_STATE_ON_TOP = 1 << 5,
	AK_WINDOW_STATE_CLICK_THROUGH = 1 << 6,
};

// A window's options when it opens (the WindowOptions command, a
// Window's Config prop): f32 width, height, min width, min height, max
// width, max height, x, y; u32 flags; u8 TitleBar; u64 background; u32
// parent; u32-length state key; then the title, to the end.
typedef struct {
	float width, height;
	float min_width, min_height;
	float max_width, max_height;
	float x, y;
	uint32_t flags;     // AK_WINDOW_*
	uint8_t title_bar;  // AK_TITLE_BAR_*
	ak_color background;
	ak_id parent;
	ak_span state_key;
	ak_span title;
} ak_window_config;

void ak_append_window_config(ak_buf *b, const ak_window_config *c);
// ak_decode_window_config reads what ak_append_window_config writes;
// args cut short leave the rest zero, and under 32 bytes they're the
// older form (width, height, min width, min height, then the title).
ak_window_config ak_decode_window_config(const uint8_t *b, size_t n);

// A WindowState event: f32 x, y, width, height, u8 flags.
typedef struct {
	float x, y, width, height;
	uint8_t flags;  // AK_WINDOW_STATE_*
} ak_window_state;

void ak_append_window_state(ak_buf *b, const ak_window_state *s);
ak_window_state ak_decode_window_state(const uint8_t *b, size_t n);

// A WindowAction command: u8 WindowAction, then for Size, Fit and
// Position f32 x, y.
void ak_append_window_action(ak_buf *b, uint8_t action, float x, float y);
bool ak_decode_window_action(const uint8_t *b, size_t n, uint8_t *action, float *x, float *y);

// What a flyout window hangs from (a Window's Flyout prop): u32 tray, f32
// x, y, width, height, u8 placement, f32 gap.
typedef struct {
	uint32_t tray;  // 0: the rectangle
	float x, y, width, height;
	uint8_t placement;  // AK_PLACEMENT_*
	float gap;
} ak_flyout;

void ak_append_flyout(ak_buf *b, const ak_flyout *f);
bool ak_decode_flyout(const uint8_t *b, size_t n, ak_flyout *f);

// A Router's address (ops/location.go). The Location command is u8
// LocationAction, i32 delta, a u32-length href, state and window key; the
// Location event is the href and the state, u32-length each.
typedef struct {
	uint8_t action;  // AK_LOCATION_ACTION_*
	int32_t delta;
	ak_span href, state, window;
} ak_location;

void ak_append_location(ak_buf *b, const ak_location *l);
bool ak_decode_location(const uint8_t *b, size_t n, ak_location *l);
void ak_append_location_event(ak_buf *b, ak_span href, ak_span state);
bool ak_decode_location_event(const uint8_t *b, size_t n, ak_span *href, ak_span *state);

// The error overlay asuka dev shows (ops/deverror.go): the DevError
// command is u8 flags (AK_DEV_ERROR_RELOAD, AK_DEV_ERROR_BUILD), then the
// title, the message, the source and the details, u32-length each.
enum { AK_DEV_ERROR_RELOAD = 1, AK_DEV_ERROR_BUILD = 2 };

typedef struct {
	uint8_t flags;
	ak_span title, message, source, details;
} ak_dev_error;

void ak_append_dev_error(ak_buf *b, const ak_dev_error *e);
bool ak_decode_dev_error(const uint8_t *b, size_t n, ak_dev_error *e);

#endif
