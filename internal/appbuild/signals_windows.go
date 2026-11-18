package appbuild

import "os"

// forwardedSignals are those a program run on the VM hears through
// asuka run: Ctrl-C reaches every process of the console, so asuka run
// waits for the program's end instead of ending first.
var forwardedSignals = []os.Signal{os.Interrupt}
