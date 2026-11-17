# RFC 0010: what each host implements

The wire additions RFC 0010 (headless components) made, and what every
host does with them. ops/schema.go is the source of truth for names and
types; the decoders are generated (`go generate ./ops`) and committed.
The core and the fake host (core/host/fake) already implement their
side: read core/src/ui/focus.c, core/src/ui/floating.c,
core/src/ui/place.c, and the fake host's `ak_fake_focus`, `ak_fake_tab`,
`report_anchors` and `ak_fake_outside_press` (core/host/fake/fake.c) for
the reference behavior. lang/compile/testdata/ui/overlay.asuka,
focusstates.asuka and a11ystates.asuka show what apps do with them.

## Common props (every view)

| Prop | Type | What the host does |
|---|---|---|
| `Focusable` | bool | The view takes the keyboard's focus: a Tab stop in the views' order where a keyboard moves focus (desktops, the web, a hardware keyboard on iOS/iPadOS and Android), and a primary press with a mouse or trackpad focuses it (desktops, the web) unless `NoFocusOnPress`. Pressable views (they listen for Press) and adjustable ones (`A11yAdjustable`) already were tab stops on Windows, Linux and the web; they stay so, and now on every host. |
| `NoFocusOnPress` | bool | A pointer's press doesn't move the focus to it. |
| `NoFocusRing` | bool | The host draws no focus indicator of its own around it (no CSS outline, no Fluent focus rect, no UIFocusEffect / UIFocusHaloEffect, no Android `defaultFocusHighlightEnabled`, no AppKit focus ring, no GTK focus outline). The app draws its own. |
| `FocusTrap` | bool | While the focus is inside the view, Tab and Shift+Tab (and a hardware keyboard's focus movement) cycle among the focusable views inside it and never leave it. |
| `A11yChecked` | enum A11yChecked: None, Unchecked, Checked, Mixed | A checkable view's state. None with a checkable role (Checkbox, Radio, Switch, MenuItemCheckbox, MenuItemRadio) reads as unchecked. |
| `A11yExpanded` | enum A11yExpanded: None, Collapsed, Expanded | Whether what the view controls shows. None: not expandable. |
| `A11ySelected` | bool | Selected among its group (a tab, an option). |
| `A11yValue` | string | What screen readers read as its value ("40%"). Empty: none. |
| `A11yRange` | bytes | Empty: none; else f64 min, f64 max, f64 now, little endian (ops.DecodeRange). |
| `A11yModal` | bool | Screen readers stay inside it; the rest of its window is hidden from them while it shows. |
| `A11yControls`, `A11yLabelledBy`, `A11yDescribedBy` | int | A node ID (0: none) this one controls / is labelled by / is described by. Use the platform's relation where it has one; where it hasn't, a labelledBy's text may serve as the label and a describedBy's as the hint (iOS, Android). A relation to a view not made yet arrives later as another SetProp. |

`A11yRole` gained cases (enum Role, in this order after Search):
Checkbox, Radio, RadioGroup, Switch, Tab, TabList, TabPanel, Slider,
SpinButton, ProgressBar, Meter, Menu, MenuBar, MenuItem,
MenuItemCheckbox, MenuItemRadio, Listbox, Option, ComboBox, Dialog,
AlertDialog, Tooltip, Group, Separator, Toolbar, Status. Map each to the
platform's closest role: UIAccessibilityTraits (+ accessibilityValue,
iOS 17's `.toggleButton` for checkboxes/switches,
`accessibilityContainerType` for groups/lists/tablists); Android's
AccessibilityNodeInfo className (android.widget.CheckBox, RadioButton,
Switch, SeekBar, ProgressBar, Spinner, TabWidget...), roleDescription,
`setCheckable/setChecked`, `stateDescription`, `RangeInfo`,
`setSelected`, CollectionInfo where it applies; NSAccessibilityRole
(+ subrole) on macOS; UIA control types and patterns (Toggle,
ExpandCollapse, SelectionItem, RangeValue, Value) on Windows;
GtkAccessibleRole and states/properties/relations on Linux; ARIA roles
and aria-* on the web.

## Events

