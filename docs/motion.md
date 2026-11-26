# Motion and gestures

Asuka animates with each platform's own machinery where it has one:
UIView animations on iOS, animators on Android, AppKit's implicit
animations and Core Animation on macOS, the Web Animations API in a
browser. Windows and GTK have no animations of a view's frame, so on
Windows and Linux Asuka steps them itself, each frame, with the same
curves. You change state inside `animate`, and what that change moves
on screen animates. This page covers animating changes, transitions in
and out, shared elements, frame-driven animation, gestures with touch,
mouse and trackpad, haptics and Reduce Motion.

## Animate a change

Wrap the write in `animate`. Whatever the write changes on screen then
animates: frames that layout moves, and the visual props listed below.

```asuka
component Details {
  state expanded = false

  Column {
    gap: 12
    padding: 20
    Button("More") {
      on press { animate(() => { expanded = !expanded }) }
    }
    View {
      height: if expanded { 240 } else { 80 }
      radius: 16
      background: .secondaryBackground
    }
  }
}
```

The box grows, and everything below it slides down with it: layout runs
once, and each frame that changed animates to its new place. You don't
animate positions yourself.

The same write without `animate` is applied at once. `animate` runs on
the main thread, as every state write does (a `spawn`'s worker hands
its result back first). It returns at once; `await animate(…)` waits
until the animation has ended.

## Duration, curve and repeat

`animate(Animation(…), body)` takes the animation's timing:

- `duration` is a `Duration` (`350ms`, the default; `0s` means 250ms).
- `curve` is `.easeInOut` (the default), `.linear`, `.easeIn`,
  `.easeOut` or `.spring`. A spring is a damped spring that settles
  by the end of `duration`: UIKit's spring animation on iOS, the same
  spring's interpolator on Android, Windows, Linux and the web.
  `Animation.spring`, `.linear` and `.quick` (200ms) are ready made.
