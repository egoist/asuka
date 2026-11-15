//go:build e2e

package e2e

import (
	"fmt"
	"strings"
	"testing"
)

// A page's viewport units are the WebView's height, as innerHeight is:
// Android's WebView makes them 0 when its view's height wraps its content.
func TestWebViewportAndroid(t *testing.T) {
	s := androidApp(t, "e2e/testdata/webviewport", "dev.asuka.webviewport")
	var line string
	until(t, func() bool {
		line = ""
		for _, l := range s.texts() {
			if strings.HasPrefix(l, "measured: ") {
				line = l
			}
		}
		var vh, dvh, inner int
		if _, err := fmt.Sscanf(line, "measured: vh %d dvh %d inner %d", &vh, &dvh, &inner); err != nil {
			return false
		}
		near := func(a, b int) bool { return a-b <= 1 && b-a <= 1 }
		return inner > 0 && near(vh, inner) && near(dvh, inner)
	}, func() string { return fmt.Sprintf("%q, want 100vh and 100dvh as tall as innerHeight", line) })
}
