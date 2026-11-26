# Components

A component is a declaration that shows views (see [the
language](language-tour.md)). This page covers the elements Asuka gives you,
grouped by what they do, and the standard library's packages for
Markdown, SVG, Lottie, charts and shaders. Each one is the platform's own
widget where the platform has one: UIKit on iOS, Android Views and
Material 3 on Android, AppKit on macOS, Win32's controls drawn as
Windows 11's (Fluent) on Windows, GTK 4 and libadwaita on Linux, the
page's elements in a browser. How views are sized and placed is in
[Layout](layout.md); screens, tabs and sheets are in
[Navigation](navigation.md).

Every prop is a binding: a value, or an expression of state the view
follows as it changes. A control the user changes (a field, a toggle)
takes `bind`, the state it writes as the user changes it: `bind text:
query`. See [Concepts](concepts.md).

## Each platform's widgets

| Component | iOS | Android | macOS | Windows | Linux | Web |
|---|---|---|---|---|---|---|
| `Text` | `UILabel` | `TextView` | `NSTextField` (a label) | drawn by Asuka | `GtkLabel` | the page's text |
| `Button` | `UIButton` | `MaterialButton` | `NSButton` | `BUTTON`, drawn as Fluent's | `GtkButton` | `<button>` |
| `TextField` | `UITextField`; `UITextView` multiline | `EditText` | `NSTextField`, `NSSecureTextField`; `NSTextView` multiline | `EDIT` in a frame drawn as Fluent's | `GtkEntry`, `GtkPasswordEntry`; `GtkTextView` multiline | `<input>`; `<textarea>` multiline |
| `Toggle` | `UISwitch` | `MaterialSwitch` | `NSSwitch` | a `BUTTON` check box drawn as a toggle switch | `GtkSwitch` | `<input type="checkbox" switch>` |
| `Slider` | `UISlider` | Material `Slider` | `NSSlider` | a trackbar | `GtkScale` | `<input type="range">` |
| `Segmented` | `UISegmentedControl` | Material segmented buttons | `NSSegmentedControl` | drawn, as the Windows Community Toolkit's | linked `GtkToggleButton`s | buttons in a radio group |
| `Stepper` | `UIStepper` | two Material icon buttons | `NSStepper` | two buttons | linked − and + buttons | two buttons |
| `Wheel` | `UIPickerView` | `NumberPicker` | `NSPopUpButton` | a drop-down `COMBOBOX` | `GtkDropDown` | `<select>` |
| `ColorPicker` | `UIColorWell` | a swatch and a Material dialog | `NSColorWell` and the color panel | a swatch and the color dialog (`ChooseColor`) | `GtkColorDialogButton` | `<input type="color">` |
| `DatePicker` | `UIDatePicker` | buttons opening Material's pickers | `NSDatePicker` | the date and time picker (`SysDateTimePick32`) | a button opening `GtkCalendar` and spin buttons | `<input type="date">`, `time`, `datetime-local` |
| `Progress` | `UIProgressView` | `LinearProgressIndicator` | `NSProgressIndicator` | drawn as Fluent's | `GtkProgressBar` | `<progress>` |
| `ProgressRing` | drawn as SwiftUI's circular gauge; `UIActivityIndicatorView` without a value | `CircularProgressIndicator` | `NSProgressIndicator`, spinning, at 16 and 32 points; drawn in its style at other sizes or in a `color` | Fluent's `ProgressRing`, drawn | drawn in GNOME's style; `AdwSpinner` without a value | an SVG ring |
| `Activity` | `UIActivityIndicatorView` | `CircularProgressIndicator` | `NSProgressIndicator`, spinning | Fluent's progress ring, drawn | `AdwSpinner` (`GtkSpinner` before libadwaita 1.6) | a spinner, drawn |
| `Image` | `UIImageView` | `ImageView` | `NSImageView` | drawn with GDI+ | drawn from GDK textures | `<img>` |
| `lottie.View` | lottie-ios | lottie-android | lottie-ios | nothing yet (an empty box) | nothing yet (an empty box) | lottie-web |

| `Symbol` | SF Symbols | Material Symbols | SF Symbols | Segoe Fluent Icons | the icon theme's, else Material Symbols | Material Symbols |
| `Video` | `AVPlayerViewController` | Media3's player | `AVPlayerView` | a `<video>` in WebView2 | `GtkVideo` (GStreamer) | `<video>` |
| `WebView` | `WKWebView` | `WebView` | `WKWebView` | WebView2 | `WebKitWebView` (WebKitGTK 6.0) | `<iframe>` |
| `Scroll` | `UIScrollView` | `NestedScrollView` | `NSScrollView` | Asuka's own, with Fluent's scroll indicator | `GtkScrolledWindow` | a scrolling element |
| `List` | `UICollectionView` | `RecyclerView` | rows Asuka recycles, in an `NSScrollView` | rows Asuka recycles | rows Asuka recycles, in a `GtkScrolledWindow` | rows Asuka recycles |

The sections below say where a platform behaves differently.

## Text

### Text

`Text` shows a line or paragraph of text. It is the platform's label,
and wraps by default; `lines` limits how many lines it takes, and cuts
the rest with an ellipsis.

```asuka
component Inbox {
  state count = 3

  Column {
    gap: 8
    Text("Inbox") { font: .title2.bold() }
    Text("{count} unread") { color: .secondaryLabel }
    Text("A long description that stops after two lines.") { lines: 2 }
  }
}
```

The style's `font`, `color`, `textAlign` and `lines` are the text's.
Text styles (`.body`, `.headline`, `.footnote`...) follow the text size
the user picked.

### RichText

`RichText` shows styled text: a list of `Span`s. A `"\n"` in a span
starts a new paragraph. A span's fields left at their defaults come
from the `RichText`'s style.

```asuka
import platform/device

component About {
  RichText([
    Span(text: "asuka", font: .body.bold()),
    Span(text: " draws "),
    Span(text: "native", italic: true),
    Span(text: " views. Run "),
    Span(text: "asuka dev", mono: true),
    Span(text: ", or read "),
    Span(text: "the docs", link: "https://example.com/docs"),
    Span(text: ".\nA second paragraph, "),
    Span(text: "in red", color: .red),
    Span(text: "."),
  ]) {
    on link(url) { device.openURL(url) }
  }
}
```

- A `Span` has its `text`, `font`, `italic`, `mono`, `underline`,
  `strike`, `color`, `background` (drawn on each line the span covers,
  with rounded corners) and `link`.
