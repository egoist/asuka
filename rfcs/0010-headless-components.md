# 0010: Headless components

- **Status**: Built: the language, the core, the fake host and std/base;
  each host by the contract in [0010-host-contract.md](0010-host-contract.md)
- **Created**: 2026-11-17
- **Related**: 0009 (composable styles), docs/accessibility.md,
  docs/components.md, core/src/ui/overlay.c, core/src/ui/interaction.c

## Summary

A standard library package, `std/base`, of components that have
behavior and no look: a `Select`, a `Checkbox`, a `Dialog`, a `Menu`, a
`Slider`, `Tabs`... Each one handles its state, the keyboard and its
focus, the screen readers and its placement over the screen, and shows
nothing the app doesn't style. They're written in Asuka from `View`,
`Text` and the style props, so a styled `base.Select` looks and behaves
the same on iOS, Android, macOS, Windows, Linux and the web, and the app
gives it its brand. This is what Base UI and Radix are to React.

```asuka
import std/base

component FruitPicker {
  state fruit = "apple"

  base.Select { bind value: fruit
    base.SelectTrigger { style: field
      base.SelectValue { placeholder: "Pick a fruit" }
      Symbol("chevron.down")
    }
    base.SelectPopup { style: popup
      for f in fruits key f {
        base.SelectItem(f) { style: option
          highlighted { background: .accent.opacity(0.12) }
          selected { font: .headline }
          Text(f.capitalized())
        }
      }
    }
  }
}
```

Native elements stay the default and nothing about them changes: an app
that sets no style still looks native. `std/base` is the second path,
for an app whose design is its own.

Most of the work isn't the package: it's what the core and the hosts
must give a component written in Asuka so it can be as good as a
platform's widget. Focus and the keyboard on any view, roles and states
for screen readers, a layer over the window that content can be placed
in next to a view, and states a component declares that its caller
styles. Each is useful without `std/base`, and each is in every host.

## As built

What changed from the design below as it was built (the sections below
keep the reasoning; DESIGN.md has the result):

