// ui_ext.h: the UI runtime's media, graphics and interaction parts, and
// the hooks the shared files (element.c, elements.c, root.c, style.c)
// call into them.
//
//	interaction.c  interaction states (pressed, hovered, a drag, a drag
//	               over), the style's variants, gestures, screen
//	               readers' actions, drags and drops
//	keys.c         views' keys (the keys style key), the root's
//	animate.c      animate { }, the frames a display draws (onFrame)
//	media.c        Image (hash placeholders, animated images), Video,
//	               WebView, native views
//	imagehash.c    BlurHash and ThumbHash decoders
//	rich.c         RichText and its inline components, Selectable, a
//	               selectable Text, a rich TextField, its paste types,
//	               formats and accessory
//	canvas.c       Canvas and Graphics, Path, Shape, Shader
//	svg.c          SVG documents read at run time, drawn as a Canvas's
//	               commands (std/svg's Document, an Image's SVG)
//	share.c        share, haptics, the clipboard, URLs, announcements,
//	               native modules' calls (Call and Result), what other
//	               apps open the app with or share with it
//	locale.c       the language the app shows, its direction, regional
//	               formats; catalogs (i18n.c, plural_gen.c)
#ifndef AK_UI_EXT_H
#define AK_UI_EXT_H

#include "ui/ui_int.h"

// ---- A view's interaction state (ak_view's ix) ----

// A style layer (ak_ui_layer): style values laid over a view's own
// while it's on. Of the layers on that set a key, the last in order
// wins: by tier, then (state layers) by state, then as they were made.
typedef struct {
	uint8_t tier;   // AK_LAYER_* (ui.h)
	uint8_t state;  // an AK_STATE_ bit: on only while the view is in it; 0: none
	bool on;        // its condition's (true without one)
	bool block;     // the block's variant of its state (ak_ui_variant's)
} ak_layer;

// A layer's value of a key.
typedef struct {
	uint16_t layer;
	uint16_t key;
	ak_val v;
} ak_layer_ent;

// A variant the component that shows the view declares: its id (the
// lowering's) and its condition (retained).
typedef struct {
	int64_t id;
	ak_closure *cond;
} ak_vdecl;

typedef struct ak_ix {
	uint8_t state;    // the states it's in (AK_STATE_*)
	uint8_t listens;  // the states its view reports
	ak_node sig[AK_STATE__COUNT];  // per state, for ui.state's readers
	AK_VEC(ak_layer) layers;       // by id, as they were made
	AK_VEC(ak_layer_ent) lents;    // their values, a key once per layer
	AK_VEC(ak_style_ent) base;     // the style's own values of the keys layers set
	AK_VEC(ak_vdecl) vdecls;       // the variants its components declare
	bool hooked;      // leaves its states when its owner is disposed
	// restoreFocus: the view that had the focus when it came (retained).
	ak_view *restore;
	// a11yControls, a11yLabelledBy, a11yDescribedBy as last sent (focus.c).
	ak_id rel_sent[3];
	// Keys: the keys style key's entries in the Keys prop's order.
	uint32_t *key_order;
	uint32_t nkeys;
	// A drag's items as last sent, encoded.
	ak_buf drag_sent;
	bool drag_known;
	// The a11y actions' names as last sent, encoded.
	ak_buf actions_sent;
	bool actions_known;
	// A Window's view's keys (WindowKeys'), as the root's (keys.c).
	struct ak_rootkeys *wkeys;
} ak_ix;

// ak_ix_of is e's interaction state, made the first time.
ak_ix *ak_ix_of(ak_view *e);
void ak_ix_free(ak_view *e);

// ak_ix_set_state sets a state the core works out itself (focused,
// focusVisible), for what follows it; ak_ix_styles_state reports whether
// e has a variant of state.
void ak_ix_set_state(ak_view *e, uint8_t bit, bool on);
bool ak_ix_styles_state(const ak_view *e, uint8_t bit);

// ---- The keyboard's focus (focus.c) ----