| Event | Payload | When |
|---|---|---|
| `FocusChange` | int | Sent without being listened for, for every view that takes or loses the keyboard's focus except TextFields (which keep Focus and Blur): 0 lost it, 1 took it from a pointer's press or programmatically, 2 took it from the keyboard (Tab, arrows, or Focus right after a key: the platform's or CSS's :focus-visible heuristics). |
| `KeyText` | string | Listened for. Text typed while the view has the focus that no Keys entry (its own or a view's around it) took, and that isn't Enter or Space pressing it: a listbox's typeahead. |
| `PressPoint` | point | Listened for. Where a press began on the view, in points from its top left, sent just before Pressed(true) or Press. |
| `OutsidePress` | point | To an Overlay (it listens). A press (finger, pen, mouse button) began in the overlay's root but on no view of the overlay: where, in the root's coordinates. |
| `AnchorFrame` | bytes | To an Overlay (it listens): f32 x, y, w, h of its Anchor view in its Root's coordinates, then u8 1 if any of the anchor is visible (in the window, not hidden, not clipped/scrolled out of view by a view around it), else 0 (ops.AppendAnchorFrame). Sent once the overlay is made and its anchor is set, and again each time it changes: layout, a scroll of any scroll view around the anchor, a window resize, the anchor moving or going. Throttle to once per frame at most; never send an unchanged one. |

## Commands

- `Focus` / `Blur` sent to any view that can take the focus (a
  Focusable or pressable container, a control, a field): give it the
  keyboard's focus / take it away. Focus that comes right after a key
  press is visible (FocusChange 2), else 1.

## The Overlay kind

A new kind of view, made by the core with `Create`:

- `Root` (int): the layout root it goes over: the root view (main
  window), a Window's view, a Sheet's view, or a Popover's view. The
  host adds the overlay's view over everything else in that root's
  view hierarchy (on iOS over the presented sheet's view controller's
  view when Root is a sheet; on Android the Dialog's window for a sheet),
  sized to that root's bounds. Its frame is that root's whole area; its
  children's frames (the core's SetFrames) are in that root's
  coordinates. A host never mirrors them.
- It's a host-sized provider: report `Environment` for it (as for a
  sheet or a popover: its size, the root's safe-area insets, the
  keyboard) once it's made and whenever that changes.
- `Present`: bring it over the root's other overlays (z-order: the last
  presented on top). It's shown from Create until Destroy; the core
  sends Hidden on it while it waits for the first AnchorFrame.
- `Anchor` (int): a node ID (0: none). Report AnchorFrame for it (see
  above). Watch for changes: after each frames batch is applied, on
  scroll of any ancestor scroll view of the anchor, on resize.
- `Modal` (bool): while true, presses anywhere in the root reach only
  the overlay's views: one that lands on no child of the overlay is
  taken by the overlay's own view (and reported as OutsidePress); what's
  behind gets nothing (no press, hover, scroll). Screen readers see only
  the overlay (iOS accessibilityViewIsModal on the overlay, Android
  importantForAccessibility noHideDescendants on the root's other
  content, AppKit/UIA/AT-SPI/web: the rest hidden or inert). While false
  (the default), the overlay's own view takes no presses at all (hit
  testing passes through it to what's under it; only its children take
  presses), and a press that begins outside every child of the overlay
  is still reported to it as OutsidePress (iOS: a window-level gesture
  recognizer that doesn't cancel touches; Android: the root's
  dispatchTouchEvent; AppKit: a local event monitor; Win32: the message
  loop; GTK: a capture-phase GtkGestureClick on the window; web: a
  capture-phase pointerdown on the document). Report it before the
  press reaches its target.
- `InterceptBack` (bool, a common prop): while true, the host's going
  back (Android's Back and predictive back, the web's nothing) comes to
  the topmost overlay with it as a `Back` event, before screens and
  sheets.
- Escape needs nothing from the host: the core registers it as one of
  the window's keys (the root's Keys prop) while an overlay that closes
  on Escape is open.
- Destroy removes it. Content the core removes inside animate { } plays
  its Transition in the overlay, as anywhere else.

## Tests every host adds

Unit or automation tests in the host's own suite, following its
conventions: decoding the new props (golden files already updated by
`go test ./ops -update` — run the host's decoder tests), the Overlay's
placement over its root and z-order, AnchorFrame reporting (including
after a scroll), outside presses (modal and not), FocusChange values,
Tab within FocusTrap, NoFocusRing, and that each A11y prop reaches the
platform's accessibility object (read back through the platform's
automation API where the host's tests do).
