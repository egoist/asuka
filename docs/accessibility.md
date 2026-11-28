# Accessibility

Asuka shows the platform's own widgets, so most of what screen readers,
the keyboard and the text size settings need is there without code. This
page covers what you get for free, and the props for the rest: labels,
roles, grouping, custom actions, adjustable views, announcements, the
keyboard, text size and Reduce Motion, and how to test it.

Asuka gives the same props to each platform's accessibility API, and
the screen readers read them from there:

| Platform | Accessibility API | Screen readers |
| --- | --- | --- |
| iOS | UIAccessibility | VoiceOver |
| Android | Android's accessibility framework (`AccessibilityNodeInfo`) | TalkBack |
| macOS | NSAccessibility | VoiceOver |
| Windows | UI Automation | Narrator, NVDA |
| Linux | GTK's accessibility, over AT-SPI | Orca |
| The web | ARIA roles and attributes on the page's elements | the browser's: VoiceOver, NVDA, TalkBack... |

## What you get without asking

- **Controls are the platform's.** `Button`, `Toggle`, `Slider`,
  `TextField`, `Segmented`, `Stepper`, `DatePicker` and the rest are
  UIKit's, Material's, AppKit's, Win32's, GTK's and the browser's own
  controls, read and operated as in any app.
- **Pressable views are buttons.** A `View` with `on press` is one
  element:
  - iOS: a button read as the texts and labeled images in it, joined;
    a double tap presses it.
  - Android: a clickable view, whose texts TalkBack reads together.
  - macOS: a button (`AXButton`).
  - Windows: a button named by the texts in it, joined, which a screen
    reader presses (UI Automation's Invoke).
  - Linux: a button.
  - The web: an element with the `button` role.

  On Windows, Linux and the web the keyboard reaches it too (see
  [The keyboard](#the-keyboard)).
- **Text scales.** Every font, the semantic ones (`.body`,
  `.headline`...), a fixed size (`Font(size: 15)`) and your own fonts,
  follows the user's text size: Dynamic Type, Android's font scale,
  Windows' text size, GNOME's text scaling, the browser's text size.
  When the user changes it, Asuka measures the text again and lays out
  what it moves. A Mac has no text size setting.
- **Colors adapt.** Semantic colors (`.label`, `.secondaryLabel`,
  `.background`, `.accent`...) are the platform's (see
  [Layout and style](layout.md)): they follow dark mode, on iOS and
  macOS Increase Contrast, and on Linux GNOME's High Contrast.
- **Layout-only views stay out of the way.** A `View`, `Row` or `Column`
  with no events and no accessibility props makes no native view, so it
  never shows in the accessibility tree.
- **Pictures are quiet until described.** An `Image` without a `label`,
  a `Symbol` and a `Shape` are hidden from screen readers. On Linux and
  the web, a common symbol is the exception: it's read by an English
  name (Back, Archive) unless it's labeled.

## Labels and hints

`label` is what a screen reader reads for a view. `hint` says what
using it does, when the label doesn't.

```asuka
component Labeled {
  Row {
    gap: 12
    Button("") {
      icon: "paperplane"
      label: "Send"
      variant: .filled
      on press { send() }
    }
    Image("https://picsum.photos/id/1015/600/400") {
      label: "A river in a valley"
      aspectRatio: 3.0 / 2
    }
    View {
      label: "Favorite"
      role: .image
      Symbol("heart.fill") { color: .red }
    }
  }
}

fn send() {}
```

- A `Button` reads its title. Give an icon-only button a `label`: the
  platforms name only a few symbols themselves, and Android only in
  English.
- A `TextField` or `Toggle` (including a checkbox) takes a `label` too,
  for example `label: "Email"`. Put it on the control itself:
  its name stays separate from its text or checked state, and the control
  remains editable or toggleable. On Android a text field's label is its
  accessibility hint, which TalkBack reads as the field's name.
- An `Image`'s `label` describes it; without one it's decorative. An
  icon from [`std/svg`](stdlib.md) takes a `label` the same way.
- A `Symbol` has no label of its own: wrap it in a `View` with a
  `label` and `role: .image` when it means something.
- `hint` is iOS's accessibility hint, read after a pause (users can turn
  hints off), and Android's state description, read with the label. On
  macOS it's the view's help (`accessibilityHelp`), on Windows its help
  text, on Linux its accessible description, and on the web its
  `aria-description`. Keep it short: "Opens the message".

`label` and `hint` are bindings, as every prop is: a fixed text, or an
expression of state that the view follows as it changes, the app's
language included (`label: tr.favorite()`). Leave `label` out of a
pressable view to have its texts read instead.

## Roles

`role` says what a view is:

| Role | iOS | Android | macOS | Windows | Linux | Web |
| --- | --- | --- | --- | --- | --- | --- |
| `.off` | nothing (the default) | nothing | nothing | nothing | nothing | nothing |
| `.button` | button trait | read as a button | `AXButton` | Button | `button` | `button` |
| `.link` | link trait | read as a button | `AXLink` | Hyperlink | `link` | `link` |
| `.header` | header trait | heading | `AXHeading` | Text, heading level 1 | `heading` | `heading` |
| `.image` | image trait | read as an image | `AXImage` | Image | `img` | `img` |
| `.text` | static text trait | nothing | `AXStaticText` | Text | `label` | nothing |
| `.search` | search field trait | nothing | `AXTextField` | Edit | `search` | `searchbox` |

Windows' are UI Automation's control types, Linux's GTK's accessible
roles, the web's ARIA roles. A pressable `View` is a button already. A
view with a `role` is an element of its own on iOS and macOS, so give it
a `label` too, or it's read as nothing but its role.

A control you build from views (see [Your own look](custom-ui.md))
takes the role of what it is, and each platform reads it as its own
control of that kind: a checkbox as UIKit's toggle button, Android's
`CheckBox`, `AXCheckBox`, UI Automation's CheckBox, GTK's and ARIA's
`checkbox`.

| Role | What it is |
| --- | --- |
| `.checkbox`, `.switch` | on or off; its state is `a11yChecked` |
| `.radio`, `.radioGroup` | one of a group's choices, and the group |
| `.tab`, `.tabList`, `.tabPanel` | a tab, the row of them, and what one shows |
| `.slider`, `.spinButton` | a value in a range (`a11yRange`, `a11yValue`), stepped by `on adjust` or the arrow keys |
| `.progressBar`, `.meter` | how far a task has come, a measurement in a range |
| `.menu`, `.menuBar`, `.menuItem`, `.menuItemCheckbox`, `.menuItemRadio` | a menu, a menu bar, and their items |
| `.listbox`, `.option`, `.comboBox` | a list of options, an option, a field or button that opens one |
| `.dialog`, `.alertDialog` | a window over the app's; an alert asks for an answer |
| `.tooltip`, `.status` | a short description; a message read without moving the focus |
| `.group`, `.toolbar`, `.separator` | views that belong together, a toolbar, a line between groups |

## States

A control of your own says its state as the platform's say theirs:

| Prop | What it says |
| --- | --- |
| `a11yChecked: .checked` | a checkbox's, switch's, radio's or checkable menu item's state: `.unchecked`, `.checked`, `.mixed` (neither: a parent of some checked boxes). `.off`, the default: it's not checkable |
| `a11yExpanded: .expanded` | whether what it opens shows: `.collapsed`, `.expanded` (a disclosure, a menu's button, a combo box). `.off`: it opens nothing |
| `a11ySelected: true` | the one chosen among its group: a tab, an option |
| `a11yValue: "40%"` | what's read as its value, in words |
| `a11yRange: ValueRange(min: 0, max: 100, now: 40)` | a slider's, progress bar's or meter's range |
| `a11yModal: true` | screen readers stay in it, the rest of its window hidden from them: a dialog |
| `a11yControls: list` | the view (a `Ref`) it controls: a combo box's list, a tab's panel |
| `a11yLabelledBy: title` | the view whose text names it: a field's label, a dialog's title |
| `a11yDescribedBy: hint` | the view whose text describes it: a field's description or error |

```asuka
component Disclosure(title: string) {
  state open = false
  let panel = Ref()

  Column {
    View {
      role: .button
      a11yExpanded: if open { .expanded } else { .collapsed }
      a11yControls: panel
      on press { open = !open }
      Text(title)
    }
    if open {
      View { ref: panel; Text("Details") }
    }
  }
}
```

The relations go to the platforms that have them: Windows (UI
Automation's ControllerFor, LabeledBy, DescribedBy), Linux and the web.
On iOS and Android a view labelled by another reads that one's text as
its label, and a described one its description as its hint; macOS
names a view by its title element. `std/base`'s components set all of
these themselves.

## Grouping

A screen reader moves through elements one by one. A row of three texts
is three stops; often it should be one. Two ways:

- **Make it pressable.** A `View` with `on press` is one element, read as
  what's in it. This is the usual row:

```asuka
struct Message {
  from: string
  subject: string
}

component MessageRow(m: Message) {
  event open

  View {
    direction: .row
    gap: 12
    padding: [12, 20]
    hint: "Opens the message"
    on press { emit open }
    View {
      a11yHidden: true
      Symbol("person.fill") { color: .secondaryLabel }
    }
    Column {
      grow: 1
      gap: 2
      Text(m.from) { font: .headline }
      Text(m.subject) { color: .secondaryLabel }
    }
  }
}
```

- **Give it a `label`.** A `View` with a `label` is read as its label.
  On iOS and macOS what's in it isn't read separately; on Android the
  label is the view's content description; on Windows it's the view's
  name, on Linux its accessible label, on the web its `aria-label`. Say what the eye takes in at a
  glance, in words: "Night Drive by Ada, 4 minutes", not "4:00".

```asuka
component Track(title: string, artist: string, minutes: int) {
  Row {
    gap: 8
    label: "{title} by {artist}, {minutes} minutes"
    Text(title) { font: .headline }
    Text(artist) { color: .secondaryLabel }
    Text("{minutes}:00") { color: .secondaryLabel }
  }
}
```

## Decoration

`a11yHidden` hides a view and everything in it from screen readers:
an ornament, an icon next to a text that says the same, a checkmark in a
toast that reads "Saved".

```asuka
component Saved {
  Row {
    gap: 6
    View {
      a11yHidden: true
      Symbol("checkmark") { color: .green }
    }
    Text("Saved")
  }
}
```

## Headings

Screen reader users jump between headings (VoiceOver's rotor, TalkBack's
navigation by headings, NVDA's and Orca's H key, a browser screen
reader's headings list). Mark a section's title as one: a `RichText`
with `role: .header`, or a `View` with a `label` and the role.
[`std/markdown`](stdlib.md) marks its headings already.

```asuka
component Headings {
  Column {
    gap: 12
    RichText([Span(text: "Recent")]) {
      role: .header
      font: .title2.bold()
    }
    View {
      label: "Settings"
      role: .header
      Text("Settings") { font: .title2 }
    }
  }
}
```

A `RichText`'s links are elements of their own, which a screen reader
finds and activates.

## Custom actions

What a sighted user does with a swipe, a long press or a small button
inside a row, a screen reader user reaches through actions: VoiceOver's
Actions rotor, swiping up or down; TalkBack's actions menu. `actions`
lists them on a view:

```asuka
component Thread(subject: string) {
  event reply
  event archive

  View {
    padding: [12, 20]
    actions: [
      A11yAction(name: "Reply", run: () => { emit reply }),
      A11yAction(name: "Archive", run: () => { emit archive }),
    ]
    Text(subject)
  }
}
```

A view with actions is one element, read as its texts. Where they go:

- **iOS** and **macOS**: the view's custom actions
  (`UIAccessibilityCustomAction`, `NSAccessibilityCustomAction`), in
  VoiceOver's Actions rotor on iOS and its Actions menu on a Mac
  (VO-Command-Space).
- **Android**: TalkBack's actions menu.
- **Windows**: the view's context menu, which the keyboard opens too
  (Shift+F10, the Menu key) and the mouse with a right-click. The view
  becomes a tab stop, so the keyboard reaches it.
- **Linux**: the view's actions over AT-SPI, which Orca lists and does,
  and its context menu (a secondary click, the Menu key, Shift+F10). The
  view becomes a tab stop.
- **The web**: ARIA has no custom actions, so they aren't offered
  there. Give a view the same commands another way too (a button, a
  `Menu`) when the app runs in a browser.

A `List` row's `swipeActions` are offered to screen readers for you:
UIKit's list offers its swipe actions to VoiceOver itself, and on
Android Asuka adds them to the row as TalkBack actions. Don't list them
again in `actions`.

```asuka
state items = ["Milk", "Bread", "Eggs"]

component Groceries {
  List {
    for (i, item) in items.enumerated() key item {
      Row {
        padding: [12, 20]
        swipeActions: [
          .button(title: "Delete", icon: "trash", role: .destructive, run: () => { items.removeAt(i) }),
        ]
        Text(item)
      }
    }
  }
}
```

A `Menu`'s actions, and buttons with a `Menu`, are the platform's menus,
accessible as they are.

## Adjustable views

`on adjust(by)` makes a view adjustable, as a slider is: VoiceOver's swipe up
and down, and TalkBack's (or the volume keys), step it by 1 or -1. Use
it for a control of your own that picks a value; a `Slider` or
`Stepper` already is one.

On a Mac the view is an incrementor (`AXIncrementor`), which VoiceOver
steps with VO-Up and VO-Down. On Windows, Linux and the web it's a tab
stop that the arrow keys step: up and right by 1, down and left by -1
(on Windows and Linux, left and right swap in a right-to-left
language). Linux and the web give it the slider role.

```asuka
component Rating {
  state stars = 3

  Row {
    gap: 4
    align: .center
    on adjust(by) { stars = min(max(stars + by, 0), 5) }
    for i in 0..<5 key i {
      Symbol("star.fill") { color: if i < stars { .orange } else { .tertiaryLabel } }
    }
    Text("Rating: {stars} of 5")
  }
}
```

The view is read as its text, which says where it is and changes as it
steps. It has no `label`, which would be read instead.
Clamp the value yourself: the screen reader keeps asking.

## Announcements

`accessibility.announce` (`platform/accessibility`) has the screen reader, if one is on, read a text: what
changed where focus isn't. A toast, a count of search results, a save
that finished.

```asuka
import platform/accessibility

component SaveButton {
  state shown = false

  Column {
    Button("Save") {
      on press {
        animate(() => { shown = true })
        accessibility.announce("Saved") // a screen reader wouldn't notice the toast
      }
    }
    if shown { Text("Saved") }
  }
}
```

It's `UIAccessibility`'s announcement on iOS,
`announceForAccessibility` on Android, an announcement request on
macOS, a UI Automation notification on Windows, GTK's announcement on
Linux (GTK 4.14 and later), and a polite `aria-live` region on the web.
Without a screen reader, it does nothing. Don't announce what the
focused view says already.

## Moving focus

`accessibility.focus(ref)` moves the screen reader's focus to a view (a `Ref` to it),
for when the screen changed under the user and what matters is
somewhere else: the error under a form that failed to submit, the first
result of a search, the title of a panel that slid in.

```asuka
import platform/accessibility

component SignIn {
  state message = ""

  let error = Ref()

  async fn submit() {
    do {
      try await signIn()
    } catch e {
      message = e.message
      accessibility.focus(error)
    }
  }

  Column {
    gap: 12
    Button("Sign In") { on press { await submit() } }
    Text(message) {
      ref: error
      color: .red
    }
  }
}

async fn signIn() throws { throw Error("The password is wrong") }
```

It's sent when the screen next updates, once the view exists and has
its frame:
`UIAccessibility`'s screen-changed notification with the view on iOS,
`ACTION_ACCESSIBILITY_FOCUS` on Android, a layout change naming the view
on macOS and the focus event on Windows. A container with no view of
its own focuses its first view. On those four, without a screen reader
it does nothing, and it never moves the keyboard's focus (that's a
`Ref`'s `focus()`).

On Linux and the web it moves the keyboard's focus to the view, and the
screen reader follows it: GTK and the page have no other way to point a
screen reader at a view.

## Is a screen reader on

`environment().screenReader` is true while a screen reader is on, and
what reads it follows it being turned on or off:

- iOS and macOS: VoiceOver.
- Android: TalkBack.
- Windows: the system's screen reader flag, which Narrator and other
  screen readers set.
- Linux: GNOME's screen reader setting (Orca).
- The web: never. Browsers don't say whether a screen reader is on.

Use it for what works differently by ear, not to hide things: keep a toast up until
it's dismissed rather than for three seconds, or turn a carousel that
advances by itself into one that waits.

## Text size

Text scales with no code, so what's left is layout that still works at
large sizes:

- Don't fix the height of what holds text. Let it grow; use `minHeight`
  for a minimum.
- Avoid `lines: 1` on text that matters: at large sizes it's cut short.
- Put long rows in a `Scroll` or a `List`.

`environment().fontScale` is the user's text size, 1 being the
default: Dynamic Type's Large, Android's default font scale, Windows'
100% (Settings, Accessibility, Text size), GNOME's scaling of 1, the
browser's default text size (16 CSS pixels). On a Mac it's always 1.
Read it in a style to change a layout at large sizes, as iOS's own apps
stack a row's label and value:

```asuka
component Adaptive {
  let large = environment().fontScale > 1.5

  View {
    direction: if large { .column } else { .row }
    gap: if large { 4 } else { 12 }
    align: if large { .stretch } else { .center }
    Text("Wi-Fi") { grow: 1 }
    Text("Home") { color: .secondaryLabel }
  }
}
```

## Reduce Motion

`environment().reduceMotion` is iOS's and macOS's Reduce
Motion, Android's animations turned off, Windows' animation effects
off, GNOME's animations off, or the browser's `prefers-reduced-motion`.
On iOS Asuka makes transitions dissolve instead of moving; elsewhere its
animations end at once. See [Motion](motion.md) for what's left to you.

## The keyboard

Asuka's views take the keyboard as the platform's own controls do, on a
desktop, on the web, and with a hardware keyboard on an iPad, an iPhone
or an Android device:

- **Tab** moves between the controls and the pressable views, in the
  views' order; Shift+Tab goes back.
- **Enter** and **Space** press a focused pressable view, as a button.
- A focused view shows the platform's focus ring: AppKit's, Fluent's on
  Windows, an outline in the accent on Linux, the browser's on the web,
  UIKit's focus effect, Android's focus highlight.
- A view with `actions` or `on adjust` is a tab stop: its context menu
  opens with Shift+F10 or the Menu key (Windows, Linux), and the arrow
  keys step it.

A pointer's press moves the focus on a desktop and the web, not a tap
on a touch screen.

### Views that take the focus

`focusable: true` makes any container a tab stop, focused by a click
too; `on focusChange(focused)` hears it come and go. `pressKeepsFocus:
true` leaves the focus where it was when a pointer presses the view (a
toolbar's button, a menu's trigger).

Two interaction states follow the focus, as `pressed` follows a press,
for styles and reads:

- `focused`: the focus is in the view (it, or a view inside it: CSS's
  `:focus-within`), so a field's frame styles itself while the field in
  it has the focus;
- `focusVisible`: the same, while the focus came from the keyboard or is
  in a field: what a focus ring is for (CSS's `:focus-visible`).

```asuka
component Card(title: string) {
  state opened = false

  View as card {
    focusable: true
    padding: 12
    radius: 8
    focusVisible { border: 2; borderColor: .accent }
    on press { opened = true }
    Text(if card.focused { "{title}, focused" } else { title })
  }
}
```

A view that styles `focusVisible` draws its own ring: the platform's
goes. `hideFocusRing: true` takes it away without one.

A `Ref`'s `focus()` and `blur()` move the keyboard's focus to a view
(a field, a control, a focusable or pressable view) and away; focus
that follows a key is visible.

### Keeping the focus in a part of the window

- `trapFocus: true`: while the focus is in the view, Tab and Shift+Tab
  go round among the views inside it (a dialog's).
- `autofocus: true`: once the view is laid out, the focus goes to it if
  it takes the focus, else to the first view inside it that does.
- `restoreFocus: true`: when the view goes, the focus goes back to the
  view that had it when it came, unless the user moved it elsewhere.

`std/base`'s dialogs, menus and popovers do all three.

### Typing to a view

`on keyText(text)` hears what's typed while the view has the focus and
no `keys` took it: a list's typeahead, which moves to the item that
starts with what's typed.

## Testing with a screen reader

Try each screen with the screen reader on, eyes off the screen if you
can: can you reach everything, does each stop say what it is, does
every swipe and drag have an action? On a desktop, try it with the
keyboard alone too.

- **VoiceOver** runs on an iPhone: Settings, Accessibility, VoiceOver.
  Set the Accessibility Shortcut (a triple click of the side button) to
  turn it on and off. The simulator has no VoiceOver; Xcode's
  Accessibility Inspector (Xcode, Open Developer Tool) shows each
  element's label, traits and actions there.
- **TalkBack** runs on an Android phone, or an emulator with Google
  Play, from the Android Accessibility Suite: Settings, Accessibility,
  TalkBack.
- **VoiceOver on a Mac**: Command-F5, or System Settings,
  Accessibility, VoiceOver. Accessibility Inspector (Xcode, Open
  Developer Tool) inspects the macOS app's elements directly.
- **Narrator** on Windows: Ctrl+Windows+Enter. NVDA is a free download.
  Accessibility Insights for Windows shows each element's UI Automation
  name, control type and patterns.
- **Orca** on Linux: Super+Alt+S in GNOME, or Settings, Accessibility,
  Screen Reader. Accerciser shows the AT-SPI tree.
- **The web**: the browser's developer tools show the accessibility
  tree (Chrome's and Firefox's Accessibility panels); try VoiceOver with
  Safari, NVDA with Chrome or Firefox.
- **Text size**: iOS's Settings, Accessibility, Display & Text Size,
  Larger Text, or Xcode's Environment Overrides on the simulator;
  Android's Settings, Display, Font size; Windows' Settings,
  Accessibility, Text size; GNOME's Settings, Accessibility, Large Text;
  the browser's text size. Try the largest.
- **Reduce Motion**: iOS's Settings, Accessibility, Motion; Android's
  Settings, Accessibility, Remove animations; macOS's System Settings,
  Accessibility, Display, Reduce motion; Windows' Settings,
  Accessibility, Visual effects, Animation effects; GNOME's Settings,
  Accessibility, Reduce Animation; the system's setting for a
  browser.

`asuka dev` keeps running as you change these: the app follows each
setting as it changes.