- `damping` is a spring's damping ratio: 1 settles without passing its
  end, less overshoots and comes back (0 means 0.85, a little).
  `velocity` is its speed at the start, as a fraction of the change per
  second (UIKit's `initialSpringVelocity`), to carry on a fling.
- `delay` waits that long before it starts.
- `done` is called once the animation is logically complete (after
  `delay` and `duration`, each repeat), as `await animate(…)` returns:
  chain the next step from it.
- On macOS, AppKit's implicit animations have neither springs nor
  delays: a bouncy spring overshoots on a curve, and a delay is skipped.
- `repeat` runs it again this many times more; `Animation.forever`
  repeats it until the props change again. With `autoreverses`, each
  run goes back before the next. macOS and Windows don't repeat: the
  animation runs once.

```asuka
component Pulse {
  state on = false

  Row {
    gap: 16
    View {
      width: 24
      height: 24
      radius: 12
      background: .red
      transform: if on { Transform(scale: 1.6) } else { Transform() }
      opacity: if on { 0.35 } else { 1 }
    }
    Button("Pulse") {
      on press {
        animate(Animation(duration: 800ms, curve: .easeInOut, repeat: Animation.forever, autoreverses: true), () => { on = true })
      }
    }
    // Set without an animation: the pulse stops.
    Button("Stop") { on press { on = false } }
  }
}
```

A gesture's end often hands over to a spring. Its velocity, divided by
the distance left to go, is the spring's `velocity`, so the view keeps
the speed the finger gave it:

```asuka
import std/math

component Flung {
  state dragX = 0.0
  state dragY = 0.0

  View {
    width: 64
    height: 64
    radius: 32
    background: .accent
    transform: Transform(x: dragX, y: dragY)
    on pan(g) {
      if g.state != .ended {
        // Follows the finger.
        dragX = g.translationX
        dragY = g.translationY
        return
      }
      let d = math.hypot(g.translationX, g.translationY)
      // The speed toward home, along the way back.
      let v = if d > 1 { -(g.velocityX * g.translationX + g.velocityY * g.translationY) / d / d } else { 0.0 }
      animate(Animation(duration: 600ms, curve: .spring, damping: 0.55, velocity: v), () => {
        dragX = 0
        dragY = 0
      })
    }
  }
}
```

Steps that follow each other chain by awaiting each (or through
`done`); a toast that comes in, stays, and goes:

```asuka
component Toast {
  state shown = false

  async fn show() {
    await animate(() => { shown = true })
    await animate(Animation(delay: 1500ms), () => { shown = false })
  }

  Column {
    Button("Save") { on press { await show() } }
    if shown {
      Text("Saved") { transition: .slide }
    }
  }
}
```

A repeating animation stops when its props are set again without an
animation. On Android, only opacity and transforms repeat; on iOS,
Linux and the web, everything that animates does; macOS and Windows run
it once.

## What animates

| Change | iOS | Android | macOS | Windows | Linux | Web |
| --- | --- | --- | --- | --- | --- | --- |
| Frames (position and size, from layout) | yes | yes | yes | yes | yes | yes |
| `opacity` | yes | yes | yes | no | yes | yes |
| `transform` | yes | yes | yes | yes | yes | yes |
| `background` (a color) | yes | yes | no | no | no | yes |
| `radius` | yes | yes | yes | no | no | no |
| A `Shape`'s `trimStart` and `trimEnd` | yes | yes | yes | no | yes | no |
| A `Shape`'s path | yes | no | yes | no | no | no |

Other props (`borderColor`, `shadow`, `gradient`, text color and fonts)
change at once. A view only animates if it's on screen; views that
appear during `animate` don't animate in, unless they have a
[transition](#enter-and-exit).

A view faded to `opacity: 0` is still laid out. To fade a view out of
the layout, remove it with a `Transition.opacity` transition.

## Transforms

`transform` moves, scales and rotates a view around its center as
it's drawn. Layout doesn't see it: its neighbors stay where they are.
A `Transform` has:

- `x`, `y`: an offset, in points.
- `scale`: both axes (1, the default, is its size); `scaleX`, `scaleY`
  scale one axis.
- `rotation`: degrees, clockwise.

```asuka
component Flip {
  state flipped = false

  View {
    height: 120
    radius: 20
    background: .accent
    transform: if flipped { Transform(scale: 1.04, rotation: 6) } else { Transform() }
    on press { animate(Animation(duration: 500ms, curve: .spring), () => { flipped = !flipped }) }
  }
}
```

## Enter and exit

`transition` is the state a view comes in from when it's added during
`animate` (an `if` turning true, an item added to a `for`), and goes out
to when it's removed during one. A removed view stays on screen until
it has gone out, and takes no touches meanwhile.

- `.opacity` fades in and out.
- `.pop` fades and grows in from 90% (a popover, a badge).
- `.slide` fades in from 24 points below (a toast, a new row).
- `Transition(…)` is your own: `fade`, `scale` (0: none) and an offset
  `x`, `y` in points. They combine: `Transition(fade: true, scale: 0.5,
  y: -8)`.

```asuka
import platform/accessibility

component SaveToast {
  state shown = false

  async fn save() {
    animate(() => { shown = true })
    accessibility.announce("Saved")
    await sleep(1500ms)
    animate(() => { shown = false })
  }

  View {
    grow: 1
    Button("Save") { on press { await save() } }
    if shown {
      View {
        absolute: true
        left: 20
        right: 20
        bottom: 20
        background: .secondaryBackground
        radius: 16
        padding: [14, 18]
        transition: .slide
        Text("Saved")
      }
    }
  }
}
```

Transitions run only inside `animate`, with its duration and curve.
The same `shown = true` without it shows the toast at once. A view
only transitions in if its parent is already on screen.

## Shared elements

`shared` is a key a view shares with another, as SwiftUI's
`matchedGeometryEffect`. When, during `animate`, a view with the key
goes and another with it comes, the new one grows from the old one's
place and size into its own, while the old one moves along with it and
fades. Here a row opens into a card, its artwork and title moving with
it (from [`examples/gallery`](../examples/gallery)):

```asuka
component Matched {
  state open = false

  fn toggle() { animate(Animation(duration: 500ms, curve: .spring), () => { open = !open }) }

  if open {
    View {
      gap: 12
      align: .center
      on press { toggle() }
      Art(240, 24)
      Text("Night Drive") {
        font: .title2
        shared: "album-title"
      }
    }
  } else {
    Row {
      gap: 12
      align: .center
      on press { toggle() }
      Art(56, 10)
      Text("Night Drive") {
        font: .headline
        shared: "album-title"
      }
    }
  }
}

component Art(size: float, corner: float) {
  View {
    width: size
    height: size
    radius: corner
    background: .orange
    shared: "album"
  }
}
```

- Every platform runs it, desktops and the web included.
- Both views must change in the same `animate`: one removed, one
  added.
- Keys belong to the root: one view at a time has each.
- A view that goes with a key and has no partner fades out.

## Zoom into a pushed screen

A screen's `zoomFrom` names the `shared` key of a view on the screen
below. The pushed screen grows out of that view, and shrinks back into
it when it's popped. It works for a `List`'s cells, as in this grid of
photos:

```asuka
component Photos {
  let ids = [10, 11, 12, 13, 14, 15]

  List {
    columns: 3
    gap: 2
    for id in ids key id {
      View {
        label: "Photo"
        aspectRatio: 1
        shared: "photo-{id}"
        on press { push(Photo(id)) }
        Image("https://picsum.photos/id/{id}/300/300") {
          mode: .fill
          grow: 1
        }
      }
    }
  }
}

screen Photo(id: int) {
  title: "Photo"
  zoomFrom: "photo-{id}"
  Image("https://picsum.photos/id/{id}/1200/1200") {
    mode: .fit
    grow: 1
  }
}
```

- **iOS 18 and later** use UIKit's zoom transition; the interactive
  pop gesture shrinks the screen back into the view. Earlier versions
  push as usual.
- **Android** runs Material's container transform: the screen grows
  from the view into its bounds, rounded, the two cross-fading near the
  view. A predictive back gesture shrinks it back.
- **macOS, Windows, Linux and the web** push as usual.
- If no view with the key is on the screen below, it pushes as usual.

You don't need `animate` for this: pushes animate on their own. See
[navigation](navigation.md).

## Shapes that draw themselves

A `Shape`'s `trimStart` and `trimEnd` are the part of its outline
drawn, from 0 to 1. Change them inside `animate` and the stroke
draws itself: a progress ring, a check mark.

```asuka
import std/math

fn ring(w: float, h: float) -> Path {
  let r = min(w, h) / 2 - 4
  return Path().arc(w / 2, h / 2, r, start: -math.pi / 2, end: 3 * math.pi / 2)
}

component Ring {
  state done = 0.3

  Column {
    gap: 12
    Shape(ring) {
      stroke: .accent
      lineWidth: 8
      lineCap: .round
      trimEnd: done
      width: 72
      height: 72
    }
    Button("Finish") {
      on press { animate(Animation(duration: 800ms), () => { done = 1 }) }
    }
  }
}
```

## Frame by frame

When the motion is computed rather than declared (physics, a game, a
gauge following a value), `frames()` is the frames the display draws,
60 or 120 times a second, as a stream of their times: a task reads it
with `for await`. What the loop writes shows in that frame. It runs
until the task is cancelled, as the component goes. While nothing
reads it, no frames are asked for. (`onFrame(fn)` calls a function
at each frame instead, until the closure it returns is called.)

```asuka
component Spinner {
  state angle = 0.0

  task {
    var last: Duration? = none
    for await t in frames() {
      if let l = last {
        if !environment().reduceMotion {
          angle = (angle + (t - l).seconds() * 180) % 360 // half a turn a second
        }
      }
      last = t
    }
  }

  View {
    a11yHidden: true
    transform: Transform(rotation: angle)
    Symbol("gear") {
      font: .largeTitle
      color: .accent
    }
  }
}
```

Base the motion on the time between frames, not on the count of
frames: the rate differs between devices.

## Gestures

A `View` takes gestures, recognized by the platform's recognizers:

- `on press` and `on longPress`: a tap, and a touch held.
- `on pan(g)`: a drag.
- `on pinch(g)`: two fingers spreading or closing.
- `on rotate(g)`: two fingers turning.
- `on swipe(direction)`: a quick flick, with its `SwipeDirection`:
  `.left`, `.right`, `.up` or `.down`.

With a mouse or a trackpad, the same handlers get what the desktop
does for each:

| Gesture | macOS | Windows | Linux | Web |
| --- | --- | --- | --- | --- |
| `press` | a click | a click, or Enter or Space on a focused view | a click, or Enter or Space on a focused view | a click, or Enter or Space on a focused view |
| `longPress` | a secondary click, or the button held | a right-click, or the button held | a secondary click, or the button held | a right-click, or the button or a touch held |
| `pan` | a drag with the button down | a drag with the button down | a drag with the button down, a touch or the touchpad | a drag with the pointer down |
| `swipe` | a drag that ends fast; a trackpad's two-finger flick; a three-finger swipe, where the system leaves it to apps | a drag that ends fast | a drag that ends fast; a touchscreen's flick | a drag that ends fast |
| `pinch` | a trackpad's pinch | a touchpad's pinch, Ctrl and the wheel, or two fingers on a touch screen | a touchpad's or touch screen's pinch, or Ctrl and the wheel | a trackpad's pinch, Ctrl and the wheel, or two fingers on a touch screen |
| `rotate` | a trackpad's rotation | two fingers turning on a touch screen | a touchpad's or touch screen's rotation | two fingers turning on a touch screen |

Few people hold a mouse button down to see what it does, so give what
`on longPress` does another way in too where it matters: a menu item, a
button. On Windows and Linux, Ctrl and the wheel pinch 10% a notch, and
the pinch ends once the wheel rests. A two-finger flick on a Mac's
trackpad still scrolls what's around the view: it swipes up or down only
where nothing around it scrolls that way.

Pan, pinch and rotate run their handler with a `Gesture` as it goes:

- `state`: `.began`, `.changed`, then `.ended`, or `.cancelled` if the
  system took the touch away.
- `x`, `y`: where the touch is, in the view, in points.
- `translationX`, `translationY`: how far a pan has moved since the
  finger came down, so `x - translationX` is where it came down.
- `velocityX`, `velocityY`: a pan's speed, in points per second.
- `scale`: a pinch's scale since it began (1 for a pan).
- `rotation`: a rotation's angle since it began, in radians, clockwise.

A dragged view follows the finger through its transform, and springs
back when it's let go:

```asuka
component Puck {
  state dragX = 0.0
  state dragY = 0.0

  View {
    width: 64
    height: 64
    radius: 32
    background: .accent
    transform: Transform(x: dragX, y: dragY)
    on pan(g) {
      if g.state == .ended || g.state == .cancelled {
        animate(Animation(duration: 450ms, curve: .spring), () => {
          dragX = 0
          dragY = 0
        })
        return
      }
      dragX = g.translationX
      dragY = g.translationY
    }
  }
}
```

Scale and rotation are since the gesture began, so keep where the last
one left off to add the next to it. Gestures on the same view work
together: pinch and turn at once.

```asuka
import std/math

component Turner {
  state angle = 0.0
  state base = 0.0
  state scale = 1.0
  state baseScale = 1.0

  View {
    label: "A square to turn"
    width: 160
    height: 160
    radius: 24
    background: .purple
    transform: Transform(scale: scale, rotation: angle)
    on rotate(g) {
      angle = base + g.rotation * 180 / math.pi
      if g.state == .ended || g.state == .cancelled { base = angle }
    }
    on pinch(g) {
      scale = min(max(baseScale * g.scale, 0.5), 2)
      if g.state == .ended || g.state == .cancelled { baseScale = scale }
    }
  }
}
```

Once a pan, pinch or rotation has begun, it has the touch: a scroll
view around it doesn't take it over, and a pressable view inside it
gets no press, as a drag that starts on a button doesn't press it.

A swipe or a pan can't be done with a screen reader on. Offer what it
does as an [accessibility action](accessibility.md) too.

## Pressed and hover states

A `View` reports its interaction states as they change, and a style
can follow them in three ways. Start with the first one that does the
job:

1. **Variants**, for the view's own look. The blocks `pressed { … }`,
   `hovered { … }`, `dragging { … }` and `dragOver { … }` in its block
   are laid over the rest of its style while the view is in that state.
   They are CSS's `:active` and `:hover`, or Tailwind's `active:` and
   `hover:`.
2. **A `Ref`**, for other views and anything else that follows the
   state. Give the view one (`ref: card`), then read `card.pressed`
   or `card.hovered` wherever you need them: another view's style, a
   `Text`, an `if`. This is how a child follows its parent (a group's
   hover) and a sibling follows its neighbor (a peer's).
3. **Handlers**, for what isn't a style, such as starting a recording:
   `on pressedChange(on)` and `on hoverChange(on)`.

```asuka
component Card(title: string) {
  event open

  let card = Ref()

  Row {
    ref: card
    padding: 16
    radius: 12
    background: .secondaryGroupedBackground
    on press { emit open }
    hovered { background: .secondaryBackground }
    pressed {
      background: .separator
      transform: Transform(scale: 0.97)
    }
    // The card's hover, followed by its title: a group's hover.
    Text(title) {
      grow: 1
      color: if card.hovered { .accent } else { .label }
    }
  }
}
```

The states:

- **Pressed** is true while a finger, or the mouse's primary button, is
  down on the view.
  - In a scrolling container it comes on a moment late, so a scroll
    doesn't flash it.
  - It goes off when the finger lifts, moves off the view (moving back
    on presses it again), or a scroll, a long press or another gesture
    takes over the touch.
  - `on press` runs only for a release while pressed.
- **Hovered** is true while a pointer is over the view or its children:
  a mouse, a trackpad, an iPad's pointer, or a hovering Apple Pencil or
  stylus. A touch screen alone never hovers.
- **Dragging** is true while the view is being dragged (its
  `dragItems`).
- **DragOver** is true while a drag that its `dropTypes` take is over
  it.

How variants combine:

- A variant sets only the keys it names. The rest stay as the style
  has them.
- Variants apply in the order hovered, pressed, dragging, dragOver, so
  pressed wins over hovered.
- A key can follow a state itself, as any binding can: `hidden:
  !row.hovered` shows a view only while its row is hovered.

```asuka
component Person(name: string) {
  event remove

  let row = Ref()

  Row {
    ref: row
    Text(name) { grow: 1 }
    // The delete button shows while the row is hovered.
    View {
      hidden: !row.hovered
      label: "Delete"
      on press { emit remove }
      Symbol("trash")
    }
  }
}
```

A style follows any state the same way: `background: if selected {
.accent } else { .clear }`.

A view tracks only the states that something reads: its variants, or
reads of its `Ref`'s.

- A view with no `pressed` variant and no reader of `pressed` doesn't
  report its pressed state, and keeps the platform's highlight.
- Once a view reports its pressed state, it draws its own. The
  platform's highlight (Android's ripple, Windows' hover and pressed
  fills) is left off.

Each state is reported once per change. A view that goes away while in
a state leaves it, so nothing that follows it stays stuck.

Without `on press`, a press held is a hold, as in hold to record. The
recording is a side effect, so it goes in the handler:

```asuka
import platform/record

component HoldToRecord {
  View {
    label: "Hold to record"
    padding: 16
    pressed { background: .red }
    on pressedChange(on) {
      if on {
        try? await record.audio(maxDuration: 60s)
      } else {
        record.stopAudio()
      }
    }
    Symbol("mic")
  }
}
```

## Drag and drop

`dragItems` makes a view draggable. On a touch screen, a long press
then a move lifts it, and that long press doesn't also run `on
longPress`. With a mouse, a move with the button held drags it. The
view's image follows the finger or pointer, within the app and into
other apps. `dropTypes` makes a view take drops.

A `DragItem` is a text, a link (`kind: .link`), a file (`path`), or a
value of the app's (`value`, a string: an id):

- Inside the app, a drop gets each item as it is, value and all.
- Other apps get the item's text, link or file. An item that's only a
  value goes nowhere else.
- `mime` is the item's type, which targets accept it by. Texts, links
  and files get theirs filled in. Give a `value` a type of the app's
  own, so only its targets take it.

```asuka
struct Task {
  id: string
  title: string
  done: bool = false
}

component TaskCard(t: Task) {
  View {
    dragItems: [DragItem(value: t.id, mime: "application/x-myapp-task", text: t.title)]
    padding: 12
    // Faded while it's dragged.
    dragging { opacity: 0.4 }
    Text(t.title)
  }
}

component DoneColumn {
  prop children: View
  event move(id: string)

  View {
    dropTypes: ["application/x-myapp-task"]
    gap: 8
    padding: 8
    border: 2
    borderColor: .clear
    // Outlined while a task is dragged over it.
    dragOver { borderColor: .accent }
    on drop(d) {
      for it in d.items { emit move(it.value) }
    }
    children
  }
}
```

The drag source:

- `dragItems` is a binding, so what it reads keeps it up to date. A
  drag carries what the items were when it began.
- `on dragPhase(phase)` reports `.began` as the view lifts. Then it
  reports either `.dropped`, when a target took it (for a move, remove
  what was dragged), or `.cancelled`. For the dragged look, use the
  `dragging` variant or its `Ref`'s `dragging`.

The drop target:

- `dropTypes` lists MIME types: exact (`"text/plain"`), a family
  (`"image/*"`) or anything (`"*/*"`).
- A drag with no item of those types never comes over the view.
- Of nested targets, the innermost one that takes a drag gets it.
- `on dropOver(over)` reports a drag coming over the view and leaving
  it, the same state as the `dragOver` variant.
- `on dropMove(x, y)` reports where the drag is over the view, as an
  insertion point.
- `on drop(d)` gets the items it takes and where they were dropped.
  `d.local` says the drag came from the app.

Drops from other apps (files from Finder or Files, a photo from Photos,
a link from a browser) come once their items are loaded:

- On iOS and Android, files are copied into the app's caches first and
  `d.copied` is true. Move them where the app keeps them, or remove
  them when done.
- On macOS, Windows and Linux, `path` is the user's file itself. Don't
  remove it.
- In a browser, a dropped file is copied into the app's files (the
  page's, in memory) and `d.copied` is true.

## Keys

A view takes keys while the keyboard's focus is in it, the view or one
inside it, with `keys`: `Shortcut`s, each a key and what it does.
A key goes to the nearest view that has it, from the focused one out,
before the focused control and the menu bar, so a search field's Up and
Down can move through the results under it:

```asuka
component Search {
  prop results: [string] = []
  event open(result: string)
  state query = ""
  state sel = 0

  fn move(by: int) { sel = max(0, min(sel + by, results.count() - 1)) }

  TextField {
    bind text: query
    keys: [Shortcut(key: "down", run: () => move(1)), Shortcut(key: "up", run: () => move(-1))]
    on submit {
      if sel < results.count() { emit open(results[sel]) }
    }
  }
}
```

A key is a character or a named key (`enter`, `escape`, `tab`, `space`,
`delete`, `backspace`, `up`, `down`, `left`, `right`, `home`, `end`,
`pageup`, `pagedown`, `f1` to `f12`, and `playpause`, a TV remote's
Play/Pause: see [TVs](tv.md#the-remote)), with exactly the modifiers given:
`Shortcut(key: "d", primary: true, run: …)`, `Shortcut(key: "up", alt:
true, run: …)`. Held, it repeats. A
character with no modifiers takes typing from a field inside the view,
so give it one, or put it where no field is.

`useKeys([…])` in a component's body gives the window keys of its own:
a key reaches them last, and first when nothing has the focus, as a
game's arrows or a viewer's Space. They go with the component that
called it.

Keys come from a hardware keyboard: a desktop's, an iPad's or a
Chromebook's, a browser's. Never make one the only way to do
something: a phone usually has none. An input method composing text (a
Japanese or Chinese keyboard's) keeps its keys. `examples/launcher`
moves through its results with Up and Down.

## Haptics

`device.haptic` plays feedback: `.selection` (a value ticked over),
`.light`, `.medium` and `.heavy` (impacts), and `.success`, `.warning`
and `.error` (an outcome).

```asuka
import platform/device

component Pager {
  state page = 0

  View {
    grow: 1
    on swipe(d) {
      if d == .left {
        page += 1
      } else if d == .right {
        page = max(page - 1, 0)
      }
      device.haptic(.selection)
    }
    on longPress { device.haptic(.medium) }
    Text("Page {page + 1}")
  }
}
```

On iOS these are `UIFeedbackGenerator`'s. Android has no exact
equivalents: Asuka plays the closest of `performHapticFeedback`'s
constants, and success, warning and error are confirm and reject from
Android 11; before that, a key press or a long press.

On a Mac with a Force Touch trackpad, `.selection` is
`NSHapticFeedbackManager`'s alignment and the others its level change;
the trackpad plays them while a finger is on it. Windows and Linux have
no haptics: `device.haptic` does nothing there. In a browser it's a short
vibration where the browser has `navigator.vibrate` (Chrome on
Android), and nothing elsewhere.

## Reduce Motion

`environment().reduceMotion` is true when the user asked for less
motion. What reads it follows the user changing the setting:

- **iOS**: Settings, Accessibility, Motion, Reduce Motion.
- **Android**: animations turned off (Developer options, or
  Accessibility, Remove animations).
- **macOS**: System Settings, Accessibility, Display, Reduce motion.
- **Windows**: Settings, Accessibility, Visual effects, Animation
  effects off.
- **Linux**: GNOME's animations off (Settings, Accessibility, Reduce
  Animation; GTK's `gtk-enable-animations`).
- **The web**: the browser's `prefers-reduced-motion`, which follows
  the system's setting.

What Asuka does about it on its own:

- **iOS**: transitions dissolve in place instead of moving or scaling,
  as UIKit's own do then, and shared elements (`shared`) don't fly: the
  views come and go with their transitions. Other animations run.
- **Android**: the system's animator scale is 0, so every animation,
  Asuka's and the platform's, ends at once.
- **macOS, Windows, Linux and the web**: `animate`'s changes apply at
  once, transitions and shared elements included. A stack pushes and
  pops without sliding on macOS, cross-fades in a browser, and on Linux
  does as libadwaita's navigation does with animations off.

What's left to you is motion Asuka can't tell is decorative: a
`frames()` loop (as the spinner above stops), a repeating pulse, an
animation that moves something far. Read `reduceMotion` and do less.
