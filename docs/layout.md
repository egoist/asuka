# Layout and style

An element's style props say how it's laid out and how it looks. Layout
is CSS flexbox and grid, in points; the look is colors, corners,
borders, shadows and text. This page covers both, and styles that
change. The layout is the same on every platform: Asuka computes
it and places the platform's views where it says.

## Platform defaults

A style you don't set keeps the platform's default. An app that sets no
style looks as the platform's own apps do, with their fonts, colors,
dark mode and text sizes:

- **iOS**: UIKit's.
- **Android**: Material 3's.
- **macOS**: AppKit's.
- **Windows**: Windows 11's (WinUI's Fluent look, which Asuka draws
  on Win32's controls).
- **Linux**: GNOME's (GTK 4 and libadwaita).
- **The web**: the browser's controls, with a light and dark palette of
  Asuka's own around them (see [the web](web.md)).

Set only what you mean to change, and prefer the semantic colors and
fonts below to fixed ones, so the app keeps following the platform.

## Style

Every element takes the style props in its block, after its own:

```asuka
Text(title) { font: .headline; color: .secondaryLabel }
Button("Follow") { shrink: 0 }
View { height: 1; background: .separator }
```

A `style` declares props to reuse, and `style:` applies one or a list
of them. A later entry wins a prop both set, and the block's own props
win over the list:

```asuka
style card {
  padding: 16; radius: 12; gap: 8
  background: .secondaryBackground
}

style chip(tint: Color) {
  padding: [4, 10]; radius: 999
  background: tint.opacity(0.15); color: tint
}

View { style: card; background: .accent }
Text("New") { style: chip(.green) }
Row { style: [card, if selected { chip(.accent) }] }
```

A style takes parameters as a function does, applies other styles with
its own `style:`, and an `if` or a `match` in a list applies a style
while it holds, putting each prop back when it stops. A style named
after an element (`style Button { radius: 8 }`) is the default of every
one in the package. For the whole syntax, and themes a part of the app
changes, see [Styles](language/styles.md).

Look props apply where the native widget supports them: a button takes
`color` as its tint, a view ignores `font`.

## Stacking views

`Column` stacks its children top to bottom, `Row` leading to trailing.
`View` is a container whose direction is its `direction`: `.column`, the
default, or `.row`.

```asuka
Row {
  align: .center; gap: 12
  Avatar()
  Column {
    grow: 1; gap: 2
    Text("Ada Lovelace") { font: .headline }
    Text("Notes on the Analytical Engine") { font: .subheadline; color: .secondaryLabel; lines: 1 }
  }
  Button("Follow") { shrink: 0 }
}
```

A container with only layout props makes no native view: its children
are placed in the nearest one that has one. It costs nothing to nest
them.

## Flexbox

Layout follows CSS flexbox with React Native's defaults: column
direction, and sizes that include padding and borders.

- **`gap`**: the space between children, and between lines when they
  wrap.
- **`justify`** places the children along the main axis: `.start` (the
  default), `.center`, `.end`, `.spaceBetween`, `.spaceAround`,
  `.spaceEvenly`.
- **`align`** places them across it: `.stretch` (the default: they fill
  the cross axis), `.start`, `.center`, `.end`, `.baseline`.
- **`alignSelf`** overrides the parent's `align` for one child; `.auto`,
  the default, keeps it.
- **`wrap`** lets children flow onto more lines when they don't fit.
- **`grow`** is a child's share of the space left along its parent's
  main axis. A growing child can also shrink below its content's size,
  so a growing view around a `Scroll` scrolls rather than taking the
  scroll's full height.
- **`shrink`** is how much a child gives up when the line overflows, as
  CSS's `flex-shrink`: 1 when it isn't set (text wraps, controls keep
  their minimum size), 0 keeps its size (a number column, an icon, a
  button beside text that wraps).
- **`basis`** is a child's starting size along the main axis; `.auto`,
  the default, is its content's.