- A tapped link runs `on link(url)`. Without it, links do nothing (in a
  router, a link to one of the app's hrefs goes there).
- `role: .header` makes it a heading to screen readers.

On iOS and Android each paragraph is typeset by the platform's engine
(CoreText, StaticLayout) and cached by its content and width: appending
to a long text, a reply streaming in, typesets only the paragraph that
changed. On macOS it's a read-only `NSTextView` (TextKit), on Windows a
read-only rich edit (`RICHEDIT50W`), on Linux a `GtkLabel` styled with
Pango's attributes, and in a browser spans in paragraphs.
[`examples/richtext`](../examples/richtext) streams one.

For plain text, use `Text`: it is the platform's label, with its native
behavior.

**Inline views.** A span's `inline` puts a component in the text: it
sits on the baseline and wraps with the words. Its key keeps the same
component, and its state, from one text to the next.

```asuka
component Status {
  RichText([
    Span(text: "Status: "),
    Span(inline: Inline(key: "badge", build: () => Live())),
    Span(text: " since this morning."),
  ])
}

component Live {
  View {
    background: .green
    radius: 6
    padding: [2, 6]
    Text("live") {
      font: .caption
      color: #ffffff
    }
  }
}
```

### Selectable text

`Selectable` makes the `RichText` views inside it selectable, as one
text across all of them: a long press selects a word, the platform's
handles extend the selection, and its edit menu copies it. With a
mouse, a drag selects and the platform's copy shortcut copies. Linux
and the web select across the texts as one too; on macOS and Windows
each text selects on its own.

```asuka
component Answer(question: [Span], answer: [Span]) {
  Selectable {
    gap: 12
    RichText(question) { role: .header }
    RichText(answer)
  }
}
```

A `Text` with `selectable: true` is selectable on its own; to select
across several, use `RichText`s in a `Selectable`. To copy something
without a selection, `device.copy(text)` puts it on the clipboard.

### Markdown

`std/markdown` shows Markdown (CommonMark, with GitHub's tables, task
lists, strikethrough and autolinks) as the platform's own styled text:
headings, emphasis, code, quotes, lists, tables, images and links, which
the user's text size scales. Raw HTML in the text shows as text.

```asuka
import std/markdown

component Reply(text: string, writing: bool) {
  markdown.View(text) { streaming: writing; padding: 16 }
}
```

`streaming` says the text is still being written, as a language model's
reply streams in: its end shows what it will most likely become (a code
fence closed, `**bold` as bold, a link being written as a link) rather
than half-written syntax. `markdown.heal(text)` is that, on its own.

Its look and its links are the app's:

- `theme` is a `markdown.Theme`: the text's font and color, the
  headings', links', code's (and its background), quotes' and rules',
  and the spacing between blocks and of an indent. What it doesn't say
  is the platform's.
- `render` draws a block the app's way: given a `markdown.Block` (its
  `kind`, plain `text`, styled `spans`, a heading's `level`, a code
  block's `language`, an image's `url`, a table's `rows`), it returns a
  view, or `none` for Markdown's own. A code block highlighted, a
  diagram drawn from its code, an image of the app's.
- `on link(url)` hears a link tapped, its destination as written (a
  relative one too): what it opens is the app's to say. Without it, a
  web, mail or phone link opens in the app that takes it, and other
  links do nothing.

```asuka
import std/markdown

component Code(text: string, language: string) {
  Column {
    background: .secondaryBackground; radius: 8; padding: 12
    Text(language) { font: .caption; color: .secondaryLabel }
    Text(text) { font: Font.callout.family("Menlo") }
  }
}

component Answer(text: string) {
  state opened = ""

  markdown.View(text) {
    theme: markdown.Theme(font: .callout, linkColor: .purple, spacing: 8)
    render: b => if b.kind == .code { Code(b.text, language: b.language) } else { none }
    on link(url) { opened = url }
  }
}
```

Asuka reads the Markdown, the same on every platform, and draws each
block as the language's own views: a [`RichText`](#richtext) per
paragraph in a [`Selectable`](#selectable-text), a grid for a table,
`Image` for an image. The text is read again as it changes, but its
blocks are kept by their place: as a reply streams in, only its last
block or two are drawn again, and what's above keeps its layout and
selection. See [`examples/markdown`](../examples/markdown).

## Buttons

`Button` is the platform's button: a `UIButton` configuration on
iOS, a Material button on Android, an `NSButton` on macOS, a `BUTTON`
drawn as Fluent's on Windows, a `GtkButton` on Linux, a `<button>` in a
browser.

```asuka
component Actions {
  state saving = false

  Row {
    gap: 12
    wrap: true
    Button("Save") {
      variant: .filled
      disabled: saving
      on press { saving = true }
    }
    Button("Compose") {
      icon: "square.and.pencil"
      variant: .tinted
    }
    Button("") {
      icon: "trash"
      label: "Delete"
      variant: .gray
    }
  }
}
```

- Its title is its text; `icon` a symbol before it (see
  [Symbols](#symbols-and-icons)). A button with an icon and an empty
  title is the icon alone.
- `label` is what VoiceOver and TalkBack read, the title by default.
  Give an icon-only button one.
- `tooltip` is a short text shown while a pointer rests on it: an
  iPad's pointer (`UIToolTipInteraction`), a mouse on Android, where a
  long press shows it too (`tooltipText`), and on macOS, Windows, Linux
  and the web (the element's `title`). Give an icon-only button one, its
  name. `tooltip` is the same on any view. A touch screen alone never shows it on iOS,
  so it can't be the only place something is said.
- `disabled` disables it, the platform's way.
- `font` and `color` change its text and tint.
- `href` goes to a route when it's pressed (see
  [Routes](navigation.md#routes)).

`variant` is its look:

| Variant | iOS | Android | macOS | Windows | Linux | Web |
| --- | --- | --- | --- | --- | --- | --- |
| `.plain` (default) | plain | text button | borderless, in the accent color | subtle, in the accent color | flat, in the accent color | plain |
| `.filled` | filled | filled button | push button in the accent color | accent button | suggested action | filled |
| `.tinted` | tinted | tonal button | push button, its title in the accent color | a wash of the accent | a wash of the accent | tinted |
| `.gray` | gray | tonal button | push button | standard button | standard button | gray |
| `.bordered` | bordered | outlined button | push button | standard button | standard button | outlined |
| `.glass` | Liquid Glass (`glass()`, iOS 26; gray before) | tonal button | the glass bezel, a capsule or a circle around an icon alone (macOS 26; push button before) | as `.gray` | standard button | as `.gray` |
| `.glassProminent` | tinted Liquid Glass (`prominentGlass()`, iOS 26; filled before) | filled button | the glass bezel tinted in the accent color, as the primary action (macOS 26; as `.filled` before) | as `.filled` | suggested action | as `.filled` |

The glass buttons are capsules, and a glass button with an icon alone is
a circle: a sheet's close button, as iOS 26's own.

### Menus

A button with a `menu` shows its actions when it's tapped, instead of
running `on press` (a `UIMenu` on iOS, a `PopupMenu` on Android, an
`NSMenu` on macOS, a pop-up menu on Windows, a `GtkPopoverMenu` on
Linux, a popover in a browser). Each is an `Action`: `.button(title:,
icon:, role:, disabled:, checked:, run:)`, its fields but the title
optional. `role: .destructive` shows it in red. `run` may await (`run:
() => { await save() }`): it runs as a task of the code that made the
action, which the menu doesn't wait for, as a handler does; that code's
stack, sheet and router are its own, so `push` and `navigate` work from
it. `runAction(a)` runs an action the same way, for a menu of your own.

```asuka
import platform/device

component More {
  Button("") {
    icon: "ellipsis.circle"
    label: "More"
    menu: [
      .button(title: "Copy", icon: "doc.on.doc", run: () => device.copy("https://example.com")),
      .button(title: "Share", icon: "square.and.arrow.up", run: () => device.share("https://example.com")),
      .button(title: "Delete", icon: "trash", role: .destructive, run: () => {}),
    ]
  }
}
```

An action can be `disabled` (dimmed, it can't be chosen) or `checked`
(a checkmark by it), and both follow what they read while the menu is
open. `.submenu(title:, icon:, items:)` is a submenu. `.section(title:,
items:)` is a section of the menu instead, set off by dividers under
its title (SwiftUI's `Section` in a `Menu`). A section with a checked
item is a choice:

```asuka
component Sorting {
  state sortBy = "Name"
  state locked = false

  fn sorted(by: string) -> Action = .button(title: by, checked: sortBy == by, run: () => { sortBy = by })

  Button("") {
    icon: "ellipsis.circle"
    label: "More"
    menu: [
      .section(title: "Sort by", items: [sorted("Name"), sorted("Date")]),
      .submenu(title: "Move to", icon: "folder", items: [
        .button(title: "Inbox", run: () => {}),
        .button(title: "Archive", run: () => {}),
      ]),
      .button(title: "Delete", role: .destructive, disabled: locked, run: () => {}),
    ]
  }
}
```

| | iOS | Android | macOS | Windows | Linux | Web |
|---|---|---|---|---|---|---|
| `disabled` | `.disabled` | disabled item | disabled item | grayed item | disabled action | `disabled`, `aria-disabled` |
| `checked` | `state = .on` | checked item; a section's choice as radio buttons | `state = on` | `MF_CHECKED` | stateful action (a check) | `menuitemcheckbox` |
| `.submenu` | nested `UIMenu` | `SubMenu` | submenu | submenu | GMenu submenu | submenu (the arrow key toward it opens it) |
| `.section` | `displayInline` `UIMenu` | group with dividers, its title a disabled item | separators, a section header (macOS 14) | separators, an owner-drawn header | GMenu section with its label | `group` between separators, under a heading |

The same holds for a screen's bar buttons (its `toolbar`'s): a
disabled one is dimmed, and one with a `menu` opens it (SwiftUI's
`Menu` in a toolbar). An alert's buttons can be disabled too, and
follow `disabled` while it shows (an OK that waits for a name). Swipe
actions show neither.

`Menu` puts a menu on any content. With `longPress: true` it's a
context menu, shown on a long press; with a mouse (macOS, Windows,
Linux, the web), on a secondary click:

```asuka
component Message {
  Menu {
    longPress: true
    actions: [
      .button(title: "Reply", icon: "message"),
      .button(title: "Delete", role: .destructive),
    ]
    View {
      padding: 16
      background: .secondaryBackground
      radius: 12
      Text("Long-press for a menu")
    }
  }
}
```

## Text fields

`TextField` is a text input. `bind text` is its text: typing writes the
state, and writing the state from code replaces the text.

```asuka
component SignIn {
  state email = ""
  state password = ""

  let pw = Ref()

  Column {
    gap: 12
    TextField {
      bind text: email
      placeholder: "Email"
      keyboard: .email
      contentType: "email"
      autocapitalize: .off
      autocorrect: .no
      returnKey: .next
      on submit { pw.focus() }
    }
    TextField {
      ref: pw
      bind text: password
      placeholder: "Password"
      secure: true
      contentType: "password"
      returnKey: .go
      on submit { pw.blur() }
    }
  }
}
```

- `secure` hides what's typed, for passwords.
- `keyboard`: `.default`, `.number`, `.decimal`, `.email`, `.url`,
  `.phone`.
- `returnKey` is the return key's label: `.default`, `.done`, `.go`,
  `.next`, `.search`, `.send`. Pressing it runs `on submit`.
- `contentType` says what it holds, for the system's AutoFill:
  `"username"`, `"password"`, `"newPassword"` (the platform offers a
  strong password), `"oneTimeCode"` (the code a message just brought
  shows above the keyboard: UIKit's `oneTimeCode`, Android's SMS code
  hint), `"email"`, `"phone"`, `"name"`, `"address"`, `"postalCode"`,
  `"cardNumber"`... Each platform names it its own way.
- `autocapitalize`: `.sentences` (the default), `.off`, `.words`,
  `.characters`. `autocorrect`: `.default`, `.yes`, `.no`.
- `on focus` and `on blur` follow the keyboard, and `bind focused`
  both follows it and moves it. A `Ref`'s `focus()` asks for it,
  `blur()` dismisses it.

A desktop has no on-screen keyboard to change, so `keyboard` and
`returnKey` mostly don't show there; Return still runs `on submit`. On
macOS `contentType` is the field's AutoFill content type. On Windows
`.number` takes digits only. On Linux `keyboard`, `contentType`,
`autocapitalize` and `autocorrect` tell the input method what the field
takes (GTK's input purpose and hints). In a browser they're the input's
`inputmode`, `enterkeyhint`, `autocomplete`, `autocapitalize` and
`spellcheck`, which a phone's keyboard follows.

**Lines of text.** `multiline: true` makes it a text area, for a
message or a comment: return starts a new line instead of running `on
submit`, and it grows with its text up to `lines` lines (0: no limit),
then scrolls.

```asuka
component Composer {
  state message = ""

  Row {
    gap: 8
    align: .end
    TextField {
      bind text: message
      placeholder: "Message"
      multiline: true
      grow: 1
      lines: 5
    }
    Button("") {
      icon: "paperplane"
      label: "Send"
      variant: .filled
      disabled: message.isEmpty()
      on press { message = "" }
    }
  }
}
```

**The text alone.** `plain: true` drops the field's border, background
and inset: its text and placeholder, for a field in a card or a form of
your own, as Notes' and Reminders' are.

```asuka
component NoteField {
  state note = ""

  TextField {
    bind text: note
    placeholder: "Note"
    multiline: true
    plain: true
  }
}
```

A `SafeArea` is padded by the keyboard, so a field at the bottom of one
stays above it.

## Controls

Each control binds the state it changes, and takes `disabled`. None of
them has a label of its own but `Picker` and `Toggle`'s `title`: put a
`Text` beside it.

```asuka
import std/time

component Settings {
  state wifi = true
  state volume = 0.5
  state period = 0
  state cups = 2.0
  state size = 1
  state milk = -1
  state tint: Color = .blue
  state when = time.now()

  Column {
    gap: 16
    Row {
      align: .center
      Text("Wi-Fi") { grow: 1 }
      Toggle { bind on: wifi }
    }
    Slider { bind value: volume }
    Segmented(["Day", "Week", "Month"]) { bind selected: period }
    Row {
      gap: 12
      align: .center
      Text("{cups.fixed(0)} cups") { grow: 1 }
      Stepper {
        bind value: cups
        min: 1
        max: 8
      }
    }
    Wheel(["Small", "Medium", "Large"]) { bind selected: size }
    Picker(["Whole", "Oat", "None"]) {
      bind selected: milk
      prompt: "Milk"
    }
    Row {
      gap: 12
      align: .center
      Text("Tint") { grow: 1 }
      ColorPicker {
        bind value: tint
        alpha: true
        title: "Tint"
      }
    }
    Row {
      gap: 12
      align: .center
      Text("Starts") { grow: 1 }
      time.Picker {
        bind value: when
        mode: .dateTime
        min: time.now()
      }
    }
  }
}
```

- **Toggle** is a switch, `bind on`. With `checkbox: true` it's a
  checkbox: Android's `MaterialCheckBox`, a Mac's and Windows' check
  box, a `GtkCheckButton`, a browser's check box, and on iOS, which has
  none, a circle that fills with a checkmark, as Reminders' does.
- **Slider** picks a `float` from `min` to `max` (by default 0 to 1).
- **Segmented** is one of a few options side by side, `selected` the
  index: UIKit's segmented control, Material's segmented buttons.
  Windows has none: Asuka draws one, as the Windows Community
  Toolkit's `Segmented`.
- **Stepper** steps a `float` with minus and plus, by `step` (default
  1), from `min` to `max` (by default 0 to 100). It's a `UIStepper` on
  iOS and an `NSStepper` on macOS; elsewhere it's two buttons (Material
  icon buttons on Android, linked buttons on Linux, as GNOME's spin
  buttons show them).
- **Wheel** picks one of many items on a spinning wheel:
  `UIPickerView`, Android's `NumberPicker`. The desktop and the web
  have no wheel: it's a pop-up menu of the items there (`NSPopUpButton`,
  a drop-down combo box drawn as WinUI's `ComboBox`, `GtkDropDown`,
  `<select>`).
- **Picker** picks one of many items from a menu, showing the chosen
  one (SwiftUI's `Picker` in its menu style; Material's exposed dropdown
  menu). `selected` out of range (-1, the default) is none chosen, and
  the picker shows its `prompt`, which also names it for screen readers;
  on Android the prompt is the field's label, floating above the chosen
  item. Unbound, the picker keeps the user's choice.

  | | Picker |
  |---|---|
  | iOS | a plain `UIButton` in the tint color with the pop-up indicator, its menu checking the chosen item (`changesSelectionAsPrimaryAction`) |
  | Android | an outlined `TextInputLayout` in the exposed dropdown menu style over a `MaterialAutoCompleteTextView` the user can't type in; TalkBack reads a drop-down list |
  | macOS | `NSPopUpButton` (the label a hidden item, shown while none is chosen) |
  | Windows | a drop-down list combo box drawn as WinUI 3's `ComboBox` (the control fill and stroke, the chevron at the end; its list owner-drawn on the flyout surface, the chosen item's accent pill at the start, rounded by DWM on Windows 11); the label drawn in the secondary text color while none is chosen, and its name for UI Automation |
  | Linux | `GtkDropDown` over a `GtkStringList`, the label a dim label where the chosen item goes |
  | Web | `<select>`, its first option a hidden placeholder showing the label |
- **ColorPicker** is a swatch that opens the platform's picker, writing
  a `Color`. `alpha` lets the user pick an opacity, `title` names the
  picker. On iOS it's `UIColorWell` and the system's picker; Android has
  none, so it's a Material dialog of a palette, with an opacity slider
  for `alpha`. On macOS it's an `NSColorWell` opening the color panel,
  on Linux a `GtkColorDialogButton`, in a browser `<input
  type="color">`. Windows' color dialog has no opacity, so `alpha` does
  nothing there.
- **DatePicker** picks a moment, shown in the device's time zone:
  `std/time`'s `time.Picker` binds a `time.Time` (the prelude's
  `DatePicker` binds milliseconds since 1970). `mode` is `.date` (the
  default), `.time` or `.dateTime`; `min` and `max` bound it.

  | | Compact (default) | `inline: true` |
  |---|---|---|
  | iOS | UIKit's compact date picker | the calendar and time in place, for a sheet of its own |
  | Android | buttons showing the date and time, opening Material's pickers | the same buttons |
  | macOS | `NSDatePicker`'s text field and stepper | its calendar and clock |
  | Windows | the date and time picker, its calendar dropping down | a month calendar, a time picker under it for `.dateTime` (`.time` stays compact) |
  | Linux | a button opening a popover of GTK's calendar and hour and minute spin buttons | the calendar and spin buttons in place |
  | Web | `<input type="date">`, `"time"`, `"datetime-local"` | the same input |

## Progress and activity

`Progress` is a progress bar: its `value` from 0 to 1, or `none` when
how far is unknown. Its style's `color` is its fill. `ProgressRing` is
the same in a circle: the ring fills clockwise from the top, animating
as the value changes, and `none` shows the platform's indeterminate
circular indicator. `Activity` is a spinner.

```asuka
component Upload {
  state done = 0.4

  Column {
    gap: 12
    Progress { value: done }
    Progress()
    ProgressRing { value: done }
    ProgressRing {
      value: done
      width: 24
      color: .green
    }
    ProgressRing()
    Activity()
  }
}
```

A ring is measured. Each platform gives it its own size (iOS 58 points,
Android 48dp, a Mac and Windows 32, Linux and the web 32). A style that
sets one side sets the other too, so the ring stays square. Where its
parent would stretch it (a `Column`'s default `align`), it keeps that
size, centered, as the platforms' indicators do. Screen
readers read it as a progress indicator with its percentage.

| | ring | without a value |
|---|---|---|
| iOS | UIKit has no determinate ring, and SwiftUI's circular `ProgressView` ignores its value there. So the ring is drawn as SwiftUI's circular capacity `Gauge` is: a round-capped stroke in the tint over a track of the tint at 30%, the stroke a tenth of the ring's size | `UIActivityIndicatorView` |
| Android | Material's `CircularProgressIndicator`, at Material 3's size for its frame (extra small, small or medium: their tracks, insets and gaps) | the same, indeterminate |
| macOS | `NSProgressIndicator`'s determinate circle at AppKit's own sizes, 16 and 32 points, with no `color`. AppKit draws its circle only at those sizes and only in the accent color, so any other size, or a `color`, draws what it draws, filling the frame: a 5-point round-capped stroke at 16 to 32 points (the 32-point circle's proportions above, the 16-point one's below) over AppKit's track, in the `color` or in the colors AppKit draws with, read from its own indicator (the accent, gray in an inactive window, light and dark). The value animates as AppKit's animator moves it, a quarter second, unless Reduce Motion is on. VoiceOver reads it as AppKit's indicator either way | its spinner at 16 and 32 points; else its spinner's 8 spokes drawn at the size (scaled, AppKit's blurs, and it takes no color), the leading spoke the `color` |
| Windows | Fluent's `ProgressRing` (WinUI's): an arc with no track. Narrator reads it as a progress bar | the arc turning, as `Activity`'s |
| Linux | drawn as `AdwSpinner` draws its circle: a track of the text color at 15%, and an arc in the accent color | `AdwSpinner` (`GtkSpinner` before libadwaita 1.6) |
| Web | an SVG ring, `role="progressbar"` with `aria-valuenow` | the arc turning, slower with `prefers-reduced-motion` |

## Images

`Image` shows a picture. The platform loads and decodes it off the
main thread, and caches it in memory. iOS and Android keep an image that
came over http(s) on disk too, across launches; the desktops keep
it in memory only, and a browser in its own cache.

```asuka
struct Photo {
  url: string
  blurHash: string
}

component Pictures(photo: Photo) {
  Column {
    gap: 12
    Image("logo.png") { width: 64 }
    Image("https://picsum.photos/id/1015/600/400") {
      placeholder: "placeholder.png"
      mode: .fill
      label: "A river in a valley"
      aspectRatio: 3.0 / 2
      radius: 12
      clip: true
    }
    Image(photo.url) {
      blurHash: photo.blurHash
      mode: .fill
      transition: 0.4
      aspectRatio: 1
      radius: 12
    }
  }
}
```

- Its source is an http(s) URL, a path in the app's `assets` directory
  (`"logo.png"` is `assets/logo.png`), or an absolute path: a file the
  app wrote, a photo the user picked.
- `mode` is how it fills its box: `.fit` (the default, all of it,
  letterboxed), `.fill` (covers the box, cropped), `.stretch` (the box's
  aspect ratio), `.center` (its natural size, centered).
- `placeholder` is shown until the source loads, and if it fails. Use an
  asset, which is there at once. Give the image a size so the
  placeholder has room.
- `blurHash` and `thumbHash` are placeholders from a hash an app keeps
  with the image ([BlurHash](https://blurha.sh),
  [ThumbHash](https://evanw.github.io/thumbhash/), in base64), as
  expo-image's are. Asuka decodes the hash into a small picture,
  once (the last few hundred are kept), and shows it at once in
  `placeholder`'s place. A BlurHash has no shape of its own: it's
  decoded at the style's `aspectRatio`, and without one it's square and
  stretched to the box. A ThumbHash keeps its image's aspect ratio and
  transparency. They're bindings, as a list's rows change images.
- `placeholderMode` is how the placeholder fills the box: `.fit`,
  `.fill`, `.stretch`, `.center`; by default, as the image does.
- `transition` is how long, in seconds, the image takes to fade in as
  it arrives: cross-dissolving over its placeholder, or from nothing.
  By default an image fades in only over a placeholder, for the
  platform's short duration (0.25 seconds; Android's
  `config_shortAnimTime`); `-1` shows it at once.
- `animated` plays an animated image while true, and stops it where it
  is while false. Without it, an animated image plays, as many times as
  its file says, unless the user asks for less motion (Reduce Motion,
  Android's animations off, `prefers-reduced-motion`): then it shows its
  first frame.
- `label` describes it to screen readers; without one, the image is
  decorative.
- `on loaded(width, height)` gets its natural size once it's loaded;
  `on error(message)` a message when it can't be.

Until it has loaded, an image without a size is empty. Then it takes its
natural size, one point per pixel, no wider than the space it has. With
one side set (a `width`, or `grow` in a row) it keeps its aspect ratio;
`aspectRatio` sets one before it loads.

Each platform decodes what its own decoders read:

| | Decodes | Plays (GIF, APNG, animated WebP) |
|---|---|---|
| iOS | ImageIO: PNG, JPEG, GIF, WebP, HEIC, AVIF | ImageIO's `CGAnimateImageDataWithBlock`, which times and loops it as the file says (HEIF sequences too) |
| Android | `BitmapFactory`, then `ImageDecoder`: PNG, JPEG, GIF, WebP, HEIF, AVIF | `AnimatedImageDrawable`: GIF and WebP (and AVIF sequences where the system plays them); an APNG shows its first frame |
| macOS | `NSImage`: PNG, JPEG, GIF, WebP, HEIC, AVIF, SVG | as iOS |
| Windows | WIC: PNG, JPEG, GIF, BMP, TIFF, ICO, and WebP, HEIF and AVIF where their codecs are installed (WebP's comes with Windows 11; HEIF needs the HEIF and HEVC Video Extensions, AVIF the AV1 Video Extension) | Asuka, frame after frame; an animated WebP's pictures by WIC's WebP codec |
| Linux | GDK's loaders: PNG, JPEG, GIF, and WebP, AVIF, HEIF, SVG and JPEG XL as installed | Asuka, frame after frame; an animated WebP's pictures by GDK's WebP loader |
| Web | the browser | WebCodecs' `ImageDecoder` on a canvas (AVIF sequences too); where there's none (Safari), the `<img>` itself, which shows its first frame while paused |

An SVG is an image on the Mac, Linux and the web only. For an SVG icon
on every platform, draw it with [`std/svg`](#svg-icons), as vectors.

## Symbols and icons

`Symbol` shows a symbol. It's an SF Symbol on iOS and macOS, the
Material Symbol for it on Android and the web, the Segoe Fluent Icons
glyph for it on Windows (Segoe MDL2 Assets before Windows 11), and on
Linux the icon theme's symbolic icon for it (Adwaita's), else the
Material Symbol. It's sized as text in its style's `font`, so it
follows the text size, and colored with its `color` (the label color by
default).

```asuka
component Symbols {
  Row {
    gap: 14
    align: .center
    Symbol("bell")
    Symbol("star.fill") {
      color: .yellow
      font: .title
    }
    Symbol("archivebox") { color: .secondaryLabel }
  }
}
```

A symbol's name is an SF Symbol name. Buttons, menu items, bar buttons
and tabs take one too: `"house"`, `"magnifyingglass"`, `"gear"`,
`"plus"`, `"xmark"`, `"trash"`, `"square.and.arrow.up"`,
`"ellipsis.circle"`..., and `".fill"` after a name is its filled form
(`"star.fill"`).

iOS and macOS draw any SF Symbol. The others draw a curated set, the
common ones and more, mapped to Material Symbols, and to Segoe Fluent
Icons' glyphs on Windows. A name outside the set shows nothing there
(the log has a warning), so check a name you write, like
`"archivebox"`, on each platform you ship. On Linux a name ending in
`-symbolic` is an icon theme's name, used as it is.

### SVG icons

For icons of your own, `std/svg` draws SVG with the platform's vector
drawing; nothing is rasterized. An `svg.Icon`'s markup is a string
literal, which the compiler parses: a markup it can't read is a compile
error, and nothing is parsed on the device.

```asuka
import std/svg

let download = svg.Icon(r"""<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><polyline points="7 10 12 15 17 10"/><line x1="12" x2="12" y1="15" y2="3"/></svg>""")

component DownloadRow {
  Row {
    gap: 8
    svg.View(download) { color: .accent; width: 24; height: 24 }
    Text("Download")
  }
}
```

`color` paints what the SVG paints with `currentColor`, and fills it
doesn't give; it's `.label`, the text color, by default. Without a width
or height in its style, the view is the SVG's own size; with both, the
icon is fitted in, its aspect kept. It reads what icon sets use (Lucide,
Material, Heroicons, Feather): paths, basic shapes, groups, `use`,
fills, strokes and transforms. It skips text, gradients (painted with
the icon's color), clipping, masks, filters and `fill-rule="evenodd"`.

`svg.Shape` draws paths the app makes: each `svg.Layer` a `svg.Path`,
filled (`fill`, fading to `fade` down the box) and stroked (`stroke`,
`width`, `cap`, `join`). A `svg.Point` is in points from the box's top
left plus fractions of its size, so a shape follows its box without the
app working it out again:

```asuka
import std/svg

let tick = svg.Path().move(svg.Point(x: 4, fy: 0.5)).line(svg.Point(fx: 0.4, fy: 1, y: -4)).line(svg.Point(fx: 1, x: -4, y: 4))

component Done {
  svg.Shape([svg.Layer(path: tick, stroke: .green, width: 3, cap: .round, join: .round)]) {
    width: 32
    height: 32
  }
}
```

It's Core Graphics on iOS and macOS, Android's `Canvas`, an `<svg>` in a
browser, GDI+ on Windows and Cairo on Linux.

## Video

`Video` plays a video with the platform's player and its controls:
`AVPlayerViewController` on iOS, Media3's ExoPlayer on Android,
`AVPlayerView` on macOS, a `GtkVideo` (GStreamer) on Linux, a
`<video>` in a browser. Win32 has no video view, so on Windows it's a
`<video>` in WebView2, with Edge's controls. A `Ref`'s `play()`,
`pause()` and `seek(seconds)` drive it.

```asuka
component Clip {
  let v = Ref()

  Column {
    gap: 12
    Video("https://example.com/clip.mp4") {
      ref: v
      autoPlay: true
      muted: true
      loop: true
      radius: 12
    }
    Button("Start over") {
      on press {
        v.seek(0)
        v.play()
      }
    }
    Button("Pause") { on press { v.pause() } }
  }
}
```

- Its source is an http(s) URL (a file, an HLS stream) or a file in the
  app's assets.
- `mode` is `.fit` (letterboxed, the default) or `.fill`.
- `hideControls: true` hides the platform's controls: play, the timeline, full
  screen, and on iOS AirPlay and picture in picture.
- `on end` runs each time it plays to its end; `on error(message)` if
  it can't play.

It sizes itself as an `Image` does: empty until the video's size is
known, unless its style gives it one, then keeping the video's aspect
ratio. `seek` takes seconds from the start.

## Lottie animations

`std/lottie` plays Lottie animations (After Effects animations
exported as Lottie JSON, `.json`, or as dotLottie, `.lottie`) with each
platform's own player: lottie-ios on iOS and the Mac, lottie-android on
Android, lottie-web on the web. Windows and Linux have no player to call
yet: there the view is an empty box.

```asuka
import std/lottie

component Success {
  state playing = true
  state progress = 0.0
  state done = false

  Column {
    lottie.View("success.json") {
      bind playing: playing
      bind progress: progress
      overrides: [lottie.colors("Ring", light: #0a84ff, dark: #64d2ff)]
      width: 200
      on complete { done = true }
    }
    Slider { bind value: progress }
  }
}
```

- Its source is the file: an http(s) URL, a path in the app's assets, or
  an absolute path; `data` is the file's bytes instead. `animation`
  picks one of a dotLottie's animations by its ID.
- `playing` plays it while true and pauses it where it is while false;
  bound, it turns false once it played through. `loop` plays it again
  and again; otherwise it stops at its end and `complete` is reported.
  `speed` scales its frame rate (negative: backwards).
- `progress` is where it is, from 0 to 1. Bound, the view writes it as
  it plays, and a value the app writes moves it there: a slider scrubs
  it, a gesture drives it.
- `segment` (frames, `from` to `to`) or `marker` (one of its markers, by
  name) is the part it plays. `loaded` gives its `Info`: its size,
  frames, frame rate and markers.
- `fit` is how it fills its box: `.contain` (the default), `.cover`, `.stretch` or `.center`. Without a size in its style, it takes the animation's.
- `overrides` change its colors and values by keypath, for a theme or
  dark mode: `lottie.color(keypath, c)` (the fills and strokes under a
  keypath), `lottie.colors(keypath, light:, dark:)`,
  `lottie.opacity(keypath, v)` and `lottie.strokeWidth(keypath, w)`. A
  keypath is the names of layers, groups and shapes from the root,
  separated by dots, with `*` for any one name and `**` for any number
  of them: `"Ring"`, `"Circle.Disc.Fill 1"`, `"**.Stroke 1"`.

An app that imports the package is built with these players: a Swift
package (lottie-spm), a Gradle library (`com.airbnb.android:lottie`) and
lottie-web's script, loaded once a view shows; apps that don't import it
go without. It's a [native package](native.md), so under `asuka dev` on
a phone, an app with it runs as a build of its own rather than in the
player.

## Web pages

`WebView` shows a web page in the platform's web view, with its
scrolling, zooming and selection: `WKWebView` on iOS and macOS,
Android's `WebView`, WebView2 (Edge's runtime) on Windows, WebKitGTK
6.0's `WebKitWebView` on Linux. Without WebKitGTK installed, it shows
nothing on Linux and runs `on error` once, saying what to install. In a
browser it's an `<iframe>`, which can do all of what follows only for
pages of the app's own site; see [the web](web.md#web-views). With no
height and no `grow` it grows to fill the space it's given.

```asuka
import platform/device

component Browser {
  state page = WebState()

  let web = Ref()

  Column {
    grow: 1
    Row {
      gap: 8
      padding: [8, 12]
      align: .center
      Button("") {
        icon: "chevron.backward"
        label: "Back"
        disabled: !page.canGoBack
        on press { web.goBack() }
      }
      Text(page.title) {
        grow: 1
        lines: 1
      }
      Button("") {
        icon: "arrow.clockwise"
        label: "Reload"
        on press { web.reload() }
      }
    }
    if page.loading { Progress { value: page.progress } }
    WebView("https://example.com") {
      ref: web
      block: ["doubleclick.net", "/ads/"]
      on state(s) { page = s }
      on message(m) { print("the page says {m}") }
      on newWindow(url) { device.openURL(url) }
    }
  }
}
```

What it shows:

- Its URL is an http(s) URL, or a file in the app's assets (its relative
  links are the assets' too). `html` is markup to show instead.
- `userAgent` is what pages are fetched with, and
  `navigator.userAgent`; empty, the platform's.
- `preload` is script run before each page's own, in every frame: to
  hook `fetch`, or set globals.
- `block` keeps requests from loading, as an ad blocker does. Each entry
  is a host, which matches its subdomains too (`"doubleclick.net"`), or,
  with a `/`, a part of the URL (`"/ads/"`).

`preload`, `userAgent` and `block` are set when the view is made.

What it reports:

- `on loaded(url)` gets the URL of each page that has loaded; `on
  error(message)` a message when one couldn't.
- `on state(s)` follows the page, as an address bar does: a `WebState`
  with its `url`, `title`, `loading`, `progress` (0 to 1), `canGoBack`
  and `canGoForward`.
- `on newWindow(url)` gets the URL the page would open in a new window
  (`window.open`, a `target="_blank"` link). Without it, that page loads
  in this view.
- `on download(d)` gets a response the page can't show, a file to save
  (a link to a `.zip`, a `Content-Disposition: attachment`), as a
  `WebDownload` with its `url`, `mime`, `filename` and `size` (-1 when
  unknown). It doesn't download it.
- `on request(url)` gets the URL of each request the page makes (scripts,
  images, media, `fetch`). **Not on iOS and macOS**: `WKWebView`
  doesn't let an app see them.

A `Ref`'s `goBack()`, `goForward()`, `reload()` and `stopLoading()`
move through its history, load the page again, or stop it loading.

### Talking to the page

The page posts to the app with `window.asuka.postMessage(text)`, which
runs `on message`. The app runs script in the page with a `Ref`'s
`run(script)`, which returns nothing, or `try await eval(script)`,
which returns the value as JSON: the last statement's, or what a
returned promise resolves to (`null` for `undefined`), or throws the
error it threw.

```asuka
component Page {
  let web = Ref()

  WebView {
    ref: web
    html: "<button onclick=\"asuka.postMessage('tapped')\">Tap</button>"
    on message(m) {
      let title = try? await web.eval("document.title")
      print("{m} {title ?? ""}")
    }
  }
}
```

`eval` awaits a promise with the page's `eval`, which a page's Content
Security Policy can block (without `'unsafe-eval'`). On such a page, on
iOS, Android, macOS and Linux, it runs the script as `run` does, which no policy applies to: the value is still the
last statement's, and an error is still what the script threw, but a
promise isn't awaited (Android answers `{}` for it, the others with an
error), and on iOS and macOS an object's keys can come back in another
order. Windows needs no `eval` and awaits as usual. On the web the
browser applies the page's policy to what the app runs in it, so a page
that blocks `eval` refuses both `run` and `eval`.

`try await webview.cookies(url)` gets the cookies web views have for a URL, as
a `Cookie` header: what the user signed in to in a page, for your own
requests.

[`examples/downloader`](../examples/downloader) uses all of this: a
browser with an address bar, a preload script, blocking and downloads.

## Shapes

`Shape` draws an outline, filled, stroked or both. Its path function
makes the outline for the shape's size; it runs again when the size
changes, and when state it reads changes. A shape takes the size its
style gives it (a width and height, an aspect ratio, `grow`); with none,
it's empty.

```asuka
component Triangle {
  Shape((w, h) => Path().move(w / 2, 0).line(w, h).line(0, h).close()) {
    fill: .orange
    width: 48
    height: 48
  }
}
```

A `Path` is in points from the shape's top left. Its methods add to it
and return it, to chain: `move`, `line`, `quad`, `cubic`, `arc` (angles
in radians, clockwise from the x axis), `close`, and whole figures,
`rect`, `roundedRect` and `ellipse`.

`stroke` and `lineWidth` (default 1) draw the outline, with a `lineCap`
(`.butt`, `.round`, `.square`) and a `lineJoin` (`.miter`, `.round`,
`.bevel`). `trimStart` and `trimEnd` draw part of it, from 0 to 1: a
progress ring, a line that draws itself. Changing them in `animate`
animates them (see [Motion](motion.md)). `dash` dashes the stroke:
lengths in points, drawn and skipped in turn (`[6, 4]`; an odd count
repeats), as SwiftUI's `StrokeStyle(dash:)` does. `dashPhase` is how far
into the pattern it starts. [`examples/usage`](../examples/usage) draws a forecast with
one.

```asuka
import std/math

fn ring(w: float, h: float) -> Path {
  let r = min(w, h) / 2 - 4
  return Path().arc(w / 2, h / 2, r, start: -math.pi / 2, end: 3 * math.pi / 2)
}

component ProgressCircle(done: float) {
  View {
    label: "Progress"
    width: 72
    height: 72
    Shape(ring) {
      stroke: .secondaryBackground
      lineWidth: 8
      absolute: true
      width: 100%
      height: 100%
    }
    Shape(ring) {
      stroke: .accent
      lineWidth: 8
      lineCap: .round
      trimEnd: done
      absolute: true
      width: 100%
      height: 100%
    }
  }
}
```

### Canvas

`Canvas` draws what the app draws: a chart, a gauge, a signature, a
game's board, whatever no view is. Its function draws with a `Graphics`,
in points from its top left:

```asuka
component Level(level: float) {
  Canvas(g => {
    let w = g.width
    let h = g.height
    g.fill(g.path().roundedRect(0, 0, w, h, 12), .secondaryBackground)
    g.fill(g.path().roundedRect(0, 0, w * level, h, 12), .accent)
    g.text("{(level * 100).fixed(0)}%", w / 2, h / 2 + 6, CanvasText(weight: .semibold, align: .center))
  }) {
    height: 36
  }
}
```

- `g.path()` is a path to build (as a Shape's), reused from draw to draw.
- `fill` and `stroke` (with a `StrokeStyle`: width, caps, joins,
  dashes) paint a path with a color, a `LinearGradient` or a
  `RadialGradient` (its center, radius, and a focus it starts from);
  `text` draws a line of text, its baseline at y, its left edge, center
  or right edge at x (a `CanvasText`'s `align`).
- `save` and `restore` bracket `translate`, `scale`, `rotate` (degrees,
  clockwise), `transform` (any affine matrix, a skew too), `clip`,
  `opacity` and `evenOdd`, which fills and clips by the even-odd rule
  (by default, the nonzero rule).
- Colors can be semantic (`.label`, `.separator`, `.accent`): they
  follow dark mode.

It draws once it's laid out, and again when its size changes or the
state the function read changes, like an effect. Each draw records a
display list, sent only when it differs from the last, and the platform
replays it: Core Graphics on iOS and macOS, Android's `Canvas`, GDI+ on
Windows, GTK's snapshots (GSK) on Linux, a 2D `<canvas>` in a browser.
Recording allocates nothing once the canvas has drawn as much
before. A canvas has no presses of its own: wrap it in a `View` with
`on press` or `on pan`, which report points in its coordinates. It
isn't read by screen readers: give the view around it a `label`.

### Charts

`std/chart` draws charts: bars, lines, areas and points over a value
axis, with the platform's text for their labels (scaled with the user's
text size). It's written in Asuka: the scale and the marks are worked
out once, drawn by `svg.Shape`, so it's the same on every platform.

```asuka
import std/chart

component Week(steps: [float], goal: [float]) {
  chart.View([
    chart.Series(name: "Steps", kind: .bar, values: steps),
    chart.Series(name: "Goal", kind: .line, values: goal, color: #ff9500),
  ]) {
    labels: ["Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"]
    label: "Steps this week"
    height: 180
  }
}
```

- A series has a value per label. Bar series stand side by side; areas,
  lines and points go over them. `smooth` draws a monotone curve, which
  never overshoots its values; `width` is a line's width or a dot's
  size.
- The value axis fits the values, rounded out to whole ticks (and down
  to zero for bars and areas), unless `low` and `high` set it; its
  labels are as short as the ticks allow (`2.5k`), or what `format`
  makes of each (`format: v => "${v.fixed(2)}"`), which screen readers
  say too. `grid: false` leaves
  out the lines behind the marks.
- Colors default to the app's accent, then the system's
  (`chart.palette`).
- Screen readers read the chart as one image: its `label`, then each
  series' values. Its height is 200 when its style gives none.

### Shaders

A shader colors each pixel of a box with a GPU program: flowing
gradients, noise, glows, ripples. `shader Name(params) { … }` declares
one, in Asuka's own syntax, and it's used as a view: its parameters are
its uniforms, set as a component's are, and it takes the style as a
component does. It's compiled when the app compiles, for every
platform: a mistake in it is a compile error at its line, and only the
platform's own program is compiled on the device.

```asuka
/// A glow at the box's center, breathing.
shader Glow(tint: Color, level: float = 1) {
  let speed = 2

  fn fragment(pos: vec2) -> vec4 {
    let d = distance(pos / size, vec2(0.5))
    let g = smoothstep(0.5, 0, d) * (0.8 + 0.2 * sin(time * speed)) * level
    return vec4(tint.rgb * g, 1) * tint.a
  }
}

component Card(level: float) {
  Glow(tint: .accent, level: level) {
    animated: true
    height: 200
    radius: 16
    clip: true
    background: #0b1030
  }
}
```

- `fn fragment(pos: vec2) -> vec4` gives the color of a pixel,
  premultiplied, red, green, blue and alpha from 0 to 1, from `pos`, the
  pixel's center in points from the box's top left. Inside a shader,
  `size` is the box's size in points, `scale` the pixels a point, and
  `time` the seconds since it showed.
- A parameter is a `float`, an `int`, a `bool` or a `Color` (a `vec4`
  inside, its channels from 0 to 1). A change to one draws the box
  again. `animated: true` runs `time`: the box draws at every frame the
  display draws.
- Its values are `bool`, `int`, `float`, `vec2` to `vec4`, `ivec2` to
  `ivec4` and `mat2` to `mat4`, with swizzles (`v.rgb`, `p.yx`); a color
  literal is a `vec4`. It calls GLSL's functions (`sin`, `mix`,
  `smoothstep`, `dot`, `normalize`...) and functions of its own (`fn
  wave(x: float) -> float = …`), and has constants (`let`), `let` and
  `var`, assignments, `if`, `while`, `for i in 0..<n`, `return`, `break`
  and `continue`. What a GPU can't run (closures, strings, optionals,
  an `if` as a value) is a compile error that says so.
- The compiler also reports what of a shader Android can't run, for
  every platform alike: AGSL is GLSL ES 1.00's language, so no unsigned
  integers, bitwise operations or integer remainders (`mod` of floats
  does), derivatives, or loops between values that aren't constants.

It's Metal on iOS and macOS (a `CAMetalLayer`), AGSL on Android 13 and
later (a `RuntimeShader`; an empty box before), WebGL 2 in a browser
(GLSL ES 3.00), Direct3D 11 on Windows (HLSL; WARP, the system's
software rasterizer, where there's no GPU) and OpenGL on Linux (a
`GtkGLArea`; Mesa's software rasterizer where there's no GPU). Each
platform's program is compiled with the app; the device compiles only
its own shading language's. On Windows a frame renders off the main
thread and shows when it's ready. Linux without GL (`GDK_DISABLE=gl`)
shows an empty box. Give the box a `background` that stands in until
the first frame.

## Containers

`View` holds other views. `Column` and `Row` are views that stack their
children top to bottom and leading to trailing. `SafeArea` is a column padded by the notch, the system bars and the keyboard. How
they size and place what's in them is in [Layout](layout.md).

A view with no look, events or accessibility props makes no native view
of its own: its children go in the nearest one that does. So nesting
rows and columns costs nothing.

```asuka
import platform/device

component Person {
  state open = false

  Row {
    gap: 12
    padding: 16
    background: .secondaryBackground
    radius: 12
    label: "Ada, 2 unread"
    on press { open = !open }
    on longPress { device.haptic(.medium) }
    Symbol("person.fill")
    Column {
      grow: 1
      gap: 2
      Text("Ada") { font: .headline }
      Text("2 unread") { color: .secondaryLabel }
    }
  }
}
```

A `View`, `Row`, `Column` or `SafeArea` takes:

- `on press` and `on longPress`.
- Gestures, with the platform's recognizers: `on pan(g)`, `on
  pinch(g)`, `on rotate(g)` (each given a `Gesture`) and `on
  swipe(direction)`. See [Motion](motion.md).
- Interaction states. The view's own style follows them through its
  variants (`pressed { … }`, `hovered { … }`, `dragging { … }`,
  `dragOver { … }`). Other views follow them through its `Ref`
  (`card.hovered`, `card.pressed`). Side effects go in `on
  pressedChange` and `on hoverChange`. Drag and drop: `dragItems` and
  `dropTypes`, with `on drop(d)`. See
  [Motion](motion.md#pressed-and-hover-states).
- `on layout(frame)`, run with the view's `Frame` once it's laid out
  and each time that changes.
- `href`, a route it goes to when it's pressed (a link: give it `role:
  .link` too). See [Routes](navigation.md#routes).
- Accessibility: `label`, `hint`, `role`, `a11yHidden`, `actions` and
  `on adjust(by)`. See [Accessibility](accessibility.md).

### Scroll

`Scroll` scrolls its children along one axis. Give it a size, or
`grow`, so it has something to scroll within. Its style is the scroll
view's box; its children stack in a column inside it (a `SafeArea` or
a `Column` in it pads and spaces them).

```asuka
component Page {
  state down = false

  let scroll = Ref()

  View {
    grow: 1
    Scroll {
      ref: scroll
      grow: 1
      on scroll(_, y) { down = y > 300 }
      SafeArea {
        padding: 20
        gap: 16
        for i in 0..<50 key i { Text("Line {i + 1}") }
      }
    }
    if down {
      View {
        absolute: true
        right: 20
        bottom: 20
        Button("") {
          icon: "arrow.up"
          label: "Back to top"
          variant: .filled
          on press { scroll.scrollTo(0, 0) }
        }
      }
    }
  }
}
```

- `on scroll(x, y)` runs as it scrolls, with its content offset in
  points.
- A `Ref`'s `scrollTo(x, y, animated:)` scrolls to an offset (past the
  end is the end); `scrollToEnd(animated:)` to its bottom, or a
  horizontal one's trailing edge. Both work on a `List` too.
- `horizontal: true` scrolls sideways:

```asuka
component Tags(tags: [string]) {
  Scroll {
    horizontal: true
    Row {
      gap: 8
      padding: [0, 16]
      for t in tags key t { Text(t) }
    }
  }
}
```

**Pages.** `paging: true` makes it rest only at whole pages: a
carousel, an onboarding's steps, a feed of full-screen videos. Each
child is a page, as big as the scroll view along its axis. `bind page`
is the page shown: a swipe writes it as it settles, and writing it
scrolls there. `PageControl` shows where it is, a dot per page, and
moves it too when one is tapped:

```asuka
component Carousel {
  state page = 0

  Column {
    gap: 8
    Scroll {
      horizontal: true
      paging: true
      bind page: page
      height: 200
      for i in 0..<3 key i { Text("Card {i + 1}") }
    }
    PageControl(3) { bind page: page }
  }
}
```

| | iOS | Android | macOS | Windows | Linux | Web |
|---|---|---|---|---|---|---|
| Paging | `isPagingEnabled` | each swipe snaps to the next page, or back | settles on the nearest page when a swipe ends | a page per wheel notch (FlipView's) | a page per wheel notch; a touchpad's or touchscreen's swipe settles on a page; the arrow keys, Page Up and Page Down move a page | CSS scroll snapping |
| `PageControl` | `UIPageControl` | dots in the theme's primary color (Material has none) | dots in the accent color | pips (PipsPager's) | dots drawn as libadwaita's, the current one larger and darker | dots, the current one in the label color |

Pages count from the reading start: right to left, the first is the
rightmost. When the scroll view's size changes (a rotation), it stays on
its page. Screen readers say "page 2 of 3" on the dots and adjust them.

**Under a bar.** A scroll view (a `Scroll`, a `List`) whose top is
under a bar its content can show through, iOS's navigation bar or a
desktop window's hidden title bar, starts its content below the bar and
scrolls it under. `edgeEffect` is how the content is set off from the
bar as it goes under: `.automatic` (the platform's), `.soft` (it fades)
or `.hard` (a clear edge), the scroll edge effect of iOS 26 and macOS 26
(on a Mac, the style of the split view column it's in). Elsewhere it
does nothing.

**Pull to refresh.** `on refresh` turns it on (`UIRefreshControl`,
`SwipeRefreshLayout`). Set `refreshing` true while you refresh; the
spinner hides when it turns false. Without `refreshing`, it hides as
soon as the handler returns. On macOS a trackpad's or Magic Mouse's
scroll pulls it past its top, as on iOS. Windows and Linux refresh the
desktop's way: F5 (on Linux, Ctrl+R too) refreshes the scroll view the
focus is in, or else the innermost one on screen, a spinner over its
top meanwhile. In a browser a touch pulls it down.

```asuka
component Feed {
  state refreshing = false

  Scroll {
    grow: 1
    refreshing: refreshing
    on refresh {
      refreshing = true
      await sleep(1s) // fetch
      refreshing = false
    }
    Text("Pull down")
  }
}
```

**A bar on the keyboard.** A `TextField`'s `accessory { … }` is a bar shown on top of the
keyboard while the field is focused: a count and Done, formatting
buttons, quick replies (SwiftUI's keyboard toolbar). It's as wide as the
window and as tall as its content, built with the field and disposed
with it.

```asuka
component Note {
  state note = ""
  state focused = false

  TextField {
    bind text: note
    bind focused: focused
    multiline: true
    accessory {
      Row {
        padding: [6, 16]
        align: .center
        material: .bar
        Text("{note.count()} characters") { grow: 1 }
        Button("Done") { on press { focused = false } }
      }
    }
  }
}
```

On iOS it's the field's `inputAccessoryView`: it moves with the
keyboard, through its interactive dismissal too. Android has none of its
own: the bar goes in the field's window (a sheet's too) at the top of the
keyboard, following it frame by frame. macOS, Windows and Linux, with no
on-screen keyboard, don't show it; a browser shows it only on a touch
screen.

**Styled text.** `bind rich` makes a field a text area of styled text, for a
mail's body or a formatted message: bold, italic, underline,
strikethrough, monospaced code and links (colors and text styles a span
has are kept as they are). It's the same `[Span]` a `RichText` shows.
The user's typing and formatting write it; writing it replaces the text.
A `Ref`'s `toggleFormat(.bold)` turns a style on for the selection
unless all of it has it, then off (`format(.bold, on:)` sets it either
way); with no selection, it's what's typed next. `setLink(url)` links
the selection (`""` unlinks; at the cursor, the link it's in). `on
formats(f)` reports the styles where the cursor is, or that all the
selection has, for a toolbar to show.

```asuka
component Composer {
  state body = [Span(text: "Hi,\n\nThe "), Span(text: "1.0", font: .body.bold()), Span(text: " build is ready.")]
  state formats = TextFormats()

  let field = Ref()

  Column {
    grow: 1
    Button("") {
      icon: "bold"
      label: "Bold"
      background: if formats.bold { .secondaryBackground } else { .clear }
      on press { field.toggleFormat(.bold) }
    }
    TextField {
      ref: field
      bind rich: body
      plain: true
      grow: 1
      on formats(f) { formats = f }
    }
  }
}
```

| | iOS | Android | macOS | Windows | Linux | Web |
|---|---|---|---|---|---|---|
| The view | `UITextView`, its attributed text | `EditText`, its spans | `NSTextView`, rich | a rich edit (`RICHEDIT50W`), through RTF | `GtkTextView`, its buffer's tags | a `contenteditable` element |
| The user's formatting | the edit menu's B/I/U | a hardware keyboard's Ctrl+B, Ctrl+I, Ctrl+U | the Format menu, ⌘B ⌘I ⌘U | Ctrl+B, Ctrl+I, Ctrl+U | Ctrl+B, Ctrl+I, Ctrl+U | the browser's ⌘B ⌘I ⌘U (Ctrl elsewhere) |
| Undo of formatting | yes | yes: the field's own history (Android's keeps text only), Ctrl+Z, Ctrl+Shift+Z, Ctrl+Y | yes | yes | GTK's history (Ctrl+Z, Ctrl+Shift+Z), which keeps text only | yes: the page's editing commands |

[`examples/mail`](../examples/mail) has a composer with a toolbar.

**Padding.** A text's or a field's `padding` (and `border`) is
inside its frame, around its content, as in CSS: its background fills
it, its text is drawn inside it, and a field's editing happens inside it.

## Lists

`List` shows a row per item in the platform's recycling list
(`UICollectionView`, `RecyclerView`). The desktop and the web have no
list that holds rows of any view, so there Asuka recycles rows
itself, in the platform's scroll view. Rows are made only for the cells
on screen; a cell scrolled away is reused for another item, and only the
props that differ are updated. [`examples/list10k`](../examples/list10k)
has ten thousand rows.

```asuka
struct Message {
  id: int
  from: string
  subject: string
}

state messages = [Message(id: 1, from: "Ada", subject: "Lunch?"), Message(id: 2, from: "Grace", subject: "The report")]

component Inbox {
  List {
    for m in messages key m.id {
      Column {
        gap: 2
        padding: [12, 20]
        Text(m.from) { font: .headline }
        Text(m.subject) { color: .secondaryLabel }
      }
    }
  }
}
```

Its `for` takes the items and a key for each, and builds a row from an
item (and its index, `for (i, m) in messages.enumerated()`). Keys say
which item is which from one list to the next: a changed item updates
its row, a moved one moves it, and the platform animates the
difference. A row's bindings read its item, so a reused row follows its
new one.

A list fills the space it's given: with no height and no `grow`, it
grows. It draws the platform's separators between rows.

A `List` takes:

- `swipeActions`, a row's actions, revealed by swiping it: on the
  row's top view (`swipeActions: [...]`), or for the list, a function of
  the row's index. On
  iOS they are its trailing actions, a destructive one red, the first
  run by a full swipe. On Android a full swipe runs the first, over its
  color and icon, as Material's swipe to dismiss. On macOS, Windows,
  Linux and the web they're the row's context menu (a secondary click;
  on Linux and the web a long press too), leading then trailing.
- `on endReached`, run when the last rows are about to show, once for
  each length the list has: load more there.
- `on refresh` and `refreshing`, pull to refresh as on a `Scroll` (F5
  on Windows and Linux).
- `on scroll(x, y)`, as on a `Scroll`.
- `appearance`, its look. `.plain` is rows edge to edge, apart by
  separators; `.insetGrouped` is the platform's grouped list
  (libadwaita's boxed lists on Linux); `.bare` has no separators and nothing behind its rows, so what's
  behind the list shows through, for rows that draw their own cards. A bare
  list's rows leave their own space, and can still be swiped.

```asuka
struct Message {
  id: int
  subject: string
}

fn fetch(from: int, n: int) -> [Message] = (from..<from + n).map(i => Message(id: i, subject: "Message {i + 1}"))

component Inbox {
  state messages = fetch(0, 30)

  List {
    on endReached {
      messages += fetch(messages.count(), 30)
    }
    for m in messages key m.id {
      Row {
        padding: [12, 20]
        swipeActions: [
          .button(title: "Delete", icon: "trash", role: .destructive, run: () => messages.removeWhere(x => x.id == m.id)),
          .button(title: "Archive", icon: "archivebox", run: () => messages.removeWhere(x => x.id == m.id)),
        ]
        Text(m.subject)
      }
    }
  }
}
```

`leadingSwipeActions` are revealed by swiping the other way, from the
row's leading edge (Mail's Unread, Pin): the first is at that edge, and
a full swipe's. iOS tints them; Android shows them as Material's
filled buttons.

A `Ref`'s `scrollToItem(index, animated:)` scrolls a list so an item
shows; `scrollTo` and `scrollToEnd` work too.

### Chats: a list anchored at its bottom

`anchor: .bottom` is a chat's list: it opens at its last row,
rows that don't fill it sit at its bottom, and while it's at its end it
stays there: as rows are added at the end, as they change height, and
as the list gets shorter (the keyboard). The user scrolling up leaves
the end; scrolling back to it, or `scrollToEnd()`, sticks again. `on
startReached` runs when the first rows are about to show, once for each
first item: load older messages there.

Whatever the anchor, rows added or removed above the ones on screen
don't move those (React Native's `maintainVisibleContentPosition`):
older messages loaded at the top, or an item removed above, leave the
user reading what they were. Only at the edge the list is anchored to
does it show the new rows: at the top of a list read from the top (a
new mail), at the end of a chat.

```asuka
struct Message {
  id: int
  text: string
}

component Chat {
  state msgs: [Message] = []
  state next = 0

  let list = Ref()

  fn send(text: string) {
    next += 1
    msgs.push(Message(id: next, text: text))
    list.scrollToEnd() // one's own message shows, wherever the list was
  }

  Column {
    grow: 1
    List {
      ref: list
      anchor: .bottom
      on startReached { print("load older") }
      for m in msgs key m.id { Text(m.text) }
    }
    Button("Send") { on press { send("Hello") } }
  }
}
```

[`examples/chat`](../examples/chat) is a whole conversation.

### Sections

`section` puts rows in sections: a function of a row's index to its
section's key. Rows next to each other with the same key are one,
headed by `header`'s view (a day in a chat, a letter in contacts, Today
and Yesterday in a mailbox). `header` gets the key, and is followed: a
header is a row of its own, made as it comes into view and reused for
another section after.

```asuka
struct Message {
  id: int
  day: string
  text: string
}

state msgs = [Message(id: 1, day: "Yesterday", text: "Hi"), Message(id: 2, day: "Today", text: "Hello")]

component Chat {
  List {
    section: i => msgs[i].day
    header: day => Text(day) {
      font: .footnote.semibold()
      padding: [6, 20]
    }
    for m in msgs key m.id { Text(m.text) }
  }
}
```

A plain or bare list's header stays at the top while its section's rows
scroll under it, and the next header pushes it up (UITableView's plain
style; on Android, which has none, a cell over the list, bound to the
section at the top; on the desktop and the web, the header row
itself). A plain list's header is on the list's background; a bare
list's draws its own (a chat's pill over the messages). An inset grouped
list's sections are groups of their own, each under its header, which
scrolls with it.

### Selecting rows

`selecting` puts a list in selection mode (Mail's Edit): each row shows
the platform's selection control, a tap selects or deselects it instead
of pressing it, and rows don't swipe. A row's `selected` says whether
it is (it's followed, so a selection kept in state shows as it
changes), and `on select(on)` hears the user's choices; what the app
doesn't take goes back. The rows are laid out narrower by the control's room.

```asuka
struct Message {
  id: int
  text: string
}

state msgs = [Message(id: 1, text: "Hi"), Message(id: 2, text: "Hello")]

component Mailbox {
  state selecting = false
  state chosen: [int] = []

  List {
    selecting: selecting
    for m in msgs key m.id {
      Row {
        padding: [12, 20]
        selected: chosen.contains(m.id)
        on select(on) {
          if on { chosen.push(m.id) } else { chosen.removeWhere(id => id == m.id) }
        }
        on selectBegin { selecting = true }
        Text(m.text)
      }
    }
  }
}
```

| | iOS | Android | macOS, Windows, Linux | Web |
|---|---|---|---|---|
| The control | UIKit's edit mode circles | a Material checkbox at the row's end | the row highlighted (the accent's selection color; a WinUI list's pill on Windows) | a circle at the row's leading edge that fills with a checkmark, the row tinted |
| `on selectBegin` | a two-finger pan over rows, which goes on to select the rows it crosses | a long press (Gmail's) | a ⌘-click on the Mac, a Ctrl-click on Windows and Linux (Finder's, Explorer's) | a ⌘-click (Ctrl-click off Apple's systems); a long press on a touch screen |
| Screen readers | each row selectable | each row a checkbox, its text the label | — | each row selected or not (`aria-selected`) |

### Grids

`columns` makes a list a grid (a `LazyVGrid`, a `GridLayoutManager`):
items fill rows of that many, each as wide as the others, `gap` apart
across and down. A row is as tall as its tallest item. A grid has no
separators and no swipe actions.

```asuka
component Photos(ids: [int]) {
  List {
    columns: 3
    gap: 2
    for id in ids key id {
      Image("https://picsum.photos/id/{id}/300/300") {
        mode: .fill
        aspectRatio: 1
      }
    }
  }
}
```

### List or for in a Scroll

A `for` outside a `List` makes a view per item too, keyed the same way,
but it makes all of them, as plain views among their siblings. Use it in a `Scroll` for
a short list that's part of a page: a few tags, a form's rows, a
section under a header.

```asuka
component Tags(tags: [string]) {
  Scroll {
    grow: 1
    Column {
      padding: 20
      gap: 8
      Text("Tags") { font: .headline }
      for t in tags key t { Text(t) }
    }
  }
}
```

Use `List` when the items are many or unbounded (a feed, an inbox,
search results, photos), or you want swipe actions, separators, loading
more at the end, or a grid. A `List` scrolls by itself: don't put it in
a `Scroll`.

## Your own look

The elements above are each platform's own controls: an app that sets no
style looks native everywhere, a `Picker` a menu on iOS and a drop-down
on Windows. When an app has a design of its own (its brand's colors,
shapes and motion on every control, the same on a phone and in a
browser), or needs a control the platforms don't have (a combo box, a
toggle group, a range slider), `std/base` gives the behavior without a
look: a select, a menu, a dialog, a checkbox, tabs, a slider. Each
handles its state, the keyboard and its focus, screen readers, and its
place over the window; what it shows is only what the app styles.

```asuka
import std/base

style field {
  padding: [8, 12]
  radius: 8
  border: 1
  borderColor: .separator
  focusVisible { borderColor: .accent; border: 2 }
}

style menu {
  padding: 4
  radius: 10
  background: .background
  shadow: Shadow(radius: 12, y: 4)
}

style option {
  padding: [6, 10]
  radius: 6
}

component FruitPicker {
  state fruit = ""

  base.Select {
    bind value: fruit
    base.SelectTrigger {
      style: field
      base.SelectValue { placeholder: "Pick a fruit" }
    }
    popup {
      base.SelectPopup {
        style: menu
        for f in ["Apple", "Banana", "Cherry"] key f {
          base.SelectItem(f) {
            style: option
            highlighted { background: .accent.opacity(0.15) }
            selected { font: .headline }
            Text(f)
          }
        }
      }
    }
  }
}
```

A component is parts: a root that holds the state (`Select`), and parts
the app arranges and styles inside it (`SelectTrigger`, `SelectValue`,
`SelectPopup`, `SelectItem`). Each part takes `style:` and the style
props, and its states are variants the app styles, as `pressed { … }`
is: `checked { … }`, `open { … }`, `highlighted { … }`, `selected { … }`,
`disabled { … }`. A root's value is the caller's when it's bound (`bind
value: fruit`), its own otherwise. A popup's parts go in its root's
`popup` slot, which shows them next to the trigger; `backdrop` dims the
window behind a dialog.

| Group | Components |
|---|---|
| Actions | `Button`, `Toggle`, `ToggleGroup`, `Toolbar` |
| Choices | `Checkbox` (`CheckboxGroup`), `Switch`, `RadioGroup` and `Radio`, `Select`, `Listbox`, `Combobox` |
| Values | `Slider`, `NumberField`, `Progress`, `Meter` |
| Forms | `Field` (its label, description and error), `Input`, `Fieldset`, `Form` |
| Disclosure | `Collapsible`, `Accordion`, `Tabs` |
| Popups | `Dialog`, `AlertDialog`, `Popover`, `Tooltip`, `PreviewCard`, `Menu`, `ContextMenu`, `Menubar` |
| Other | `Avatar`, `ScrollArea`, `Separator`, toasts (`Toasts`, `ToastViewport`) |

Their keys follow the platforms' conventions for the same controls:
arrow keys move through a menu's, a select's or a radio group's items
and typing jumps to an item, Escape closes a popup and Back on Android
does too, and the focus goes into a popup as it opens and back to its
trigger as it closes. `asuka doc std/base` lists every part, its props
and its variants.

Prefer the platform's control where it does more than its role: iOS's
date picker's wheels, a field's AutoFill, a share sheet. The two mix on
one screen.

### Overlay

`std/base`'s popups are made of `Overlay`, which shows its children over
the window while `open`, next to an anchor:

```asuka
component Hint {
  state open = false
  let anchor = Ref()

  Column {
    View {
      ref: anchor
      on press { open = !open }
      Text("What's this?")
    }
    Overlay {
      bind open: open
      anchor: anchor
      side: .bottom
      align: .start
      offset: 4
      on dismiss(reason) { print("closed by {reason}") }
      Text("Overlays are placed next to their anchor") {
        padding: 8
        background: .secondaryBackground
      }
    }
  }
}
```

| Key | What it does |
|---|---|
| `open` | shows the children while true: built as it opens, disposed as it closes (in `animate { }`, their transition plays) |
| `anchor` | the view it's next to (a `Ref`); none: in the window (a dialog, a drawer, a toast) |
| `side`, `align` | the anchor's side (`.bottom`, `.top`, `.start`, `.end`, or `.center` over it) and where along it (`.start`, `.center`, `.end`; `.stretch`: as long as the anchor) |
| `offset`, `alignOffset` | the gap from the anchor, and a move along the side |
| `flip` | to the other side when it doesn't fit (true by default) |
| `edgeGap` | the least distance from the window's edges and the safe area (8 by default); it's moved along its side to stay in, and made no bigger than the room left |
| `matchWidth` | at least as wide as the anchor |
| `modal` | presses reach only it, and screen readers see only it |
| `dismissOn` | what closes it: `.outsidePress` (not on the anchor), `.escape`, `.back`, `.focusOut`, `.anchorHidden` (scrolled out of view); all by default |
| `backdrop { … }` | what covers the window behind it while it's open |
| `on place(placed)` | the side it ended on, where the anchor's middle is along it (an arrow's place), the room it has |

The overlay follows its anchor as it moves (a scroll, a resize, the
keyboard) and flips as it runs out of room, the same on every platform.
It's in the window the anchor is in, over a sheet when the anchor is in
one.

## Your own views

When a platform has a widget Asuka doesn't, write it yourself as a
`native component`: in Swift on iOS and macOS, Kotlin on Android, C on
Windows and Linux, and JavaScript on the web. See [Native
code](native.md).
