//go:build darwin

package mactest

import (
	"strings"
	"testing"

	"github.com/egoist/asuka/lang/compile"
	"github.com/egoist/asuka/lang/target"
)

// tryThrowSrc throws and catches one of the app's own errors, an enum case
// with a payload: freeing the error reads its shape, which is the
// module's.
const tryThrowSrc = `
enum Oops: Error {
  failed(code: int)
}

fn tried(code: int) -> string {
  do {
    throw Oops.failed(code: code)
  } catch e {
    return "caught {code}"
  }
}
`

// The app before the reload throws as its state starts; the app after it
// has a second state, which starts, and throws, after the reload.
const (
	reloadBeforeSrc = tryThrowSrc + `
component App {
  state first = tried(1)
  Column {
    Text("first: {first}")
  }
}

app { App() }
`
	reloadAfterSrc = tryThrowSrc + `
component App {
  state first = tried(1)
  state second = tried(2)
  Column {
    Text("first: {first}")
    Text("second: {second}")
  }
}

app { App() }
`
)

// The last error thrown, which asuka dev's player keeps for its trace, is
// let go before a hot reload releases the module whose type it is: the
// next error thrown, after the reload, lets the kept one go, and that
// must not read the old module's shape. Nothing is pressed: the app
// throws as its states start.
func TestReloadAfterThrownError(t *testing.T) {
	a := Run(t, reloadBeforeSrc)
	a.UntilText("first: caught 1")

	res := compile.Files(map[string][]byte{"app.asuka": []byte(reloadAfterSrc)}, &compile.Options{Platform: target.MacOS})
	if err := res.Err(); err != nil {
		t.Fatalf("the reloaded app doesn't compile:\n%v", err)
	}
	a.srv.Send(target.MacOS, res.Module.Encode())
	a.UntilText("second: caught 2")
	if strings.Contains(a.Logs(), "error:") {
		t.Errorf("the player reported an error:\n%s", a.Logs())
	}
}
