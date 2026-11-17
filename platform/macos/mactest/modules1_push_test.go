//go:build darwin

package mactest

import "testing"

// The push module by name: a rewrite needs a notification service
// extension, which a Mac app doesn't have; the messages wait to be asked
// for, and "stop" fails each queue's waiting call on its own. (APNs's
// answer to registering and a data message kept: NotifyTests.swift, as no
// test can register with APNs.)
func TestPushModule(t *testing.T) {
	a := Run(t, m1CallSrc+`
		component Waits(method: string) {
		  state s = "waiting"
		  task {
		    do {
		      let b = try await host.call("push", method, bytes())
		      s = "came {b.string()}"
		    } catch e {
		      s = "err {e.message}"
		    }
		  }
		  Text("{method}: {s}")
		}

		component App {
		  Column {
		    Call("nextRewrite", "push", "nextRewrite", bytes())
		    Call("rewritten", "push", "rewritten", "\{}".bytes())
		    Waits("nextReceived")
		    Waits("nextOpened")
		    Button("Stop received") { on press { host.send("push", "stop", "nextReceived".bytes()) } }
		    Button("Stop opened") { on press { host.send("push", "stop", "nextOpened".bytes()) } }
		  }
		}
		app { App() }
	`)
	m1Until(t, a,
		"nextRewrite: err platform/push: rewriting needs a notification service extension, which a Mac app doesn't have",
		"rewritten: ok []", "nextReceived: waiting", "nextOpened: waiting")
	m1UntilState(t, a, func(m m1State) bool { return m.Push.Received.Waiting == 1 && m.Push.Opened.Waiting == 1 })
	// Each queue stops on its own.
	a.Do("tap Stop received")
	m1Until(t, a, "nextReceived: err stopped", "nextOpened: waiting")
	a.Do("tap Stop opened")
	m1Until(t, a, "nextOpened: err stopped")
	m1UntilState(t, a, func(m m1State) bool { return m.Push.Received.Waiting == 0 && m.Push.Opened.Waiting == 0 })
}

// platform/push itself, on the notify player: its streams wait for the
// host's next message, and leaving them stops the calls.
func TestPushPackage(t *testing.T) {
	a := RunWith(t, `
		import platform/push

		component Messages {
		  state received = 0
		  state opened = 0
		  task {
		    for await m in push.received() {
		      received += 1
		    }
		  }
		  task {
		    for await m in push.opened() {
		      opened += 1
		    }
		  }
		  Text("pushes: {received} {opened}")
		}

		component App {
		  state on = true
		  Column {
		    if on { Messages() }
		    Button("Leave") { on press { on = false } }
		    if !on { Text("left") }
		  }
		}
		app { App() }
	`, Options{Player: hostPlayer(t)})
	a.UntilText("pushes: 0 0")
	m := m1UntilState(t, a, func(m m1State) bool { return m.Push.Received.Waiting == 1 && m.Push.Opened.Waiting == 1 })
	if !m.Notify.Delegate {
		t.Errorf("the center's delegate (which hands pushes on): %+v", m.Notify)
	}
	a.Do("tap Leave")
	a.UntilText("left")
	m1UntilState(t, a, func(m m1State) bool { return m.Push.Received.Waiting == 0 && m.Push.Opened.Waiting == 0 })
}
