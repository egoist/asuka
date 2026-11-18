//go:build !windows

package appbuild

import (
	"os"
	"syscall"
)

// forwardedSignals are those a program run on the VM hears through
// asuka run: Ctrl-C (the terminal's, its too), a terminate, a hangup.
var forwardedSignals = []os.Signal{os.Interrupt, syscall.SIGTERM, syscall.SIGHUP}