// ak_focus_report hears a view take the focus (how 1: a pointer or the
// app; 2: the keyboard) or lose it (0).
void ak_focus_report(ak_view *e, int how);
// ak_focus_current is the view with the focus in r (NULL: none known).
ak_view *ak_focus_current(ak_ui_root *r);
// ak_focus_state_now is a focus state of e, from where the focus is.
bool ak_focus_state_now(ak_view *e, uint8_t bit);
void ak_focus_forget(ak_ui_root *r);
// ak_focus_destroyed hears e's view go: if it had the focus, the views
// around it no longer have it.
void ak_focus_destroyed(ak_view *e);
// ak_focus_container_set applies a container's focus keys: false when
// key isn't one.
bool ak_focus_container_set(ak_view *e, uint16_t key, ak_val v, const ak_val *prev);
// ak_focus_ring_styled hears that e styles focusVisible: the host's ring
// goes; ak_focus_view_made sends that as e gets its view.
void ak_focus_ring_styled(ak_view *e);
void ak_focus_view_made(ak_view *e);
// ak_focus_autofocus gives the focus to e, or the first view in it that
// takes it.
void ak_focus_autofocus(ak_view *e);
// The accessibility keys that name views: ak_focus_rel_set sends one,
// and ak_focus_rels_flush sends again what changed (a Ref's view came).
bool ak_focus_rel_key(uint16_t key);
void ak_focus_rel_set(ak_view *e, uint16_t key);
void ak_focus_rels_flush(ak_ui_root *r);

// ---- Overlays (floating.c) ----

// ak_place is where an overlay of size w by h goes next to anchor, in
// bounds (its root's, inside the safe area and above the keyboard), all
// in the root's coordinates (place.c): its side flipped to the opposite
// when it doesn't fit and there's more room there, moved along its side
// to stay gap from the bounds' edges, and the room it has on that side.
typedef struct {
	float x, y, w, h;
} ak_place_rect;

// An Align's cases (types.asuka's), as an overlay's align takes them.
enum { AK_PLACE_ALIGN_AUTO, AK_PLACE_ALIGN_START, AK_PLACE_ALIGN_CENTER, AK_PLACE_ALIGN_END, AK_PLACE_ALIGN_STRETCH, AK_PLACE_ALIGN_BASELINE };

typedef struct {
	int side;   // AK_SIDE_*
	int align;  // AK_PLACE_ALIGN_*
	float offset, align_offset, gap;
	bool flip;
	bool rtl;         // start is the right
	bool has_anchor;  // without: placed in the bounds, against side's edge (or centered)
} ak_place_opts;

typedef struct {
	float x, y;
	int side;               // AK_SIDE_*, after flipping
	float arrow;            // the anchor's middle, along the side, from the overlay's start
	float avail_w, avail_h;  // the room it has
} ak_place_result;

ak_place_result ak_place(ak_place_rect anchor, float w, float h, ak_place_rect bounds, const ak_place_opts *o);


// ak_overlay_focus_moved hears the focus move to f (NULL: nowhere): an
// overlay that closes when it leaves does.
void ak_overlay_focus_moved(ak_ui_root *r, ak_view *f);
// ak_overlays_layout lays out and places the open overlays (after the
// providers); ak_overlays_sync sends their anchors again (the flush's
// start); ak_overlays_report calls their place handlers (after the
// frames), true when it called one.
void ak_overlays_layout(ak_ui_root *r, ak_lpass *p);
void ak_overlays_sync(ak_ui_root *r);
bool ak_overlays_report(ak_ui_root *r);
bool ak_overlays_report_due(ak_ui_root *r);
extern const ak_shape ak_ui_shape_placed;

// Hooks.
// ak_ix_ev_ops is the ops event an extension's AK_EV_ is reported by.
ak_event_kind ak_ix_ev_ops(uint16_t ev);
// ak_ix_on hears of a handler set (an adjust makes the view adjustable).
void ak_ix_on(ak_view *v, uint16_t ev);
// ak_ix_set_base takes a style key set on a view whose layers set it:
// true when it did (the value laid on is what the layers say).
bool ak_ix_set_base(ak_view *v, uint16_t key, ak_val value);
// ak_ix_event handles the events every view may have: interaction
// states, gestures, keys, screen readers' actions, drags and drops.
bool ak_ix_event(ak_view *e, const ak_event *ev);
// ak_ix_container_set applies a container's own keys (dragItems,
// dropTypes).
void ak_ix_container_set(ak_view *e, uint16_t key, ak_val v, const ak_val *prev);
// The style keys this part applies (keys, actions): ak_ix_style_set
// applies one, which changed (prev NULL: unset before).
bool ak_ix_style_key(uint16_t key);
void ak_ix_style_set(ak_view *e, uint16_t key, ak_val v, const ak_val *prev);

