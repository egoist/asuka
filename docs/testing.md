# Testing

An app's tests are `test` blocks beside its code. `mount` mounts a
component without a device, a window or a display, and the test drives
it as a user would, by what the screen shows:

```asuka
component Counter {
  state count = 0
  Column {
    Text("Count: {count}")
    Button("Add") { on press { count += 1 } }
  }
}

test "Add counts" {
  let app = mount(Counter())
  app.tap("Add")
  app.tap("Add")
  app.expect("Count: 2")
}
```

```sh
asuka test                    # the app or package in this directory
asuka test --run Add          # the tests whose names contain Add
asuka test path/to/app
```

```
ok   Add counts
FAIL Remove empties the list (app.asuka:41)
     the screen doesn't show "No items"
     View 0,0 390x844
       Text 0,0 390x20 "Item 1"
asuka: 1 of 2 tests failed
```

It runs anywhere the CLI does, in CI too, with no simulator or emulator:
`asuka test` compiles the package with its tests and runs them on this
computer; the first run builds the test runner, which needs a C
compiler. It exits with 1 when a test fails. For the
platforms' real views, see [testing on the
platforms](#testing-on-the-platforms) below.

`asuka run file.asuka` compiles one file and runs it the same way: its
tests, if it has any, then its app, printing what its screen shows, for
trying out a component or a function without making an app.

## How it works

`mount` mounts the view on a stand-in for the platform, which keeps the
views the app makes instead of making the platform's. Everything else
is your app as it runs: its state, effects and tasks, Asuka's layout,
navigation, lists binding their rows, sheets and
alerts.

The app runs as on an iPhone (iOS, 390 × 844 points). `asuka test
--platform android` (or `mac`, `windows`, `linux`, `web`) says it's
another platform, for what the app does by platform: the tests are
compiled for it too, with its packages' files for that platform and
`@platform` its own.

What isn't the platform's is how text is measured: 8 points a
character on 20-point lines, whatever the font. Frames come from
Asuka's real layout, so where views are relative to each other holds;
their sizes to the point don't.

Time moves only when nothing else can run: a `sleep(5s)` in a test, or
a task's timer, takes no time, and a test that waits for something that
never comes fails at once ("the test never finished").

## What the screen shows

Tests name things by the text a user sees: a text, a button's title, a
field's text or placeholder, an accessibility label, a screen's title,
a tab, an alert's button. Text matches exactly.

```asuka
app.expect("Inbox")       // fails the test unless it shows
app.expectNo("Message 1") // fails it if it does
print(app.dump())         // what the screen shows, a view a line
```

Only what a user could see counts:

- the top screen of each stack (the screens under it don't show);
- the selected tab's page, and every tab's title in the tab bar;
- the sheets, alerts and popovers presented, over the rest;
- a list's rows that fit its height: Asuka binds a list's rows as the
  platform asks for them, and so does `asuka test`, as a collection
  view would;
- nothing hidden.

When a check fails, the message has the screen: each view that shows,
with its kind, frame and texts, so you can see what was there instead,
and `asuka test` says where the test is (`app.asuka:41`).

## Doing what a user does

Each action settles before it returns: what it changed is laid out,
lists are bound, and what the app's tasks posted has run.

```asuka
app.tap("Add")                      // a button, a row, a tab, a toggle, a segment, a menu's item
app.type("ada@example.com", into: "Email")  // the field with that placeholder or label
app.tapLink("https://asuka.dev")     // the link in a RichText
app.share([SharedItem(kind: .link, text: "https://asuka.dev")])  // another app shares it: onShare hears it
await app.idle()                    // until nothing is left to run: tasks, timers, loads
```

A tap goes where a finger would. A text in a row taps the row, and a
text in a button taps the button. A view with a menu opens it, and the
next `tap` chooses from it.

## Platform services and native packages

A [native package](native.md)'s interface is the platform's, which
`asuka test` hasn't: its calls throw, as on a platform without the
service. Give your components what they use through a store or a
`provide`, and the test its own:

```asuka
test "archiving removes the row" {
  let app = mount(Inbox(), provide: Mailbox.fake([sample]))
  await app.idle()
  app.tap("Archive")
  app.expectNo("Hello")
}
```

## Testing on the platforms

`asuka test` tests your app's behavior: what shows, and what happens
when the user acts. How each platform draws it is the platform's. To
see your app on the platforms:

- `asuka dev --platform ios|android|web` runs it on the simulator, an
  emulator or a phone, or in a browser, and reloads it as you save.
- On a desktop, `ASUKA_SNAPSHOT=out.png` writes the window to a PNG
  once it has shown, without taking the focus, then quits
  (`ASUKA_DUMP=1` writes its view tree beside it). `ASUKA_SCRIPT`
  runs steps first (`tap:Add`, `type:hello`):
  see [Development](development.md#snapshots-and-scripts).
- On Linux, under Xvfb (`xvfb-run -a dbus-run-session`), that works
  with no display, in CI.
