# 0009: Styles as values

- **Status**: Draft
- **Created**: 2026-11-17
- **Related**: 0007 (the C core and the Asuka language), docs/language.md
  ("Styles"), docs/layout.md, lang/LOWERING.md ("Elements", "Interaction
  states and variants")

## Summary

A style becomes a value of type `Style`. `style card { … }` declares a
constant one; `style chip(tint: Color) { … }` declares a style with
parameters, a function that returns one; any `fn` may return one, and a
component may take one as a prop. An element applies styles with the
`style:` prop, which takes one style or a list, with conditional
entries:

```asuka
View { style: [card, rowStyle(m), if selected { chip(.accent) }]; opacity: 0.9 }
```

A style composes others the same way (`style big { style: [card,
chip(.red)]; padding: 24 }`), may `use` what's provided (a theme that
differs in a part of the app), and a package may give an element kind a
default style (`style Button { radius: 8 }`).

The compiler resolves what it can see (names, calls of a style with
parameters, conditional entries) and lowers it as today: prop sets
inlined in the element, a binding per prop. Only a `Style` known at run
time (a `fn`'s result, a prop, state) costs more: one layer on the view,
whose keys the runtime compares when its value changes. Conditional
entries and the interaction-state variants (`pressed { … }`) become the
same mechanism: layers laid over the view's own values while something
holds.

## Motivation

Today a style is a declaration and nothing else:

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
- **Not a value.** A function can't choose a style (`fn rowStyle(m:
  Message)`), and a component can't take one from its caller beyond the
  style props its one element takes.
- **Themes are one value for the whole app.** A style reads its
  package's `let`s, the app's `state` and `environment()` (its values
  are bindings, evaluated in the declaring package's scope), which
  covers an app-wide token. It can't `use` a provided store, so a part
  of the app (a denser settings sheet, a screen with its own palette)
  can't have different tokens.
- **No defaults.** "Every bordered view has a border of 2" is a style
  applied by hand on each element.

CSS answers these with classes, selectors and the cascade; React Native
with style arrays and `StyleSheet.compose`; SwiftUI with view modifiers
and `ViewModifier`s as values; Compose with `Modifier` chains. The
common ground is that a style is a value that composes in order, and a
condition picks whether it applies. Asuka has static types and a
compiler that sees most styles whole, so it can keep a value's
flexibility without its run-time cost where the style is known.

## Decisions

| Question | Decision | Why |
|---|---|---|
| What a style is | A value of type `Style`: style keys and their values | A function can return it, a component take it, a list hold it. |
| Declaring one | `style name { props }` (a constant), `style name(params) { props }` (with parameters: a function returning `Style`) | The declaration form stays; parameters are a function's. |
| Applying | The `style:` prop, one `Style` or a list; it replaces the `style name` item | One form; a prop, so it's a binding like every other. |
| Order | Later entries win; the block's own props win over the whole list, wherever `style:` is written | CSS's inline style over classes; no rule that depends on where `style:` sits in the block. |
| Conditional entries | `if c { s }` (no `else`: nothing), `if … else if … else`, `match`, in a style list only | The condition is said once for every prop of the style. |
| Composition | `style:` inside a style's body, with the same list | One spelling for using a style anywhere; no spread operator. |
| Run-time cost | Styles the compiler sees: inlined prop sets and bindings, as today. A conditional entry: a layer. A `Style` known only at run time: a layer whose keys can change | Values in the language, constant folding in the compiler. |
| Variants | Layers whose condition is an interaction state; `pressed { … }` stays as their short form | One mechanism for "these props, while this holds". |
| Themes | Tokens are `let`s and `state`; `use` in a style resolves where the style is made | Works with what exists (provide, use); a part of the app can have its own. |
| Defaults | `style View { … }`, `style Button { … }`: the element kind's default style, for the elements written in the declaring package | CSS's type selectors, without restyling other packages' components. |
| Unknown keys | A style holds style keys only; one an element doesn't support is ignored where it's applied | As look props are today ("a view ignores `font`"). |

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

A style with parameters is a function returning `Style`: it takes
default and named arguments as a `fn` does, and it's called the same
way (`chip(.red)`, `chip(.red, size: 15)`). A style without parameters
is a constant, named without parentheses (`card`).

A style's body holds style props (`name: value`), `style:` and `use`
(see [Themes](#themes)). Its values are evaluated in the scope of the
package that declares it, as today: its `let`s, its `state`, its
parameters.

An anonymous style is an expression: `Style { padding: 8; radius: 4 }`,
for a function that builds one inline. (`Style` is today the prelude's
pseudo-component that lists the style props; it becomes this type.)

### Applying

```asuka
View { style: card }
View { style: [card, chip(.pink)]; background: .accent }   // .accent wins
Text(m.title) { style: rowStyle(m) }
```

`style:` takes a `Style` or a list of them (`[Style]`). Entries apply
in order, a later one's value of a key over an earlier one's, whole
values (a `font` replaces the font, not its size). The block's own
props win over every entry. `style:` is a prop like any other: once per
block, and a binding, so `style: chip(tint)` runs again when `tint`
changes.

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
both branches. When a condition changes, the props that entry set go
back to what the entries below it, and the block, say (and to the
platform's default where nothing set them): no prop is left as the
entry left it.

### Values

```asuka
fn rowStyle(m: Message) -> Style {
  if m.urgent { chip(.red) } else if m.read { card } else { chip(.accent) }
}

