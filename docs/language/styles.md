# Styles

Every element takes the style props: layout first (`direction`,
`justify`, `align`, `gap`, `padding`, `margin`, `width`, `height`,
`grow`, `shrink`, `absolute`, `top`, `grid`, ...), then look
(`background`, `radius`, `border`, `opacity`, `shadow`, `font`,
`color`, `lines`, ...). See [Layout and style](../layout.md).

A `style` declares props to reuse, and `style:` applies one, or a list
of them:

```asuka
style card {
  padding: 16; radius: 12; gap: 8
  background: .secondaryBackground
}

style pink { background: #ff2d55 }

View { style: [card, pink]; opacity: 0.9 }
```

A later entry wins a prop both set, and the block's own props win over
the whole list, wherever `style:` is in the block. A style applies
others the same way, and its own props are over theirs:

```asuka
style largeCard { style: card; padding: 24 }
```

A style may take parameters, as a `fn` does (defaults, named
arguments). A parameter is a binding where it's used: `chip(tint)`
follows `tint`, and only the props that read it change.

```asuka
style chip(tint: Color, size: float = 13) {
  padding: [4, 10]; radius: 999
  background: tint.opacity(0.15); color: tint; font: Font(size: size)
}

Text(tag) { style: chip(.green) }
Text(count) { style: [card, chip(tint, size: 15)] }
```

An entry may be an `if` or a `match`: the styles of the branch it takes
apply, and when it takes another, each prop they set goes back to what
the entries under it say (or the platform's default). Here an `if` needs
no `else`; a `match` needs no arm for every case and binds no names, and
an arm's `[]` applies nothing.

```asuka
View {
  style: [
    card,
    if selected { chip(.accent) },
    if m.urgent { urgent } else if m.read { read },
    match m.kind { .note => note, .task => [task, bold], _ => [] },
  ]
}
```

A style holds style props, `style:`, variants (below) and `use`. A
`pub style` is another package's too: `style: theme.card`, its values
read in the package that declares it. A style can't apply itself,
through others either.

An element that sets no style looks like the platform's own.

**Themes.** A style's values read its package's `let`s and `state`, so
a token is one of those (`pub let hairline = 2.0`), and a `state` one
changes everywhere it's read. A style may `use` a store a component
above provides, found where the style is applied: a part of the app
that provides its own has its own tokens.

```asuka check
pub store Theme(hairline: float = 2) {
  pub state border = hairline
}

pub style framed { use t: Theme; border: t.border; borderColor: .separator }

app {
  provide Theme()
  Stack { Settings() }
}

component Settings {
  provide Theme(hairline: 1)   // thinner in here
  Column { style: framed }
}
```

**Default styles.** A style named after an element (or a component that
takes the style props) is its default: every one of that kind written in
the package takes it, under its `style:` list. Another package's
elements, and the ones inside its components, keep theirs.

```asuka check
style Button { radius: 8 }
style View { borderColor: .separator }
```

A container (`View`, `Row`, `Column`, `SafeArea`) takes **variants**:
blocks of style props named after an interaction state, laid over its
style while it's in that state (CSS's `:hover` and `:active`). They are
`hovered`, `pressed`, `dragging` (while it's dragged) and `dragOver`
(while a drag it takes is over it), laid in that order, so `pressed`
wins over `hovered`. A variant sets only the props it names, and its
values are bindings like any prop's:

```asuka
View {
  padding: 16; radius: 12; background: .secondaryBackground
  hovered { borderColor: .accent }
  pressed { background: .separator; opacity: 0.8 }
  on press { open() }
}
```

A view reports a state only once something asks for it, so one with no
`pressed` variant keeps the platform's own pressed look. A variant may
take a `style:` too (`pressed { style: raised }`), under its own props,
and a style may hold variants, which the containers it's applied to
take (and the other elements leave):

```asuka check
style button {
  padding: [8, 16]; radius: 8; background: .accent
  pressed { opacity: 0.7 }
}
```

A component may declare variants of its own (`variant checked =
checked`, see [Components](components.md#variants-a-component-declares)),
and a style may hold their blocks as it holds the interaction states':
on an element whose component declares the variant, the block is laid
on while it holds, over the states'; on any other element it does
nothing. A misspelled name is an error: a style names only the variants
of its own package's components and other packages' `pub` ones.

```asuka
// For a component that declares variant selected (a listbox's option).
style option {
  padding: [6, 10]; radius: 6
  pressed { opacity: 0.8 }
  selected { background: .accent; color: .onAccent }
}
```

A component's variants are over its caller's props: a caller's
`background` replaces the component's, and its `pressed` still darkens
it. Other views follow a
view's states through its name ([Naming a view](#naming-a-view)):
`View as card { … }` then `card.hovered` (or `card.pressed`,
`card.dragging`, `card.dragOver`) in any binding, a child's (a group's
hover) or a sibling's. What isn't a style uses the handlers: `on
pressedChange(p)`, `on hoverChange(h)`.

## Naming a view

Some things are done to a view rather than set on it: play a video,
run script in a page, put text in a field, scroll a list. A `Ref` is a
view's handle: `ref: r` on any element puts its view in `r`, and
`Element as name` declares one for the rest of the block it's in (on a
component that shows one element, that element's view).

```asuka check
component Player {
  let video = Ref()

  Video("intro.mp4") { ref: video }
  Button("Start over") {
    on press {
      video.seek(0)
      video.play()
    }
  }
}
```

A Ref holds its view while the view is shown, and nothing before or
after: a command then does nothing, and a state is false. Its states
are tracked, so a binding that reads one follows it:

| On | Ref's members |
|---|---|
| any view | `hovered`, `pressed`, `dragging`, `dragOver` (bools, tracked); `focus()`, `blur()` |
| a component's (`Checkbox as box`) | its variants: `box.checked` (bools, tracked) |
| `Scroll`, `List` | `scrollTo(x, y, animated: true)` |
| `Video` | `play()`, `pause()`, `seek(seconds)` |
| `WebView` | `try await eval(script)` (its value, as JSON), `run(script)` (without waiting), `goBack()`, `goForward()`, `reload()`, `stopLoading()` |
| `TextField` | `insert(text)`, `format(.bold, on)`, `toggleFormat(.italic)`, `setLink(url)` (a rich field's selection) |

A Ref can be passed along (`device.shareFiles(paths, from: button)`,
`accessibility.focus(field)`), and kept in a file's `let` for views of several
components.
