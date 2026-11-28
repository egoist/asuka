# Your own look

Asuka gives an app two ways to make its interface, and one screen can
use both:

- **The platform's controls** (`Button`, `Toggle`, `Picker`, `TextField`,
  `Sheet`...): each platform's own widget, so an app that sets no style
  looks as its apps do, UIKit on iOS and Material 3 on Android. See
  [Components](components.md).
- **`std/base`**: components with behavior and no look of their own. A
  select, a menu, a dialog, a switch, tabs and a slider handle their
  state, the keyboard and the focus, screen readers and their place
  over the window. What they show is only what the app styles, so the
  app looks the same on a phone, a desktop and in a browser.

Choose `std/base` when the app has a design of its own: its brand's
colors, shapes and motion on every control, the same everywhere, as a
design system on the web would have it. It also gives controls that some
platforms don't have: a combo box, a toggle group, a range slider, a
menu bar in the window, toasts.

Keep the platform's control where it does more than draw. iOS's date
picker has its wheels, a text field has AutoFill and the system's text
editing, and a share sheet reaches the platform's apps. `std/base`'s
`Input` is a styled `TextField` underneath, so typing, selection and the
keyboard stay the platform's.

## A whole screen

`std/base` comes with Asuka: `import std/base`, and its components are
`base.Select`, `base.Switch`, `base.Dialog`... This is a settings card
in a theme of its own:

```asuka check
import std/base

/// The app's look: every style reads its colors and corners.
store Theme(dark: bool) {
  pub let ink: Color = if dark { #f2f2f7 } else { #1c1c1e }
  pub let paper: Color = if dark { #18181c } else { #ffffff }
  pub let line: Color = if dark { #3a3a44 } else { #d1d1d6 }
  pub let accent: Color = #6d5dfc
  pub let corner: float = 10
}

style card {
  use t: Theme
  padding: 16
  gap: 12
  radius: t.corner
  background: t.paper
  border: 1
  borderColor: t.line
}

style field {
  use t: Theme
  direction: .row
  align: .center
  gap: 8
  padding: [8, 12]
  radius: t.corner
  border: 1
  borderColor: t.line
  focusVisible {
    border: 2
    borderColor: t.accent
  }
}

style option {
  use t: Theme
  padding: [6, 10]
  radius: t.corner - 4
  highlighted { background: t.accent.opacity(0.15) }
}

style track {
  use t: Theme
  width: 46
  height: 28
  radius: 14
  padding: 3
  background: t.line
  checked { background: t.accent }
}

style thumb {
  width: 22
  height: 22
  radius: 11
  background: #ffffff
  checked { margin: Edges(start: 18) }
}

style primary {
  use t: Theme
  padding: [8, 16]
  radius: 999
  background: t.accent
  pressed { opacity: 0.7 }
}

component Settings {
  use t: Theme
  state units = "metric"
  state sync = true
  state confirming = false

  Column {
    style: card

    base.Select {
      bind value: units
      labels: ["metric": unitName("metric"), "imperial": unitName("imperial")]
      base.SelectTrigger {
        style: field
        base.SelectValue {
          color: t.ink
          grow: 1
        }
        Symbol("chevron.down") { color: t.ink }
      }
      popup {
        base.SelectPopup {
          style: card
          padding: 4
          gap: 0
          for u in ["metric", "imperial"] key u {
            base.SelectItem(u) {
              style: option
              label: unitName(u)
              Text(unitName(u)) { color: t.ink }
            }
          }
        }
      }
    }

    Row {
      gap: 10
      align: .center
      base.Switch {
        style: track
        bind checked: sync
        label: "Sync"
        base.SwitchThumb { style: thumb }
      }
      Text("Sync") { color: t.ink }
    }

    base.Dialog {
      bind open: confirming
      base.DialogTrigger {
        style: primary
        Text("Reset…") { color: #ffffff }
      }
      backdrop { View { background: #00000066 } }
      popup {
        base.DialogPopup {
          style: card
          width: 300
          base.DialogTitle("Reset settings?") {
            color: t.ink
            font: .headline
          }
          base.DialogDescription("Units and sync go back to their defaults.") { color: t.ink }
          Row {
            gap: 8
            justify: .end
            base.DialogClose {
              style: option
              Text("Cancel") { color: t.ink }
            }
            base.Button {
              style: primary
              on press {
                units = "metric"
                sync = true
                confirming = false
              }
              Text("Reset") { color: #ffffff }
            }
          }
        }
      }
    }
  }
}

fn unitName(u: string) -> string = if u == "metric" { "Metric" } else { "Imperial" }

component Screen {
  provide Theme(dark: environment().dark)

  SafeArea {
    padding: 20
    Settings()
  }
}

app { Screen() }

test "units and a reset" {
  let app = mount(Screen())
  app.tap("Metric")
  app.tap("Imperial")
  app.expect("Imperial")
  app.tap("Reset…")
  app.expect("Reset settings?")
  app.tap("Reset")
  app.expectNo("Reset settings?")
  app.expect("Metric")
}
```