// A window's keys (keys.c): ak_keys_register gives target (the root, or
// a Window's view) keys while the current owner lives; *slot holds the
// registration until it goes.
struct ak_keyreg;
struct ak_keyreg *ak_keys_register(ak_view *target, ak_arr *keys, struct ak_keyreg **slot);
// ak_keys_window is the target keys registered now go to: the view of the
// Window the calling code is built in, or the root (window.c).
ak_view *ak_keys_window(void);
void ak_keys_update(struct ak_keyreg *g, ak_arr *keys);
void ak_keys_forget(struct ak_keyreg *g);
void ak_rootkeys_free(struct ak_rootkeys *k);
// ak_window_view reports whether e is a Window's view (window.c).
bool ak_window_view(const ak_view *e);

// ---- The root's (ak_ui_root's ext) ----

typedef struct ak_frame_fn ak_frame_fn;

typedef struct ak_rext {
	// animate { }: what the next flush's changes animate with.
	bool animating;
	uint8_t anim_args[22];
	// onFrame's callbacks; on: the host was asked for frames.
	AK_VEC(ak_frame_fn *) frames;
	bool frames_on;
	// Layout roots of their own: accessories, RichTexts' inline
	// components' holders (retained).
	ak_view_vec roots;
	AK_VEC(struct ak_rich *) inlines;
	ak_buf scratch;
	struct ak_rootkeys *keys;  // ui.use_keys's (keys.c)
	// The keyboard's focus (focus.c): the view that has it, whether it
	// shows; the views with relation keys. By ID: a view holds its root.
	ak_id focus;
	bool focus_visible;
	AK_VEC(ak_id) rels;
	// The Overlays' layers (floating.c), oldest first, by ID.
	AK_VEC(ak_id) overlays;
	// The Window elements (window.c), in the order they were made (not
	// retained: each leaves as it goes): a window the system restores
	// finds its own by its state key.
	ak_view_vec windows;
} ak_rext;

ak_rext *ak_rext_of(ak_ui_root *r);
void ak_rext_free(ak_ui_root *r);

// Hooks of the flush (root.c).
void ak_anim_before_apply(ak_ui_root *r);
void ak_anim_frames(ak_ui_root *r, bool begin);
void ak_anim_flushed(ak_ui_root *r);
// ak_ext_layout lays out the root's own layout roots (inline components
// first, before what holds them: the host measures it with their sizes).
void ak_ext_layout_inlines(ak_ui_root *r, ak_lpass *p);
void ak_ext_layout(ak_ui_root *r, ak_lpass *p);
// ak_ext_dispatch handles the events that aren't a view's: a frame, a
// module call's result, a URL or files the app is opened with, a share.
bool ak_ext_dispatch(const ak_event *ev);
// ak_ext_mounted runs once a root's view is made (its locale).
void ak_ext_mounted(ak_ui_root *r);
// A preview's showing (preview.c): its color scheme and text size told to
// a root's host as it mounts; its frame around the root's view; whether
// one runs.
void ak_preview_host(void);
void ak_preview_mounted(ak_ui_root *r);
ak_view *ak_preview_wrap(ak_ui_root *r, ak_view *v);
void ak_preview_enter(void);
void ak_preview_color_scheme(int scheme);
// ak_ui_preview_locale shows lang (a tag; NULL or "": none) laid out in
// dir ("rtl", "ltr"; NULL or "": the language's) over the app's and asuka
// dev's choices (locale.c), while a preview shows.
void ak_ui_preview_locale(ak_str *lang, ak_str *dir);
// ak_ext_environment hears of the environment the host reported.
void ak_ext_environment(ak_ui_root *r);
// ak_ext_unmount runs as a root is disposed: the calls its host won't
// answer fail.
void ak_ext_unmount(ak_ui_root *r);
// ak_ext_shutdown lets go of what the process keeps across roots.
void ak_ext_shutdown(void);

