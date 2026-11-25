# Stores

A store is state with the functions that change it, shared by the views
that use it:

```asuka
pub store Mailbox {
  state messages: [Message] = []
  let unread = messages.count(m => !m.read)

  pub fn markRead(id: string) {
    if let i = messages.index(m => m.id == id) { messages[i].read = true }
  }

  pub async fn refresh() throws {
    messages = try await http.get("https://example.com/inbox").json()
  }
}
```

A store is made like a struct (`Mailbox()`) and belongs to the component
or store that made it. `provide` makes it available to everything below,
and `use` gets it:

```asuka
app {
  provide Mailbox()
  Stack { Inbox() }
}

screen Inbox {
  use mailbox: Mailbox
  …
}
```

What a component provides is for the views it shows: two components
side by side that each provide a `Theme` see their own, not each
other's.

A `use` that nothing above provides is an error, which says the store
to provide (in a test, `mount(…, provide: [Api.fake(), Mailbox()])`,
made in order as `provide`s are). `use` with an optional type is none
when nothing above provides one: a
component that works alone and in a group (a checkbox, in a checkbox
group or not).

```asuka check
store Group {
  pub state chosen: [string] = []
}

component Choice(value: string) {
  use group: Group?

  Text(if let g = group, g.chosen.contains(value) { "✓ {value}" } else { value })
}
```

`state` at the top of a file is the app's: one for the whole app.

A store takes type parameters as a struct does, inferred from its
arguments or from what's expected:

```asuka check
store Kept<T>(key: string, initial: T) {
  pub state value: T = initial
}

let size = Kept("size", 17.0)                 // a Kept<float>
let tags: Kept<[string]> = Kept("tags", [])
```

In a store's fns, `self` is the store itself, to hand to what it makes.