```asuka
// Two boxes sharing the width, one twice the other.
Row {
  gap: 8; height: 60
  View { grow: 2; background: .secondaryBackground }
  View { grow: 1; background: .secondaryBackground }
}

// A label and a value at either end.
Row {
  justify: .spaceBetween; align: .center
  Text("Total")
  Text("$42") { font: .title2 }
}

// Chips that wrap onto more lines.
Row {
  wrap: true; gap: 8
  Text("Asuka"); Text("iOS"); Text("Android")
}
```

## Sizes

Lengths are a `Length`:

- a number: points: iOS's and macOS's points, Android's dp, a pixel at
  100% scaling on Windows (1/96 inch), GTK's logical pixels on Linux,
  and CSS pixels on the web;
- a percentage, `50%`, of the parent's size along the same axis;
- `.auto`, the default: the content's size, or what flexbox gives.

`width` and `height` set the size; `minWidth`, `minHeight`, `maxWidth`
and `maxHeight` bound it. `aspectRatio` is width over height: with one
side known, it sets the other.

```asuka
View {
  width: 50%; minWidth: 120; maxWidth: 320
  aspectRatio: 16.0 / 9
  background: .gray
}
```

Text and controls measure themselves, with the platform's own
measuring: give them a size only to change it.

A point is as many pixels as the display's scale: 2 or 3 on a phone, 2
on a Retina Mac, the display's scaling on Windows and Linux (1.5 at
150%), the browser's device pixel ratio on the web. The environment's
`scale` says which (`environment().scale`). A desktop window moved to a display of another
scale is drawn at that display's scale; its layout, in points, stays as
it was.

## Padding and margin

`padding` and `margin` are `Edges`, per side, in points:

- `16`: 16 on every side;
- `[8, 16]`: 8 at the top and bottom, 16 at the left and right;
- `[4, 8, 4, 8]`: top, right, bottom, left;
- `Edges(top: 24)`: each side by name.

```asuka
Column {
  padding: [8, 16]; margin: Edges(top: 24)
  Text("Padded")
}
```

A `Button`'s `padding` replaces the platform's own, as CSS's does, on
iOS, Android and the web: `minHeight: 48; padding: [10, 20]` is 48
points high there, as the same props on a `View` are. A button without
`padding` keeps the platform's, a border too. On the desktop, a button's
padding is around its native bezel.

In a language written right to left, a `Row` runs from the right,
`justify` and `align` follow it, and the side a line of text starts on
is the right. `left` and `right` stay where they say, as in CSS. To put
a space on the side text starts or ends on, say `start` or `end`:

```asuka
View { padding: Edges(start: 16, end: 8) } // 16 on the left in English, on the right in Arabic
```

See [Languages](i18n.md).

## Absolute positioning

`absolute: true` takes a view out of the flow; `top`, `right`, `bottom`
and `left` place it in its parent. Here a badge sits on the corner of
an avatar:

```asuka
View {
  width: 44; height: 44
  Avatar()
  View {
    absolute: true; top: -4; right: -4
    width: 18; height: 18; radius: 9
    background: .red; align: .center; justify: .center
    Badge()
  }
}
```

Later children are drawn over earlier ones.

## Grid

With `grid` set, a container lays out its children in a CSS grid. Its
tracks are `Track`s: `.fr(1)`, `.points(80)`, `.percent(25)`, `.auto`,
`.minContent`, `.maxContent`, `.minmax(a, b)`, `.repeat(n, [...])`,
`.autoFill([...])`, `.autoFit([...])`. A child spans tracks with
`gridColumn` and `gridRow` (`.line(2)`, `.span(3)`, `.between(1, 3)`).

```asuka
View {
  gap: 8
  grid: Grid(columns: [.repeat(3, [.fr(1)])])
  Avatar(); Avatar(); Avatar(); Avatar()
  View { gridColumn: .span(3); height: 20; background: .gray }
}
```

For a grid of many items that scrolls, use a `List` with `columns` (see
[Components](components.md)).

## The safe area, the keyboard and the window

On a phone, the root fills the screen, under the status bar, the notch
and the home indicator. `SafeArea` is a `Column` padded by the safe
area's insets, on top of its own padding. Its bottom padding is the
keyboard's height while the keyboard is up, so what's at its bottom (a
text field, a send button) stays above it.