- **No FocusScope element.** Trapping, restoring and autofocus are a
  container's keys: `trapFocus` (a wire prop each host's focus system
  follows), `restoreFocus` and `autofocus` (the core's: it knows the
  tree and the focused view). Containers also take `focusable`,
  `pressKeepsFocus` and `hideFocusRing`; `ref.focused` and
  `ref.focusVisible` read the states (a Window's `focused` is still the
  window's).
- **The host reports the anchor.** An overlay's anchor can be in a
  List's cell or under a scroll the core doesn't follow, so the host
  reports where it is (`AnchorFrame`), when the overlay is made and as
  it moves; the core places the content (`ak_place`, place.c) and keeps
  the layer hidden until the first report. Overlay's keys are `side`
  and `align` (an `Align`, `.stretch` as long as the anchor) instead of
  one `Placement`; `edgeGap` is the collision padding; `dismissOn` is a
  `[DismissReason]`; `backdrop { }` is a slot.
- **Accessibility keys** are `a11yChecked: Checked` (`.unchecked`,
  `.checked`, `.mixed`; `.off` not checkable), `a11yExpanded: Expanded`
  (`.collapsed`, `.expanded`; `.off`), `a11ySelected`, `a11yValue`,
  `a11yRange: ValueRange?`, `a11yModal`, and the relations
  `a11yControls`, `a11yLabelledBy`, `a11yDescribedBy`, which take a
  `Ref`.
- **A press's point** is an event of its own, `pressStart(x, y)`,
  rather than parameters added to `pressedChange`.
- **Popups' parts go in the root's `popup` slot**, which the root shows
  in its Overlay: a part can't be both the caller's styled element and
  inside an element the root owns. Every popup root provides a
  `Placement` its parts read (the side variants, `Arrow`).
- **Variants and assigned props** are as the language section says; a
  variant can't be named after a keyword, `as` works on any component
  that shows one element, and a bound prop's component learns it's
  bound from a hidden argument (lang/LOWERING.md).
- **What the components needed of the language**, fixed or added on the
  way: what a component provides is its own views' (siblings that
  provided the same store saw the last one's); `use x: Store?` is none
  when nothing provides one; `ref.id`; a slot that defaults to none is
  none; an optional slot alone in an element's lazy children was built
  twice. `asuka doc` lists a component's props, events and pub variants.
- **Tests** drive the keyboard on the fake host: `app.tab()`,
  `app.focused()`, and `app.pressKey` from the focused view, with a
  desktop's Tab, Enter, Space and typeahead.

## Motivation

Asuka's elements are the platforms' widgets (docs/components.md,
"Each platform's widgets"). That's the right default, and it's why
`Picker` is `UIMenu` on iOS and a drop-down on Windows. It isn't what
every app wants:

- **A brand.** A design system (the app's colors, radii, type and
  motion on every control) is restyling widgets the platform draws.
  The style props reach them only where the widget supports the key
  (DESIGN.md, "Style": "applied where the native widget supports them
  and ignored elsewhere"), so a `Toggle` can be tinted but not shaped,
  and a `Picker`'s menu not at all.
- **The same app everywhere.** A product whose web app and phone app
  share a design wants a select that looks one way, not six.
- **Controls the platforms don't have.** A combobox, a toggle group, a
  hover card, a tabs strip that isn't navigation, a range slider with
  two thumbs. Today each is written by hand from `View`, and each
  author has to rediscover the keyboard, focus and screen reader
  behavior, and can't, because the primitives aren't there:

  - A `View` takes the keyboard's focus only when it's pressable or
    adjustable, only on Windows, Linux and the web, and shows the
    platform's focus ring, which the app can't restyle or style around
    (docs/accessibility.md, "The keyboard"). There's no focused state to
    style, and a `Ref`'s `focus()` reaches fields only.
  - `Role` has seven cases (`button`, `header`, `image`, `link`, `text`,
    `search`, `off`) and no states. A checkbox drawn from views can't
    tell VoiceOver it's checked, and a disclosure can't say it's
    expanded.
  - There's nowhere to put a popup. `Popover` is the platform's bubble,
    with its arrow and its material; `absolute` places a view in its
    parent, under its parent's clip and under later siblings. A select's
    list, a menu, a tooltip need a layer over the window, placed next to
    the view that opened them, flipped when there's no room, dismissed
    by a press outside or Escape or Back.
  - A component's state can't be styled by its caller. RFC 0009's
    variants are the four interaction states (`hovered`, `pressed`,
    `dragging`, `dragOver`). A caller can't write `checked { … }` on a
    checkbox or `highlighted { … }` on a menu item.
  - A component's prop is controlled only: `bind value: s` works, but a
    component with no binding can't keep its own value, so every
    `Disclosure` needs its caller's `state open`.

Base UI, Radix, React Aria and Ark (on Zag's state machines) agree on
the shape of the answer: a component is parts (a root, a trigger, a
popup, items) that share the root's state, each part one element the
app styles, its states exposed to the styles (`data-state="open"`,
`data-highlighted`), controlled or not, and the accessibility written
once in the part. The parts are what's portable; the platform only has
to give them focus, the keyboard, roles and a layer to show in.

## Decisions

| Question | Decision | Why |
|---|---|---|
| Native or headless | Both. Native elements stay the default; `std/base` is opt-in | The default should look native; a brand is a choice the app makes. |
| Where it lives | `std/base`, plain Asuka, no native code | The same everywhere is what it's for: std, not sdk. Plain Asuka means it can't need what an app's component couldn't have. |
| Shape | Parts: a root that provides its state, each part a component that shows one element | Each part takes `style:` (0009's components that take the style); the caller arranges and styles them as they like. Base UI's and Radix's. |
| Part names | Flat: `base.Select`, `base.SelectTrigger`, `base.SelectItem` | Works with today's names. `Select.Trigger` (a component's namespace) is a later nicety, not a need. |
| State the caller styles | A component's `variant` declarations: `variant checked = …` in the part, `checked { … }` in the caller's block or a style | 0009's state layers with a condition the component owns; Base UI's `data-*` attributes, typed and known to the compiler. |
| State the caller reads | The same variants through a ref: `base.Checkbox as c` then `c.checked` | As `card.hovered` reads an interaction state today. |
| Controlled or not | A prop with its `event <prop>Change` may be assigned in its component: bound, the write goes to the caller; unbound, the component keeps it | Svelte's `$bindable`, Vue's `defineModel`, Base UI's `useControlled`, without a second `defaultValue` prop to keep in step. |
| Focus | Any container can take the keyboard's focus (`focusable`), on every host; `focused` and `focusVisible` are variants; the platform's ring goes when the view styles `focusVisible` | A control of views must be reachable and must show it, in the app's look. |
| Moving focus | `Ref.focus()` reaches any focusable view; `FocusScope` keeps Tab inside it and gives focus back when it goes; arrow keys are the component's (`keys`) | A trap and a return are the platform's to do right; roving focus is a few lines of Asuka over them. |
| Screen readers | `Role` grows the ARIA roles the parts need; new props for their states (`checked`, `expanded`, `a11ySelected`, `valueText`, `modal`) | Every host's accessibility API has these; a part can't be accessible without them. |
| Popups | An `Overlay` element: content laid out in a layer over the window, placed next to an anchor by the core, with flip and shift | Placement is the same arithmetic everywhere: the core's (floating-ui's algorithm in C, in physical frames). The layer is each host's. |
| Dismissal | `Overlay` reports `dismiss(reason)`: a press outside, Escape, Back, focus leaving | Each host knows its own Back and Escape; the component decides what closes. |
| Styled components | Later, as source the app owns (`asuka add select`), over `std/base` and a theme store | shadcn's model: a starting look the app edits, not a second dependency to restyle. |

## The language

### Variants a component declares

A component may declare a variant, a bool its caller can style:

```asuka
pub component Checkbox {
  prop checked = false
  event checkedChange(checked: bool)
  prop disabled = false

  variant checked = checked        // the caller's checked { … } applies while it's true
  variant disabled = disabled

  View {
    role: .checkbox
    a11yChecked: if checked { .on } else { .off }
    focusable: !disabled
    on press { if !disabled { checked = !checked } }
    keys: [Shortcut(key: " ", run: () => { if !disabled { checked = !checked } })]
    children
  }
}
```

```asuka
base.Checkbox { bind checked: agree
  style: box
  checked { background: .accent; borderColor: .accent }
  focusVisible { border: 2; borderColor: .accent }
  base.CheckboxIndicator { Symbol("checkmark") }
}
```

- `variant name = expr` takes a bool expression, a binding. It may be
  `pub` in a `pub` component: only then may another package's caller
  style it. Its name may be a prop's or a state's (the common case
  is the same name); it can't be an interaction state's.
- The component must show one element (check's `TakesStyle`): the
  variant's layers go on that element.
- A style may hold a block of any variant; it applies on an element
  whose kind declares it, and is ignored elsewhere, as `pressed { … }`
  is ignored on an element that takes no variants. A variant a kind
  doesn't declare, written in that kind's block, is an error.
- The order (0009's "The order, in full", step 6) gains the declared
  variants after the interaction states, in their declaration order: a
  `disabled { … }` declared last wins over `pressed { … }`, which is
  what Base UI's attribute order and CSS's usual ordering give.
- `View as c` exposes them: `c.checked`, a tracked bool, as `c.hovered`.

Parts share state through `provide` and `use` (docs/language/stores.md):
the root provides a store, each part uses it, and a part's variant
reads the store (`variant highlighted = s.highlighted == value`). An
item's variants are per item, since each item's binding is its own.

### Props that may be bound

A prop that has its change event (`prop open`, `event openChange(open:
bool)`) may be assigned in its component, as a state is:

```asuka
pub component Collapsible {
  prop open = false
  event openChange(open: bool)
  …
  on press { open = !open }
}
```

- If the caller wrote `bind open: s`, the assignment emits
  `openChange(new)` and nothing else: `s` changes and the prop follows
  it, as now. The caller may refuse a change by not writing `s`.
- If the caller wrote `open: x` (a value, not a binding) or nothing,
  the component keeps the assigned value as its own from then on, and
  emits `openChange` for a caller with `on openChange`. A later change
  of `x` replaces it: the caller's value wins whenever it changes.
- A prop without its change event can't be assigned: the error says to
  declare the event.

Lowering: such a prop is a signal of the component's, initialized from
the caller's value and rewritten by its binding when there is one; an
assignment is `if bound { emit } else { write }`. No allocation per
write beyond a state's.

### Focus

New container keys and variants (lang/prelude/elements.go):

| Key or event | Type | What it does |
|---|---|---|
| `focusable` | `bool` | takes the keyboard's focus: a Tab stop in the views' order, focused by a press with a pointer (`focusOnPress`) |
| `focusOnPress` | `bool` | default true; false for a trigger that keeps focus where it was (a toolbar's button) |
| `on focusChange(focused)` | event | as a field's |
| `focused`, `focusVisible` | variants | the view, or one in it, has focus (CSS's `:focus-within` for containers is `focused`); `focusVisible` only when it came from the keyboard (CSS's `:focus-visible`) |

A pressable or adjustable view stays focusable as today. A view whose
block or styles set `focusVisible { … }` draws its own ring: the host's
goes (the rule `pressedChange` already follows, "it then draws its own
pressed look").

`Ref.focus()` and `blur()` reach any focusable view. Focus that comes
from `focus()` after a key (a menu opened with Enter) is visible; after
a press, it isn't.

`FocusScope` is a new container:

```asuka
FocusScope { trap: true; restore: true; autofocus: .first
  …
}
```

- `trap`: Tab and Shift+Tab cycle inside it; VoiceOver and TalkBack's
  swipes stay inside it while it's `modal` (below).
- `restore`: when it goes, focus goes back to the view that had it
  before it came, if that view is still shown.
- `autofocus`: `.first` (the first focusable view), `.none`, or a ref.

### Screen readers

`Role` gains: `checkbox`, `radio`, `radioGroup`, `switch`, `tab`,
`tabList`, `tabPanel`, `slider`, `spinButton`, `progressBar`, `meter`,
`menu`, `menuBar`, `menuItem`, `menuItemCheckbox`, `menuItemRadio`,
`listbox`, `option`, `comboBox`, `dialog`, `alertDialog`, `tooltip`,
`group`, `separator`, `toolbar`, `status`. New accessibility keys:

| Key | Type | Maps to |
|---|---|---|
| `a11yChecked` | `Checked?` (`.on`, `.off`, `.mixed`) | iOS: the value ("1", "0", "2") and the toggle trait; Android: checkable, `checked`, `stateDescription`; macOS: `NSAccessibilityValue`; Windows: UIA's Toggle pattern; Linux: `GTK_ACCESSIBLE_STATE_CHECKED`; web: `aria-checked` |
| `expanded` | `bool?` | iOS: "expanded"/"collapsed" in the value (iOS 18: `accessibilityExpandedStatus`); Android: `setExpandedState` (API 36) else actions expand/collapse; macOS: `AXExpanded`; Windows: ExpandCollapse; Linux: `GTK_ACCESSIBLE_STATE_EXPANDED`; web: `aria-expanded` |
| `a11ySelected` | `bool` | the selected trait / `isSelected` / SelectionItem / `aria-selected` |
| `valueText` | `string` | what's read as the value (a slider's "40%"); iOS `accessibilityValue`, Android `stateDescription`, `aria-valuetext` |
| `valueRange` | `ValueRange?` (min, max, now) | Android `RangeInfo`; macOS `AXMinValue`…; Windows RangeValue; `aria-valuemin`… |
| `modal` | `bool` | iOS `accessibilityViewIsModal`; Android: the rest `importantForAccessibility = noHideDescendants`; macOS `AXModal`; Windows/Linux: the rest hidden; web `aria-modal`, the rest `inert` |
| `controls`, `labelledBy` | `Ref?` | relations where the platform has them (Windows' ControllerFor, `aria-controls`, `aria-labelledby`); ignored where not |

A host maps a role to its closest widget role (`option` is a button on
iOS with the selected trait; Android's className
`android.widget.CheckBox` gives TalkBack its words). docs/
accessibility.md's "Roles" gets the table per host.

### Overlay

An element that shows its content over the window, placed next to an
anchor:

```asuka
Overlay { open: s.open; anchor: trigger
  placement: .bottom(.start); offset: 4; flip: true; shift: 8
  matchWidth: true; modal: false
  on dismiss(reason) { s.open = false }
  content { … }   // built as it opens, disposed as it closes (after its transition)
}
```

| Key | Type | Default |
|---|---|---|
| `open` | `bool` | false |
| `anchor` | `Ref?` | none: the window's center (a dialog) |
| `placement` | `Placement` (`top`, `bottom`, `start`, `end`, each with `.start`, `.center`, `.end`; `.center` of the window) | `.bottom(.center)` |
| `offset` | `float` | 0 |
| `flip` | `bool` | true: to the other side when it doesn't fit |
| `shift` | `float` | 8: the least distance kept from the window's (safe area's) edge |
| `matchWidth` | `bool` | false: its content's width; true: the anchor's, at least |
| `modal` | `bool` | false. True: a scrim the caller styles (`scrim { … }` slot), the rest hidden from screen readers and inert, scrolling behind locked on the web |
| `dismissOn` | `[DismissReason]` | all: `.outsidePress`, `.escape`, `.back`, `.focusOut`, `.anchorHidden` |
| `on dismiss(reason)` | event | |
| `on place(side: Side)` | event | the side it ended on after flip, for the arrow and the transition's origin |

The core lays the content out as a layout root (as a `Sheet`'s or a
`Popover`'s content is, overlay.c), then places it: the anchor's frame
in the window, the content's size, the window's size and safe area, the
placement mirrored in a right-to-left layout. Floating UI's
`computePosition` with `offset`, `flip`, `shift` and `size` middleware
is the reference; the arithmetic is a pure function in C
(`ak_place(anchor, size, bounds, opts)`), tested against its cases. It
places again when the anchor's frame changes, the window resizes, the
keyboard shows, or a `Scroll` around the anchor scrolls (hosts already
report the offsets lists use; Scrolls report them while an overlay is
anchored in them). The frame it sends is physical, as every frame is.

Overlays stack: the last open is on top and takes Escape and Back
first (the back stack `ak_back` already orders). One opened from inside
another (a submenu) is its child: a press inside the child isn't
outside the parent.

Each host has one overlay layer per window, over its content and under
the platform's own presentations made after it:

| Host | The layer |
|---|---|
| iOS | a pass-through `UIView` on top of the presenting view controller's view (a sheet's own while the anchor is in a sheet) |
| Android | a `FrameLayout` on top of the activity's content (a `Dialog`'s window when the anchor is in one); Back through `OnBackPressedDispatcher` |
| macOS | a top subview of the window's content view; later, a borderless child `NSPanel` for menus that leave the window |
| Windows | a top child `HWND` of the window, drawn as the content is |
| Linux | a `GtkOverlay` child over the window's content |
| Web | an element in the top layer (`popover="manual"`), `position: fixed` |

Escape on a hardware keyboard on iOS and Android too; Back on Android
and the web's history isn't touched (an overlay isn't a route).

### Pointers and keys

Two smaller gaps the parts need:

- `on pressedChange` gains where: `pressedChange(pressed, x, y)` (a
  slider's track jumps to the press; a menu opened on press releases
  over an item). Existing handlers with one parameter keep working.
- Typeahead: `on keyText(text)` on a focused view, the characters
  typed that no `keys` took (a listbox's jump to "Ba…").

## The package

`std/base` holds the parts, each documented with what it does with
keys and screen readers (lang/doc requires its `///`):

| Group | Components |
|---|---|
| Actions | `Button`, `Toggle`, `ToggleGroup`, `Toolbar` |
| Choices | `Checkbox`, `CheckboxGroup`, `Switch`, `RadioGroup` + `Radio`, `Select`, `Combobox`, `Listbox` |
| Values | `Slider` (one or two thumbs), `NumberField`, `Progress`, `Meter` |
| Text | `Field` (label, description, error, validity, linking them to its control), `Input` (a `TextField { plain: true }` with the field's wiring), `Fieldset`, `Form` |
| Disclosure | `Collapsible`, `Accordion`, `Tabs` |
| Popups | `Dialog`, `AlertDialog`, `Drawer`, `Popover`, `Tooltip`, `PreviewCard`, `Menu`, `ContextMenu`, `Menubar` |
| Other | `Avatar`, `Separator`, `ScrollArea`, `Toast` |

Each has the parts Base UI gives it (Select: `Select`, `SelectTrigger`,
`SelectValue`, `SelectIcon`, `SelectPopup`, `SelectItem`,
`SelectItemIndicator`, `SelectGroup`, `SelectGroupLabel`,
`SelectSeparator`, `SelectArrow`) and its variants (`open`, `selected`,
`highlighted`, `disabled`, `side` as `top`/`bottom`/`left`/`right`
variants for a popup).

Behavior follows the platform where they differ: a `Menu` opens on
press on phones and on press-and-release on desktops (and a release
over an item selects it, as AppKit's do); a `Tooltip` shows after a
hover delay with a pointer and on long press on a touch screen; a
`Dialog` on a phone may be placed at the bottom (`placement: .bottom`
of the window) by its caller. Keys follow the WAI-ARIA Authoring
Practices, with ⌘ for Ctrl on Apple platforms.

`base.Tabs` is a tabs control, not navigation's `Tabs` (which is the
platform's tab bar and keeps being the one for an app's sections); the
docs say so in both places.

## Native or headless: what the docs say

docs/components.md gets a section, "Your own look", before "Your own
views": native elements first, `std/base` when the app has a design of
its own or needs a control the platforms lack. The two mix: a screen
may use `Picker` and `base.Combobox`. What `std/base` gives up is said
plainly: the platform's look and its future changes (a new iOS's
controls), and where a widget does more than its role (iOS's
`UIDatePicker` wheels, Android's autofill on a field), `std/base` has
no equivalent and the native element stays the better one.

## Checking

- `variant` in a component whose body isn't one element: "a component
  with variants shows one element".
- A variant block a kind doesn't declare (`checked { … }` on a `View`):
  "View has no variant checked", with the kinds' variants listed.
- A variant's expression isn't a bool, or names an interaction state.
- Assigning a prop with no change event: "declare event openChange(open:
  bool) to assign open".
- `anchor:` takes a `Ref`; an Overlay's `content` is a lazy slot.

## Lowering and the core

- A declared variant is a state layer (0009's `ak_layer`, tier
  `AK_LAYER_STATE`) whose condition is the component's binding:
  `ui.layer(v, AK_LAYER_STATE, AK_STATE_CUSTOM)` and `ui.layer_when(v,
  l, closure{expr})`, made once per element, in declaration order after
  the interaction states. Toggling it allocates nothing, as any layer.
- `focused` and `focusVisible` are two more interaction states
  (`AK_STATE_FOCUSED`, `AK_STATE_FOCUS_VISIBLE`), reported by the host
  once a variant or `ui.state` asks, as `hovered` is.
- `FocusScope` and `Overlay` are elements in `ops/schema.go` (new kinds
  of native view: the scope's container, the overlay's layer), their
  keys in elements.go, their behavior in core/src/ui (focus.c, and
  overlay.c's new section), placement in core/src/ui/place.c.
- The accessibility keys go on the wire as props of every view, in the
  props batch: no per-key crossing.

Tests:

- core/test: `ak_place` against Floating UI's cases (flip, shift, size,
  RTL), no allocation per placement; an overlay's content disposed with
  its owner and when it closes; dismiss reasons from the fake host;
  overlays stacking and Back's order; a focus scope's trap and restore
  in the fake host's focus model; a declared variant's layer on and off.
- lang: variants and bindable props on the VM and compiled to C
  (lang/compile/testdata/ui), their errors (lang/check/testdata/errors).
- std/base: each part's test blocks on the fake host, keys and focus
  included (`app.key("ArrowDown")`, `app.focused()`, `app.role(…)` in
  `asuka test`).
- Each host: conformance for overlay placement (the window's edges,
  RTL, the keyboard), the e2e suites for focus and screen reader
  properties read back through each platform's automation (XCUITest,
  UiAutomator, AX, UIA, AT-SPI, the DOM's accessibility tree).

## Later: styled components

`std/base` gives behavior. Most apps also want a starting look: shadcn's
answer is source the app copies in and owns, over the headless parts
and a theme of tokens.
For Asuka:

- `asuka add select` writes `ui/select.asuka` into the app: a `Select`
  composed of `base.Select*` parts with styles that read a `Theme` store
  (`ui/theme.asuka`, written with the first `add`): colors, radii,
  spacing, type, motion.
- The registry is a directory of the repository, versioned with the
  CLI; `asuka add --diff select` shows what changed upstream.
- Its own RFC, after `std/base`'s first group ships.

## Alternatives

- **Restyling native widgets further.** More style keys per widget
  (a `Toggle`'s thumb shape, a `Picker`'s menu rows). Each platform
  allows a different subset, so the result is still six looks, and the
  menus and popups of most platforms can't be styled at all.
- **Asuka drawing every control (Flutter's way).** One look, but the
  framework's premise is the platforms' widgets; it would be a second
  renderer, and text input, accessibility and IME would all be redone.
  Headless components draw with the same native views `View` and
  `Text` already are.
- **Headless as native components** (each part's behavior in Swift,
  Kotlin, C and JavaScript). Six implementations of every keyboard
  model. The parts' behavior is the same everywhere; only focus, roles
  and the layer are the platform's.
- **Context-free parts with slots** (`base.Select { trigger { … }
  popup { … } }`). Fewer names, but a slot's content is the caller's
  views, so the trigger's role, focus and keys would go on a wrapper the
  caller can't style. Parts are elements the caller styles directly.
- **A `className`-like render function** (`style: (s) => …`). Styles
  aren't values (0009, "Later: styles as values"); declared variants
  give the same reach with styles the compiler inlines.
- **Using the native `Popover` for popups.** It brings its bubble, arrow
  and material on iOS and macOS, can't be placed or styled freely, and
  differs per platform: the opposite of what this is for.

## Open questions

1. **The name.** `std/base` (Base UI's) or `std/headless`; the
   former reads better at the call site (`base.Select`).
2. **Overlays outside the window on desktops.** A menu near a small
   window's edge is shifted to fit inside it here; AppKit's, Win32's and
   GTK's menus may leave the window. A child window per overlay (NSPanel,
   a `WS_POPUP`, a GtkPopover without its chrome) is possible but its
   own work: Wayland can't place one freely.
3. **iOS keyboard focus.** UIKit's focus system reaches custom views
   with a hardware keyboard (`canBecomeFocused`, iPadOS 15), and Full
   Keyboard Access on iPhone. Whether `focusable` turns both on, and the
   system's focus effect off when the view styles `focusVisible`.
4. **Namespaced parts.** `Select.Trigger` would need a component's
   namespace in the language; flat names until it's clear it's worth it.
5. **Animations of a part's size** (an accordion's panel opening): its
   height animated in `animate { }` with `clip`, or a measured-height
   variable the part exposes (Base UI's `--accordion-panel-height`).

## Milestones

1. **The language.** Declared variants (checker, lowering, a layer with
   a component's condition) and bindable props. Tests on the VM and in
   C. Useful at once for any app's components.
2. **Focus.** `focusable`, `focused`/`focusVisible`, `Ref.focus()` on
   views, `FocusScope`, on the fake host first, then iOS and Android,
   then macOS, Windows, Linux and the web.
3. **Accessibility roles and states.** `Role`'s cases and the new keys,
   every host, read back in the e2e suites.
4. **Overlay.** `ak_place`, the element, dismissal and stacking, every
   host's layer; conformance for placement.
5. **`std/base`, first group.** `Button`, `Toggle`, `Checkbox`,
   `Switch`, `RadioGroup`, `Collapsible`, `Tabs`, `Dialog`, `Popover`,
   `Tooltip`, `Menu`, `Select`; docs (components.md's "Your own look",
   accessibility.md, the zh and ja translations); an example
   (examples/base) styled two ways.
6. **The rest.** `Combobox`, `Listbox`, `Slider`, `NumberField`,
   `Field`/`Form`, `Accordion`, `ContextMenu`, `Menubar`, `PreviewCard`,
   `ScrollArea`, `Toast`, `Avatar`, `Meter`, `Toolbar`.
7. **Styled components** (their own RFC).