// ---- Elements (their classes) ----

extern const ak_el_class ak_class_image, ak_class_video, ak_class_web, ak_class_native, ak_class_rich, ak_class_selectable,
	ak_class_canvas, ak_class_shape, ak_class_shader;
const ak_el_class *ak_ext_class_of(uint16_t el);

// A Text: its selectable key (rich.c). ak_text_finalize makes a
// selectable Text's views; ak_text_selectable_target is where its keys go.
void ak_text_finalize(ak_view *e);
ak_view *ak_text_style_target(ak_view *e, uint16_t key);
void ak_text_send(ak_view *e);

// A TextField's rich text, paste types, formats and accessory (rich.c).
void ak_field_set_ext(ak_view *e, uint16_t key, ak_val v, const ak_val *prev);
void ak_field_finalize_ext(ak_view *e);
bool ak_field_event_ext(ak_view *e, const ak_event *ev);
bool ak_field_rich(const ak_view *e);
// ak_field_text_reported tells a field's state (elements.c's) the plain
// text a rich field sent (its text follows it).
void ak_field_text_reported(ak_view *e, ak_str *s);

// ---- Shapes the runtime builds (types.asuka's) ----

extern const ak_shape ak_ui_shape_gesture, ak_ui_shape_shared_item, ak_ui_shape_drag_item, ak_ui_shape_drop, ak_ui_shape_span,
	ak_ui_shape_font, ak_ui_shape_text_formats, ak_ui_shape_web_state, ak_ui_shape_web_download, ak_ui_shape_video_track, ak_ui_shape_animation,
	ak_ui_shape_region;

// ---- Shared helpers ----

// ak_ext_ev maps a View event to the same event of e's container
// element (Row's, Column's, SafeArea's).
uint16_t ak_ext_ev(const ak_view *e, uint16_t view_ev);
uint16_t ak_ext_key(const ak_view *e, uint16_t view_key);
// ak_str_of_span is a new string of s.
static inline ak_str *ak_str_span(ak_span s) { return ak_str_new(s.p, s.len); }
static inline ak_span ak_span_str(const ak_str *s) { return s ? (ak_span){s->data, s->len} : (ak_span){"", 0}; }
// ak_call_closure calls c (borrowed) with args under owner o (NULL: as
// it is); an error goes to o's catcher.
void ak_call_closure(ak_closure *c, ak_owner *o, ak_val *args, uint32_t nargs);
// ak_item_wire fills a wire item from a DragItem or SharedItem struct:
// a file's path made absolute (*abs: the string it made, owned, or
// NULL), its name and type worked out.
void ak_item_wire(const ak_struct *s, bool drag, ak_item *out, ak_str **abs);
void ak_ix_unmount(void);
// ak_items_value makes an array of SharedItem (or DragItem) structs of
// decoded items.
ak_arr *ak_items_value(const ak_item *items, uint32_t n, bool drag);
// ak_ext_module_call sends a native module's method call (Call); done
// (retained) gets the result: fn(bytes?, string) (the bytes, or none and
// the error's message). NULL: no answer wanted.
void ak_ext_module_call(ak_span module, ak_span method, const void *args, size_t n, ak_closure *done);
// ak_ext_call_future is the same as a future of the answer's bytes,
// rejected with an error.
ak_future *ak_ext_call_future(ak_span module, ak_span method, const void *args, size_t n);

// Environment, for media (Reduce Motion) and locale.
bool ak_ext_reduce_motion(ak_ui_root *r);

// ---- Locale (locale.c) ----

// ak_locale_rtl is whether the app is laid out right to left now.
bool ak_locale_rtl(void);
// ak_locale_now is the language shown (borrowed); reading it follows it.
ak_str *ak_locale_now(void);
// The user's regional formats' hour cycle (12 or 24) and first day of
// the week (1 Sunday ... 7 Saturday); reading them follows them.
int ak_locale_hour_cycle(void);
int ak_locale_first_weekday(void);
// ak_locale_forget_choice forgets the language the app chose (setLocale),
// not one the run forced (asuka dev --lang): tests' mounts start from the
// user's languages.
void ak_locale_forget_choice(void);
// ak_locale_changed tells the locale the catalogs' languages changed.
void ak_locale_changed(void);

#endif