The card is drawn the same on iOS, Android, macOS, Windows, Linux and
the web: its colors, corners and spacing are the app's. What changes
from one platform to another is what each user expects of a control: a
press on a touch screen and a click with a mouse, the keys a desktop
has, Back on Android, and each platform's screen reader.

## Parts, slots and states

A component is parts. A root holds the state (`Select`, `Dialog`,
`Tabs`), and the parts the app arranges and styles go inside it
(`SelectTrigger`, `SelectValue`, `SelectPopup`, `SelectItem`). Each part
shows one element, so it takes `style:` and the style props as a `View`
does, and the app puts what it likes in it: a symbol, an image, a row
of text.

- **Values.** A root's value is the caller's when it's bound (`bind
  value: units`, `bind checked: sync`, `bind open: confirming`), and its
  own otherwise. Each reports changes as an event too (`on
  valueChange(v)`, `on checkedChange(c)`, `on openChange(o)`).
- **Slots.** A popup's parts go in its root's `popup` slot, which the
  root shows over the window next to its trigger (a dialog's in the
  middle, or against an edge with `side:`). `backdrop` is what covers
  the window behind a dialog.
- **States.** Each part's states are variants the app styles, as
  `pressed { … }` is styled on a `View`: `checked { … }`, `open { … }`,
  `highlighted { … }`, `selected { … }`, `disabled { … }`. A style can
  hold them too (`style track { checked { … } }`), and a ref reads them:
  `base.Switch as s`, then `s.checked`.

`asuka doc std/base` prints every part, its props, its events and its
variants, and so does [the reference](std/base.md).

## A theme

A theme is a store of the values the styles read: colors, corners,
spacing, fonts. A style takes it with `use`, and `provide` gives it to a
screen's components:

```asuka
store Theme(dark: bool) {
  pub let ink: Color = if dark { #f2f2f7 } else { #1c1c1e }
  pub let accent: Color = #6d5dfc
}

style label {
  use t: Theme
  color: t.ink
}

component Screen {
  provide Theme(dark: environment().dark)
  Text("Hello") { style: label }
}
```

Reading `environment().dark` follows dark mode as the user switches it.
A theme that's state (a setting the user picks) switches every style
that reads it at once. The semantic colors (`.label`, `.accent`,
`.separator`) work too, where the app wants the platform's color in its
own shapes.

## The components

| Group | Components |
|---|---|
| Actions | `Button`, `Toggle`, `ToggleGroup`, `Toolbar` |
| Choices | `Checkbox` (`CheckboxGroup`), `Switch`, `RadioGroup` and `Radio`, `Select`, `Listbox`, `Combobox` |
| Values | `Slider`, `NumberField`, `Progress`, `Meter` |
| Forms | `Field` (its label, description and error), `Input`, `Fieldset`, `Form` |
| Disclosure | `Collapsible`, `Accordion`, `Tabs` |
| Popups | `Dialog`, `AlertDialog`, `Popover`, `Tooltip`, `PreviewCard`, `Menu`, `ContextMenu`, `Menubar` |
| Other | `Avatar`, `ScrollArea`, `Separator`, toasts (`Toasts`, `ToastViewport`) |

They're written in Asuka from `View`, `Text`, `Overlay` and the style
props, as Base UI's are from the DOM.

## Keyboard, focus and screen readers

Their keys follow the platforms' conventions for the same controls:

- Arrow keys move through a menu's, a select's, a radio group's or a
  tab list's items, and typing jumps to an item.
- Escape closes a popup, and so does Back on Android.
- The focus goes into a popup as it opens and back to its trigger as it
  closes. A dialog keeps it inside while it's open, and hides the rest
  of the window from screen readers.
- Tab moves between controls; a radio group or a tab list is one Tab
  stop.
- `focusVisible { … }` styles the focus ring only when the keyboard
  moved it, not on a tap.

Each part has the role of what it is, so each platform's screen reader
reads a `Switch` as a switch, on or off: VoiceOver, TalkBack, Narrator,
Orca and the browsers' readers. A switch or a checkbox with no text in
it takes a `label:`, as the one above does. See
[Accessibility](accessibility.md).

## Motion

A popup's content is built as it opens and disposed as it closes. Open
it in `animate { }`, or bind it to state that's written in one, and the
transitions its views declare play: a menu that fades and scales in, a
drawer that slides from an edge. See [Motion and
gestures](motion.md).

## Testing

`std/base`'s components are views like any other, so `test` blocks drive
them as a user would: `app.tap("Metric")` opens a select, `app.tap` on
an item chooses it, `app.pressKey("down")` moves through a menu, and
`app.expect` checks what shows. See [Testing](testing.md).

[`examples/base`](../examples/base) is a settings screen in two themes,
with a form, a select, a switch, a checkbox, a tooltip, tabs, a slider,
a dialog and toasts.

## Building your own

`std/base`'s components are made of what every app has: `View` with a
`role`, `focusable`, `keys` and the accessibility props, and `Overlay`
for what shows over the window ([Overlay](components.md#overlay)). A
control it doesn't have is written the same way, in a package of the
app's own, with `pub variant`s for its states. See [Variants a component
declares](language/components.md#variants-a-component-declares).