component Badge(text: string) {
  prop look: Style = Style {}
  Text(text) { style: [pill, look] }
}

Badge("3") { look: chip(.green) }
```

A `Style` is an immutable value: `chip(tint)` captures `tint` as it is
when it's called. What changes goes in a binding (`style:`, a prop),
which runs again; a style's values that read state (`padding: if compact
{ 8 } else { 16 }`, in a declared style) are bindings in the element
that applies it, as today.

`Style` values compare by their keys and values (so a binding that makes
an equal style sends nothing), and aren't otherwise operated on: no
`+`, no field access. Composition is the list.

### Components that take the style

A component that shows one element (check's `TakesStyle`) takes
`style:` as it takes the style props: its caller's list and props go on
that element, over the component's own entries and props, as the
caller's style props do today.

### Variants

`hovered`, `pressed`, `dragging` and `dragOver` blocks stay, and take
`style:` like any block (`pressed { style: raised }`). A declared style
may hold them too, applied by the elements that take variants:

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
may `use` a provided value, resolved where the style is made (for an
entry of an element's `style:`, that element's place in the tree):

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

`use` finds what's provided under the current owner (`ui.use`), so a
style that uses it is made where there is one: in a view, or in code
that runs under a view's owner (a binding, a handler). Whether a `fn`
may make one is open question 3.

### Default styles

A style named after an element (or a component that takes the style)
is that kind's default, for the elements of that kind written in the
declaring package:

```asuka
style View { borderColor: .separator }
style Button { radius: 8 }
style Badge { look: … }            // an error: a default holds style props only
```

The default is the bottom of the order, under the `style:` list. It
applies only to the elements written in the package that declares it:
an app's `style Button` doesn't restyle the buttons inside another
package's components, which keeps a library's look its own and an app
that sets no style native. A package declares at most one default per
kind; it may be `pub`, and a package that wants another's defaults
says so (`style Button { style: theme.Button }`).

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

- `style:` takes `Style` or `[Style]`; a list literal's entries may be
  conditional, as above.
- A style's body: style props (each checked against the style props'
  types, the prelude's `Style`), `style:`, `use`, and variant blocks.
  An element's own props (a Button's `title`) aren't style props, and
  aren't allowed.
- A style that includes itself, directly or through others (`style a {
  style: b }`, `style b { style: a }`), is an error naming the cycle;
  a style with parameters may not call itself either.
- Every element takes every style prop, so a `Style` fits any element
  that takes the style; a key its native widget doesn't support is
  ignored there, as a look prop written in its block is today.
- `style name` (the item) is an error with a fix: `style: name`.

## Lowering

A style the compiler sees is resolved while lowering the element, as
`style card` is today (lang/LOWERING.md, "Elements"):

| Source | Lowered |
|---|---|
| `style: card`, `style: [card, chip(.red)]` | each style's sets, inlined, the arguments substituted (`chip`'s `tint` is `.red`): `ui.set` of constants, `ui.bind` of what reads state; a later set of a key replaces an earlier one, and a key the block sets drops every entry's |
| `if c { s }` in the list | a layer: `l = ui.layer(v, rank)`, its sets `ui.layer_set(l, key, x)` / `ui.layer_bind(l, key, thunk)`, and `ui.layer_bind_on(l, thunk{c})` |
| `if … else if … else`, `match` | a layer per branch, each on when its branch is taken |
| `style: s` where `s` is a `Style` known at run time (a `fn`'s result, a prop, state) | `l = ui.layer(v, rank)` and `ui.layer_bind_style(l, thunk{s})`: the layer's keys and values are the style's |
| `Style { … }` | a constant `Style` when its values are constants; otherwise `ui.style_new` and a set per key, under the binding that makes it |
| a call of a declared style, as a value (not inlined) | a function returning `ui.style_new` with its sets |
| `use t: Theme` in a style | CALLB `ui.use(key)` under the owner the style is made under |
| `style View { … }` | its sets inlined into every `View` written in the package, before the list's (they're overridden by it as any earlier set) |
| `pressed { … }` | a layer whose condition is the pressed state (`ui.variant` becomes `ui.layer` with a state) |

Most of an app's styles are names and calls with constant or state
arguments: they compile to exactly what `style card` compiles to today,
with no layer and no run-time style value.

The ranks order a view's layers: the list's in list order, the
caller's after the component's, the variants after all. The block's own
keys never reach a layer: the compiler drops a conditional entry's set
of a key the block sets; for a layer known at run time, the view keeps
a bit per style key its block set (as it keeps one per key its caller
set, `ui.set_outer`), and a layer's value of such a key is skipped.

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
`apply(e, key)` starts from the base and lays the value of each layer
that's on (a state layer: while the view is in its state) and sets the
key. A condition's binding toggles `on` and applies the layer's keys;
a run-time `Style`'s binding replaces the layer's entries and applies
the union of the old and new keys, so a key the new style doesn't set
goes back.

- Layers are made with the element, so turning one on or off, or a
  binding running again with an equal value, allocates nothing (the
  hot-path rule). A run-time `Style` is a value made by the code that
  makes it, like any struct: a sorted array of `(key, value)`, built
  once and shared.
- Layers move from containers to every element that takes the style
  (variants stay on the containers: only they report states).
- `ak_ix_set_base` keeps taking a set of a key a layer sets as the
  base, so the block's bindings, the caller's and the layers' compose
  in any order of their effects running.
- The style target (`cls->style_target`, for an element whose style
  goes on a child view) applies to a layer's keys as to the variants'.
- A layer belongs to the view's owner: disposing it drops the layer's
  bindings with the view's.

Tests (core/test): a layer on and off returns the key to its base and
to unset; layers in rank order; a block key over a run-time layer's; a
run-time style changing from keys `{a, b}` to `{b, c}` resets `a`; no
allocation toggling a layer or rerunning an equal style
(`ak_alloc_count`); variants unchanged through the layer path.

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

## Alternatives

- **Several names in the item** (`style card, pinkCard`). Cheapest, but
  it composes only names: no conditions, no parameters, no values.
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
- **Only names in the list, no run-time `Style`.** Everything compiles
  to sets and layers, and every prop is checked against its element,
  but a function can't choose a style and a component can't take one.
  The run-time layer is what makes `Style` a value; the compiler keeps
  the common cases free.

## Open questions

1. **Variants in run-time styles.** A `Style` value holding a
   `pressed { … }` block needs the view to report the state; does the
   layer ask for it when the style arrives, and stop when it goes?
2. **The caller's layer and the variants.** The order puts the
   component's variants over the caller's props (6 over 5). Check how
   `ui.set_outer` and `ak_ix_set_base` compose today, and decide
   whether a caller's prop should instead beat the component's
   `pressed`.
3. **`use` in a style made in a `fn`.** It resolves under the owner the
   `fn` runs under; the checker can't see that owner. Allow it and
   fail at run time when nothing's provided (as `use` without a
   `provide` does), or require a style with `use` to be made in a view?
4. **Default styles across packages.** Lexical defaults keep libraries
   their own; an app that wants its buttons and a library's to match
   gives the library a `Style` prop. Is there a case for an app-wide
   default a library opts into (`style Button` that a package's
   components read as `default`)?
5. **Animations.** A layer turning on inside `animate { }` animates its
   keys as a prop set does; a layer of a run-time style changing
   keys animates the union. Confirm with motion.md's rules.

## Milestones

1. **`style:` with names and lists.** The prop, the list, composition
   in a style's body, the cycle check; `style name` an error with its
   fix; `asuka fmt` rewrites it; the repository and docs migrated. No
   core change: everything inlines.
2. **Styles with parameters.** Declaration, calls inlined with
   arguments substituted.
3. **Layers.** `ak_layer` in the core, variants moved onto it (their
   tests unchanged), conditional entries lowered to layers; every host
   unaffected (layers only set keys).
4. **`Style` values.** The type, `Style { … }`, `fn`s returning it,
   props and state of it, the run-time layer.
5. **Themes and defaults.** `use` in a style; default styles per kind.
6. **Docs.** language.md and layout.md and their translations,
   LOWERING.md, DESIGN.md.
