# 0009: Composable styles

- **Status**: Draft
- **Created**: 2026-11-17
- **Related**: 0007 (the C core and the Asuka language), docs/language.md
  ("Styles"), docs/layout.md, lang/LOWERING.md ("Elements", "Interaction
  states and variants")

## Summary

An element applies styles with the `style:` prop, which takes one style
or a list, with conditional entries; a style may take parameters, and
composes others the same way:

```asuka
style chip(tint: Color) { padding: [4, 10]; radius: 999; color: tint }
style largeCard { style: card; padding: 24 }

View { style: [card, if selected { chip(.accent) }]; opacity: 0.9 }
```

A style may `use` what's provided (a theme that differs in a part of
the app), and a package may give an element kind a default style
(`style Button { radius: 8 }`).

Every style is known to the compiler: a list's entries are names of
styles and calls of styles with parameters, never a value computed at
run time. What changes is the arguments (bindings, as any prop's) and
the conditions. The compiler inlines a style as it inlines `style card`
today; a conditional entry becomes a layer on the view, the mechanism
the interaction-state variants (`pressed { … }`) already use,
generalized.

Styles as values (a `fn` returning one, a prop or state holding one)
are left for later; see [Later: styles as values](#later-styles-as-values).

## Motivation

Today:

- **One name at a time.** `style card` applies one style; two take two
  items (`style card; style pinkCard`). The parser reads one name
  (`lang/syntax/parser.go`, `StyleUse`).
- **No composition.** A style's body holds props only ("a style holds
  props: name: value", `lang/check/view.go`), so `largeCard` can't be
  `card` with more padding; its props are copied.
- **No conditional styles.** A selected card, a dark card or an urgent
  row is written prop by prop: `background: if selected { … } else {
  … }`, `border: if selected { … } else { … }`, each prop repeating the
  condition, and each style's values copied into the branches.
- **No parameters.** A chip in five tints is five styles, or none and
  the props written at each use.
- **Themes are one value for the whole app.** A style reads its
  package's `let`s, the app's `state` and `environment()` (its values
  are bindings, evaluated in the declaring package's scope), which
  covers an app-wide token. It can't `use` a provided store, so a part
  of the app (a denser settings sheet, a screen with its own palette)
  can't have different tokens.
- **No defaults.** "Every bordered view has a border of 2" is a style
  applied by hand on each element.

CSS answers these with classes, selectors and the cascade; React Native
with style arrays; SwiftUI and Compose with modifier chains. The common
ground is that styles compose in order and a condition picks whether
one applies. Asuka's compiler sees every style whole, so composition,
parameters and conditions can cost what a prop set costs.

## Decisions

| Question | Decision | Why |
|---|---|---|
| Declaring | `style name { props }`; `style name(params) { props }` with parameters | The declaration form stays; parameters are a function's. |
| Applying | The `style:` prop, one style or a list; it replaces the `style name` item | One form; a prop, so a binding like every other. |
| Entries | Names (`card`, `theme.card`) and calls of styles with parameters (`chip(.red)`), each possibly conditional | The compiler sees every style: no run-time style value. |
| Order | Later entries win; the block's own props win over the whole list, wherever `style:` is written | CSS's inline style over classes; no rule that depends on where `style:` sits. |
| Conditional entries | `if c { s }` (no `else`: nothing), `if … else if … else`, `match`, in a style list only | The condition is said once for every prop of the style. |
| Composition | `style:` inside a style's body, with the same list | One spelling for using a style anywhere; no spread operator. |
| Run-time cost | Unconditional entries: inlined sets and bindings, as today. A conditional entry: a layer | Nothing allocated as conditions change. |
| Variants | Layers whose condition is an interaction state; `pressed { … }` stays as their form | One mechanism for "these props, while this holds". |
| Themes | Tokens are `let`s and `state`; `use` in a style resolves at the element that applies it | Works with what exists (provide, use); a part of the app can have its own. |
| Defaults | `style View { … }`, `style Button { … }`: the kind's default style, for the elements written in the declaring package | CSS's type selectors, without restyling other packages' components. |
| Styles as values | Later | Each form of it (a `fn`'s result, a prop, state) needs a style known only at run time, and its machinery; the compile-time forms cover the cases that motivate this. |

## The language

### Declaring

```asuka
style card {
  padding: 16; radius: 12; gap: 8
  background: .secondaryBackground
}

style chip(tint: Color, size: float = 13) {
  padding: [4, 10]; radius: 999
  background: tint.opacity(0.15); color: tint; font: .system(size)
}

pub style largeCard { style: card; padding: 24; font: .title3 }
```

A style with parameters takes default and named arguments as a `fn`
does (`chip(.red)`, `chip(.red, size: 15)`), and appears only where a
style does: in a `style:` list. It isn't a function that can be called
elsewhere, and a style isn't a value that can be stored or passed.

A style's body holds style props (`name: value`), `style:`, `use` (see
[Themes](#themes)) and variant blocks (see [Variants](#variants)). Its
values are evaluated in the scope of the package that declares it, as
today (its `let`s, its `state`), and its parameters.

### Applying

```asuka
View { style: card }
View { style: [card, chip(.pink)]; background: .accent }   // .accent wins
Text(m.title) { style: chip(tint) }                          // runs again when tint changes
```

`style:` takes a style or a list literal of them. Entries apply in
order, a later one's value of a key over an earlier one's, whole values
(a `font` replaces the font, not its size). The block's own props win
over every entry. `style:` is a prop like any other: once per block,
written anywhere in it. An argument that reads state is a binding: each
prop it reaches runs again when the state changes, and only those.

A list entry may be conditional:

```asuka
View {
  style: [
    card,
    if selected { chip(.accent) },
    if m.urgent { urgent } else if m.read { read },
    match m.kind { .note => note, .task => task },
  ]
}
```

An `if` without `else` adds nothing when its condition is false. This
form is the style list's only: elsewhere an `if` expression still has
both branches. A branch holds one entry or a list (`if dark { [card,
darkCard] }`). When a condition changes, the props that entry set go
back to what the entries below it say (and to the platform's default
where nothing set them): no prop is left as the entry left it.

Choosing a style by a value is a `match` or an `if` in the list, where
a `fn` returning a style would be used elsewhere:

```asuka
View { style: [card, match m.priority { .high => chip(.red), .low => chip(.gray), _ => [] }] }
```

(`[]`, an empty list, is the entry that adds nothing.)

### Components that take the style

A component that shows one element (check's `TakesStyle`) takes
`style:` as it takes the style props: its caller's list and props go on
that element, over the component's own entries and props, as the
caller's style props do today. That's how a caller passes a style to a
component, in place of a prop holding one.

### Variants

`hovered`, `pressed`, `dragging` and `dragOver` blocks stay, and take
`style:` like any block (`pressed { style: raised }`). A declared style
may hold them too, applied by the elements that take variants (and
ignored by the others):

```asuka
style button {
  padding: [8, 16]; radius: 8; background: .accent; color: .onAccent
  pressed { opacity: 0.7 }
}
```

### Themes

A token is a value the styles read:

```asuka
// theme.asuka
pub let hairline = 2.0                 // fixed
state compact = false                  // the app's, changed by a setting

pub style card {
  border: hairline; borderColor: .separator
  padding: if compact { 8 } else { 16 }
}
```

This works today. For tokens that differ in a part of the app, a style
may `use` a provided value:

```asuka
pub store Theme {
  pub state border = 2.0
  pub state radius = 12.0
}

pub style card { use t: Theme; border: t.border; radius: t.radius }

app {
  provide Theme()
  Stack {
    Inbox()
    Settings() { provide Theme(border: 1) }   // denser below here
  }
}
```

A style is applied only in an element's `style:` (or a style's, that
one applied in an element's), so its `use` resolves at that element's
place in the tree, under the owner the element is built under
(`ui.use`), as a `use` in the element's component would.

### Default styles

A style named after an element (or a component that takes the style)
is that kind's default, for the elements of that kind written in the
declaring package:

```asuka
style View { borderColor: .separator }
style Button { radius: 8 }
```

The default is the bottom of the order, under the `style:` list. It
applies only to the elements written in the package that declares it:
an app's `style Button` doesn't restyle the buttons inside another
package's components, which keeps a library's look its own and an app
that sets no style native. A package declares at most one default per
kind and it holds what any style may; it may be `pub`, and a package
that wants another's defaults says so (`style Button { style:
theme.Button }`). A default takes no parameters.

### The order, in full

For one key of one view, lowest first:

1. The platform's default (the key unset).
2. The package's default style for the element's kind.
3. The `style:` entries, in order.
4. The block's own props.
5. For a component's element: the caller's `style:` entries, then the
   caller's props.
6. The variants of the states the view is in, in the states' order
   (`hovered`, `pressed`, `dragging`, `dragOver`).

## Checking

- `style:` takes a style entry or a list literal of them; an entry is a
  style's name (`card`, `pkg.card`), a call of a style with parameters,
  a conditional (`if`, `match`) whose branches are entries, or `[]`.
  Anything else (a variable, a `fn` call, a field) is an error: "a
  style list holds styles: a name or a style's call".
- A style's arguments are checked as a `fn`'s; a style without
  parameters called (`card()`) or one with required parameters named
  bare (`chip`) is an error.
- A style's body: style props (each checked against the style props'
  types, the prelude's `Style`), `style:`, `use`, and variant blocks.
  An element's own props (a Button's `title`) aren't style props, and
  aren't allowed.
- A style that includes itself, directly or through others (`style a {
  style: b }`, `style b { style: a }`), is an error naming the cycle.
- Every element takes every style prop, so any style fits any element
  that takes the style; a key its native widget doesn't support is
  ignored there, as a look prop written in its block is today.
- `style name` (the item) is an error with a fix: `style: name`.

## Lowering

Everything is resolved while lowering the element, as `style card` is
today (lang/LOWERING.md, "Elements"):

| Source | Lowered |
|---|---|
| `style: card`, `style: [card, chip(.red)]` | each style's sets, inlined, the arguments substituted (`chip`'s `tint` is `.red`): `ui.set` of constants, `ui.bind` of what reads state; a later set of a key replaces an earlier one, and a key the block sets drops every entry's |
| `chip(tint)`, `tint` reactive | the sets that read `tint` are bindings (`ui.bind(v, AK_KEY_COLOR, thunk{tint})`), the others constants |
| `if c { s }` in the list | a layer: `l = ui.layer(v, rank)`, its sets `ui.layer_set(l, key, x)` / `ui.layer_bind(l, key, thunk)`, and `ui.layer_on(l, thunk{c})` |
| `if … else if … else`, `match` | a layer per branch, each on while its branch is the one taken |
| `use t: Theme` in a style | CALLB `ui.use(key)` where the element is built, once per element (two styles using the same key share it) |
| `style View { … }` | its sets inlined into every `View` written in the package, before the list's (overridden by it as any earlier set) |
| `pressed { … }` | a layer whose condition is the pressed state (`ui.variant` becomes `ui.layer` with a state) |
| a variant block in a style | its sets inlined into the element's variant of that state |

A style without a condition compiles to exactly what `style card`
compiles to today: no layer.

The ranks order a view's layers: the list's in list order, the
caller's after the component's, the variants after all. The block's own
keys never reach a layer: the compiler drops a conditional entry's set
of a key the block sets, and the caller's (`ui.set_outer`) keep the bit
per key they keep today.

## The core

The variants (core/src/ui/interaction.c) already lay values over a
view's own and take them back: `ak_ix` keeps each variant `(state, key,
value)` and the view's base value of each key a variant sets, and
`apply` computes a key from the base and the variants of the states the
view is in. A layer generalizes the variant:

```c
typedef struct {
	uint16_t rank;     // its place among the view's layers
	uint8_t state;     // AK_STATE_* for a variant; 0: on while `on`
	bool on;
	AK_VEC(ak_style_ent) ents;
} ak_layer;
```

`ak_ix` holds a vector of layers in rank order instead of variants;
`apply(e, key)` starts from the base, lays the value of each layer
that's on (a state layer: while the view is in its state), and sets the
key. A condition's binding toggles `on` and applies the layer's keys.

- A view's layers and their entries are made with the element, so a
  condition changing allocates nothing (the hot-path rule).
- Layers move from containers to every element that takes the style
  (state layers stay on the containers: only they report states).
- `ak_ix_set_base` keeps taking a set of a key a layer sets as the
  base, so the block's bindings, the caller's and the layers' compose
  in any order of their effects running.
- The style target (`cls->style_target`, for an element whose style
  goes on a child view) applies to a layer's keys as to the variants'.
- A layer belongs to the view's owner: disposing it drops the layer's
  bindings with the view's.

Tests (core/test): a layer on and off returns the key to its base and
to unset; layers in rank order; a state layer under a conditional one
of a higher rank; no allocation toggling a layer (`ak_alloc_count`);
the variants' tests unchanged through the layer path.

## Migration

`style name` items (17 in the repository's Asuka and 4 in the docs'
examples, in `examples/`, `std/`, `sdk/` and `docs/`) become `style:
name`, and adjacent ones a list (`style a; style b` → `style: [a, b]`).
`asuka fmt` rewrites them; the checker's error says the fix in the
meantime. Variant blocks don't change.

The docs (docs/language.md's "Styles", docs/layout.md's "Style" and
"Styles that change", their zh and ja translations) describe the new
forms; lang/LOWERING.md's "Elements" and "Interaction states and
variants" the lowering; DESIGN.md the layers.

## Later: styles as values

A `Style` type (a `fn` returning a style, `prop look: Style`, state
holding one, `Style { … }` as an expression) is left out. Each form
means a style known only at run time, which needs:

- a run-time representation of a style (its keys and values, made by
  the code that makes it);
- a layer whose keys change: when its style changes, the runtime
  applies the union of the old and new keys, so one the new style
  doesn't set goes back;
- a bit per key the block set, so a run-time layer can't override the
  block's own props (the compiler can't drop those sets for it);
- answers for `use` in a style made by a `fn` (which owner?) and for
  variants in a run-time style (who asks the view to report the
  state?).

This design leaves room for it: the list's order, the layers and the
ranks are what a run-time style would use, and today's entries (names,
calls) would be its constant cases, still inlined. Until then, a choice
of style by a value is a `match` in the list, and a component takes its
caller's style through `style:`.

## Alternatives

- **Several names in the item** (`style card, pinkCard`). Cheapest, but
  it composes only names: no conditions, no parameters.
- **A spread in a style's body** (`style big { ...card, padding: 24 }`).
  A second spelling for using a style, borrowed from objects; Asuka has
  no spread elsewhere.
- **Class names and selectors** (CSS's `.card.pink`, descendant
  selectors). A selector matches at run time across the tree, which is
  what makes CSS hard to reason about and to make fast; the compiler
  would see none of it. Default styles take the one selector that
  earns its place, the element kind, and only lexically.
- **Styles as modifiers** (SwiftUI's, Compose's chains). Order is the
  same idea, but a modifier wraps the view in another; Asuka sets keys
  on one native view and lays it out once.
- **Styles as values now.** See [Later](#later-styles-as-values): the
  run-time path it needs is most of the core's work in this RFC, for
  cases a `match` in the list covers.

## Open questions

1. **The caller's layer and the variants.** The order puts the
   component's variants over the caller's props (6 over 5). Check how
   `ui.set_outer` and `ak_ix_set_base` compose today, and decide
   whether a caller's prop should instead beat the component's
   `pressed`.
2. **Default styles across packages.** Lexical defaults keep libraries
   their own; an app that wants its buttons and a library's to match
   has the library take its caller's `style:`. Is there a case for an
   app-wide default a library opts into?
3. **Animations.** A layer turning on inside `animate { }` animates its
   keys as a prop set does. Confirm with motion.md's rules.

## Milestones

1. **`style:` with names and lists.** The prop, the list, composition
   in a style's body, the cycle check; `style name` an error with its
   fix; `asuka fmt` rewrites it; the repository and docs migrated. No
   core change: everything inlines.
2. **Styles with parameters.** Declaration, calls inlined with
   arguments substituted.
3. **Layers.** `ak_layer` in the core, variants moved onto it (their
   tests unchanged), conditional entries lowered to layers; the hosts
   unaffected (layers only set keys).
4. **Themes and defaults.** `use` in a style; variant blocks in a style;
   default styles per kind.
5. **Docs.** language.md and layout.md and their translations,
   LOWERING.md, DESIGN.md.
