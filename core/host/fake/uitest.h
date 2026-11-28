// uitest.h: an app's UI tested on the fake host, as a user drives it,
// by what the screen shows: a port of the Go uitest package (uitest/*.go)
// for `asuka test` (the test.* builtins) and the core's C tests.
//
// The app runs as on an iPhone (390x844 points) unless the environment
// given says otherwise. Everything runs on the main thread: an action
// settles before it returns (what was posted runs, the UI flushes, the
// rows lists show are bound, again until nothing changes). The fake host
// measures text at 8 points a character on 20-point lines.
//
// What the screen shows is what a user could see: the root and what's
// presented over it, a List's rows that fit its height (bound as a
// collection view would), and nothing hidden. A text names a view that
// shows it: a Text's text, a Button's title, a field's text or
// placeholder, a picker's chosen item, an accessibility label, a
// segment.
#ifndef AK_UITEST_H
#define AK_UITEST_H

#include "fake/fake.h"
#include "ui/ui.h"

typedef struct ak_uitest ak_uitest;  // an rt object (AK_TAG_OPAQUE)

// ak_uitest_mount mounts build's view on a new fake host, settled. env
// (NULL: an iPhone's) is the environment it reports. The UI runtime has
// one root: mounting unmounts what was.
ak_uitest *ak_uitest_mount(ak_closure *build, const ak_environment *env);
// ak_uitest_mount_at is ak_uitest_mount on platform (AK_PLATFORM_*; -1:
// env's, or the default), with the main window's Router starting at href
// ("": its initial), as a browser opened at that address does.
ak_uitest *ak_uitest_mount_at(ak_closure *build, const ak_environment *env, int platform, ak_span href);
// ak_uitest_set_platform sets the platform a mount with no environment
// reports (AK_PLATFORM_IOS by default): asuka test -platform.
void ak_uitest_set_platform(uint8_t platform);
// ak_uitest_set_measure sets what measures views in the mounts that
// follow, instead of the fake's own rules (NULL: those again): a test
// that needs a real host's answers.
void ak_uitest_set_measure(ak_fake_measure_fn fn, void *ctx);
// ak_uitest_set_button_max sets the widest the fake host of the mounts
// that follow draws a Button's content (ak_fake's button_max; 0: no
// limit, the default), as a watch's.
void ak_uitest_set_button_max(float w);
// ak_uitest_unmount unmounts the app and reports in err (if not NULL)
// the native views that outlived it (an owner left undisposed). False
// if any did.
bool ak_uitest_unmount(ak_uitest *a, ak_buf *err);
ak_fake *ak_uitest_host(ak_uitest *a);

// ak_uitest_settle runs what was posted, flushes and binds lists' rows,
// until nothing changes.
void ak_uitest_settle(ak_uitest *a);
// ak_uitest_idle settles, moving the clock past each timer as it comes
// due (ten minutes of the app's time at most).
void ak_uitest_idle(ak_uitest *a);
// ak_uitest_pump delivers what the mounted app's fake host posted (a
// module call's answer, an image's load) and settles: the runner's
// settle calls it, so an await of the host's answer outside app.idle()
// completes. It reports whether there was any.
bool ak_uitest_pump(void);
// ak_uitest_log_ops says whether the mounted app's fake host keeps a log
// of the ops it applies (test.ops'). A bench's measured part runs
// without (what's timed isn't keeping every iteration's ops), and the
// log starts again after it.
void ak_uitest_log_ops(bool on);

