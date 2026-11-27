// Package watchos embeds Asuka's watchOS host (Sources: the SwiftUI host
// and its decoder, the core's header; App: its entry point), for the Asuka
// CLI, which compiles it with an app's native packages' Swift and links it
// with the C core into the watch app's executable.
package watchos

import "embed"

// Files holds Sources and App.
//
//go:embed Sources App
var Files embed.FS