```asuka
SafeArea {
  padding: 16; gap: 12
  Text("Todos") { font: .largeTitle.bold() }
  Scroll {
    grow: 1
    for t in todos key t.id { TodoRow(t) }
  }
  Composer()
}
```

The insets are those of the screen it's in: in a stack screen, a tab or
a sheet, that screen's. `environment()` gives them, and the keyboard's
height, for placing things yourself (see
[Concepts](concepts.md#the-environment)).

On macOS, Windows and Linux the root fills the window's content, below
its title bar (Linux's header bar). A desktop window has no safe area
and no on-screen keyboard over it: the insets and the keyboard's height
are 0, and `SafeArea` is a `Column` with its own padding. Use it anyway;
the same screen then fits a phone too.

The window is 800 by 600 points unless the app says otherwise (see
[Navigation](navigation.md)); a phone has none. As the user resizes the
window, the environment's `width` and `height` change and layout runs
again: there's nothing to handle. To change the layout at a size (a
sidebar that shows when there's room), branch on the environment's
`widthClass`, as on a tablet ([below](#tablets-and-foldables)):

```asuka
if environment().widthClass == .expanded {
  Row { grow: 1; Sidebar(); Detail() }
} else {
  ItemList()
}
```

In a browser, an app that fills the page takes the device's safe area
(`env(safe-area-inset-*)`, on a phone with a notch) and the on-screen
keyboard's height from the visual viewport. An app embedded in a part of
a page has neither.

## Tablets and foldables

On a tablet, a foldable or a desktop the window is wide, and the user
changes its size: an iPad's Split View, Slide Over and Stage Manager,
Android's split screen and desktop windows, a foldable opened or
folded. The environment's `width` and `height` follow it, and its
`widthClass` and `heightClass` say what the room is for:

| | Compact | Medium | Expanded |
|---|---|---|---|
| Width | under 600 points: a phone, a narrow split | 600 to 840: a tablet in portrait, a foldable opened | 840 and over: a tablet in landscape, a desktop window |
| Height | under 480: a phone in landscape | 480 to 900 | 900 and over |

Android reports Material's window size classes, as above; iOS reports
UIKit's, compact or regular (as `.expanded`), which an iPad's split
views make compact in their narrow parts. Branch on the class, not on a
width of your own:

```asuka
if environment().widthClass == .compact {
  ItemList()
} else {
  Row { grow: 1; ItemList(); Detail() }
}
```

Navigation adapts on its own: `Tabs` has a navigation rail on Android
from medium, and a sidebar the user can show on an iPad; a `Split` stands
its sidebar beside the content where there's room, and folds into a stack
or a drawer where there isn't ([Navigation](navigation.md#split-views)).
An iPad can show the app's `Window`s beside it
([Navigation](navigation.md#windows-on-an-ipad)).

A foldable says how it's held. `environment().posture` is `.flat` (as
every other device is), `.tabletop` (half open, its fold across the
window: the top half stands up, as a laptop's screen) or `.book` (half
open, its fold down the window: two pages). `environment().fold` is
where the fold or hinge crosses the window, in points from its top left
(a `Frame`; zero when none does): put what's read above it and what's
touched below it on a tabletop, a page each side of it on a book.

```asuka
let env = environment()
if env.posture == .tabletop {
  Column {
    grow: 1
    Column { height: env.fold.y; VideoPlayer() }
    Column { grow: 1; Controls() }
  }
} else {
  VideoPlayer()
}
```

Android reports them from Jetpack WindowManager's folding feature, as
they change; the other platforms are always flat.

## Scrolling

`Scroll` scrolls its children, vertically or, with `horizontal: true`,
sideways. Give it a size, or `grow: 1`, so it has something to scroll
within. Its size props are the scroll view's own box; its `padding`,
`gap`, `justify`, `align` and `wrap` lay out its content.

```asuka
Scroll {
  grow: 1; padding: 16; gap: 12
  Text("Long content")
}
```

Pull to refresh and scroll positions are in
[Components](components.md), and long lists in
[Lists](components.md#lists).

## Colors

A `Color` is one of:

- **The platform's default**: a prop left unset.
- **A semantic color**, resolved by the platform, so it follows dark
  mode:
  - text: `.label`, `.secondaryLabel`, `.tertiaryLabel`;
  - backgrounds: `.background`, `.secondaryBackground`,
    `.groupedBackground`, and `.secondaryGroupedBackground` for a
    grouped list's rows on it (iOS's inset grouped cells);
  - `.separator`, `.accent` (the app's accent), `.onAccent` (what reads
    on it: an icon on a button filled with the accent), `.clear`
    (transparent);
  - the system's hues: `.red`, `.orange`, `.yellow`, `.green`, `.blue`,
    `.purple`, `.pink`, `.gray`.
- **A fixed color**: `#7c3aed`, or `#7c3aed80` with its alpha. A color
  the app only has as it runs (a label's from a server, a theme the user
  saved) is `Color.parse("#7c3aed")` (a `Color?`: none for text that
  isn't one; the `#` may be left out) or `Color.rgb(124, 58, 237, alpha:
  0.5)`, its channels 0 to 255.

Where the semantic colors come from:

| Platform | Semantic colors |
| --- | --- |
| iOS | UIKit's (`labelColor`, `systemBackground`, `systemRed`...) |
| Android | Material 3's color roles |
| macOS | AppKit's (`labelColor`, `windowBackgroundColor`, `controlBackgroundColor`, `separatorColor`, `systemRedColor`...) |
| Windows | Windows 11's theme colors (WinUI's Fluent theme resources), light or dark |
| Linux | libadwaita's named colors (`window_fg_color`, `window_bg_color`, `card_bg_color`, `destructive_color`...), as the theme defines them, high contrast included |
| The web | Asuka's palette, as CSS variables, light and dark |

On the desktops the grouped backgrounds are the plain ones:
`.groupedBackground` is the window's background, and
`.secondaryGroupedBackground` a card's, as `.secondaryBackground` is.

```asuka
Column {
  padding: 16; gap: 4; background: .background; radius: 12
  Text("Title") { font: .headline }
  Text("Details") { font: .subheadline; color: .secondaryLabel }
}
```

`c.opacity(a)` is a color drawn at an opacity from 0 to 1 (SwiftUI's
`.opacity`, Compose's `copy(alpha =)`). A semantic color stays one, so
the accent at `opacity(0.15)` is a tint of it in light and dark alike: a
chip's background, a chart's fill, a track.

```asuka
let tint: Color = .accent
View { padding: [4, 10]; radius: 12; background: tint.opacity(0.15); Text("New") }
```

`color` is the text's color, or a control's tint.

## Backgrounds, borders and shadows

- **`background`**: the view's background color.
- **`radius`**: rounds its corners, in points: one radius for all
  (`radius: 12`; 999 makes a capsule), or each its own:
  `[16, 16, 0, 0]` (top left, top right, bottom right, bottom left, as
  CSS's) or `Corners(topStart: 12, bottomStart: 12)`, whose start and
  end corners follow the text's direction. Radii too large for their
  side are scaled down together, as CSS's are.
- **`border`** and **`borderColor`**: a border, drawn inside the box. It
  insets the content, as padding does. Its width is the same on every
  side (`border: 1`), or each side's own, as `padding` takes them:
  `border: Edges(bottom: 1)`, `border: [0, 0, 1, 0]`. Where a rounded
  corner joins two sides, its inner curve is its radius less their
  widths.
- **`borderStyle`**: `.solid` (the default), `.dashed` (dashes three
  times the border's width, gaps as long) or `.dotted` (round dots as
  wide as the border, a width apart). The web draws CSS's own dashes
  and dots. Where the sides' widths differ, every platform but the web
  draws a dashed or dotted side straight, its corners square.
- **`shadow`**: a drop shadow, cast by the view's shape (its bounds,
  rounded by `radius`), not by what's drawn in it: `Shadow(color:,
  radius:, x:, y:)`, where `radius` is a blur radius (as CSS's and
  SwiftUI's: twice the blur's σ) and `x` and `y` its offset. A view that
  clips its children (`clip`) still casts it, as a rounded card that
  clips its image does. Android draws its elevation shadow instead: the
  radius is the elevation, the color tints it, and the light sets the
  offset. macOS, Windows, Linux and the web draw it as iOS does, with
  the same blur and offset.
- **`gradient`**: a gradient over the background,
  `Gradient(stops: [...])`. Each `Stop` is a color at a location from 0
  to 1. Its `kind`:
  - `.linear` (the default): top to bottom unless `startX`, `startY`,
    `endX` and `endY` say otherwise, in units of the view's size ((0, 0)
    its top left corner, (1, 1) its bottom right one).
  - `.radial`: a circle around `centerX`, `centerY` (0.5, 0.5: the
    middle), whose `radius` is a fraction of the view's shorter side:
    0.5, the default, reaches the nearer sides. Past it, the last
    stop's color.
  - `.conic`: sweeping clockwise around the center, from `angle`
    (degrees clockwise from the top; 0, the default, starts at the
    top).
- **`material`**: a translucent background that blurs what's behind it,
  iOS's materials: `.ultraThin`, `.thin`, `.regular`, `.thick`, `.bar`.
  - iOS: `UIVisualEffectView`'s blurs.
  - macOS: an `NSVisualEffectView` behind the view's content, blurring
    what's behind it in the window.
  - Windows: Fluent's acrylic, which Asuka draws: what's behind the
    view, blurred and tinted light or dark.
  - The web: CSS's `backdrop-filter`, a blur and saturation.
  - Android and Linux, which can't blur behind a view, show the surface
    color (Linux: libadwaita's card color), translucent.
- **`clip`**: clips the children to the view's bounds and corners.

```asuka
View {
  height: 120; radius: 16
  gradient: Gradient(stops: [Stop(color: #6366f1, at: 0), Stop(color: #ec4899, at: 1)])
  shadow: Shadow(color: #00000050, radius: 16, y: 8)
}

View {
  padding: 12; border: 1; borderColor: .separator; radius: 8; clip: true
  Text("Bordered")
}

// Left to right.
View {
  height: 80
  gradient: Gradient(stops: [Stop(color: .blue, at: 0), Stop(color: .purple, at: 1)], startY: 0.5, endX: 1, endY: 0.5)
}

// A sheet's top: its top corners rounded.
View { height: 200; background: .secondaryBackground; radius: [20, 20, 0, 0] }

// A hairline under a row.
View {
  padding: 12; border: Edges(bottom: 1); borderColor: .separator
  Text("Row")
}

// A place to drop files.
View { height: 120; border: 2; borderStyle: .dashed; borderColor: .accent; radius: 12 }

// A glow, and a color wheel.
View {
  height: 160
  gradient: Gradient(kind: .radial, stops: [Stop(color: #fde68a, at: 0), Stop(color: #f97316, at: 1)], radius: 0.8)
}
View {
  width: 120; height: 120; radius: 60
  gradient: Gradient(kind: .conic, stops: [Stop(color: #ef4444, at: 0), Stop(color: #eab308, at: 0.33), Stop(color: #3b82f6, at: 0.66), Stop(color: #ef4444, at: 1)])
}
```

## Opacity and hiding

`opacity` is from 0 (transparent) to 1 (opaque, the default). To hide a
view, set `hidden`. A hidden view still takes its space in the layout;
to take it out, and its views with it, put it in an `if` (see
[Concepts](concepts.md#showing-and-repeating)).

## Transforms

`transform` moves, scales and rotates a view as it's drawn,
`Transform(x:, y:, scale:, scaleX:, scaleY:, rotation:)` (degrees,
clockwise); layout doesn't see it. Every platform draws it.
Transforms, `transition` and `shared` are for animation: see
[Motion](motion.md).

## Text

`font` sets a text's font, a `Font`:

- **Semantic fonts**, the platform's text styles, scaled with the
  user's text size: `.largeTitle`, `.title`, `.title2`, `.title3`,
  `.headline`, `.subheadline`, `.body`, `.callout`, `.footnote`,
  `.caption`, `.caption2`.
- **`Font(size: 20)`**: the system font at a size, in points.
- **A weight**: `.bold()`, `.semibold()`, `.medium()`, or `weighted(w)`
  with any of `.ultralight`, `.thin`, `.light`, `.regular`, `.medium`,
  `.semibold`, `.bold`, `.heavy`, `.black`.
- **Variants**: `.italic()`; `.monospacedDigits()`, every digit as wide,
  for numbers that change in place (a timer, a column of prices); and
  `.smallCaps()`, lower case letters as small capitals. Or
  `Font(size: 34, italic: true, monospacedDigits: true)`. A font with no
  italic face is slanted, and one with no small capitals draws smaller
  capitals. Windows' buttons and fields take italic, not the other two.

`color` is the text's color, `textAlign` its alignment (`.start`,
`.center`, `.end`, `.justify`), and `lines` the most lines it shows; 0,
the default, is no limit. Text wraps by default.

```asuka
Text("Brand") { color: #7c3aed; font: .title.bold() }
Text("A long paragraph that stops after two lines.") { font: .body; lines: 2; textAlign: .center }
Text("Fixed size") { font: Font(size: 20).medium() }
Text("00:42.17") { font: Font(size: 48, weight: .light, monospacedDigits: true) }
```

How a text is set:

- **`letterSpacing`**: points added between characters (negative ones
  tighten them), as React Native's and CSS's.
- **`lineHeight`**: the distance from one line's baseline to the next,
  in points, as React Native's `lineHeight` and CSS's in pixels; the
  text sits in the middle of each line. 0, the default, is the font's
  own. Apple Watch sets lines no tighter than the font's own.
- **`textShadow`**: a `Shadow` cast by the glyphs, its `radius` a blur
  radius as `shadow`'s is. Layout doesn't see it.
- **`truncation`**: where a text that doesn't fit its `lines` is cut:
  `.tail` (the default) ends it with an ellipsis, `.head` starts it
  with one, `.middle` puts one in its middle, and `.clip` cuts it at its
  edge. Android and the web cut the head or the middle of one line
  only: a text of more lines ends with an ellipsis there.
- **`minimumScale`**: a text that doesn't fit its width in its lines
  (one, unless `lines` says more) shrinks, down to this fraction of its
  size, before it's cut: `minimumScale: 0.5` (UIKit's
  `minimumScaleFactor`, React Native's `adjustsFontSizeToFit`). Its
  letter spacing and line height shrink with it, and layout gives it
  the size it's drawn at.

`letterSpacing` and `lineHeight` scale with the user's text size, as
the font does. They and the font's variants apply to a selectable Text
and a RichText too (Windows' RichText takes no letter spacing); the
shadow, truncation and shrinking are a `Text`'s.

```asuka
Text("OVERVIEW") { font: .footnote.semibold(); letterSpacing: 1.5; color: .secondaryLabel }
Text("A paragraph with room between its lines.") { font: .body; lineHeight: 26 }
Text("IMG_20260412_183015_HDR.jpg") { lines: 1; truncation: .middle }
Text("A title that fits on one line") { font: .title; minimumScale: 0.6 }
Text("Over a photo") { color: .white; textShadow: Shadow(color: #00000080, radius: 6, y: 1) }
```

For text with several styles in it, links and inline views, see
[RichText](components.md).

The text styles are iOS's names; each platform maps them to its own
type ramp, so `.body` is the size the platform's apps use for body
text:

| Platform | Text styles | `.body` |
| --- | --- | --- |
| iOS | UIKit's text styles, in San Francisco | 17 |
| Android | Material 3's type scale (`.body` is Body Large), in Roboto | 16 |
| macOS | AppKit's text styles (`NSFont.preferredFont(forTextStyle:)`), in San Francisco | 13 |
| Windows | Windows 11's type ramp, in Segoe UI Variable (Segoe UI before Windows 11) | 14 |
| Linux | libadwaita's type classes (`.title-1`, `.heading`, `.caption`...), relative to the desktop's interface font | the interface font's size |
| The web | Asuka's ramp, in the system's font (`system-ui`) | 16 |

On Windows the styles take the nearest of the ramp's: `.largeTitle` is
Title Large (40), `.title` Title (28), `.title2` Subtitle (20),
`.headline` Body Strong (14, semibold), `.footnote` and `.caption`
Caption (12). On Linux, with GNOME's 11 pt interface font, `.largeTitle`
is libadwaita's `.title-1` (181%, heavy), `.title` `.title-2`,
`.headline` `.heading` (bold), and `.caption` `.caption` (82%).

What scales with the user's text size:

- **iOS** (Dynamic Type) and **Android** (font scale): every font,
  semantic, fixed and custom.
- **Windows**: every font, by Settings, Accessibility, Text size.
- **Linux**: every font, by the desktop's text scaling (GNOME's Large
  Text, GTK's font DPI). The interface font the styles are relative to
  is the desktop's too (GTK's `gtk-font-name`).
- **The web**: every font, by the browser's text size: fonts are set in
  `rem`.
- **macOS** has no text size setting: fonts stay as they are.

The environment's `fontScale` is that size, 1 by default (see
[Accessibility](accessibility.md)).

## Custom fonts

Put font files (`.ttf`, `.otf`) in the app's `assets/fonts`. A font's
`family`, `Font(family: "Inter")` or `.body.family("Inter")`, picks one:

- a file's name: `assets/fonts/Inter-Bold.ttf` is `"Inter-Bold"`;
- a family of files: `"Inter"` is `Inter-Regular`, `Inter-Bold`... as
  the font's weight says;
- a generic family: `"serif"`, `"monospace"`, `"rounded"`. On iOS and
  macOS they're the system font's designs (New York, SF Mono, SF
  Rounded); on Windows Georgia and Cascadia Mono; on Linux and Android
  the system's serif and monospace families; on the web `ui-serif`,
  `ui-monospace` and `ui-rounded`, where the browser has them. Android,
  Windows and Linux have no rounded one, and show the system font;
- a family the system has, such as `"Georgia"`.

A custom font scales as its text style does. One the platform doesn't
have shows in the system font. Every platform loads the files in
`assets/fonts` for the app alone: they aren't installed for other apps.

```asuka
Text("Inter Bold") { font: .title.family("Inter-Bold") }
Text("Inter") { font: .body.family("Inter").semibold() }
Text("Code") { font: .footnote.family("monospace") }
```

## Dark mode

Semantic colors change with dark mode on their own: an app that uses
them needs no code for it. A fixed color stays as it is; to pick one per
mode, read `dark` from the environment:

```asuka
View {
  height: 80; radius: 12
  background: if environment().dark { #1c1c1e } else { #ffffff }
}
```

## Styles that change

A prop that reads state is a binding: it runs again when that state
changes, and sends that one prop to the view. A layout prop that
changes lays out again; a look prop that changes only updates the view.

```asuka
component Choice {
  state selected = false

  View {
    padding: 12; radius: 10
    background: if selected { .accent } else { .secondaryBackground }
    border: if selected { 2 } else { 0 }
    borderColor: .label
    on press { selected = !selected }
    Text("Tap me")
  }
}
```

Make the change inside `animate { }` and it animates (see
[Motion](motion.md)).

## The accent color

`.accent`, and the controls that use it, take the app's accent color
from `asuka.toml`:

```toml
accent = "#7c3aed"
```

It's the tint on iOS, and the seed of the Material 3 color scheme on
Android (Android 12 and later). With `dynamicColor = true` under `[android]`,
Android takes the colors from the user's wallpaper where the system has
them, as Material apps do. On macOS, Windows and Linux it replaces the
system's accent in Asuka's controls and colors; on the web it's the
controls' `accent-color` too.

Without an accent, the platform's is used. On macOS and Windows that's
the accent the user picked (macOS's System Settings, Appearance;
Windows' Settings, Personalization, Colors), on Linux libadwaita's
blue, and on the web a blue of Asuka's. See [Shipping](shipping.md).

## Light and dark

The app follows the system's appearance: iOS's and macOS's Dark Mode,
Android's dark theme, Windows' app mode (Settings, Personalization,
Colors), GNOME's dark style, the browser's `prefers-color-scheme`. An
app can show itself light or dark whatever the system's is, as an app's
own appearance setting does (SwiftUI's `preferredColorScheme`; on
Android, AppCompat's night mode; on macOS, the app's `NSAppearance`; on
Linux, libadwaita's style manager): see [the app](navigation.md). The
semantic colors, the platform's controls and the system bars follow,
without remaking the app, and the environment's `dark` says which it
is. Keep the user's choice with [`std/store`](stdlib.md).