// Actions and expectations: true, or false with what went wrong and what
// the screen shows in err.
//
// tap taps what shows text: a button, a text in something that takes a
// press, a toggle (flips), a text field (focuses), a segment, a menu's
// item while it's open (a button's menu opens on its tap).
bool ak_uitest_tap(ak_uitest *a, ak_span text, ak_buf *err);
// type types text into the field label names (its placeholder, text or
// label, or the field after a text that says label, or in a view that
// says it), or, label empty, the field tapped or typed in last.
bool ak_uitest_type(ak_uitest *a, ak_span text, ak_span label, ak_buf *err);
bool ak_uitest_has(ak_uitest *a, ak_span text);
bool ak_uitest_expect(ak_uitest *a, ak_span text, ak_buf *err);
// frame is the frame, in the window's coordinates, of the first view that
// shows text.
bool ak_uitest_frame(ak_uitest *a, ak_span text, float out[4], ak_buf *err);
// ak_uitest_link taps the link to url in the RichText that has one.
bool ak_uitest_link(ak_uitest *a, ak_span url, ak_buf *err);
// set_languages reports the user's preferred languages changed (BCP 47
// tags, most preferred first, joined by commas), settled.
void ak_uitest_set_languages(ak_uitest *a, ak_span languages);
bool ak_uitest_expect_no(ak_uitest *a, ak_span text, ak_buf *err);
// As uitest's (actions.go): long_press long-presses what shows text;
// disabled reports whether the one thing showing text is disabled;
// submit presses return in the field label names ("": the one typed in
// last), or an alert's; toggle flips the switch label names, slide moves
// a slider; select chooses an option of the picker, wheel or segmented
// control that has it; swipe presses a list row's swipe action; back
// goes back as the user does (a stack's top screen, else the top sheet,
// alert or popover); dismiss dismisses the top sheet, alert or popover;
// refresh pulls what shows text to refresh; scroll_to scrolls lists until
// one shows text.
bool ak_uitest_long_press(ak_uitest *a, ak_span text, ak_buf *err);
bool ak_uitest_disabled(ak_uitest *a, ak_span text, bool *out, ak_buf *err);
bool ak_uitest_submit(ak_uitest *a, ak_span label, ak_buf *err);
bool ak_uitest_toggle(ak_uitest *a, ak_span label, ak_buf *err);
bool ak_uitest_slide(ak_uitest *a, ak_span label, double value, ak_buf *err);
bool ak_uitest_select(ak_uitest *a, ak_span option, ak_buf *err);
bool ak_uitest_pick_color(ak_uitest *a, ak_span label, ak_color color, ak_buf *err);
bool ak_uitest_pick_date(ak_uitest *a, ak_span label, int64_t ms, ak_buf *err);
bool ak_uitest_swipe(ak_uitest *a, ak_span text, ak_span action, ak_buf *err);
bool ak_uitest_back(ak_uitest *a, ak_buf *err);
bool ak_uitest_dismiss(ak_uitest *a, ak_buf *err);
bool ak_uitest_refresh(ak_uitest *a, ak_span text, ak_buf *err);
bool ak_uitest_scroll_to(ak_uitest *a, ak_span text, ak_buf *err);
// ak_uitest_until settles, moving the clock to each timer as it comes
// due, until cond holds; false (what's waited for, in err) once no timer
// is left, or after ten minutes of the app's time. wait_for waits for
// text to show (gone: to go).
bool ak_uitest_until(ak_uitest *a, bool (*cond)(void *ctx), void *ctx, ak_span what, ak_buf *err);
bool ak_uitest_wait_for(ak_uitest *a, ak_span text, bool gone, ak_buf *err);
// ak_uitest_texts calls f with the texts the screen shows, in the order
// a screen reader reads them.
void ak_uitest_texts(ak_uitest *a, void (*f)(ak_span s, void *ctx), void *ctx);
// ak_uitest_snapshot describes what the screen shows: each layer's
// views, with their kinds, frames, texts and states, one a line,
// indented by depth (uitest's Snapshot).
void ak_uitest_snapshot(ak_uitest *a, ak_buf *out);

// ak_uitest_set_app gives mountApp the module's app function (its
// closure; NULL: none).
void ak_uitest_set_app(ak_closure *build);

// ak_uitest_builtins_init registers the test.* builtins (uitest.c's
// table) for `asuka test`.
void ak_uitest_builtins_init(void);

#endif
