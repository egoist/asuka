// Package windows embeds the Windows host's C (src/: Win32, the common
// controls, GDI+, DWM, WebView2 and WinRT through COM, RFC 0008's H2) for
// the asuka CLI, which builds it with the core and the app into the app's
// executable (internal/appbuild's WindowsHost). Its tests (windows_test.go)
// build test/*_test.c with it and run them on Windows.
package windows

import "embed"

// Sources holds src, and installer: the installer and uninstaller of an
// app (internal/appbuild's WindowsInstaller).
//
//go:embed src installer
var Sources embed.FS
