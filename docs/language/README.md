# Language reference

Asuka apps are written in Asuka, a small language made for interfaces
that use each platform's own widgets. It's statically typed, compiles
ahead of time for release builds, and reloads in place during
development, so a save shows up in milliseconds, with every value and
every native view kept.

```asuka check
component Counter {
  state count = 0

  SafeArea {
    justify: .center; align: .center; gap: 16; padding: 24

    Text("Count: {count}") { font: .largeTitle.bold() }
    Row {
      gap: 12
      Button("Reset") { variant: .gray; on press { count = 0 } }
      Button("Add") { variant: .filled; on press { count += 1 } }
    }
    if count >= 10 {
      Text("That's a lot") { color: .secondaryLabel }
    }
  }
}

app { Counter() }
```

These pages cover the whole language, a topic each. For a first look, read [A tour of Asuka](../language-tour.md); the elements themselves (`Text`, `Button`, `List`, `Stack`, ...) have their own page: [Components](../components.md).

## The pages

- [Files, packages and imports](packages.md): files, packages and imports; files for one platform; apps, programs and libraries
- [Values and types](types.md): values and types, operators, built-in methods, value semantics
- [Declarations](declarations.md): let, var, state and the rest; structs and enums
- [Functions and closures](functions.md): functions and closures; interfaces and generics
- [Control flow](control-flow.md): if, match, loops and patterns; code for one platform (@platform)
- [Components](components.md): components: their bodies, handlers, two-way bindings, lists of views, effects and tasks
- [Stores](stores.md): state shared by the components that use it
- [Async](async.md): async functions, tasks, streams, spawn; data that loads
- [Errors](errors.md): throwing, catching, and errors as values
- [Styles](styles.md): styles, and naming a view to reach it
- [Animation](animation.md): animate { }, animating values, and per-frame work (onFrame)
- [Drawing](drawing.md): Canvas, Shape, paths and shaders
- [The system](system.md): what the app asks of the system (alerts, the share sheet, haptics, windows) and the events it hears (URLs, files, quitting)
- [Navigation](navigation.md): screens, tabs and stacks; push, present, pop and dismiss
- [Text in other languages](languages.md): text in other languages
- [Native code](native.md): native interfaces and components
- [Tests](tests.md): test blocks, mount, and driving and checking an app

## Memory and performance

There's no garbage collector, so there are no pauses. Values are
reference counted and copied on write. State, stores, effects and tasks
belong to the component that made them, and go with it.

- A change runs only the bindings that read what changed, and sends the
  platform only the props that changed, once a frame.
- Release builds are compiled ahead of time, so there's no interpreter
  in an app you ship.
- Lists build only what's on screen and reuse cells.

## Keywords

`app as async await bind break catch component continue defer do effect
else emit enum event extend extern false fn for if import in interface
key let load match mut native none on prop provide pub reload return
screen self spawn state store struct style task test throw throws true
try type use var while worker`

`mut`, `extend` and `worker` are keywords since 0.2.7, `reload` since
0.2.9: a name that was one is an error that says to rename it.

`shader` (a [shader](../components.md#shaders)'s declaration), `at` (a
routed screen's pattern), `static` (a type's function: `static fn`) and
`variant` (a component's variant) are keywords only there: elsewhere
they're names.

`load` followed by a call is the [load](async.md#data-that-loads)
keyword's (`load api.user(id)`, or `load (api.user(id))`), and so are
parentheses that hold several elements, one a call at least (`load
(api.user(id), api.posts(id))`, [a group](async.md#loads-that-land-together));
followed by parentheses that hold anything else (`load()`, `load(id)`),
it calls a function named `load`. `reload` followed by a name is the
[reload](async.md#reloading-what-a-change-touched) keyword's (`reload
api.notes(id)`, `reload api.notes`); followed by `(`, it calls a
function named `reload`.
